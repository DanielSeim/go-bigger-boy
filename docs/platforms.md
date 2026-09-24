# Platform guides

This guide covers the Linux, Android, and Web frontends, including packaging,
local development builds, persistence, and platform-specific limitations.

## Linux desktop

Install a C++ compiler, CMake, SDL3 development files, curl, and an XDG desktop
portal. On Debian/Ubuntu distributions where SDL3 packages are available:

```sh
sudo apt install build-essential cmake curl libsdl3-dev xdg-desktop-portal
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux --parallel
./build-linux/gbb
```

For a per-user installation with an application-menu entry and ROM file
association:

```sh
cmake --install build-linux --prefix "$HOME/.local"
```

The installed desktop entry accepts `.gb` and `.gbc` files from Linux file
managers. The native ROM picker uses SDL's portal-backed file dialog on
Wayland and supported X11 desktops.

Release builds for Linux are distributed as an x86_64 AppImage. Download it,
make it executable, and launch it directly:

```sh
chmod +x go-bigger-boy-*.AppImage
./go-bigger-boy-*.AppImage
```

Keyboard controls are arrows for the D-pad, X for A, Z for B, Enter for
Start, Backspace for Select, and Escape to quit. Standard gamepads are also
supported. Desktop shortcuts are Space to pause, Ctrl+R to reset, F11 for
fullscreen, Ctrl+L for the recent-ROM list, Ctrl+1 through Ctrl+9 for quick
recent selection, Ctrl+K to configure and persist controls, Ctrl+P to choose
between Grayscale, Classic green, Game Boy Pocket, Amber, and the automatic
Game Boy Color compatibility palette,
the configurable SaveState, LoadState, FastForward, and Rewind shortcuts, and
F1 for help. Recent ROMs,
window positions, and per-ROM quick saves are stored with the desktop data. On
Windows, all desktop data (including saves, recent-ROM metadata, quick states,
printer output, updater files, and settings) is kept beside `gbb.exe`, making
the extracted folder portable. Linux and macOS continue to use SDL's per-user
preferences directory. Desktop keyboard/gamepad bindings and the display palette
are stored in the human-readable `settings.ini` at the data root. Each
Game Boy button accepts up to two space-separated keyboard keys (for example,
`keyboard.B = Z Y`). Copy the file to another GBB installation to share the
same setup. On Windows, the native Settings page is organized into General,
Controls, Link cable, and Advanced sections. The Controls section presents a
clear primary/secondary binding table: click a slot and press its new key, or
press Delete to clear it. Duplicate keys move from their previous action, and
the complete change set is staged until Apply. The same page also exposes Fast
Forward, Rewind, Save State, and Load State shortcut buttons. The Ctrl+K dialog
can also rebind keyboard or gamepad input; Space skips an optional secondary
key and Escape cancels an in-progress setup. GBB
generates a complete default
`settings.ini` at startup whenever it is missing and appends defaults for any
recognized entries omitted from an existing file. Older `controls.txt` and
`palette.txt` preferences migrate automatically when `settings.ini` is first
created, and automatic updates preserve an existing portable file. On Android,
tap the in-game menu button in its configured corner and choose
`Display palette` to select and persist the same five palette options. The
Android Settings page also provides touch-control size and opacity sliders;
these values are stored in `settings.ini` as `touch.Size` and `touch.Opacity`.
It also provides a **Generate audio** toggle. Disabling it silences the
emulator and skips APU mixer/resampler work to reduce CPU use, while channel
timing and CGB PCM register behavior continue normally. Desktop Settings
provides the same toggle, persisted as `audio.Enabled`.
The desktop, Android, and browser frontends also provide an opt-in **Show FPS
counter** setting. It displays the measured presentation rate over the running
game and is disabled by default; the desktop and Android values are persisted
as `video.ShowFps`, while the browser remembers the choice in browser storage.
Voxel modes also support touch-drag orbiting when `touch.VoxelOrbit` is enabled;
the Android Settings page provides a toggle for this gesture. The in-game menu
button can be placed at the top left or top right with `touch.MenuPosition`.
While a ROM is running, the adjacent link button opens TCP Host, Join, LAN
discovery, Retry, Stop, and Link settings. Link settings open as an Android
dialog with text input and apply to the next connection without leaving the
game.
Its layout editor has separate portrait and landscape layouts. The D-pad is
always moved as one control, while A, B, Select, and Start can be positioned
individually beside or below the emulation screen. Positions are stored as
normalized `touch.Portrait.*` and `touch.Landscape.*` coordinates.

