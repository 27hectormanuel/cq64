# PC fixes and the Android build

Fixes made while bringing up the Windows build (branch `windows-poc`, 2026-10-15), with the files
they touch and what they mean for Android. Most of the host code is shared, so a fix in a shared
file reaches Android once the branch is merged; this list says which ones Android needs, which
are PC-only, and what has been tested where.

| Fix | Files | Android | Tested |
|---|---|---|---|
| Battle HUD: the ability button (Missile Barrage etc.) showed the controller Y icon in touch mode | `host/src/gamepad.cpp` | **Needed, shared code.** The same HUD bug shows on Android from the second battle on and after any switch from controller to touch. | PC: both cases. Android: builds; not yet tested on the phone. |
| Controller mode follows the input used last (a touch turns it off until the next controller input), instead of staying on while any controller is connected | `host/src/win/win_main.cpp` (PC), `java_patch/src/com/doublefine/cq/DevTools.java` (Android) | **Needed; Android has its own copy** in DevTools.java (`sTouched`). | PC: yes. Android: compiles; not yet tested on the phone. |
| Hide GL extensions the phone's Mali GPU lacks (float render targets, ANGLE instancing, map_buffer_range, ...): fixed coloured beams and flashes in battles | `host/src/thunks_gl.cpp` | PC-only (`#ifdef _WIN32`): a phone's driver reports its own list. | PC: battles without flashes. |
| Stick and trigger dead zones (a resting stick no longer counts as controller use) | `host/src/win/win_main.cpp` | Not needed: Android filters resting sticks itself (each axis's "flat" range). | PC: yes. |
| Controller parity with Android: menus' controller look while a controller is connected, PlayStation icons for Sony pads, the controller used last drives the pad, Escape = B | `host/src/win/win_main.cpp` | Already on Android (DevTools.java). | PC: yes. |
| Debug switches `skip_shader`, `dump_shaders`, `gl_stats` (were Windows-only) and button names in `files/pad` | `host/src/thunks_gl.cpp`, `host/src/game_cq.cpp`, `host/src/game.h`, `host/src/host_main.cpp` | **Now shared:** environment variable or a file in `files/`. See [debug-console.md](debug-console.md#8-debug-switches-both-builds). | PC: `gl_stats` file and `files/pad` names. Android: builds. |
| In-game debug console (F1 or `` ` ``), test input `sdlkey` / `type`; the host's own controller-mode Lua no longer echoed in the console | `host/src/win/console_win.cpp` (new), `host/src/win/win_main.cpp`; `host/src/devtools.cpp`/`.h` gain `output_text()`, `clear_output()`, `command_list()` | PC-only. The devtools additions are shared but unused on Android (it has the Java dialog). Android still echoes `_cqPad = …` in its console; the same `\x01` prefix in `DevTools.java` would hide it. | PC: both variants. Android: not built. |
| Test input `pad <button> [ms]`, `CQ64_IGNORE_CONTROLLERS` | `host/src/win/win_main.cpp` | PC-only test tools (Android: `files/pad`, and unplugging the controller). | PC: yes. |

## How the HUD bug works

The game uses `UI/BattleHUD_DLC` (not `BattleHUD`). Its `buttons.button_y` clip has a `Console`
frame (the Y icon) and one frame per costume (`Robot`, `Knight`, ...) with the touch picture. The
script (`updateButtons`) moves it to the costume frame only when `_global.CurrentCostume` changes,
but moves it to `Console` in `resetHUD` (every battle start) and whenever `ShowPCButtons` turns
false. The stock game never changed `ShowPCButtons`, and after the first battle the costume rarely
changes, so the touch picture never came back. `gamepad.cpp` (`t_push_vars`) now sets
`_lastCurrentCostume` to false on every HUD update, so the script picks the right frame each time.

Note: `ShowPCButtons` true means the **touch** icons on Android (the name is from the PC version,
where it meant keyboard icons).

## Still to do on Android

- Build and install the optimized variant on the phone and check, in a battle: a charged ability
  shows its costume picture in touch mode (second battle too), and switches to Y and back when a
  controller is used and then the screen is touched.
