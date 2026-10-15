# The debug console on PC

How to bring Costume Quest's Lua developer console (see [debug-console.md](../docs/debug-console.md) for
how CQ64 revives it on Android) to three more places:

1. **Costume Quest Mobile for Windows** (this branch), which runs the Android `libCq.so`;
2. the **official Windows release** (`Cq.exe`);
3. the **official Linux release** (GOG, `Cq.bin.x86`).

Findings were measured on 2026-10-15 by disassembly (`objdump`) and decompilation (Ghidra 12.1.4
through a GhidraMCP server, see [Tools](#tools)). No game files are in this repository.

**Built and tested (2026-10-15, Steam Deck):**

| Where | What | Code | Tested |
|---|---|---|---|
| Costume Quest Mobile for Windows | Console drawn over the game: F1 or `` ` ``, Tab completion from the game's Lua, history, scrollback | `host/src/win/console_win.cpp` (see [windows.md](../docs/windows.md)) | Both variants under Proton: title screen and in the world; the game draws normally after it closes |
| Official Windows (`Cq.exe`) | `dinput8.dll` proxy: the game's **own** console comes back (its font, its prompt, its red errors) | [`windows/`](HOWTO.md) | Under Proton 11: opens, typing, Enter runs commands (`> 3`, errors in red), history. Not tested: blocking a **real** keyboard from the game while the console is open (see below) |
| Official Linux (GOG `Cq.bin.x86`) | `LD_PRELOAD` library: typing and drawing in the game's style, over the game | [`linux/`](HOWTO.md) | Natively with the Steam runtime: title screen, history, `~=`, and in the world (`game.AddCandyAmount(100)` → the candy counter shows 100) |

What the tests showed beyond the research:

- `Cq.exe` does create the console's font. Setting `console+0x10` and filling the input line was
  enough; the original console then draws by itself (small white text at the bottom left, errors
  in red, lines fading after 3-5 s).
- `Cq.exe` reads the keyboard through **DirectInput** (it creates `GUID_SysKeyboard` and
  `GUID_SysMouse` devices) as well as window messages. The proxy therefore also blanks the
  keyboard device's `GetDeviceState`/`GetDeviceData` while the console is open. Keys injected for
  testing (`SendInput`) don't reach an unfocused Wine window, so that part could only be checked with
  a real keyboard on a focused game window.
- The Linux game reads `keysym.scancode`, not `keysym.sym`. It ships SDL 2.0.0, which has no
  `SDL_GL_GetDrawableSize` (the library falls back to `SDL_GetWindowSize`).
- Shift+`` ` `` types `~` (Lua's `~=`) in all three, instead of closing the console.
- The game's main thread on Windows renames itself "Buddha Main" (the engine's name), so look for
  that name, not `Cq.exe`, when finding the process.

| Binary | Size | MD5 | SHA-256 | Notes |
|---|---|---|---|---|
| `Cq.exe` (Windows) | 7,294,464 | `d9919c991cafb27ec1e758d68094d622` | `9d5acea0f5da7686e1c2135de00ece8544fc6e3d7cc5c345538963fee3addef8` | PE32 i386, MSVC 2008 (`MSVCR90`), FileVersion 1.0.0.11, stripped, Direct3D 9 |
| `Cq.bin.x86` (Linux, GOG 2.0.0.3) | 21,868,972 | `dbca5c51b7584e5dfd430a86c166fab9` | `7104dc7db2e5a9bc30cfebbacb34656072a8aed4d5b86515208ba93e3d5100ef` | ELF i386, GCC, **not stripped** (59,823 symbols), SDL2 + desktop OpenGL, bundled FMOD 4.42.16 |
| `libCq.so` (Android 1.1.7) | 12,907,304 | `52e0a8bd49fa2602dd9bdfe24df47020` | | ARM32, symbols exported |

Addresses below are for these exact files. A hook should check the hash (or a few bytes at each
address) before patching and do nothing on any other version.

## 1. What each build kept

All three builds come from the same engine code, and the retail builds removed the console in
different places. The part that runs Lua (`Console::ExecuteCommand`) survived everywhere.

| Piece | Android `libCq.so` | Windows `Cq.exe` | Linux `Cq.bin.x86` |
|---|---|---|---|
| `Console::ExecuteCommand(String, playerIndex)` | present | present (`0x565150`) | present |
| `Console::QueueCommand` (network-safe queue) | present | present (`0x564f00`) | present |
| `Console::AddText` (output lines, with display time and colour) | present | present (`0x5654c0`) | present |
| `Console::HandleInput` (typing) | stub: returns false | **gone**: the stub was inlined, `GameApp::HandleInput` (`0x561840`) has no call left | stub: `xor eax,eax; ret`, but **still called** for every input event by `GameApp::HandleInput` |
| Line editing: `DoInsert`, `DoHistory`, `DoTabCompletion`, `GetCharacter`, `DoMoveCursor`, `DoDeleteToEOL`, `MaybeAddToHistory` | present | not found (presumably removed by the linker once nothing called them) | **present** |
| `Console::RequestRender` (draws the lines and the `> text_` prompt) | **present** (0x540 bytes), called every frame from `GameApp::_RequestRender` | **present** (`0x565590`, 1,452 bytes), called from `0x561aab` | stub (`repz ret`) |
| The font it draws with | **none**: `FontManager::MakeFont` returns null and `FontManager::RenderText` is a stub | D3D9 fonts (`D3DXCreateFontA` is imported); expected to work, not yet seen | **none**: `FontManager::PlatformMakeFont` returns 0 |
| `GameApp::ShowDebugMenu` | stub | not checked | stub |
| Lua `print` | prints nothing (the engine's `lua_df_print` formats the text and drops it) | not checked | same as Android |
| Lua `io` / `os` libraries | not exposed | not exposed | not exposed |

So:

- **Official Windows:** only typing is missing. The console's own drawing code is still there and
  is called every frame. Making the console active and filling its input line should bring back
  the original on-screen console with no new drawing code.
- **Official Linux:** typing and drawing are both stubbed, but every editing helper is there and the
  game still calls the stubbed `HandleInput`. Replacing that one stub restores the original typing
  logic; drawing needs new code.
- **Android, and therefore the Windows build:** drawing code is there but has no font, so CQ64's
  console needs its own UI (on Android it is the Java dialog).

## 2. Facts shared by all builds

**Console object** (pointed to by the global `g_pConsole`: Linux symbol `g_pConsole` at
`0x917eab0`; Windows `0xa915b8`; Android symbol `g_pConsole`):

| Offset | Field |
|---|---|
| `+0x10` | `bool` active: input goes to the console, and the prompt line is drawn |
| `+0x18` | `lua_State*` the console runs commands in |
| `+0x1c` | cursor position in the input line |
| `+0x20` | console spam level (`app.ToggleConsoleSpam()` flips it between 1 and 3) |
| Windows `+0x28` / Linux `+0x24` | lock (Windows `CRITICAL_SECTION`, Linux `LwMutex`) guarding the lines |
| Windows `+0x50` | lines: `Array<ConsoleLine>` (count in bits 6+, then capacity, then data at `+0x58`) |
| Windows `+0x5c` | input line: `Array<char>` (data pointer at `+0x64`) |

A `ConsoleLine` is 0x24 bytes: the text (`String`, 12 bytes), seconds left on screen (`float`,
`+0x0c`), "stays until cleared" (`bool`, `+0x10`, set when the time is negative) and an RGBA
`Color` (`+0x14`). `Console::Update` counts the times down and removes expired lines.

**`Console::SetActive(bool)`** stores the flag at `+0x10` and adds or removes one from a global
counter (most likely the "something is capturing input" count that keeps the game from reacting to
keys while the console is open). Closing
it calls `ResetInput()`.

**`Console::ExecuteCommand`** (decompiled from `Cq.exe`; the Linux one has the same shape):

1. sets the Lua global `__currentPlayer` to the player index;
2. compiles `"return " .. command`, and if that fails, the command itself (`luaL_loadbuffer`);
3. runs it with `lua_pcall(L, 0, LUA_MULTRET, 0)`;
4. **error:** `AddText(message, red (1, 0.2, 0.2, 1), 5 s)`; **success** with results: the
   results are turned into text (`Lua_StackToString` on Linux) and shown as `> results` for 3 s;
5. sets `__currentPlayer` back to false.

So the game already echoes results and errors into its own line list. A front end only has to
show those lines; it doesn't need CQ64's `hostprint` trick, except that `print()` itself prints
nothing.

**Key codes.** `InputEventData` carries the key at `+0x0c`, the event type at `+0x10` (1 = key)
and Alt at `+0x2e`. Both PC builds toggle full screen on Alt + key `0x1c`, which is DirectInput's
`DIK_RETURN`, so the engine uses DirectInput scan codes on Linux too. The console's usual key
`` ` `` is `DIK_GRAVE` (`0x29`).

**Threads.** Lua must run on the game thread. Both PC builds render on a separate thread
(`SceneGraph::_RenderThreadMainLoop` on Linux), so a buffer swap is the wrong place to run
commands, but the right place to draw.

## 3. Costume Quest Mobile for Windows

What already works (shared host code): `cq64-data/files/console` → `cq64-data/files/console.out`
(`tools/scripts/cq64-win-tests/wctl.sh lua <command>`). Missing: a way to type commands in the
game window, because the in-game menu is the Android Java `DevTools` dialog, which the Windows build
doesn't have.

**Recommended:**

1. **Quick win: a text console window.** A key (`` ` `` or F1; neither is mapped to an Android
   key in `android_key()` in `win_main.cpp`) or `CQ64_CONSOLE=1` calls `AllocConsole()`. A
   thread reads lines from it and calls `devtools::submit()`, and `devtools::output()` also
   writes each line there. About 30 lines of code, and it uses everything the Android console
   uses: the Lua wrapper, `hostprint`, the queue run from `FMOD::EventSystem::update` on the game
   thread.
2. **In-game overlay, matching the Android dialog.** Draw a console over the game in
   `t_eglSwapBuffers` (`host/src/thunks_android.cpp`, which already serves the F12 screenshot
   hook on Windows), before the real swap:
   - **UI:** Dear ImGui (MIT) with its SDL2 and OpenGL 3 (GLES 3 on ANGLE) backends gives a text
     field, history, scrollback and a completion popup. The completion list is the same
     `hostlist()` / `luahints` data the Android console uses.
   - **Input:** while the overlay is open, `win_main.cpp` sends SDL key and text events to it
     (`SDL_StartTextInput`) instead of `onNativeKeyDown`, and Escape closes it instead of acting
     as pad B.
   - **Threads:** SDL events arrive on the main thread and drawing happens on the guest's GL
     thread, so events pass through a small locked queue. The ImGui GL backend saves and
     restores the GL state it changes. The game's own state (bound program, textures, blend,
     viewport) must come out unchanged, or the next frame breaks.
   - **Cost:** about 1.5 MB of vendored source, built only for Windows.
   - **Without ImGui:** a tiny GLES 2 renderer with an embedded 8×16 bitmap font, which needs
     more of our own code for editing.
3. **Not recommended:** reviving the game's own console in `libCq.so`. The host could replace
   `Console::HandleInput` and call the surviving editing helpers, but there's no font (see §1),
   and keys would have to be turned back into the engine's `InputEventData`. It's more work than
   (2), for a worse result on a phone screen.

## 4. Official Windows release (`Cq.exe`)

There's no source and no symbols, so the code has to be loaded into the game, and then the game's
own console can do the rest.

**Loading:**

- **The DLL:** `Cq.exe` imports `DINPUT8.dll` (only `DirectInput8Create`). A `dinput8.dll` next to
  `Cq.exe` that forwards that call to the system DLL is loaded automatically. Under Wine/Proton,
  also set `WINEDLLOVERRIDES=dinput8=n,b`. `XINPUT9_1_0.dll` and `d3d9.dll` are also imported,
  but proxying them is more work.
- **Patching:** in `DllMain`, check the SHA-256 or the bytes at each patch site, then patch with
  `VirtualProtect`. The hook functions use the game's own conventions (below).

**Hook points:**

| What | Where | How |
|---|---|---|
| Keys | `GameApp::HandleInput` `0x561840` (`__thiscall`, `ret 4`, event pointer on the stack); its first 5 bytes, `push esi; mov esi,[esp+8]`, are easy to move into a trampoline | Grave key down: flip `console+0x10` (or call our own `SetActive` copy). While active, eat every key event and edit the input line: printable keys (`ToAscii`/`MapVirtualKey` with the Shift state), Backspace, Delete, Left/Right, Home/End, Up/Down history (ours), Enter. |
| Run a command | `Console::ExecuteCommand` `0x565150` | Custom convention: the `String*` in **EAX**, then `push playerIndex(-1)`, `push console`, callee pops 8. Called from the key hook, which is already on the game thread, like the original. |
| Show text | nothing to do | `ExecuteCommand` puts results and errors into the line list, and `Console::RequestRender` (`0x565590`) draws them along with `> input_` while `+0x10` is set. To add our own lines: `AddText` `0x5654c0`, console in **EAX**, then `push seconds`, `push Color*`, `push const char*`. |
| Input line | `console+0x5c` `Array<char>`, data at `+0x64`; cursor at `+0x1c` | Write it with the game's own `String` copy function (`0x70b6e0`, used by `AddText`) so the allocator matches. Or keep the text in the DLL and copy it in each frame. |
| Keyboard path | `PeekMessageA`, `SetWindowLongA` (USER32), DirectInput | If the game reads keys through DirectInput as well as `GameApp::HandleInput`, the proxy can also blank the keyboard state while the console is open. |

**Lua API addresses found on the way** (from `ExecuteCommand` and `luaapp_IsConsoleSpamEnabled`):
`lua_pushstring 0x755080`, `lua_pushnumber 0x755000`, set-global `0x755470` (with
`LUA_GLOBALSINDEX`), `luaL_loadbuffer 0x7564b0`, `lua_settop 0x754a50`, `lua_gettop 0x754a40`,
`lua_pcall 0x755760`, `lua_tolstring 0x754e10`, `lua_pushboolean 0x7551e0`. Also
`luaapp_ToggleConsoleSpam 0x565b50`, `luaapp_IsConsoleSpamEnabled 0x565b70`, console creation
`0x564ce0` (0x98-byte object) and destruction around `0x5610ca`.

**First test:** with the DLL, set `console+0x10 = 1` and type nothing. If the D3D font exists,
`> _` appears at the bottom of the screen. That one byte tells whether the original console UI
still works.

## 5. Official Linux release (`Cq.bin.x86`)

The best case of the three: symbols, typing logic still present, and `HandleInput` still called.

**Loading:** an `LD_PRELOAD` library built for i386 (`gcc -m32 -shared -fPIC`), started through
the GOG `start.sh`, Heroic or Steam as a non-Steam game. The libraries it imports
(`libSDL2-2.0.so.0`, the FMOD libraries) can be replaced function by function, and code inside
the executable is patched from a constructor (`mprotect` + `jmp rel32`; the executable isn't
position-independent, so the addresses are fixed).

**Typing: replace one stub.** `Console::HandleInput` (`0x8557790`) has 16 bytes before the next
function, which is room for a `jmp` to our version. Ours restores the original behaviour from the
pieces that are still there, all called directly by symbol address (GCC i386 `thiscall` = `this`
first on the stack):

- grave key → `Console::SetActive(!active)`;
- while active: `Console::GetCharacter(event, char&)` → `Console::DoInsert(String, bool)`;
  Backspace/Delete → `DoDeleteToEOL`/`DoMoveCursor`; Up/Down → `DoHistory(±1)`; Tab →
  `DoTabCompletion()`; Enter → `MaybeAddToHistory`, `QueueCommand(String, bool)` (on Windows it calls
  `ExecuteCommand` directly unless a network or recorded game session is running), `ResetInput()`;
- return true for keys the console used, so `GameApp::HandleInput` stops there.

This runs on the game thread, from `DFApp::PlatformLoop` → `PhysicalInputManager`.

**Drawing: new code.** `Console::RequestRender` and the platform font are stubs, so draw in an
interposed `SDL_GL_SwapWindow` (render thread; called from `GraphicsManager::Present`):

- copy the lines under the console's lock (`+0x24`, `LwMutex::Lock`/`Release`);
- draw them, then `> input_`, with fixed-function desktop GL (the game links `libGL.so.1`) and a
  bitmap font in the library;
- also print each new line to stdout, for a terminal.

Alternatively, port Android's `RequestRender` logic, which the decompilation shows (copy lines,
fade by time left, prompt with `_` cursor), to our own drawing.

