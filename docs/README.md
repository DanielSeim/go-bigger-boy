# Documentation index

These guides describe the current main branch, which may include changes after
the latest tagged release. Check [`VERSION`](../VERSION) and the
[Unreleased changelog](../CHANGELOG.md#unreleased) before comparing a guide with
an installed release.

## Start here

- [Project overview, supported features and limitations](../README.md)
- [Build and testing](build-and-testing.md)
- [Desktop settings, controls and tools](desktop.md)
- [Platform builds, packaging and storage](platforms.md)
- [Original replacement boot firmware and startup options](../firmware/README.md)
- [Release preparation, publishing and pipeline checks](release-checklist.md)

Instant startup remains the default. `replacement` and `animated` select
bundled, original firmware for the chosen hardware model; old DMG-specific
setting IDs are not migrated. AGB/AGB0 mean GB/GBC startup compatibility,
not native GBA emulation. SGB/SGB2 replacement boots cover only the GB-side
bootstrap. Ordinary HLE needs no proprietary firmware; experimental desktop
SNES-side playback still requires user-owned program and SPC IPL images.

## Compatibility and validation

| Guide | Scope |
| --- | --- |
| [Accuracy](accuracy.md) | Registered conformance gate, model/revision scope, reference methods and remaining limitations |
| [Frontend end-to-end checks](frontend-e2e.md) | Browser, Android and desktop runners and their coverage |
| [Link cable](link-cable.md) | Local/TCP/Bluetooth setup, protocol diagnostics and troubleshooting |
| [Pokémon link compatibility](link_compatibility.md) | Negotiated generation/region policy, separate from ROM-level protocol behavior |
| [DMG boot validation](dmg-boot-validation.md) | Cold handoff comparisons and retained refinement evidence |
| [DMG boot audio](dmg-boot-audio-validation.md) | Digital audio observations, reference alignment and limits |
| [DMG boot serial](dmg-boot-serial-validation.md) | Clock phase, transfers and state restoration |
| [SGB boot and LCD bridge](sgb-boot-validation.md) | Replacement bootstrap, cold reset, clocked ICD boundaries and current playback/headroom evidence |
| [SGB traces](sgb-traces.md) | Command capture/replay, privacy and inventory |
| [SGB audio engine](sgb-audio-engine.md) | Original SPC700/DSP scheduling, buffering and component states |
| [SGB firmware host](sgb-host.md) | Opt-in desktop playback, private image setup, whole-host states and qualification tools |
| [SGB validation record](sgb-validation.md) | Spec-derived contracts plus historical title/reference investigations |

Test totals depend on configuration and available inputs. A registered case,
an exact software baseline, a trusted-emulator comparison and a real-hardware
capture are different kinds of evidence. Refer to actual CI/local reports for
pass/fail status; neither a screenshot nor a historical benchmark guarantees
60 FPS on every device. The current clocked-ICD host headroom run failed its
unchanged gate; see [the measured results](sgb-boot-validation.md#playback-baseline-evidence).

## Architecture and extension contracts

- [Multi-core architecture and migration status](architecture.md)
- [Native plug-in ABI](plugin-abi.md), [freeze record](plugin-abi-freeze.md),
  [manifest](plugin-manifest.md), and [security policy](plugin-security.md)
- [Cloud-save manifest and conflict contract](cloud-save-sync.md): no provider
  or automatic cloud upload is implemented.

## Research and historical evidence

[Architecture audit](architecture-audit.md) retains a dated audit and its
progress log. [Voxel research](voxel-rendering-research.md) combines design
proposals, prototype tooling and implementation notes; proposals are not a
promise of completed rendering behavior. Historical SGB and boot captures keep
their original clocks, hashes, reference revisions and limitations. Later
summaries identify superseding behavior rather than rewriting earlier evidence.

Supporting fixture documentation lives in
[visual baselines](../tests/visual-baselines/README.md),
[external audio references](../tests/fixtures/audio-external/README.md),
[SameBoy revision review](../tests/fixtures/audio-external/sameboy-revision-matrix.md),
[fuzz corpus](../tests/fuzz/corpus/README.md) and
[local voxel review artifacts](../artifacts/voxel-review/README.md).
Private ROMs, firmware, saves, snapshots and captures are not release assets.
