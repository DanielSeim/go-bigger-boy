# Voxel rendering research and design proposal

Status: research/design note. This document does not change the emulator or renderer by itself.

## Executive summary

The current voxel mode is too close to a *pixel relief*: it starts with the complete 160×144 framebuffer and turns many individual pixels into independent extrusions. That makes the output expensive, noisy, and difficult to compose. It also gives every pixel the same geometric meaning even though a Game Boy pixel may belong to a HUD, a background tile, a platform, or a moving character.

A better direction is a **shape-aware, profile-driven voxel pipeline**:

1. Keep the emulated framebuffer and PPU/OAM data unchanged.
2. Convert the presentation data into a semantic `SceneSnapshot` containing background shapes, windows/HUD, and sprite/metasprite objects.
3. Voxelize each object as a small 2D occupancy mask. A voxel represents a *block of source pixels* (start with 2×2, not one voxel per source pixel), and each occupied cell becomes a cube or short rectangular prism with a configurable number of depth slices.
4. Give each scene layer its own depth range, material and lighting rules. Backgrounds are broad, quiet surfaces; windows are shallow floating panels; sprites are separate, grounded volumes.
5. Render the resulting world into an expanded, camera-fitted viewport. The logical Game Boy resolution remains 160×144, but it is no longer the size limit of the 3D scene.

This keeps a faithful 2D fallback while making the voxel mode closer to a 3D diorama than a stack of pixel columns.

## What existing work teaches us

### 3dSen: semantics and authoring matter

The official 3dSen material describes a pipeline that samples the PPU output into 8×8 tiles, clusters adjacent tiles into shapes, maps those shapes to 3D shapes and positions, and then applies profile scripts for depth, animation, rotation and hidden/added geometry. It explicitly treats the same tile as ambiguous: a tile may be a brick, cloud, character detail, or UI depending on context. That meaning cannot reliably be recovered from the ROM graphics alone, so hand-tuned game profiles are part of the design rather than an afterthought.

