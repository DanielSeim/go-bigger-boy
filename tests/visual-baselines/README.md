# Visual regression baselines

Baselines are grouped by renderer backend. They are generated from the
redistribution-safe deterministic scene used by the SDL and Web smoke tests;
they are not captures from a copyrighted game.

- `sdl-software/` is the headless SDL software-renderer baseline used by the
  native CTest gate.
- `webgl/` is the browser canvas baseline used by the Web workflow.

The current WebGL images were captured by the successful CI browser run that
introduced this gate. Keep the browser version and capture dimensions stable
when refreshing them.

Do not replace a baseline because of a one-off hosted-runner difference.
Review the uploaded actual image and comparison report first. If a visual
change is intentional, update only the affected backend baseline and explain
the change in the commit.
