# Desktop frontend and tools

This guide covers the SDL desktop dashboard, debugger, input movies, TAS editor,
sprite editor, GameShark manager, and desktop-specific controls.

## Desktop frontend

When SDL3 is installed, CMake builds the desktop frontend. Launching it
without arguments opens the game library dashboard:

```sh
./build/gbb
```

On Windows, the running-game window uses a native menu bar instead of drawing
a hamburger button over the Game Boy framebuffer. Its File, Emulation, View,
Tools, and Help menus expose ROM/library navigation, save states, pause/reset,
fullscreen, every palette and video pipeline, control settings, GameShark,
debugger and movie/TAS/sprite tools, shortcuts, and application information.
Unavailable game-specific actions are disabled until a ROM is loaded, while
active palette, video, pause, fullscreen, and recording choices are checked.

While a game is running, press `F12` to open the desktop debugger. It shows the
complete CPU register file, flags, execution state, cycle count, important
LCD/interrupt registers, and a nearest-neighbor framebuffer preview outlined
at the Game Boy's 160x144 visible viewport. The debugger pauses when opened;
use `F5` to run or pause, `F10` to advance one CPU instruction, and `F11` to
advance one frame. The same actions are available as clickable buttons. While
paused, click an individual CPU register value to edit it in hexadecimal;
press Enter to apply the value or Escape to cancel the edit.

For PC breakpoints, pause at the desired instruction (or edit `PC` directly)
and press `F2`; pressing `F2` again at the same PC removes that breakpoint.
Press `F3` to clear all desktop breakpoints. Breakpoints are checked before an
instruction executes, so a hit leaves the CPU and framebuffer ready for
inspection and shows the hit address in the debugger. Resuming with `F5` lets
the current instruction execute once before the breakpoint is checked again.
Breakpoints are cleared when a different ROM is loaded.

The debugger also provides deterministic input movies. Press `F6` to start a
recording and `F6` again to stop and save it; press `F7` to replay the latest
recording. A recording includes its starting emulator state, ROM fingerprint,
and cycle-timestamped Game Boy button transitions, so replay starts from the
same state and rejects a different ROM. The latest movie is stored as
`replays/last-input.gbbmovie` in GBB's settings directory (beside the executable
in the portable Windows build).

An input movie is a binary `GBBMOV1` file. For reference, the following is an
annotated, human-readable representation of a short sample recording (the
embedded starting state is abbreviated):

```text
format: GBBMOV1
rom_fingerprint: 0x87604941b8eda76f
starting_state: <embedded GBB save state, 292009 bytes>
events:
  - cycle: 0       button: Start  pressed: true
  - cycle: 70224   button: Start  pressed: false
  - cycle: 280896  button: Right  pressed: true
  - cycle: 561792  button: A      pressed: true
  - cycle: 632016  button: A      pressed: false
  - cycle: 842688  button: Right  pressed: false
```

This represents Start being held for one frame, followed later by Right and A.
Cycle offsets are measured from the embedded starting state. The fingerprint
shown above is illustrative: a real recording can only be replayed with the ROM
whose fingerprint is stored in that file. GBB creates a usable sample for the
currently loaded ROM as soon as `F6` starts and then stops a recording.

## TAS frame editor

Press `F8` in the debugger to open the tool-assisted input editor. Each row is
one emulated frame and each column is a Game Boy button; click a cell to hold
that button for the entire frame. The editor supports inserting, deleting, and
appending frames, saving the timeline, replaying it from its captured starting
state, and starting a new timeline from the emulator's current state.

- Arrow Up/Down: select a frame
- Home / Ctrl+End: jump to the first / last frame
- Page Up / Page Down: move by one visible page
- Insert: insert an empty frame before the selection
- Delete: remove the selected frame
- End: append an empty frame
- Ctrl+Z / Ctrl+Y: undo / redo timeline edits
- Ctrl+C / Ctrl+V / Ctrl+D: copy / paste / duplicate a frame
- Backspace: clear the selected frame; drag across cells to paint
- Ctrl+S: save to `replays/last-tas.gbbmovie`
- F7: save and run the timeline
- Ctrl+N: discard the timeline and capture the current emulator state