**Commands from outside:** run a FIFO or file poller like CQ64's `files/console` from an
interposed `SDL_PollEvent`, which `DFApp::PlatformLoop` calls on the game thread every loop.
Every Lua C API function has its symbol (`lua_pushcclosure 0x8df2bc0`, `lua_setfield 0x8df34d0`,
`lua_tolstring 0x8df21f0`, `luaL_loadbuffer 0x8dea8d0`, `lua_pcall 0x8df3be0`), so CQ64's
`hostprint` approach also ports directly. The FMOD hook CQ64 uses
(`FMOD::EventSystem::update`, called from `SoundManager::_InternalUpdate`) is also an import
here, but `SDL_PollEvent` is the one known to be on the game thread.

Other notes: the command line only takes `-width -height -fullscreen -window -syncframerate
-novsync -ssaa2x -ssaa4x` (`DFApp::ProcessCommandLine`); no console switch. `DFCONFIG` is empty
in both PC releases.

## 6. Open questions

- ~~Does `Cq.exe` create the console font?~~ Yes (tested).
- ~~DirectInput too?~~ Yes: the game creates a DirectInput keyboard; the proxy blanks it while the
  console is open (not yet checked with a real keyboard).
- Which Lua functions the PC builds have compared with Android's 1,117 (the list in
  `debug_commands.txt`): run the function-listing command from debug-console.md §6 once a console
  works.
