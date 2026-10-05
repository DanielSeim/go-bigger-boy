# Frontend end-to-end coverage

The end-to-end layer exercises user-visible flows in addition to the core and
frontend contract tests. Each platform owns the runner that can provide its
real input and lifecycle:

| Frontend | Runner | Covered flow |
| --- | --- | --- |
| Web | Headless Chromium + Playwright 1.52.0 | WASM startup, settings persistence across reload, ROM upload/boot, save-control visibility, model-specific bundled boot handoffs, voxel captures |
| Android | Espresso on an API 29 x86_64 emulator in CI | Library launch, settings navigation, audio toggle handler, link-settings visibility, return navigation |
| Desktop SDL | ThreadSanitizer + Xvfb + XTest/xdotool smoke | Window creation, keyboard input, shortcuts, debugger interaction and capture checks, bounded shutdown |
| Experimental Windows SGB firmware | Local PowerShell runner + caller-owned images | Real SDL window/WASAPI playback, frame-time tails, native menu pause/resume/reset/save/load in isolated model slots |

The Windows firmware runner is an optional local qualification, not a CI job
requiring private dumps. See [native Windows device qualification](sgb-host.md#native-windows-device-qualification)
for the commands, limits, and separate physical listening check.
Use its `-BundledBootstrap` option to exclude optional private GB boot overrides
without modifying the original firmware directory. The separate local
`gameboy_sgb_production_local_titles` contract checks exact stereo 48 kHz
adapter/host output and state/reset behavior with caller-owned Donkey Kong
inputs; it does not replace native-window or physical listening checks.

The Web test creates a tiny looping ROM in memory. It does not contain a game
dump and is intentionally limited to startup and presentation flow; gameplay
coverage remains in the ROM/conformance suites. Browser save coverage checks
control visibility, not a save import/export round trip. Android's
instrumentation test uses the real `LibraryActivity` and native library. It
clicks the audio toggle twice to restore its initial value; it does not assert
persistence across process restart or boot a game.

The current browser suite expands to ten tests, including two parameterized
checks that retired `replacement-dmg`/`animated-dmg` preferences select instant
startup without migration. The generic stored IDs are `instant`,
`replacement`, and `animated`; selecting a new mode writes its current ID.
Historical eight-test results in validation reports describe the earlier suite.

Run the browser flow locally after building the Web bundle:

```sh
# Terminal 1:
python3 -m http.server 8765 --bind 127.0.0.1 --directory build-web/web
# Terminal 2, from the repository root:
npm install --no-save --no-package-lock @playwright/test@1.52.0
npx playwright install chromium
npx playwright test \
  --config=tests/web/playwright.config.mjs tests/web/frontend.spec.mjs
```

Package/browser setup can download dependencies. With an existing installation,
run only the server and test commands. `GBB_WEB_BASE_URL` overrides the server
URL; `GBB_WEB_CAPTURE_DIR` retains voxel PNGs for
`scripts/visual_regression_gate.py` against `tests/visual-baselines/webgl/`.

Run Android instrumentation on a connected emulator or device:

```sh
(cd android && ./gradlew connectedDebugAndroidTest)
```

For the complete local Android regression pass, including native contracts for
touch geometry, multi-button input, SGB borders, audio generation, and frame
pacing, use:

```sh
scripts/run_android_regression.sh
```

The command builds and runs focused native contracts, then JVM tests and the
debug APK build. If an authorized ADB
device is connected, it also runs instrumentation, installs the debug APK,
opens the library, and writes a bounded smoke screenshot to
`/tmp/gbb-android-regression/library.png`. Set `ADB_SERIAL` when more than one
device is connected. To explicitly exclude device work, use
`GBB_SKIP_DEVICE_TESTS=1 scripts/run_android_regression.sh`; this still builds
native targets and an APK and may fetch missing dependencies. Override the
native build and screenshot directories with `GBB_ANDROID_REGRESSION_BUILD_DIR`
and `GBB_ANDROID_REGRESSION_OUTPUT_DIR` respectively.

For the desktop smoke command and its synthetic-ROM debugger variant, see
[building and testing](build-and-testing.md#sanitizers-and-frontend-smoke-tests).

The Web and Android jobs are deliberately separate from link-cable E2E. Link
transport tests require two cores/peers and are maintained in
`tests/link_end_to_end_tests.cpp`; adding a browser transport will add a
browser link flow later.