The desktop and Android settings pages also expose a **Hardware model**
selector. `Automatic (cartridge)` preserves normal header-based selection;
the explicit profiles are DMG-0, DMG-B/DMG, MGB, SGB, SGB2, CGB-0, CGB-C, and
CGB-E. The choice is stored as `hardware.Model` in `settings.ini` and applies
when the ROM is started again. The test runner accepts the same IDs through
`--model`, so a result can be reproduced against a named hardware revision.

Native desktop plug-ins are opt-in. Set `plugin.Discovery = true` in
`settings.ini`, then add one or more repeated `plugin.Path = ...` entries for
shared libraries or directories containing them. Relative paths are resolved
against the settings file; only platform-native library extensions are
considered, symbolic links are rejected, and at most 32 candidates are
loaded. Rejected or duplicate plug-ins are reported through the core logger.
For a stricter trust policy, set `plugin.RequireAllowlist = true` and add
repeated `plugin.AllowCore = <core-id>` entries; descriptor IDs not on that
allowlist are rejected before registration. This is an identity policy, not a
cryptographic signature system.
Capability permissions can be restricted independently with
`plugin.RequireCapabilityAllowlist = true` and repeated
`plugin.AllowCapability = <name>` entries. Stable names are
`persistent_memory`, `rtc`, `rumble`, `camera`, `printer`,
`compatibility_palette`, `cheats`, `debugger`, `sprite_editor`, `scene_layers`,
and `link_cable`. Unknown names never grant access, and a plug-in must still
pass the host adapter's capability validation.
Host integrations can use the pre-registration trust callback together with
`plugin_sha256_file` to pin approved binary digests; this is tamper detection,
not a replacement for signed manifests.
Android never loads native plug-ins.
On Windows, the Settings page exposes the discovery and allowlist switches and
shows the current loaded/rejected status; changing any switch takes effect
after restarting the emulator.
The same page also exposes the remote link transport and endpoint settings:
TCP host/bind/port and LAN advertisement, or Bluetooth Classic address and
service UUID. The link diagnostics trace can be enabled there as well; link
settings are applied to the next session and persisted in `settings.ini`.

