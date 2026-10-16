# Menu fixes: journal, stamp shop and the selection box

Costume Quest's Android release changed some of the PC menus for touch, and a few of those changes
broke the controller in those menus. CQ64 runs the Android game, so they showed up in Costume
Quest Mobile for Windows and on Android with a controller. The fixes are in shared host code
(`host/src/datapatch.cpp`, `host/ui/`) and apply to both. Researched 2026-10-16 on branch
`general-fixes-4-mobile`, by comparing the Android movies with the PC release's (decompiled with
JPEXS FFDec 26.3.0) and reading `libCq.so`'s strings. No game files are in this repository.

| Menu | Problem with a controller | Fix |
|---|---|---|
| Journal, Quests page | The sticks didn't scroll the quest lists (the mouse wheel didn't either) | `host/ui/notebook_fix.as`, added to `Notebook_DLC.gfx` |
| Stamp shop | Down stopped at the 5th stamp, so the stamps below couldn't be chosen | `host/ui/stampshop_fix.as`, added to `StampShop_DLC.gfx` |
| Main and pause menus | (colour only) the selection box was white, as on PC | The colour patch is gone: red, as on Android |
| Journal, Quests page | Two dark squares in the lower left corners of the pages (other phones, Windows; not on a Mali phone in touch mode) | Textures drawn at the size the movie declares (`host/src/gamepad.cpp`, §7) |

## 1. Which movies the game loads

The Android engine only names the `_DLC` versions of these menus (strings in `libCq.so`):
`UI/Flash/Notebook_DLC/Notebook_DLC`, `UI/Flash/StampShop_DLC/StampShop_DLC`, and also
`HUD_DLC`, `BattleHUD_DLC`, `HUDInteractions_DLC`, `CostumeChooser_DLC`, `BobbingHUD_DLC`. There
is no `UI/Flash/Notebook/Notebook` or `UI/Flash/StampShop/StampShop`, so `Notebook.gfx` and
`StampShop.gfx` are in the pack but never loaded. They take part in the fix (§5).

Android compared with the PC release (Steam/GOG data packs):

| Movie | Android vs PC |
|---|---|
| `Notebook.gfx`, `StampShop.gfx`, `Shop.gfx`, `SellScreen.gfx` | byte-identical |
| `Notebook_DLC.gfx` | touch changes (§2) |
| `StampShop_DLC.gfx` | touch changes (§3) |
| `Pause.gfx`, `FrontEnd.gfx` | touch changes; Android's `Pause.gfx` is stored uncompressed (111,639 bytes) |

`Shop.gfx` is not the shop players see: it is a developer costume shop (`shop.AddCandy(1000)` when
the player has no candy, `shop.EquipCostumePiece`). The battle stamp shop is `StampShop_DLC`.

## 2. Journal (`Notebook_DLC.gfx`), Quests page

### How the page scrolls

Class `Notebook.Quests` (an `DFControls.Control`) holds two lists, `activeQuestHolder` (left page)
and `completedQuestHolder` (right page), each with a `Notebook.Scrollbar`
(`scrollbar_left_mc`, `scrollbar_right_mc`):

- `onLoad()` fills the lists (`game.GetActiveQuestMissions()`, `notebook.GetCompletedQuestInfo(n)`),
  sets each holder's `min_y` / `max_y` from the page height (`activeEnd._y - holder._y`), calls
  `scrollbar.init(0, holder._height - pageHeight)` and sets `onEnterFrame = update`.
- `update()`, every frame: `scrollBars()` sets `holder._y = holder.max_y - scrollbar.checkpos()`
  for both lists; then, if `input.IsGamepadMode()`, `handleInput()`.
- `handleInput()`: the left stick (`_global.g_Navigator.stick_y`) scrolls the active quests and the
  right stick (`stick_rt_y`) the completed ones. Below 0.3 the stick is ignored; otherwise it moves
  the scrollbar by `10 * stick / (holder._height - pageHeight)` (`Scrollbar.SetPosition(percent)`),
  so 10 px per frame at full tilt.