- [3dSen / Geod Studio](https://geodstudio.net/)
- [The 3dSen ten-year story (Geod Studio)](https://geodstudio.net/blog/3dsen-10-year-story.html)

The important lesson for GBB is to stop treating the framebuffer as a uniform height map. We should identify reusable shapes and let a per-ROM profile refine the interpretation when automatic detection is insufficient.

### NESRecomp: presentation should be a separate compositor

The NESRecomp voxel renderer documents a presentation-only compositor that reads the framebuffer, tile IDs, CHR, palette and OAM, then produces a separate 3D scene. Its renderer supports tile groups/metatiles, a depth buffer, textured prisms, camera-facing sprite cards, per-game callbacks and configurable semantic heights. It also distinguishes engine responsibilities (mesh assembly, occlusion, alpha and shadows) from game-profile responsibilities (recognizing objects and assigning meaning).

- [NESRecomp voxel renderer design](https://github.com/mstan/nesrecomp/blob/master/docs/VOXEL_RENDERER.md)

This maps well to GBB's existing `SceneSnapshot` direction: emulation remains authoritative, while voxel presentation consumes a snapshot and may be disabled without affecting emulation.

### Height-map approaches are useful fallbacks, not the target

InRelief demonstrates a simpler technique: assign a depth to each pixel (often from luminance) and project the result. This is mathematically attractive and useful for sparse, camera-facing artwork, but sorting and overlap become fragile when many independent layers intersect.

- [InRelief graphics engine](https://notimetoplay.org/engines/inrelief/index.html)

For GBB, a height-map/pixel-relief mode should remain available as a compatibility fallback. It should not be the main path for characters and level geometry.

## Proposed representation

### 1. Presentation scene

Introduce a renderer-facing scene model (the names are provisional):

```text
VoxelScene
  coordinate_system       source pixels, world units, camera convention
  camera                  projection, position, pitch, zoom and fit margin
  layers[]                background, window, sprites, effects
  objects[]               shape, anchor, material, depth range, flags

VoxelObject
  source_rect / mask      source pixels represented by this object
  anchor                  feet, center, tile origin or profile-defined point
  footprint               source-pixel-to-voxel grouping
  depth_profile[]         per-cell or per-material front/back depth
  material                palette colors, outline policy and side shading
  render_mode              volume, billboard, flat or fallback relief
```

The scene is a presentation artifact. It must not write back to CPU, PPU, memory, timing or input state.

### 2. A voxel is a block of source pixels

The key change is the unit of geometry:

- `pixels_per_voxel = 2` means a voxel cell covers a 2×2 source-pixel footprint.
- `pixels_per_voxel = 4` is a coarser option for distant/background art.
- `pixels_per_voxel = 1` remains available for small details and the legacy mode.

Each occupied cell becomes a cube-like block, not a paper-thin column. For example, a 16×16 sprite with a 2×2 footprint becomes an 8×8 grid of cells. A depth profile can give those cells 2–6 slices, producing a compact volume while preserving a recognizable silhouette.

The logical source pixel grid is therefore an input sampling grid, not the final world resolution. The output viewport must have enough room for the block footprint and the camera’s depth perspective.

### 3. Shape masks and volumes

For each object:

1. Build a color/occupancy mask from the source tiles or OAM sprite.
2. Downsample into `pixels_per_voxel × pixels_per_voxel` cells. A cell is occupied if its coverage exceeds a configurable threshold; preserve the dominant palette index for its material.
3. Generate a filled volume from the mask. A first implementation can use a stepped prism: front cells at the object’s front depth and a small number of interior slices toward its back depth.
4. Cull internal faces between neighboring occupied cells.
5. Greedy-merge adjacent coplanar faces where possible. This preserves the block look while avoiding one mesh face per source pixel.
6. Keep a silhouette shell for outlines and transparent regions so outlines do not become giant black walls.

For characters and items, anchor the volume at the feet/baseline. Their depth should be independent of the room-sized background plane, so Mario can be near the foreground without extruding all the way through the scene.

### 4. Materials and extrusion color

The front face should use the source palette color. Side faces should be derived from the same material (a controlled darkening or hue shift), not from the background color. Outline pixels remain black/dark on the front, while their side faces use a dark material derived from that outline. This avoids the current grey/black slab effect and keeps the object visually coherent.

Lighting should be soft and directional. A small ambient term and one key light are preferable to strong black shadows; contact shadows should be short and object-local.

## Layered scene composition

The default composition should be explicit rather than inferred solely from depth sorting:

| Layer | Role | Suggested depth interval (far → near) | Geometry |
| --- | --- | --- | --- |
| Background | sky, distant scenery, broad terrain backdrop | 100 → 80 | one continuous surface or grouped tile volumes; minimal/no extrusion |
| Window/HUD | score, time, status bars and other screen-space panels | 78 → 55 | shallow floating panels; front-facing text/material |
| Sprites/objects | Mario, enemies, items and interactive scenery | 52 → 25 | separate compact volumes; grounded anchors |
| Foreground effects | optional particles/overlays | 24 → 10 | profile-defined; usually billboard or short volume |

These are world units, not hard-coded renderer assumptions. A profile may override them. The important invariant is ordering: background is farthest, windows are in front of it, and sprites are nearest. A layer priority is only a tie-breaker; the GPU depth buffer (or a deterministic software depth pass) remains authoritative for intersections.

For a background/window overlap, the background can be filled behind the window with the surrounding background material. The fill belongs to the background layer; it must not turn the window into a deep hole.

## Object discovery and profiles

Automatic extraction should be conservative:

- Group adjacent background tiles into metatiles or connected shapes.
- Assemble OAM entries into metasprites using overlap, proximity and shared animation timing.
- Track stable object identities across frames to avoid geometry popping when a sprite moves.
- Classify HUD/window regions from stable screen-space bounds and profile hints.
- Allow explicit profile rules by tile ID, OAM slot, palette, screen rectangle, shape mask, animation frame or game state.

Profiles are necessary because the ROM does not declare that a particular tile is “background” or “Mario.” A declarative profile should be enough for most games, with optional runtime callbacks/scripts for games that change graphics mid-frame or use raster effects.

Suggested profile settings:

```ini
[voxel]
mode = shape
pixels_per_voxel = 2
background_depth = 100,80
window_depth = 78,55
sprite_depth = 52,25
background_extrusion = 0
window_extrusion = 3
sprite_depth_slices = 4
sprite_depth_gap = 2
greedy_meshing = true
camera_projection = orthographic
camera_fit_margin = 24
```

Per-object rules should be able to override `pixels_per_voxel`, depth, anchor, material and render mode. The existing pixel-relief renderer can be selected with `mode = pixel_relief` when no profile is available.

## Camera and viewport budget

Because one voxel covers multiple source pixels and has physical depth, a 160×144 canvas is not enough as a world-space budget. The renderer should:

- render to an expanded internal target (for example 320×288 or a dynamic target based on camera fit),
- fit the complete scene bounds plus a configurable margin,
- use orthographic projection or very weak perspective by default,
- keep pitch constrained to the diorama-friendly range and avoid accidental roll/yaw,
- update near/far clipping planes from scene bounds,
- letterbox instead of cropping when the scene is deeper than the window,
- preserve a 2D integer-scale path for users who do not want voxel presentation.

Camera interaction should change only the presentation camera. Panning and pitch must never alter emulation timing or the logical 160×144 viewport.

## Animation and performance

Rebuilding every voxel for every framebuffer pixel is the brute-force path we are trying to leave behind. Use caches keyed by:

- tile/CHR data and palette,
- object mask and animation frame,
- profile revision,
- `pixels_per_voxel` and depth parameters.

When an object moves, update its transform and depth ordering instead of rebuilding its mesh. Rebuild only when its graphic or profile changes. Cull internal faces and greedy-merge surfaces before uploading to the GPU. Keep billboard rendering as a fallback for tiny or very distant objects.

The web and desktop renderers should consume the same `VoxelScene` data and differ only in their backend (OpenGL/SDL versus WebGL). A CPU mesh builder is useful for deterministic tests and for platforms without a full 3D backend.

## Implementation plan

### Phase A — data model (safe, no visual change)

- Add `VoxelScene`, `VoxelObject`, `VoxelMaterial`, `VoxelDepthRange` and `VoxelProfile` types.
- Serialize the profile values above.
- Add a scene debug export containing object bounds, layers, masks and camera parameters.
- Keep the current renderer behind the existing fallback mode.

### Phase B — extraction

- Convert the current `SceneSnapshot` into explicit background, window and sprite candidates.
- Assemble OAM metasprites and connected tile groups.
- Add profile overrides for Super Mario Land first, since it is the current visual reference.

### Phase C — first volumetric prototype

- Use `pixels_per_voxel = 2` for Mario and other moving sprites.
- Generate 2×2×2 block cells with four depth slices, anchored at the feet.
- Use source palette materials and material-derived side shading.
- Add internal-face culling and a depth buffer.
- Fit the scene into an expanded 320×288-equivalent world target.

### Phase D — composition and quality

- Add metatile grouping for background/terrain.
- Add shallow window/HUD panels and background fill behind their holes.
- Add per-object depth and render-mode overrides.
- Tune lighting, contact shadows and outline shells to avoid black halos.

### Phase E — platform parity and tooling

- Share scene construction between SDL desktop and WebGL.
- Add profile/scene overlays showing layer and object bounds.
- Add a voxel density and geometry-count counter.
- Compare the same ROM and camera pose across desktop, web and Android.

## First experiment and acceptance criteria

The smallest useful experiment is Super Mario Land’s gameplay screen:

1. Extract Mario’s OAM metasprite and one cloud/terrain shape.
2. Quantize each sprite to 2×2 source-pixel cells.
3. Build four shallow depth slices with a 2-unit gap from the background plane.
4. Anchor Mario at his feet and keep the cloud behind him.
5. Render with an orthographic camera at a modest pitch, then pan and verify that no object clips.

The experiment is successful when:

- Mario visibly has a coherent cube-like volume rather than a wall of pixel columns.
- His feet remain grounded and his volume is closer to the camera than the background.
- Source palette colors are retained on front and side faces; black is not used as a universal extrusion color.
- HUD/background art does not become thousands of independent towers.
- The expanded viewport keeps the entire scene visible at the default camera.
- Geometry remains bounded and animation does not rebuild unchanged objects.
- Disabling voxel mode produces the same 2D output and emulator behavior as before.

## Risks and trade-offs

- **Semantic ambiguity:** fully automatic interpretation is impossible for some tiles; profiles and authoring tools are a feature, not a failure.
- **Small details:** coarse 2×2 or 4×4 cells can erase one-pixel details. Use a hybrid rule: 2×2 for normal sprites, 1×1 for selected outline/detail masks, and billboards for tiny effects.
- **Geometry cost:** naïve cubes scale poorly. Face culling, greedy meshing, caching and distance-based LOD are required.
- **Temporal popping:** animated masks can change topology. Track object identity and use hysteresis or a short transition when a profile permits it.
- **Camera distortion:** perspective can make the Game Boy image hard to read. Orthographic projection with modest pitch should be the default.
- **Cross-platform parity:** the scene builder must be deterministic and backend-independent so SDL and WebGL do not drift visually.

## Recommendation

Do not keep extending the current framebuffer-wide pixel extrusion. Preserve it as `pixel_relief` for compatibility, but make `shape` the future default voxel mode. The next code milestone should be a profile-driven `VoxelScene` builder for one game, with 2×2 sprite cells, compact depth slices, explicit layer ranges, material-derived sides, and an expanded camera-fitted viewport. Once that produces a stable Mario volume, the same pipeline can be generalized to other Game Boy and Game Boy Color games.

## Prototype status

The voxel comparison prototypes are available as selectable video pipelines:

- `Voxel diorama` / `voxel`: the existing one-source-pixel relief renderer.
- `Voxel diorama (shape-aware)` / `voxel_shape`: the experimental renderer that
  preserves native source-pixel silhouettes and uses edge-aware depth with
  stronger layer volume. This deliberately avoids coarse 2×2 grouping, which
  caused thin outlines, HUD text, and small sprites to merge into blobs.
- `Voxel pop-up book` / `voxel_popup`: a complementary overhead-scene layout.
  The framebuffer becomes a horizontal page (source Y maps to page depth),
  while the window layer rises above it and OAM sprites plus substantial
  connected tile-layer shapes become upright, page-anchored cuboids with their
  feet grounded on the page. Small isolated texture pixels stay on the page.

All three modes share the current camera controls, ROM profiles and layer ordering, so they can be switched from the desktop video menu or the web video selector while a ROM is running. The shape-aware and pop-up modes are intentionally conservative: they are visual comparison tools and not yet replacements for profile-driven metasprite/terrain extraction. The pop-up mode now has an automatic connected-component object mask; future work can replace that heuristic with cached, profile-authored metasprite/terrain masks as described above.
