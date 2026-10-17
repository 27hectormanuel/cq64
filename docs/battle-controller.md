# Battles with a controller: parity with the PC version

How Costume Quest's battle screen looks and works with a controller on PC, what the Android
release changed, and how CQ64 makes controller battles on Mobile (Windows and Android) match the
PC. Researched 2026-10-16 on branch `general-fixes-4-mobile`: the PC reference was the Steam
`Cq.exe` played with a controller (screenshots through the dev-tools console DLL), the code was
read in `libCq.so` and `Cq.exe` (Ghidra 12.1.4, llvm-objdump) and the battle HUD movies were
decompiled with JPEXS FFDec 26.3.0. No game files are in this repository.

The earlier work (`gamepad.cpp`, 2026-10-13) already judges battle prompts the PC way (buttons,
flicks, mashing, timing) and pushes `ShowPCButtons` so the HUD shows button icons in controller
mode. This adds the PC's layout and the run-away controls.

## What the PC shows with a controller

| Element | PC (controller) | Android before |
|---|---|---|
| Action menu | Next to the fighter whose turn it is: X (ability label to its left, e.g. "Basic Attack"), Y (the special ability: its name above, the candy-corn meter until it is charged), B (battle stamp, label to the right); 32 px icons | A column of big touch buttons in the top left corner (64 px pictures), labels under them, a dark backdrop |
| Run away | White box top right joined to the screen border: LT icon, "+", RT icon, "RUN AWAY" (localised); LT + RT flees at once | Touch run-away button (opens "Are you sure? Run away! Run away!") and a MENU (pause) button |
| Prompts | Speech bubble ("PRESS (X) NOW!", "MATCH THE TIMING!" with a bar and a small button icon under it, "SUCCESS!", "DODGED!") | Same bubbles; touch tap targets around them (already hidden in controller mode by `gamepad.cpp`); a 64 px button icon 22 px lower under the timing bar |

In keyboard mode the PC shows keys instead (e.g. "R  RUN AWAY"); not relevant to Mobile.

## 1. The action menu's position

`Battle_PushFlashVars` (PC 1.0.0.11 `FUN_004222e0`, Android `Battle_PushFlashVars`) pushes the
battle HUD's variables every frame. Both compute `BattleActorsScreenPosX/Y` (for the damage
numbers): for each of the 6 fighters (`g_BattleActors`), the head position (Skeleton bone `Head`,
else `Driver_Head`, else the root: PC `FUN_00421e40`, inlined on Android) with its **height
replaced by 5.0**, projected with the active camera (Android: `CameraManager::GetActivePlayerCoCamera`
→ `CoCamera::AsRender` → `CoCamera::Proxy::WorldCoordsToScreen(vec3)`, × 1280 and × 720).

The PC also pushes `ButtonMapScreenXCenter` / `ButtonMapScreenYCenter`: the same projection for the
active fighter (`g_BattleActiveActor`, PC `DAT_00a278d4`) with the head's **real** height. Its HUD
script sets `buttons._x/_y` to it. Android has neither the variables nor that line.

CQ64 (`host/src/gamepad.cpp`): while `Battle_PushFlashVars` runs, `Skeleton::GetWorldTransform`
(the `Name` overload) is hooked to note the head's height, and `WorldCoordsToScreen` is hooked to
project each fighter again with it. The projections come in fighter order for the fighters
present; the active one's result is pushed under the PC's two names
(`FlashValueSnapshot::SetGlobalVariable(Name, float)`; softfp, the float in a core register).
Android disassembly: `movw/movt r0, #0x40a00000; str r0, [sp, #0x64]` (the 5.0) before
`AsRender(sp+56, camera)` and `WorldCoordsToScreen(result sp+80, proxy, world sp+96)` at
`libCq.so+0x7c20a4..0x7c20cc`.

## 2. The action menu's layout (`BattleHUD_DLC.gfx`)

The `buttons` sprite, PC frame "Console" vs Android:

| Child | PC | Android |
|---|---|---|
| `button_x` | (-48, -91) 100 % | (7, 32) 200 % |
| `button_y` | (-16, -123) 100 % | (7, 168) 200 % |
| `button_b` | (16, -91) 100 % | (7, 304) 200 % |
| `abilityMeter` | (0, -107) 16.45 % | (42, 196) 32.86 % |
| `xText` | (-411, -94), 360 px, right-aligned | (7, 93), 300 px, left |
| `yText` | (-248, -162), 500 px, centred | (7, 229), 300 px, left |
| `bText` | (54, -94), 360 px, left | (7, 365), 300 px, left |
| `button_b_bg` | - | (0, 272) |
| unnamed shape 117 (depth 2) | - | the column's backdrop, 256 x 286 px |

Button glyphs are 32 x 32 images in both; label fields use `$GoodDog` 30 px in both. Android's
`buttons` sprite sits at (75, 64) on the stage; the PC's is moved every frame.

