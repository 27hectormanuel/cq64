# The debug console and developer menu

How CQ64 revives Costume Quest's Lua developer console and runs commands from the in-game
developer menu or from a PC over adb. Code: `host/src/devtools.cpp`, `host/src/host_main.cpp`,
`java_patch/src/com/doublefine/cq/DevTools.java`.

## 1. What the retail game left behind

Double Fine's engine has a full Lua developer console and an on-screen debug menu (driven by
`Data/Config/DebugMenuTable.lua` in the data pack). The retail Android build removed three pieces:

| Piece | In the retail `libCq.so` |
|---|---|
| Console typing | `Console::HandleInput` is `mov r0, #0; bx lr` — key presses never reach the console |
| Debug menu | `GameApp::ShowDebugMenu` is a single `bx lr` |
| Console text drawing | `Console::RequestRender` is still there and runs every frame, but the font it draws with is never made (`FontManager::MakeFont` returns null, `FontManager::RenderText` is a stub), so nothing appears |

The part that executes code survived: `Console::ExecuteCommand(const String&, int playerIndex)`
still compiles and runs a Lua string in the game's console environment, and the global
`g_pConsole` still points at the console object, whose `lua_State*` is at offset `+0x18`. So
the job is to feed commands into `ExecuteCommand` and capture their output, without the stripped
UI.

## 2. Running commands on the game thread

Lua is not thread-safe, so commands must run on the game thread between frames, not on the
Android UI thread or an adb-polling thread.

CQ64 hooks a function the game calls once per frame on that thread: `FMOD::EventSystem::update`.
It is an import from `libfmodevent.so`, and the ELF loader resolves imports through a symbol table
in which an `override:<symbol>` entry wins over every guest definition except inside the library
that defines it, so the original can still be called:

```cpp
s32 t_EventSystem_update(u32 self) {
    run_queued();  // run pending Lua first
    return (s32)cpu::call32(sym("libfmodevent.so", "_ZN4FMOD11EventSystem6updateEv"), {self});
}
THUNK(override:_ZN4FMOD11EventSystem6updateEv, t_EventSystem_update);
```

`devtools::submit()` pushes a command onto a mutex-protected queue from any thread. Each frame
`run_queued()` takes the whole queue and runs it. While `g_pConsole` is still null (very early in
boot) commands stay queued instead of being dropped, so commands sent at startup — such as the
controller support's Lua overrides — run as soon as the console exists.

## 3. Calling the game's own C++ from the host

For each command, `run_queued()` calls into the game the way the game itself would:

1. copy the text into guest memory (`mem::hstrdup`);
2. build an engine `String` (a 16-byte `Array<char>`: count/flags, capacity/flags, data),
   zeroed and then filled by the game's own `String::Copy(const char*, int)`;
3. call `Console::ExecuteCommand(console, str, -1)` — player index `-1` means "run locally"
   rather than broadcasting to networked nodes.

`cpu::call32(fn, {args})` loads the ARM argument registers, sets up a guest stack frame, runs
the function in dynarmic until it returns and hands back `r0` — the same mechanism the JNI bridge
uses to call the game's native methods.

## 4. Getting the output back

The console's text output was stripped, so Lua is given a way to call back into the host.

**A native Lua function.** The first time commands run, the host creates a *thunk* — a guest
address whose code is `svc #id`, which traps from the JIT back into C++ — pushes it as a C closure
with the game's own `lua_pushcclosure`, and stores it as the global `hostprint` with
`lua_setfield(L, LUA_GLOBALSINDEX, "hostprint")`. When Lua calls `hostprint(text)`, the host reads
the string with `lua_tolstring` and hands it to `output()`.

**A wrapper around every command**, written in Lua so it behaves like an interactive prompt:

- try `loadstring("return " .. cmd)` first, so an expression like `debug.GetPlayer()` prints its
  value; if that fails to compile, load the command as a statement;
- temporarily replace `print` so its output goes to `hostprint`;
- run under `pcall` so errors come back as `error: …` instead of disturbing the game, then
  restore `print`;
- pass every return value through `hostprint`.

The command text is embedded as a Lua long string (`[==[ … ]==]`) with enough `=` signs that the
command itself cannot terminate it.

**Three destinations.** `output()` writes each line to logcat (tag `cqhost`), appends it to
`files/console.out` in the app's private storage (readable over adb), and keeps the last 16 KB in
a buffer that the Java console window displays.

## 5. The developer menu (Java side)

The original menu UI is gone and most entries in its Lua table call functions this build doesn't
have, so CQ64 ships its own menu in `DevTools`, compiled into a second dex file (`classes2.dex`):

- `scripts/build_apk.sh` patches `CqActivity.dispatchTouchEvent` (in smali) to call
  `DevTools.onTouch`; a third finger going down (`ACTION_POINTER_DOWN` with three pointers) opens
  the menu.