- `onMouseWheel(delta)`: 10 px per notch on the page under the pointer (`bMouseOverLeftPage`,
  `bMouseOverRightPage` from `onMouseMove` hit tests).
- `Scrollbar.SetPosition(percent)` sets `bar_curr = (bar_max - bar_min) * percent + bar_min`, which
  `checkpos()` returns. On PC that is all: stick, wheel and dragging the scrollbar all go through
  the scrollbar.

The engine fills `g_Navigator.stick_x`, `stick_y`, `stick_rt_x`, `stick_rt_y` on Android too (the
names are in `libCq.so`).

### What Android added

- A class `Notebook.QuestHolder` (registered for the `QuestHolder` symbol) for dragging a list with
  a finger: `onMouseDown` over the list sets `dragging`, `orig_y`, `start_y`; `onMouseMove` sets
  `target_y = orig_y + (_ymouse - start_y)`; `onMouseUp` clears `dragging`; `getTargetY()` returns
  `target_y`; `init()` sets `target_y = orig_y = _y`.
- In `update()`, between `scrollBars()` and `handleInput()`:
  ```
  holder._y = clamp(holder.getTargetY(), holder.min_y, holder.max_y)   // both lists
  scrollbar.ForcePosition(holder.max_y - holder._y)                    // new Scrollbar method
  ```
- `Scrollbar.init()` hides the scrollbar when there is nothing to scroll; `ForcePosition(value)`
  sets `bar_curr` and the indicator without the range checks of `SetPosition`.
- Hit tests use `_root._xmouse, _root._ymouse` instead of the PC's `_root.mouseCursor_mc`;
  `_global.touchinput = input.IsTouchMode()` decides whether the PC mouse cursor shows; a Done
  button (`done_mc`, text `CQPC018TEXT`) closes the journal; key 220 (backslash) calls
  `onBackslashButton` on the focused control; companions that aren't there are hidden.

### The bug

`target_y` only changes while a finger drags. Every frame, `update()` first puts the list where the
scrollbar is, then puts it back at `target_y` and moves the scrollbar there too (`ForcePosition`).
The stick and the wheel only move the scrollbar, so whatever they did last frame is undone: the
list never moves. Touch dragging works because it changes `target_y` itself.

### The fix (`host/ui/notebook_fix.as`)

`QuestHolder.getTargetY()` is replaced: while no drag is in progress it first sets
`target_y = _y`, which `scrollBars()` has just set from the scrollbar. Then the stick, the wheel and
the scrollbar move the list as on PC, and a drag works as before (it ends where the finger left
it, and the scrollbar follows). Method calls are looked up at run time, so replacing the
prototype's method from a script that runs after the class definitions is enough.

## 3. Stamp shop (`StampShop_DLC.gfx`)

### How choosing a stamp works

Class `StampShop.ShopMenu`: `itemControls` (one `StampShop.ShopItem` per stamp for sale),
`currentItem`, `firstVisibleControl`, `maxVisibleControls`. Up / Down on a stamp call
`ShopItem.onUp()` / `onDown()` -> `shopMenu.prevItem()` / `nextItem()` -> `setCurrentItem(i)`,
which sets `currentItem`, gives the stamp the focus (`g_Navigator.SetCurrentFocus`) and calls
`layoutControls()`. `layoutControls()` places the visible stamps from `firstVisibleControl` on,
using `ref_item1` / `ref_item2` for the position and spacing, and shows `upArrow` / `downArrow`
when there are stamps above or below.

### What Android changed

