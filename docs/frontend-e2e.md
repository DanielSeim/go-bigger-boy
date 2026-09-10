# Frontend end-to-end coverage

The end-to-end layer exercises user-visible flows in addition to the core and
frontend contract tests. Each platform owns the runner that can provide its
real input and lifecycle:

| Frontend | Runner | Covered flow |
| --- | --- | --- |
| Web | Headless Chromium + Playwright | WASM startup, settings persistence across reload, ROM picker, ROM boot, save controls |
| Android | Espresso on an API 35 emulator | Library launch, settings navigation, audio toggle persistence path, link-settings visibility, return navigation |
| Desktop SDL | Xvfb + XTest/xdotool smoke | Window creation, dashboard keyboard input, shortcuts modal, focus, clean shutdown |

The Web test creates a tiny looping ROM in memory. It does not contain a game
dump and is intentionally limited to startup and presentation flow; gameplay
coverage remains in the ROM/conformance suites. Android's instrumentation test
uses the real `LibraryActivity` and native library, rather than replacing the
screen with a test double.

Run the browser flow locally after building the Web bundle:

```sh
python3 -m http.server 8765 --directory build-web/web
npx --yes @playwright/test@1.52.0 install chromium
npx --yes @playwright/test@1.52.0 test tests/web/frontend.spec.mjs
```

Run Android instrumentation on a connected emulator or device:

```sh
(cd android && ./gradlew connectedDebugAndroidTest)
```

The Web and Android jobs are deliberately separate from link-cable E2E. Link
transport tests require two cores/peers and are maintained in
`tests/link_end_to_end_tests.cpp`; adding a browser transport will add a
browser link flow later.