- **Debug command line…** opens a dialog with a text field, a Run button and examples. Run calls
  the JNI method `nativeCommand(cmd)` → `devtools::submit`, then refreshes the output view from
  `nativeOutput()` after 150 ms and 600 ms, because the command only runs on the game's next
  frame.
- The FPS counter, frame-rate cap and render resolution entries use their own JNI calls.

## 6. Pushing Lua from a PC over adb

The build is debuggable, so `adb shell run-as` can write into the app's private storage. Both
sides poll for trigger files:

- **Host** (`host_main.cpp`): a background thread checks `files/console` once a second, submits
  each line as a command and deletes the file. Output is appended to `files/console.out`.

  ```sh
  echo 'debug.GetAllStamps()' | adb shell run-as com.doublefine.cq sh -c 'cat > files/console'
  adb shell run-as com.doublefine.cq cat files/console.out
  ```

- **Java** (`DevTools`): a 500 ms tick opens the menu or the console window when
  `files/devmenu` or `files/devconsole` appears (`adb shell run-as com.doublefine.cq touch files/devmenu`).

Useful for exploring the game's Lua API, for example listing every function it exposes:

```lua
local o={} for k,v in pairs(_G) do if type(v)=='table' and k~='_G' and k~='package' then for kk,vv in pairs(v) do if type(vv)=='function' then o[#o+1]=k..'.'..tostring(kk) end end elseif type(v)=='function' then o[#o+1]=tostring(k) end end table.sort(o) print(table.concat(o,'\n'))
```

Handy commands include `debug.AcquireAllCostumes()`, `debug.GetAllStamps()`,
`debug.ToggleSkipCutscenes()`, `cq.WinBattle()`, `cq.StartBattle()` (starts a test battle) and
`input.IsGamepadMode()`.

## 7. Testing controller input over adb

The host also watches two more files in the app's private storage (same `run-as ... sh -c 'cat >
files/NAME'` pattern as above):

- `files/pad` — lines of `BUTTON MILLISECONDS` hold virtual controller buttons in turn. BUTTON is
  a name (`a b x y back guide start ls rs lb rb up down left right`) or a mask (bit 0 = A, 1 = B,
  2 = X, 3 = Y, 11-14 = d-pad up/down/left/right), e.g. `echo 'x 120'` or `echo '4 120'` presses X.
- `files/padauto` — `1` makes the host answer every battle prompt with synthetic controller input
  (the right face button, a d-pad flick, a press inside the timing window, or a stick wiggle);
  `1 N` additionally turns every prompt into `BattlePrompt` N (0-3 press, 4-7 swipe, 8-11 mash,
  12-15 timed press, 16-19 timed swipe, 20 rub); `0 -1` turns both off. Prompt types and results
  are logged (`adb logcat -s cqhost`). These change how battles play, so switch them off after
  testing.

## 8. Debug switches (both builds)

Read once at startup, so set them before launching the game. Each is the environment variable
`CQ64_<NAME>` (handy on Windows: `CQ64_SKIP_SHADER=Particle "Costume Quest Mobile.exe"`, or before
`play-on-linux.sh`) or the file `files/<name>` (on Android: `adb shell run-as com.doublefine.cq sh
-c 'echo Particle > files/skip_shader'`, then restart the game; on Windows `cq64-data/files/`).
Delete the file to switch it off again.

| Switch | Value | Effect |
|---|---|---|
| `skip_shader` | text[,text...] | Skips every draw whose shader names (the `// Name` line the game's shaders start with) contain one of these: finds which effect draws something wrong. |
| `dump_shaders` | any | Writes every shader the game compiles to `files/shaders/<n>_<id>.glsl` (names, uniforms, code). |
| `gl_stats` | any | Logs texture uploads counted by GL format every 500 uploads (which texture formats an APK uses). |

Windows only (in `host/src/win/win_main.cpp`):

- `cq64-data/files/input` — lines `key 66` (an Android key code), `tap 0.5 0.4` (a touch at that
  fraction of the window), `pad a 200` (a virtual controller button held that long; goes through
  the same path as a real controller, so it also ends touch mode) or `wait 500`. Each line is logged
  as `test input: ...`.
- `cq64-data/files/screenshot` — the next frame is saved as `screenshot.bmp` next to the program
  (F12 does the same).
- `CQ64_IGNORE_CONTROLLERS=1` — behaves as if no controller were connected (tests touch-only play).
- The log names the controller input that ends touch mode (`controller input after a touch`).

If the guest crashes, the register dump and the guest backtrace are also appended to
`files/crash.log`, since logcat can drop them when the game logs a lot.

## Caveats

- A few bytes leak per command: the engine `String`'s buffer comes from the game's allocator and
  is not freed.
- Some commands left over from the developers' menu crash the retail build — for example
  `cq.dbgStartBattleDefault()`. `pcall` catches Lua errors, not native crashes.

For the official Windows and Linux releases (and the Windows build of CQ64), see
[debug-console-pc.md](debug-console-pc.md).