`host/ui/battlehud_fix.as` wraps `updateButtons()` (called by name from `update()` every frame):
in controller mode (`ShowPCButtons` false: on Android true means the touch pictures) it applies the
PC values above, hides `button_b_bg` and the backdrop, and moves `buttons` to
`ButtonMapScreenXCenter/YCenter`; in touch mode it restores Android's values. The backdrop is an
unnamed shape, which a script can't reach, so `datapatch.cpp` (`name_buttons_panel`) wraps it in a
new one-frame sprite placed as `cq64_panel` (found by the `button_b_bg` sibling, not by id).

The timing prompt's button (`progressBar.instruction.button`): PC (74, -2.7) at 50 % of a 64 px
glyph (= 32 px); Android (75, 19.3) at 100 % of a 64 px `buttonIcon`. Set to the PC values in
controller mode.

## 3. Running away

PC: `runAway` (sprite 145) at (1030, 80), one frame per language (`kLANG_enUS`, `esES`, `deDE`,
`frFR`, `itIT`) with the white box (shape 133: -40..200 x -31..29, scaled per language), `runText`
($GoodDog 30, right-aligned: RUN AWAY, HUIR, WEGRENNEN, FUIR, SCAPPA) and `runawayButtons`
(frame "Console": `BUTTON_TriggerLeft` at 0, "+" at (42, 13), `BUTTON_TriggerRight` at 66, the
icons shown 54 px). Shown while `BattleCanRunAway`. `Cq.exe` has no run-away question (no
`SYST025TEXT`): LT + RT flees at once.

Android: `Battle_HandleInput`, with `g_BattleCanRunAway`, opens `ModalDialog::Show(..., type 4,
BattleInput_RunAwayCallback, 0)` ("Are you sure?" / "Run away! Run away!", Yes! / No!) when the
virtual pad's left and right triggers are held (`VirtualGamepad` +0x116, +0x117), R is pressed
(+0x129) or the touch button is tapped. Type 4 is Android's (button labels `UITX014TEXT` /
`UITX016TEXT`); `MessageBox.gfx` closes on A only for types 1 and 3 and on B only for types 2 and
3, so with a controller the question could not be answered (the battle went on behind it).
`BattleInput_RunAwayCallback(result)` sets the action `"retreat"` when result is 1.

Triggers reach the engine: `SDLGamepad::UpdateDeviceState_GameController` turns trigger axes above
0x5998 into GamepadButton11 / 12, which input.lua's `SDL_Gamepad` layout binds to TriggerLeft /
TriggerRight.

CQ64:
- `gamepad.cpp` hooks `ModalDialog::Show` (the `const char *` member overload): in controller mode,
  for `BattleInput_RunAwayCallback`, the callback gets 1 at once, as on PC. Touch keeps the question.
- `battlehud_fix.as` draws the PC box in controller mode, per language (drawing API, `$GoodDog`
  text fields), and hides the touch run-away and pause buttons (Start pauses).
- The HUD doesn't import the trigger icons, so `datapatch.cpp` (`add_trigger_icons`) adds two
  ImportAssets2 tags (one symbol each; with both in one tag GFx resolved only the first) for
  `BUTTON_TriggerLeft` / `BUTTON_TriggerRight` from `Shared/ControllerIcons.swf` (CQ64's icon
  library, which carries the Xbox icons), a 32 x 32 shape filled with each, and sprites exported as
  `cq64_LT` / `cq64_RT`.
- Note: Android keeps `_global.HUDVisible` false during battles, so it can't gate the box.

The movie is served as described in [menu-fixes.md](menu-fixes.md#5-getting-the-scripts-into-the-movies)
(BattleHUD.gfx, never loaded, lies just before BattleHUD_DLC.gfx in the pack).

## Tests (Costume Quest Mobile for Windows, optimized, Steam Deck, 2026-10-16)

Battles started with `cq.StartBattle()` once the street HUD was up (earlier the game crashes in
`Battle_Reset` / `CutsceneManager::SetStickySkip`, no session yet). Controller input from the
Windows test input (`pad`, `stick`; `stick lt+rt 1 800` holds both triggers), prompt success from
`files/padauto`.

- Controller mode: action menu next to Wren, then Reynold on his turn, at the PC's sizes; no
  backdrop; "LT + RT  RUN AWAY" box top right; no touch buttons.
- Basic Attack (X): "MATCH THE TIMING!" with the 32 px icon under the bar, "SUCCESS!", damage.
  Enemy turn: "PRESS (B) NOW!". Missile Barrage (Y, once charged): fired, enemy 54 -> 19.
- LT + RT: fled at once ("gamepad: run away (LT + RT), no question asked").
- Touch (a tap): Android's column, touch run-away and MENU buttons back, no PC box.
- Not tested: Android, other costumes' abilities, battle stamps (B), languages other than English.
