# Release checklist

Use this short smoke test for releases that include local link-cable changes.

## Pokémon link session

- [ ] Start two Pokémon Red/Blue/Yellow instances from known-good saves and
      enter the Cable Club on both sides.
- [ ] Trade a different Pokémon in each direction and confirm the received
      parties are different on the two consoles.
- [ ] Start a link battle and confirm both players enter and complete a battle.
- [ ] Cancel or let a connection attempt time out, retry from both sides, and
      confirm a later attempt can connect.
- [ ] During a stalled connection, confirm the split-screen status changes to
      **TIMED OUT**, then use **Retry Link Handshake** (`Ctrl+Shift+R`) and
      confirm the games reconnect without losing either save.
- [ ] Stop the session, change player two's party or position, start another
      session, and confirm the change persists independently of player one.
- [ ] Confirm a normal session creates no link trace or diagnostic popup. Set
      `link.Diagnostics = true` only when collecting a troubleshooting trace.

## Build and publish

- [ ] Run `cmake --build build-sdl` and
      `ctest --test-dir build-sdl --output-on-failure`.
- [ ] Build the native Windows target and verify the packaged archive contains
      `gbb.exe` and the required SDL3 runtime files.
- [ ] Push the version tag and wait for Desktop, Android, and Web/Pages
      workflows to complete successfully.
- [ ] Verify the GitHub Release contains Windows, Linux, macOS, APK, and AAB
      artifacts before announcing the release.