Normal input recordings and TAS timelines use the same deterministic movie
format, but are stored separately so saving a TAS cannot overwrite a normal
recording. `F7` in the TAS editor runs the edited timeline; `F7` in the
debugger replays the latest normal recording.

## Live sprite editor

Press `F9` in the debugger to open the paint-style VRAM tile editor. The tile
browser displays all 384 current 8x8 tiles; select one to edit it in a magnified
pixel grid. Choose color index 0-3 with the palette swatches or number keys,
then paint with the left mouse button. The right mouse button paints color 0.
`Ctrl+Z` restores the tile from before the last stroke, Delete clears it, and
`B` switches between CGB VRAM banks.

Sprite edits can be stored in two patch formats:

- `Ctrl+S` saves `sprite-patches/last-sprite-edit.gbbtiles`, a GBB live-tile
  patch containing the ROM fingerprint plus original and replacement tile data.
- `Ctrl+O` imports that live-tile patch into the matching ROM session.
- `Ctrl+E` exports `sprite-patches/last-sprite-edit.ips`, a standard IPS patch
  for use with external ROM patchers.

IPS export is deliberately conservative. It only includes an edited tile when
its original 16-byte bitplane data occurs exactly once in the ROM. Tiles whose
source is compressed, generated dynamically, duplicated, or otherwise
ambiguous are skipped and reported. The GBB live-tile format remains available
for those tiles because it applies directly to VRAM instead of guessing a ROM
offset. IPS exports also include updated Game Boy header and global checksums.

Edits are applied directly to the running emulator's VRAM and appear
immediately. They do not modify the ROM file, and a game can overwrite the
edited tile after execution resumes. This makes the editor suitable for live
graphics experiments and debugging without risking the original ROM.

## GameShark cheat manager

While a desktop game is running, press `Ctrl+G` to open its ROM-specific cheat
manager. Click **Fetch for ROM** to retrieve the matching Game Boy or Game Boy
Color collection from the [Libretro Database](https://github.com/libretro/libretro-database),
or enter a description and code to add a manual cheat. Archive results, manual
entries, and enabled states are cached per ROM fingerprint in `cheats/*.cht`.
No ROM data is uploaded. Libretro Database content is distributed under
[CC BY-SA 4.0](https://github.com/libretro/libretro-database/blob/master/LICENSE).

Click the checkbox beside a cheat (or select it and press Space) to enable or
disable it. Delete removes the selected entry. Multiple writes can be joined
with `+`, for example `019973D5+019974D5`. GBB currently accepts conventional
Game Boy/Game Boy Color GameShark type-01 writes (`01VVLLHH`); unsupported
engines and code types are rejected rather than interpreted ambiguously.
Opening the manager pauses gameplay and restores the previous pause state when
the window closes. Downloads show progress and can be cancelled by closing the
manager.

The dashboard lists up to twelve fingerprint-deduplicated recent ROMs. Windows
uses a native library window with game, platform, language, and last-played
columns plus a dedicated Settings page for palettes and control remapping.
Its Shortcuts page provides a complete in-app reference using the currently
configured bindings. Press `F1` from the dashboard or while playing to open the
same reference; Linux also exposes it as a dashboard entry.
The SDL desktop dashboard uses arrow keys and gamepad D-pad navigation that
wraps through its actions, while mouse-wheel and touch scrolling stop at the
ends. Enter, Space, or the primary gamepad button activates the highlighted
action. The native Windows library follows the same Enter-to-open and
Escape-to-close flow (and also supports double-click activation).
Entries can be removed without deleting ROM or save files. It resolves canonical
metadata and caches box artwork from
Libretro without uploading ROM contents. Press Ctrl+L or choose
**File > Game Library** to return to the Windows dashboard while playing.
You can also pass a ROM path, press Ctrl+O to choose another ROM, or drag a
`.gb`/`.gbc` file onto the emulator window. On Windows, build with Visual
Studio and an SDL3 installation visible to CMake. `gbb --version` prints the
version embedded in a desktop build:

```powershell
cmake -S . -B build-windows -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH=C:\path\to\SDL3
cmake --build build-windows --config Release
.\build-windows\Release\gbb.exe
```

When using SDL3's shared package, the build also copies its runtime DLL beside
the executable.