The video pipeline is configurable across desktop, Android, and web builds:
`nearest` keeps crisp pixel edges, `bilinear` smooths the presentation, `sharp`
adds edge-aware smoothing without blanket blur, `integer` uses only whole-number
scale factors, and `lcd` applies a lightweight LCD mask with scanlines. The
setting is stored as `video.Mode` in
`settings.ini`; the web selector remembers its choice in browser storage.
For performance diagnosis, set `GBB_LOG_LEVEL=debug` to receive a rate-limited
`fps_sample` record while the FPS counter is enabled. Desktop builds also
support `GBB_FRAME_TIMING=1` (and optional `GBB_FRAME_TIMING_FILE`) for a
periodic trace containing measured frontend FPS, emulation, presentation,
audio, pacing, and link-polling timings.
Desktop, Android, and web builds also expose three experimental voxel modes. `voxel` is the
native source-pixel relief renderer with a batched textured background. `voxel_shape` (shown as “Voxel
diorama (shape-aware)”) keeps the source-pixel silhouette, then applies edge-aware
depth and stronger per-layer volume so sprites read as compact 3D forms. Its flat
framebuffer is batched as a textured plane to keep native detail responsive. Desktop and web
provide camera controls; all voxel modes share profiles, layer ordering, and the
optional framebuffer facade, and can be switched while a ROM is running.
Android currently uses the configured profile defaults. `voxel_popup` (shown as “Voxel
pop-up book”) lays the framebuffer out as a horizontal page, raises the window
layer above it, and renders OAM sprites plus substantial connected tile-layer
shapes as upright, page-anchored cuboids. Small isolated texture/dither pixels
remain on the page. This is useful for overhead games such as Pokémon, where
buildings and terrain are normally drawn by the background tile layer.
Unsupported frontends fall back to the regular 2D presentation.
In the web build, drag the voxel canvas horizontally to orbit around the
center axis and vertically to adjust the pitch. Angles are clamped to keep the
scene readable; double-click (or double-tap where supported) resets the camera.
Per-ROM depth and camera tuning can be supplied in `voxel-profiles.ini` beside
`settings.ini`; use `[default]` and a hexadecimal ROM fingerprint section with
`depth_scale`, `camera_pitch`, `camera_yaw`, `zoom`, `perspective`,
`sprite_depth`, `lighting`, `background_depth_far`,
`background_depth_near`, `background_transparent_depth`, `window_depth_far`,
`window_depth_near`, `sprite_depth_far`, `sprite_depth_near`, and
`popup_parallax`, `popup_object_height`, `popup_sprite_height`,
`popup_card_thickness`, `popup_sprite_thickness`, `popup_hud_top_rows`,
`popup_hud_bottom_rows`, and `framebuffer_facade` keys.
The popup values control page parallax, cut-out height and extrusion without
requiring a renderer rebuild. The default layer ranges are background `100` → `20`
with transparent pixels at `95`, window `90` → `50`, and sprites/objects
`45` → `25`; each range is normalized into a guaranteed background → window →
sprite ordering (larger depth values are farther from the viewer). Set
`framebuffer_facade=0` to inspect the fully voxelized mesh (the default); set it to `1` to draw the
normal framebuffer as a front-facing reference facade. The default mesh camera is centered and
slightly zoomed out so the scene remains inside the viewport. GBB creates this file with documented
defaults on first startup, so it can be copied alongside a portable install.
Set `background_debug_overlay=1` while tuning to draw candidate bounds and
write accepted-object, candidate, vertex and index counts to the frontend
diagnostic log.
The supplied Super Mario Land dump (`0x7eafc0023b31d850`) receives a built-in
profile tuned for its flat sky, layered platforms, and sparse foreground
sprites; the profile is added to existing installations without overwriting
user settings.
On Windows, the Settings page exposes these fields for the currently running
ROM and shows its fingerprint. A live preview updates as valid values change;
the framebuffer-facade option is a clear toggle for comparing the 2D front
panel with the voxel mesh. The values are staged with the other settings and
saved when you Apply and return; Reset to defaults restores the built-in
profile before applying.

The default action bindings are `keyboard.FastForward = Tab` (hold for up to
4× speed), `keyboard.Rewind = Left Shift` (hold to step backward through the last three
seconds), `keyboard.SaveState = F5`, and `keyboard.LoadState = F8`. Fast-forward
adapts its batch size to the core's measured frame cost so slower systems remain
responsive while faster systems can reach the full 4× rate. Set any of these to
a different key, or to `None` to disable it. Rewind uses in-memory snapshots
and is cleared when changing ROMs or loading a saved state.

At startup, the desktop and direct-download Android builds check GitHub's latest
stable release in a background thread. If its semantic version is newer than
the running build,
GBB can download the matching release asset and verify GitHub's published
SHA-256 digest. Nothing is downloaded without confirmation. Network failures
remain non-blocking and do not display a dialog. On desktop, a user-writable
installation is replaced after the emulator exits and the updated executable
restarts. Windows uses the system HTTP service and a native update helper
(signed when release signing is configured);
Linux and macOS use the system `curl` and archive tools. Direct-download
Android builds download the signed APK, then open the system package installer;
Android may require enabling “Allow from this source” and always controls the
final confirmation. Android installations made by Google Play use Play's
flexible in-app update flow, so they can show an in-app update prompt and
download the update through Google Play without downloading or installing
GitHub APKs. After a successful direct APK installation, Android
relaunches the updated emulator. System-wide read-only desktop installations
must still be updated through their package manager or replaced manually.

