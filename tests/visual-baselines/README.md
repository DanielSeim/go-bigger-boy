# Visual regression baselines

Baselines are grouped by renderer backend. They are generated from the
redistribution-safe synthetic ROM fixtures used by the SDL and Web smoke tests;
they are not captures from a copyrighted game.

- `sdl-software/` is the headless SDL software-renderer baseline used by the
  native CTest gate.
- `webgl/` is the browser canvas baseline used by the Web workflow.

The WebGL images were recorded as originating from the successful CI browser
run that introduced this gate; this is historical provenance, not a fresh CI
result. Keep the browser version and capture dimensions stable when refreshing
them. `webgl` is the gate's backend label for the browser SDL canvas capture,
not a separate custom voxel shader implementation.

Current wiring (local source review, 2026-10-05): `CMakeLists.txt` registers
`gameboy_sdl_visual_regression` only with SDL tests and
`GAMEBOY_BUILD_PERFORMANCE_TESTS` enabled, on native non-Android,
non-Emscripten builds without either sanitizer option. It invokes
`scripts/visual_regression_gate.py` with the render-performance capture binary
for `nearest`, `voxel`, `voxel_shape`, and `voxel_popup`; defaults require exact
RGB matches. The Web workflow captures the three voxel modes through
`tests/web/frontend.spec.mjs` in headless Chromium, then uses channel tolerance
1 and maximum mismatch fraction 0.0005. The backend fixtures and output sizes
are separate contracts, not a cross-backend pixel-equality requirement.

These gates check synthetic rendering and viewport regressions. They do not
validate game-specific object extraction, device performance, or completion of
the experimental voxel pipeline.

Do not replace a baseline because of a one-off hosted-runner difference.
Review the uploaded actual image and comparison report first. If a visual
change is intentional, update only the affected backend baseline and explain
the change in the commit.