| | PC | Android |
|---|---|---|
| `maxVisibleControls` | 6 | 5 |
| `layoutControls()`, selection outside the visible rows | scrolls the list: `firstVisibleControl = currentItem` (above) or `currentItem - (max - 1)` (below) | moves the selection: `currentItem = firstVisibleControl` or `firstVisibleControl + max - 1`, and gives that stamp the focus |
| Up / down arrows | `onRollOver` with the PC mouse scrolls the list and the selection | a tap (`onMouseDown` hit test on `upArrow` / `downArrow`) scrolls the list by one |
| Highlights | always | `_global.touchcontrol = input.IsTouchMode()` (read when the shop opens) hides the focus highlight in touch mode and shows both buy-prompt buttons |
| Other | | Done button (`done_mc`); headline `UITX026TEXT` when `game.IsDLCGame()` is false; backslash key |

### The bug

Android's `layoutControls()` was written for the touch arrows, which move the list and leave the
selection behind. With a controller it runs after the selection moves: Down on the 5th stamp sets
`currentItem` to 5, and `layoutControls()` puts it back to 4. The stamps below the first five could
not be chosen.

### The fix (`host/ui/stampshop_fix.as`)

`ShopMenu.setCurrentItem()` is replaced with the same function plus the PC's scrolling first:
if the new stamp is above the visible rows, `firstVisibleControl = item`; if below,
`firstVisibleControl = item - (maxVisibleControls - 1)`. Android's `layoutControls()` then finds the
selection visible and keeps it. The touch arrows don't go through `setCurrentItem()` and work as
before; touching a stamp (`onRollOver`) selects a visible one, which doesn't scroll.

## 4. Selection box colour (main menu, pause menu)

`FrontEnd.gfx` +0x892 and `Pause.gfx` +0xc04 (the `MenuItem` "Focus" frame's PlaceObject) hold
the selection box's colour transform: red/green/blue multiplied by 0 and red +200, over a dark
texture, so the box is **red** on Android. The PC release's box is white. CQ64 used to patch both
to "multiply by 0, add 255" (white) since its first release (2026-10-13), to match PC. That patch
is gone: the box is red, as Double Fine shipped the Android game. (The stamp shop's and journal's
own highlights are separate artwork and were never changed.)

Note for the `high-framerates-pc` branch: its `build_pause()` (the Advanced page on Windows) copies
the white-box bytes from `PATCHES[1]` into its rebuilt `Pause.gfx`. When the branches are merged
it has to keep the original (red) bytes instead.

## 5. Getting the scripts into the movies

The fixes are scripts of our own (CC0), compiled by `host/ui/build.sh` (JPEXS FFDec, as the
Advanced page's) into DoAction tags in `host/src/menu_fixes_action.h`. At startup
`datapatch.cpp` (`build_fixed()`) reads the movie from the player's own pack, adds the tag at the
end of frame 1 (after the class definitions, which are DoInitAction tags in frame 1), compresses
it again and serves it in place of the original. The pack file is never changed.

The game reads its data with its own zip code from `assets/ant.mp3`, a stored zip inside the APK;
CQ64 overlays bytes onto every read of that file (`read`, `fread`, `mmap` thunks ->
`datapatch::apply`). Same-size patches are enough for most fixes, but not here:

- Both movies are `CFX` (zlib) version 8 and already compressed at level 9. Their compressed
  streams also hold zero padding after the movie's end (the header's length): 1,335 bytes in
  `Notebook_DLC.gfx` (101,065 bytes of movie), 1,966 bytes in `StampShop_DLC.gfx` (624,722).
- Compressed again without the padding and with the fix: `Notebook_DLC.gfx` 29,142 -> 29,081 bytes
  (fits), `StampShop_DLC.gfx` 34,994 -> 35,016 (22 bytes too many). Other zlib settings were worse
  (memLevel 9: 35,431; `Z_FILTERED`: 37,935). The movies have nothing safe to drop
  (`StampShop_DLC` has no Metadata tag; its biggest tags are 254 DefineShape2 = 555 KB).

In the pack, each unused movie lies just before its `_DLC` twin, with only an empty folder entry
in between:

| Entry | Local header (APK, ant.mp3 offsets differ between APKs) | Size |
|---|---|---|
| `Data/UI/Notebook/Opt/Notebook.gfx` (never loaded) | before | 27,605 |
| `Data/UI/Notebook_DLC/Opt` (folder) | | 0 |
| `Data/UI/Notebook_DLC/Opt/Notebook_DLC.gfx` | after | 29,142 |
| `Data/UI/StampShop/Opt/StampShop.gfx` (never loaded) | before | 34,347 |
| `Data/UI/StampShop_DLC/Opt` (folder) | | 0 |
| `Data/UI/StampShop_DLC/Opt/StampShop_DLC.gfx` | after | 34,994 |

`build_fixed()` serves the rebuilt movie from the unused movie's local header on (a new local
header with the right CRC and sizes, the name, the movie), followed by new local headers for the
entries it covers (the unused movie and the folder, now both empty), and points the four
directory entries at them (CRC, sizes, local header offset). Room: 56,864 bytes for the journal,
69,461 for the shop. It checks that only empty entries lie between the two movies and that the
result fits, and otherwise leaves the movie alone (with a warning in the log). The members'
order is the same in every APK checked (they are sorted by path).