Desktop update downloads run asynchronously and show progress in the window
title; press Escape to cancel. Library artwork and metadata are resolved in
the background, with the dashboard showing item progress and cancelling
outstanding requests when it closes. The GameShark archive fetch uses the same
background behavior and can be cancelled by closing its window.

Battery-backed MBC1, MBC2, MBC3, MBC5, and Game Boy Camera games use a sibling
file with the ROM's base name and a `.sav` extension. MBC3 real-time clocks use
an additional `.rtc` file. MBC5 rumble cartridges drive the currently connected
SDL gamepad on desktop when it supports vibration. Rumble stops while paused,
unfocused, or inside a modal dialog. Unsupported cartridge controllers are
rejected explicitly instead of running with incorrect banking.

Game Boy Printer-compatible games can print normally from their in-game menus.
The desktop frontend emulates the printer protocol and saves each completed
page as a lossless, nearest-neighbor 4× BMP image. Files are placed in the
`prints` folder below the desktop data root; the emulator displays that folder
after a print completes. No physical printer is required. The web frontend
emulates the same protocol and automatically downloads completed pages as BMP
images.

Game Boy Camera ROMs use the first webcam reported by SDL on Windows and Linux.
The operating system may request camera permission when the cartridge opens.
Frames are center-cropped, reduced to the original 128×112 four-shade sensor
image, and front-facing cameras are mirrored. If no webcam is available or
permission is denied, the cartridge remains playable with a fallback image.

ROM files are not included. Only use cartridge dumps you are legally entitled
to use.

## Android build

The Android project in `android/` uses SDL3's official Android AAR and the same
C++ core/frontend as desktop. It currently supports the Android document
picker, portrait and landscape multitouch controls, external gamepads, audio, rumble,
battery saves, and optional camera permission. ROMs selected through Android's
`content://` document interface are imported into private app storage so recent
games remain launchable after a restart; they are never uploaded. Save data is
stored privately by ROM fingerprint.

The Settings screen includes **Export full backup (ZIP)** and **Import full
backup (ZIP)** actions. A full backup contains the private ROM copies, battery
saves, link saves, quick states, settings, and library metadata, so the ZIP can
be copied to a Windows or other computer through shared storage. Dedicated
save-file actions export all saves as a ZIP or import an individual `.sav`
file; imported save filenames are preserved because they include the ROM
fingerprint.

Install JDK 17 and the Android SDK command-line tools, then let the repository
install the pinned SDK 36, NDK r28c, CMake 3.31.6, and SDL3 dependencies. The
checked-in Gradle wrapper downloads and verifies Gradle 8.13 automatically:

```sh
scripts/bootstrap-android.sh
scripts/build-android.sh debug
```

Do not mix Windows Java or Gradle with a WSL build. The scripts detect that
configuration and stop with an actionable error. A release build can be made
with `scripts/build-android.sh release` after setting
`GBB_ANDROID_KEYSTORE_FILE`, `GBB_ANDROID_KEYSTORE_PASSWORD`, and
`GBB_ANDROID_KEY_PASSWORD`; the PKCS12 keystore must contain the `gbb` alias.
The wrapper can also be invoked directly from the Android directory with
`./gradlew assembleDebug` (`gradlew.bat assembleDebug` from a native Windows
command prompt).

