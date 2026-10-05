# Contributing to Go Bigger Boy

Bug reports, focused fixes, tests, and documentation improvements are welcome.
Check the [current status and limitations](README.md#current-status) and
[documentation index](docs/README.md) before starting. For larger changes,
open an issue first to discuss the scope and proposed approach.

## Reporting bugs

Include the version or commit, platform/device, relevant settings, reproduction
steps, and expected versus actual behavior. Logs and screenshots are helpful;
remove personal information, credentials, and private file paths before sharing.
For performance problems, include the hardware, build configuration, rendering
mode, and measured FPS or timing trace rather than an FPS estimate alone.
Identify a cartridge by title, revision, and hash where useful; do not upload ROMs
or proprietary firmware.

## Making changes

- Keep pull requests focused and explain the problem, approach, and limitations.
- Add regression tests for behavior changes and update affected documentation.
- Preserve emulation correctness, audio, and visuals when optimizing performance.
  Do not weaken gates or replace reference baselines just to make a test pass.
- Include test commands and results in the pull request, along with anything
  untested or requiring private inputs or physical hardware.

A basic native build and test run is:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

For multi-configuration generators, add `--config Release` to the build command
and `-C Release` to CTest. See [build and testing](docs/build-and-testing.md)
for dependencies, conformance suites, sanitizers, and performance checks, and
[frontend coverage](docs/frontend-e2e.md) for platform-specific validation.
Some reference checks require locally supplied images and are not available in
ordinary CI. Report those gaps explicitly: passing a software regression does
not establish hardware-perfect behavior or universal compatibility.

## AI-assisted development

Go Bigger Boy is developed with extensive use of AI-assisted coding tools.
Generated changes are reviewed, tested, and validated against the project's
relevant conformance and reference suites before integration. AI assistance is
not a substitute for verification: contributors remain responsible for the
correctness, provenance, and licensing of submitted changes. Mention material
AI assistance in your pull request and describe how the result was verified.

## Source and asset provenance

Contributions must respect the project's independent clean-room implementation
and [licensing and provenance notices](README.md#license). Do not submit
Nintendo source code, proprietary SDK code, dumped boot/program ROMs, game ROMs,
or other proprietary assets. Use independently written fixtures where possible;
keep legally obtained private reference inputs and captures out of commits and
public artifacts. Identify third-party code or assets and retain their required
license and attribution notices.