- Whether Linux `QueueCommand` from `HandleInput` behaves the same as `ExecuteCommand(-1)` in
  single player. It should: it only builds a network command while a game session is recording or
  playing back.

## Tools

- **GhidraMCP:** LaurieWired's GhidraMCP (github.com/LaurieWired/GhidraMCP, checked at commit
  `1e98025`, 2026-10-10) is a Ghidra plugin with an HTTP server (default `127.0.0.1:8080`) and an
  MCP bridge, and now also a PyGhidra headless mode (`--headless --binary`). Its tools
  (`decompile_function_by_address`, `get_xrefs_to`, `list_strings`, `rename_function`, ...) are
  the ones this work needs. The decompilation here was done with the headless server of its
  larger fork (`bethington/ghidra-mcp` 7.0.0), already set up on the Steam Deck with the Ghidra
  project holding `Cq.exe` and `libCq.so` (`tools/start_ghidra_mcp.sh`, port 8089,
  `GHIDRA_HEAP=2g` when memory is short). The same calls work with LaurieWired's server.
- **REA** (github.com/morluto/rea, `rea-agents` 6.4.0): used for artifact identity
  (`rea inspect-artifact` confirmed `Cq.exe`'s SHA-256 above). Its deep-analysis providers
  (Ghidra, Hopper, IDA, pwntools) refuse to run on SteamOS (`rea doctor`: `unsupported_host`). On
  a supported Linux or macOS host, `rea function <binary> <address>` would give the same
  function-level evidence.
- **objdump** (binutils, which reads PE i386 too) and the Android NDK's `llvm-objdump` for the
  byte-level checks (stub bodies, call sites, string references).

Reproduce the main checks:

```sh
nm -CS --size-sort Cq.bin.x86 | grep -E "Console::|FontManager::"   # stubs are 2-3 bytes
objdump -d --no-show-raw-insn Cq.exe | grep -n "call   0x565590"     # RequestRender call site
tools/gq get_functions function=0x565150 fields=decompiled_code      # PC ExecuteCommand (local curl helper for the GhidraMCP server)
```