The debug APK is written to
`android/app/build/outputs/apk/debug/app-debug.apk`.
The app opens on a native Android game library. Its recent cards are deduplicated
by ROM fingerprint and show the cartridge title, Game Boy platform, inferred
language, last-played time, and cached cover artwork. Entries can be removed
without deleting imported ROM or save files. Game identity is matched locally by CRC
against a cached copy of Libretro's No-Intro metadata, which also provides the
exact canonical artwork name. The separate Settings screen controls the
display palette and whether artwork may be downloaded from Libretro's public
thumbnail service; ROM contents are never sent to that service. Tapping the menu
button in the upper-left returns to the library while preserving the running
game. The library and settings screens use a native toolbar with explicit Back
navigation; Back from Settings returns to the library, Back from a library
opened over a running game resumes that game, and only the root library asks
for exit confirmation. The translucent controls
provide a D-pad, A, B, Select, and Start; Bluetooth and USB gamepads continue
to work through SDL. Android's Back button asks for confirmation before closing
the emulator; Back from the library resumes a game underneath it.
Game Boy Camera input follows the phone's physical
orientation even though the emulator interface remains in landscape. The
`Android build` GitHub Actions workflow runs
pull requests as an automatically debug-signed APK. Pushes to the repository
use encrypted GitHub secrets to produce a consistently signed release APK and
Play-ready Android App Bundle. The signing key must be kept permanently:
Android will not accept future updates signed with a different key.
Tagged builds attach both signed packages to the matching GitHub release so
direct-download installations can discover and verify the APK through the
in-app updater. After the initial manual Play Console upload, tagged builds
also publish the signed App Bundle to the closed-testing track **GBB Beta**
through GitHub Actions Workload Identity Federation; Play-installed copies
update through Google Play.

The native link end-to-end test also cross-compiles for Android. It exercises
two real emulator cores over the deterministic local cable (the TCP leg remains
desktop-only because it requires a socket-capable runner):

```sh
cmake -S . -B build-android-link-e2e \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_HOME/ndk/28.2.13676358/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 \
  -DGAMEBOY_BUILD_TESTS=ON -DGAMEBOY_BUILD_SDL=OFF
cmake --build build-android-link-e2e --target gameboy_link_end_to_end_tests
adb push build-android-link-e2e/gameboy_link_end_to_end_tests /data/local/tmp/gbb-link-e2e
adb shell chmod 755 /data/local/tmp/gbb-link-e2e
adb shell /data/local/tmp/gbb-link-e2e
```

The Android workflow performs this cross-compilation gate on every change. The
test binary is intentionally not packaged into the user-facing APK.

## Web build

The browser frontend uses SDL3 and Emscripten. It accepts local `.gb` and
`.gbc` files from the picker or by drag and drop; ROM data stays in the browser
and is never uploaded. Keyboard and standard gamepad controls match the desktop
frontend. The hardware-model, display-palette, and video-pipeline selectors
offer the same options as the desktop frontend and remember their selections
in browser storage. The selected hardware model is applied the next time a ROM
is loaded, allowing revision-specific DMG, MGB, SGB, and CGB behavior to be
tested directly in the browser. Battery-backed RAM, Game Boy Camera captures,
and MBC3 clock state are saved automatically in IndexedDB for each ROM. A
synchronous local-storage
fallback also protects the latest save when a tab is closed before IndexedDB
finishes.
Browser saves can also be imported from or exported to desktop-compatible
`.sav` and `.rtc` files. Game Boy Camera cartridges request webcam permission
and use a center-cropped 128×112 live image; when access is unavailable, they
remain playable with the built-in fallback image.

Web link-cable sessions are intentionally not enabled yet. The browser build
therefore does not register the native link E2E binary; a browser-compatible
transport (for example WebRTC or a relay-backed WebSocket) and a browser test
runner are required before cross-device Web link testing can be meaningful.

The repository bootstrap installs the pinned Emscripten 4.0.15 and SDL 3.4.2
toolchains below the ignored `.cache/toolchains/` directory. Build and optionally
serve the site with:

```sh
scripts/bootstrap-web.sh
scripts/build-web.sh
scripts/build-web.sh --serve
```

If Emscripten and SDL3 are already installed, activate Emscripten, set
`SDL3_DIR` to the Emscripten SDL3 CMake package, and run
`scripts/build-web.sh`; the bootstrap step may be skipped.

The `Web build and Pages` workflow repeats this build on every push to `main`
and deploys the result to GitHub Pages. Pull requests build the WebAssembly site
without deploying it. The repository's Pages source must be set to **GitHub
Actions** before the first deployment.