The game doesn't check CRCs (the button icon library, padded in place since 2026-10-13, keeps the
old CRC and works); the new entries carry correct ones anyway.

## 6. Tests

- **Pack (2026-10-16):** `datapatch.cpp` itself, built into a small test program (llvm-mingw, run
  with Proton's wine), on `CostumeQuest_hacked-aligned-debugSigned.apk`: both fixes added; the
  `ant.mp3` it serves, read with Python's `zipfile`: all 35,320 other entries pass their CRC check,
  no entries overlap, and only the four entries above differ from the APK. The served movies,
  decompiled with FFDec, contain the new functions after the original frame-1 script.
- **In the game (Costume Quest Mobile for Windows, optimized, under Proton on the Steam Deck,
  2026-10-16, the hacked APK and a copy of a level-1 save):** the log shows both fixes added. The
  main menu's and pause menu's selection box is red. In the journal's Quests page, the right stick
  (test input `stick ry 1 1500`) scrolled the Completed Quests list to its end and the scrollbar
  followed; `stick ry -1` scrolled it back. The Active Quests list fits its page in that save
  (no scrollbar), so the left stick had nothing to scroll. Touches on the Deck's screen happened
  during the run (the log's "controller input after a touch"), so the touch side was used but not
  checked on its own.
- **Stamp shop, same run:** with the console, `debug.CompleteMission('Burbs_MQ_01/Burbs_MQ_01_Intro')`
  (Robot Repair) and `debug.CompleteMission('L_010/L_010_NoCostume')`, `game.AddCandyAmount(5000)`,
  `game.AddExperiencePoints(10000)`: 7 stamps for sale (unlocked, seller count > 0: Bloodshot
  Eyeballs, Pumpkin Guts, Disembodied Hand, Egg, Moving Tombstone, Black Cat, Fang of the Wolf).
  The shop opened as Sadie opens it (`game.SetIngameGUIState(kIngameGUIState_StampShop)`). Down
  with the controller went past the 5th stamp: the list scrolled and the 7th (Fang of the Wolf)
  was selected, with the up arrow showing; Up went back to the first and the list scrolled back.
- **Active Quests (left page), same run:** the Suburbs missions were started from the console so
  the page overflows. A mission only shows there with a description and at least one objective:
  `game.CancelMission(game.GetMission('Burbs_MQ_03/Burbs_MQ_03'))`, then (as a separate console
  command; in the same one the restart doesn't stick)
  `game.StartMainMission(code, 1, NameLineCode, DescLineCode)` with the texts from
  `g_QuestDescriptors`, then `game.AddQuestObjective(RESOURCE('Gameplay/QuestObjectives/<name>.Quest'),
  game.GetMission(id))`. With 8 missions listed, the virtual left stick (`stick ly 1`) scrolled
  the list down to the end and `stick ly -1` back up; the scrollbar followed. The L / R hint
  arrows under the pages stay as they were when the journal opened (`updateArrows()` runs only in
  `onLoad`, on PC too).
- **Not yet tried:** touch dragging of the quest lists, the shop's touch arrows, the Deck's own
  sticks (only the test input's virtual stick), Android.
- Opening the journal from Lua (`game.SetIngameGUIState(kIngameGUIState_Notebook)`) before a
  save has finished loading crashes the game in `IngameGUIManager::LoadNotebook()` (null-page read
  at 0x14: no active session yet, `SessionManager::GetActiveSession()` returns null). A test
  mistake, not related to the fixes; wait for the street before opening menus from Lua.

## 7. The journal's "shadows" (textures smaller than declared)

On the Quests page, with a controller, a dark soft square showed in the lower left corner of each
list (Windows; also seen on other phones, not on the Pixel's Mali GPU, 2026-10-16).

**What it is.** `MapQuest` (the Quests page) places four sprites that tell there is more to scroll:
158 / 165 at the bottom of the left / right page (x -205 / +205, y +520 px) and 160 / 163 at the
top (flipped), with the instance names the script uses (`activeArrowDown`, `completedArrowDown`,
`activeArrowUp`, `completedArrowUp`; `updateArrows()` shows them in controller mode, once, when the
page loads). Each holds sprite 155, a shape (154, 128x64 px, a clipped bitmap fill of image 27,
`page_fade`) scaled 2.89 wide (a 370 px band), and the arrow (`page_arrow`). `page_fade` is a soft
black shadow: alpha rising from 0 at the top to ~0x99 at the bottom, fading at both ends.

**Why it showed as a square.** The PC data has `page_fade` as a 128x64 DXT5 texture. The Android
data has it as a 32x16 L8A8 texture (6 mipmaps); the movie still declares the image 128x64. GFx maps
a bitmap fill to the texture by the texture's own size, so the band's coordinates ran over 4x the
texture in each direction; with the clipped fill's clamping the whole gradient was squeezed into the
first quarter of the band, a ~92 x 16 px blob at its left end, and the rest showed the clamped
edge (alpha 0). That is the dark square. Why the Pixel shows nothing was not settled: in touch mode
the arrows are hidden anyway (`updateArrows()` hides them when `_global.controller_icons` is false),
and Mali may sample the clamped edge differently.

**Other textures with the same mismatch** (all Android movies scanned: image tags 1001 against the
`OGLES/Munged/ui/flash/<movie>/<name>.Txtr~c` textures; the container is a 64-byte header, then zlib,
then the engine's header and a DDS):

| Movie | Image | Declared | Android texture |
|---|---|---|---|
| Notebook, Notebook_DLC | `page_fade.dds` | 128x64 | 32x16 |
| Notebook, Notebook_DLC | `noStamp.dds` (empty Battle Stamps slots) | 64x64 | 16x16 |
| BattleHUD, BattleHUD_DLC | `life_meter_good.dds` | 64x16 | 32x8 |
| Credits | `fade_img.dds` | 256x128 | 64x32 |
| Pause | `coverup.tga` | 128x128 | 64x64 |

**The fix.** `host/src/gamepad.cpp` already relabelled the Xbox A icon's texture (64x64 drawn as a
32x32 icon). That is now general: `FlashImageCreator::CreateImage` and `RsRef<Texture>::BindFile`
are hooked always (not only with the icon library), and when an image's texture is the declared
size divided by a power of two in both directions, the Texture's size (+0x24/+0x26) and the
`GImageInfo`'s (+0x1c/+0x20) are set to the declared size (`fit_declared_size`; the log says
"gamepad: image: 32x16 texture drawn as its declared 128x64"). The shadow is now a soft band along
the whole bottom (and, when scrolled, top) of the list, as the artwork intends.

Tested on Windows (2026-10-16): the Quests page shows the band, no square; the Battle Stamps page's
empty slots fill their squares; a battle's life meters fill their bars. Android: not built.

## Rebuilding the scripts

`FFDEC=<ffdec.jar> host/ui/build.sh` (Java and JPEXS FFDec) after changing `host/ui/*.as`.
