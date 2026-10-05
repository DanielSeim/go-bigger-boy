# Release checklist

Use this checklist for the whole release, from the candidate changes through
publication and update verification. Record the version, exact commit SHA,
workflow run URLs, artifacts reviewed, and any checks not performed. Commands
below are release actions to perform deliberately, not part of a docs audit.

## Prepare the candidate

- [ ] Finish the intended changes and review the diff, including platform and
      runtime limitations. Preserve historical measurements and distinguish
      them from validation of this candidate. Do not describe the new clocked
      SGB firmware bridge as performance-qualified: the latest Linux run
      fails all four headroom profiles despite exact audio/state output.
- [ ] Update `CHANGELOG.md` with user-visible changes, fixes, limitations, and
      the release date. Bump root `VERSION` to the intended `x.y.z` version
      and move the release entries out of `[Unreleased]`. CMake and Android
      read this file; Android derives `versionCode` as
      `major * 1000000 + minor * 1000 + patch`, which must increase for Play.
- [ ] Confirm the Android signing key/`gbb` alias is retained, required signing
      secrets are configured, and tagged Play publishing has
      `GCP_WORKLOAD_IDENTITY_PROVIDER` and `GCP_SERVICE_ACCOUNT` configured.
      Windows signing is conditional on its signing configuration; distinguish
      unsigned packages from signed ones in the release evidence.
- [ ] Review and stage only the intended release files, commit the changes
      including changelog and `VERSION`, and push the release commit to
      `main`. Record `git rev-parse HEAD` for the exact candidate SHA.

## Validate before tagging

- [ ] Run the appropriate local native checks, for example
      `cmake -S . -B build-sdl -DCMAKE_BUILD_TYPE=Release`,
      `cmake --build build-sdl --parallel`, and
      `ctest --test-dir build-sdl --output-on-failure`.
      For multi-configuration generators use `--config Release` when building
      and `-C Release` with CTest. Confirm SDL3 was found when validating SDL.
- [ ] Watch all workflows for this exact pushed SHA in GitHub Actions,
      including Desktop builds, Android build, Web build and Pages, and any
      triggered hardware-model/fuzz workflows. Review failed jobs and their
      logs/artifacts, take corrective action, and watch the new runs through
      completion. Re-run transient failures; code fixes require a new commit,
      push, and candidate SHA. A green run for another commit is insufficient.
- [ ] Review native contracts, conformance/audio reports, sanitizers, plug-in
      ABI checks, render performance and visual gates, the ten browser tests,
      and Android JVM/instrumentation results. A compiled Android link binary
      is not an on-device execution result. Record skipped cases and keep
      exploratory suite failures distinct from required regressions.
- [ ] Run **Release preflight** (`release-preflight.yml`) manually with `sha`
      set to the candidate SHA after successful Desktop, Android, and Web
      runs exist for that SHA. It downloads their artifacts and invokes
      `bash scripts/verify-release-assets.sh release-assets`. PR-only Android
      builds supply a debug APK, not the signed APK/AAB needed by this gate.
- [ ] Review the actual packages: Windows ZIP must contain `gbb.exe`,
      `gbb-updater.exe`, `SDL3.dll`, and `settings.ini`; Linux AppImage must
      return `Go Bigger Boy x.y.z` with `--version`; both macOS arm64/x64
      archives must contain the app executable. Check signed APK/AAB and
      Web `index.html`, `index.js`, `index.wasm`, and logo. The preflight
      checks presence/archive structure and runs the Linux version command;
      it does not verify all platform signatures or interactive behavior.
- [ ] Perform the applicable frontend/save/update smoke checks on available
      platforms and record their scope. Back up saves first. Include settings
      persistence, ROM start, input/audio, save/load, clean exit and relaunch.
      Private firmware and physical-device qualification are separate checks;
      do not infer them from host CI or historical passing measurements.

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
      session, and confirm player two's in-game battery-saved party persists
      independently. Its running CPU/WRAM state is cloned from player one on
      each attach, so independent transient position is not guaranteed.
- [ ] Confirm a normal session creates no link trace or diagnostic popup. Set
      `link.Diagnostics = true` only when collecting a troubleshooting trace.

For link changes, also exercise the applicable TCP host/join, timeout/retry,
and LAN-discovery paths. Bluetooth needs native Windows/physical Android
qualification. Web has no link transport. Record manual results rather than
assuming a transport handshake proves a trade or battle completed.

## Tag, publish, and monitor

- [ ] Confirm root `VERSION`, the changelog release heading, and the proposed
      `vX.Y.Z` tag agree. Create the version tag on the validated SHA and push
      that specific tag, for example `git tag vX.Y.Z <validated-sha>` followed
      by `git push origin vX.Y.Z`. Do not move a published tag to conceal a
      fix; use a new version for changed release content.
- [ ] Watch every workflow triggered by the tag, including **Publish release**.
      Investigate failures, act on the logs, and monitor recovery to completion.
      The publisher is triggered when the tagged Desktop workflow completes;
      it waits for matching tag/SHA Desktop, Android, and Web runs, requires
      success, downloads their artifacts, and runs asset preflight before
      creating/uploading the GitHub Release. Tagging also triggers Android's
      Play upload, so Play configuration must be ready before pushing the tag.
- [ ] If publication fails or times out, fix/re-run the affected workflows
      and use **Publish release** manual dispatch with the same validated
      `tag` and `sha` when necessary. Its upload can replace existing assets
      (`--clobber`); review the target and packages before recovery.
- [ ] Verify the published GitHub Release contains the Windows x64 ZIP,
      Linux x64 AppImage, macOS arm64 and x64 archives, signed Android APK/AAB,
      and Web files. Confirm versions and updater-visible asset names/digests
      agree with the release. Review generated release notes against the
      changelog; publication uses `--generate-notes`, not automatic changelog
      copying. Announce only after publication and validation finish.
- [ ] Verify the `main` Web run deployed GitHub Pages and the page loads the
      matching cache-busted JS/WASM. A tag Web run supplies release files but
      does not deploy Pages.
- [ ] After the initial manual Play Console upload, confirm tagged Android
      builds publish the signed AAB to closed-testing track `GBB Beta` through
      Workload Identity Federation. Verify Play-installed copies update through
      Play's flexible update flow; direct-download copies use the signed GitHub
      APK/system installer. Preserve signing identity and test save/settings
      retention for applicable update paths before announcing availability.
- [ ] Record final workflow URLs and manual results, including unperformed
      device/private checks and unresolved qualification limits, with the release.
