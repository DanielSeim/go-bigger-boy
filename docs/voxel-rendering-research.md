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

## Research update: finding objects hidden in the background layer

### The actual problem

The pop-up-book renderer currently has a useful but deliberately weak heuristic:
it finds 4-connected groups of non-dominant-color framebuffer pixels, removes
OAM sprite pixels, and raises groups larger than a small area threshold. This
works for an isolated tree or sign, but it is the wrong abstraction for a
building. A building may touch the grass, share colors with the ground, contain
holes for windows and doors, and be drawn from many tiles that are also reused
elsewhere. One connected component can therefore contain both the building and
the page surface.

The important hardware constraint is that “background” is a rendering source,
not a semantic category. Game Boy graphics have separate Background, Window and
Object layers, but the Background and Window are tile maps and a tile is only
an 8×8 graphic selected from a shared tile-data table. The same tile can be
used as part of a building, a path, or a texture. OAM identifies hardware
sprites; it does not identify architecture assembled in the BG map. CGB
attributes add palette, bank, flip and priority information, but still do not
say “this is a building.” See [Pan Docs: Graphics](https://gbdev.io/pandocs/Graphics.html)
and [Pan Docs: Tile Maps](https://gbdev.io/pandocs/Tile_Maps).

Therefore the renderer should not try to infer a universal object label from
color or tile ID. The right target is a **background-object proposal system**:
it proposes regions that could be raised, scores them using several independent
signals, and lets a game profile or authoring tool resolve the ambiguous cases.
Unknown regions should remain flat by default. A false positive makes the whole
pop-up page look wrong; a false negative merely leaves one object flat.

### Evidence hierarchy

The signals should be considered in this order, from strongest to weakest:

1. **Native provenance.** Preserve the visible BG map coordinate for every
   source cell, including map address, tile ID, CGB attributes, scroll origin,
   palette and the decoded 8×8 occupancy mask. Keep Window and OAM ownership
   separate. This is substantially better than starting with an RGB image and
   trying to reconstruct the map afterward. The current `SceneSnapshot` already
   exposes most of the required tile-map, tile-data, attribute, scroll and OAM
   data; the missing piece is a visible-screen mapping from pixels/cells back to
   BG map coordinates.
2. **Game map/block structure.** If a profile can expose a map block or metatile
   grid, use it before individual 8×8 tiles. Buildings are normally assembled
   from repeated local arrangements: roof or eave rows, side walls, a base, a
   doorway, and sometimes windows/signs. A tile ID is weak evidence; a repeated
   2×2, 4×4 or larger arrangement with a recognizable boundary is much stronger.
   Do not assume one block size globally: make it a profile property and support
   a screen-cell fallback when map data is unavailable.
3. **Tile-graph shape.** Build a graph whose nodes are visible tile cells or
   metatile cells and whose edges connect neighbors. Candidate object regions
   can be proposed from connected groups, but edge weights should account for
   material transitions, rooflines, vertical sides, enclosed holes, repeated
   templates and contact with the walkable ground. This allows a building to be
   split from the grass even when their pixels are one connected framebuffer
   component. Superpixel/graph-cut research supports this general strategy:
   [Superpixel Cut for Figure-Ground Image Segmentation](https://isprs-annals.copernicus.org/articles/III-3/387/2016/index.html).
4. **Temporal and scroll consistency.** Track candidates in map coordinates,
   not screen coordinates. A static building should move with SCX/SCY without
   changing identity, while animated tile updates and sprite movement should not
   cause its mask to pop. Require a candidate to survive several observations or
   apply hysteresis before changing its raised/flat state. This is useful for
   rejecting one-frame animation and partial-screen artifacts, but persistence
   alone cannot distinguish a building from static grass.
5. **Semantic hints.** Collision/interaction rectangles, doors, signs, map
   triggers, object scripts and profile-provided anchors are strong confirmation
   when the frontend or a game-specific adapter can provide them. They must be
   optional: the generic Game Boy renderer cannot assume that a ROM exposes its
   map semantics in a recoverable form.
6. **Framebuffer appearance.** Palette contrast, outlines, local entropy,
   luminance and connected pixels are useful proposal features and good fallback
   hints, but they should not be the final authority. Dither, text, shadows and
   decorative texture make purely pixel-based detection too eager.

### ROM- and runtime-assisted discovery

The renderer can inspect more than the final image. It should use the ROM and
live emulator state as additional evidence, while recognizing that neither one
contains universal semantic labels.

The ROM may contain compressed graphics, map blocks, metatile tables, collision
tables, object scripts and routines that populate VRAM. At runtime, the
emulator can observe the visible BG map, VRAM tile data, CGB attributes, scroll
registers, OAM, memory writes and map transitions. Together these can recover
how a game assembles its scene even when the final framebuffer has already lost
that provenance.

The limitation is that this is effectively dynamic, game-specific reverse
engineering. Games can reuse one tile in a building and in grass, repurpose VRAM
between areas, compress or stream graphics, and store collision/map data in
different formats. “Read the ROM” is therefore a stronger source of features,
not a universal object oracle.

The preferred discovery flow is:

1. Observe the live BG map and VRAM directly.
2. Trace tile/map writes and map transitions when instrumentation is available.
3. Mine repeated tile/metatile arrangements and the routines or data structures
   that produce them.
4. Correlate candidates with collision, doors, signs, warps, scripts and stable
   map coordinates when those signals can be exposed.
5. Generate a confidence-scored object candidate and cache the result by ROM
   fingerprint, map identity and template identity.
6. Keep the result presentation-only; no inferred meaning should affect
   emulation, collision or game logic.

This suggests a useful long-term split: the generic renderer owns observation,
feature extraction, caching and visualization, while a game adapter may expose
optional map/block/collision decoders. A decoder can be generated or refined
after observing a ROM, but the runtime should consume deterministic rules once
they exist.

### Additional automatable heuristics

The tile graph should not depend on one connected-component test. It can produce
several independent signals and combine them into explainable scores:

- **Tile/metatile repetition:** find recurring 2×2, 4×4 or larger arrangements,
  including normalized variants with palette, bank or flip differences.
- **Shape grammar:** look for roof/eave rows above walls, vertical sides, a base
  line, a door or sign near the ground-facing edge, and other class-specific
  arrangements.
- **Boundary strength:** score abrupt changes in tile neighborhoods, palette,
  CGB attributes, edge direction or material statistics.
- **Symmetry:** buildings and props often have mirrored or near-mirrored sides;
  use symmetry as supporting evidence, not as a requirement.
- **Negative space:** windows, doors, arches and enclosed holes can keep a
  building together even when their colors split it into separate components.
- **Ground contact:** prefer candidates with a coherent bottom edge aligned to
  walkable or visually continuous terrain.
- **Size and aspect ratio:** reject isolated noise and implausibly thin or huge
  texture regions before they reach the mesh builder.
- **Palette/material grouping:** roofs, walls, windows and ground often form
  stable material groups even when their colors overlap elsewhere.
- **Contextual tile frequency:** learn that a tile is building-like only in a
  particular neighborhood; never assign a global object class from tile ID
  alone.
- **Map repetition:** recognize the same arrangement in multiple towns, rooms or
  map blocks, even when it is only partially visible in the current viewport.
- **Scroll persistence:** track candidates in map coordinates as they enter and
  leave the screen, extending partial objects instead of creating new ones.
- **VRAM upload co-occurrence:** tiles copied together or used by the same
  rendering routine are likely related, but this remains supporting evidence
  because VRAM is frequently reused.
- **Collision and interaction correlation:** doors, signs, impassable cells,
  warp points and scripts can confirm a building footprint when a game adapter
  exposes them.
- **OAM occlusion and priority:** sprite coverage and PPU priority can reveal
  which candidate surfaces are in front of or behind actors.
- **Animation stability:** suppress candidates that only exist during a tile
  animation or transient VRAM update; require hysteresis before changing an
  object's semantic state.
- **Screen-edge completion:** if a candidate's map-coordinate pattern continues
  beyond the viewport, preserve the same identity when more of it becomes
  visible.

For a Pokémon-style building, a first-class score can combine the strongest
signals explicitly:

```text
building_score = repeated_structure
               + roof_wall_base_shape
               + symmetry
               + ground_contact
               + door_or_interaction_hint
               + map_repetition
               + temporal_stability
               - texture_noise
               - tiny_or_fragmented_shape
               - HUD_likelihood
```

The score should resolve to a policy, not an immediate height:

```text
high confidence    -> raise as a coherent cutout or shallow volume
medium confidence  -> show in the debug overlay and keep flat by default
low confidence     -> flat page material
```

This asymmetry is intentional. A false positive turns grass, text or texture
into an incorrect pop-up object; a false negative only leaves a candidate flat
until a profile rule or authoring decision confirms it.

### Recommended detector: a hybrid tile graph

The proposed detector has four stages and produces an explicit mask rather than
just a larger voxel height:

```text
visible BG map + decoded tiles + OAM
              │
              ▼
screen-cell provenance and map-coordinate graph
              │
              ▼
candidate regions: connected groups, repeated templates, edge/ground cues
              │
              ▼
profile rules + optional map/collision hints + temporal confidence
              │
              ▼
PopupObjectMask { cells, class, anchor, depth, confidence }
```

#### Stage A: reconstruct provenance

Add a renderer-facing grid (provisional name `VisibleTileCell`) containing:

```text
screen_cell       visible x/y and partial-cell clipping
map_cell          BG map x/y after SCX/SCY wrapping
tile_id           tile number and CGB bank/attributes
palette           effective palette/material identity
opaque_mask       8×8 decoded pixel occupancy
owners            background/window/OAM/priority bits
```

The grid should be generated from the same PPU state used for the framebuffer,
including wrapping and window transitions. It should be a presentation snapshot,
not a new emulation path. The pop-up detector can then reason at 8×8 or profile
selected metatile resolution and only use pixel masks to carve the final object
silhouette.

#### Stage B: propose regions without committing to geometry

Generate several proposal families instead of trusting one connected-component
pass:

- connected tile/metatile regions after removing known sprites and HUD/window
  cells;
- repeated local arrangements found by hashing normalized tile/attribute
  neighborhoods;
- shape-grammar proposals such as a roofline over a wall rectangle with a
  ground-facing base or doorway;
- profile seeds and explicit rectangles/polygons supplied by an authoring file.

Keep proposals separate when a one-cell seam is plausible. For example, a
building roof touching grass should initially be one candidate for matching, but
the graph should contain a low-cost cut along the roof/base seam so the solver
can split the building from the page. Conversely, windows and doors should stay
inside the building candidate even if their colors form separate components.

#### Stage C: score and resolve regions

Each candidate receives a confidence score with explainable terms:

```text
score = template_match
      + roof/wall/base shape evidence
      + repeated-map evidence
      + ground-contact/anchor evidence
      + profile or collision hint
      + temporal stability
      - texture/dither evidence
      - HUD/text evidence
      - tiny-area or fragmented-shape penalty
```

The score should decide among `flat`, `raised_cutout`, `shallow_volume` and
`profile_required`, rather than directly selecting a depth per pixel. A
conservative threshold should keep uncertain regions flat. An authoring overlay
should expose each term and allow accepting, rejecting, splitting or merging a
candidate; those decisions can then become a compact profile rule instead of a
hand-authored pixel mask.

#### Stage D: build the pop-up object

The resolved region should carry:

- an object class (`building`, `tree`, `sign`, `fence`, `terrain_edge`, etc.);
- a source mask at tile and pixel resolution;
- a hinge/anchor line, normally the bottom contact with the ground page;
- a depth range and side-material policy;
- occlusion priority relative to sprites and other objects;
- a stable identity key based on map coordinates and template/profile ID.

Buildings should normally be one coherent raised cutout or shallow prism, not a
set of unrelated per-pixel towers. Their roof, walls, door and windows can keep
the native texture on the front face while the side face gets a controlled
material-derived shade. The ground beneath the building should be reconstructed
as page material, not duplicated as a second raised copy.

### Pokémon building case study

Pokémon is a good stress test because an overworld building is exactly the case
the current heuristic cannot understand: it is static BG artwork, often shares
the ground palette, usually touches the ground, and is assembled from multiple
tiles. A robust first profile should operate on map/block arrangements rather
than “all pixels whose color differs from the dominant grass.”

For a building candidate, look for a combination of:

- a broad, repeated roof/eave pattern near the top;
- left/right side boundaries or a wall rectangle below it;
- an opening, door, sign or centered entrance near the ground-facing edge;
- a stable base line where the object meets walkable terrain;
- an arrangement that repeats across towns or maps, even if its individual
  tile IDs also appear in ordinary ground art.

The detector should raise the roof/wall/door silhouette together, assign its
hinge to the base line, and leave the surrounding grass/road cells on the page.
If a building is only partially visible because of scrolling, the map-coordinate
identity should let the next frame extend the same object instead of creating a
new cutout. If the evidence is insufficient, the profile should mark that map
region as flat until the authoring overlay supplies a rule.

This is also consistent with the closest existing voxel precedents. The
NESRecomp renderer keeps engine mechanics separate from game-specific policy:
profiles decide tile heights, grouped decorations and sprite visibility, while
the engine owns assembly, occlusion and shadows ([NESRecomp voxel renderer
design](https://github.com/mstan/nesrecomp/blob/master/docs/VOXEL_RENDERER.md)).
The Pokémon-focused Dramatic Shape project likewise describes a layered
overworld rather than treating every visual element as a voxel, and reports
specific fixes for house wall layering ([project README](https://github.com/TeJota1337/DramaticShapeVoxelMod)).
These are useful precedents, not evidence that a generic tile-ID detector will
work across games.

### What not to do

| Approach | Why it is insufficient | Appropriate use |
| --- | --- | --- |
| Raise every non-dominant-color pixel | Buildings merge with grass; textures and text become props | Initial visual fallback only |
| Raise every connected component | Touching architecture and ground become one object; holes fragment objects | Proposal generator, with tile-aware splitting |
| Treat a tile ID as an object class | Tiles are reused in many map contexts and may be bank/palette dependent | Feature in a map-context rule, never a global rule |
| Run a general ML detector per frame | 160×144 pixel art is unlike normal training imagery; models add dependencies, latency and nondeterminism | Optional offline authoring assistant, not runtime core |
| Hand-author every visible pixel mask | Highest quality but expensive, brittle under animation/scrolling and impossible to generalize | Final override for landmark scenes |

The practical recommendation is a deterministic, profile-driven tile graph with
an offline authoring workflow. Classical segmentation can make proposals, but
the profile owns semantic truth. A learned model may help suggest seeds while an
author is creating a profile, provided the shipped runtime consumes only the
resulting deterministic rules.

### Proposed profile and debug contract

Extend the future voxel profile with object discovery settings rather than
hard-coding Pokémon-specific logic in `voxel_renderer.cpp`:

```ini
[popup_objects]
source = visible_bg_tiles
cell_size = 8
metatile_size = 2
proposal = tile_graph
min_confidence = 0.78
temporal_hysteresis_frames = 3
unknown_policy = flat
allow_collision_hints = true

[popup_objects.building]
match = profile_template
default_geometry = shallow_volume
anchor = ground_contact
depth = 4
```

The exact syntax is provisional. More important is the separation of concerns:
the scene snapshot supplies provenance, the detector supplies candidates, the
profile resolves semantics, and the renderer turns resolved masks into meshes.

The debug view should be able to show, in one frame:

- the visible BG map-cell grid and map coordinates;
- candidate masks and their confidence terms;
- accepted/rejected/split/merged decisions;
- object anchors and depth bands;
- the final flat-versus-raised ownership map.

This makes a Pokémon building failure diagnosable: we can tell whether the
problem was wrong map provenance, a missed roof template, an incorrect seam, or
the final mesh/occlusion pass.

### Revised implementation sequence

Add this work before the existing pop-up geometry-quality phase:

1. Export visible BG/window provenance from `SceneSnapshot`, including wrapped
   map coordinates and partial-cell ownership.
2. Add a CPU-only tile-graph proposal builder with deterministic masks and a
   JSON/debug export. Keep it disabled in the live renderer initially.
3. Add an overlay/editor that accepts, rejects, splits and merges candidates;
   save the result as profile rules keyed by ROM fingerprint and map/template
   identity.
4. Implement the generic building/tree/sign rules and hysteresis, with
   `unknown_policy = flat` as the default.
5. Add a Pokémon-style building profile as the first acceptance fixture. Verify
   that its roof, walls, door and windows rise together while ground cells stay
   flat, and that the result remains stable while SCX/SCY scrolls.
6. Only then connect resolved `PopupObjectMask` instances to the existing
   pop-up mesh builder and tune depth, lighting and side materials.

The existing connected-component mask can remain as a clearly labelled
`heuristic` proposal mode for comparison, but it should not become the default
semantic source for the pop-up-book renderer.

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

## Observation tooling prototype

The first offline observation/proposal path is now available without changing
voxel rendering:

```text
local ROM
   │
   ├─ gbb_cli --scene-jsonl observations.jsonl --frames N
   │
   └─ scripts/voxel_scene_analyzer.py observations.jsonl
                         │
                         └─ proposals.json
```

`gbb_cli` records one scene snapshot after each completed frame. Each JSONL
record includes the frame number, instruction count, ROM fingerprint and the
map-aware `visible_tile_cells` data. The analyzer removes cells covered by
visible OAM sprites, segments compatible background tile regions and emits
explainable proposal features for area, compactness, density variation,
repetition, boundary strength, ground contact, aspect ratio and temporal
stability. It does not modify the renderer and does not treat medium-confidence
regions as geometry.

Example workflow with a locally owned ROM:

```sh
build-desktop/gbb_cli /path/to/game.gb \
  --scene-jsonl /tmp/game-observations.jsonl --frames 120
python3 scripts/voxel_scene_analyzer.py \
  /tmp/game-observations.jsonl --output /tmp/game-proposals.json
```

This is deliberately a first proposal pass, not a Pokémon building detector.
The next tooling increment should add a visual overlay for accepting,
rejecting, splitting and merging proposals. The resulting decisions can then
become ROM-specific profile rules.

### Building a local ROM observation corpus

The private, ignored `roms/` directory can be inventoried without copying or
modifying any ROMs:

```sh
python3 scripts/build_voxel_rom_manifest.py
```

This writes `roms/voxel-rom-manifest.json` with SHA-256 hashes and header
metadata for every `.gb`, `.gbc`, and `.sgb` file. It also writes
`roms/voxel-observation-corpus.json`, a small reproducible selection for the
first observation pass. Exact byte duplicates are collapsed, while revisions,
translations, hacks, and betas remain separate. The selection includes
Pokémon-style/top-down scenes, platformers, SGB-enhanced cartridges,
homebrew/demos, and general games. The output contains paths only; the ROMs
remain in their original locations.

The corpus is deliberately a starting set rather than a claim that filenames
can identify rendering behavior. Runtime observations and the later overlay
review are still required to validate scrolling, window usage, sprites, and
building-like background objects.

### Deterministic input movies

`gbb_cli` accepts a frame-based input movie while recording scene JSONL. The
format is intentionally small and reviewable: each keyframe sets the buttons
held from that frame until the next keyframe.

```text
GBB_INPUT_MOVIE 1
# frame held-buttons
0 none
30 right
90 right+a
120 none
```

Supported buttons are `right`, `left`, `up`, `down`, `a`, `b`, `select`, and
`start`. Keyframes must be strictly increasing; `none` and `-` release every
button. Replay applies the state before each captured frame and records both
the numeric mask and button names in each JSONL observation.

Example:

```sh
build-desktop/gbb_cli roms/'Pokemon - Crystal Version (UE) (V1.0) [C][!].gbc' \
  --input-movie /tmp/pokemon-overworld.movie \
  --scene-jsonl /tmp/pokemon-overworld.jsonl --frames 600
```

This is still deterministic frame driving rather than a full TAS format: it
does not embed a save state, and the movie assumes the same ROM, emulator
build, initial boot state, and instruction setup. That keeps it suitable for
comparing scene observations while leaving the existing binary SDL movie
recordings unchanged.

### Automated corpus review

The bounded corpus runner generates a deterministic exploration movie for each
selected ROM, captures observations, analyzes proposals, and writes an HTML/SVG
review report in one pass:

```sh
python3 scripts/run_voxel_corpus.py --frames 900
```

For a quick smoke run, limit it to a few ROMs:

```sh
python3 scripts/run_voxel_corpus.py --limit 4 --frames 180
```

The default output is `roms/voxel-review/`, containing generated movies,
JSONL observations, proposal JSON, per-ROM logs, `run-manifest.json`, and
`index.html`. The exploration pattern is intentionally conservative and
repeatable: it taps `start`/`a`/`b` and holds each cardinal direction for a
bounded interval. Slow LCD-off boot sequences are allowed up to the batch
runner's bounded `--max-instructions-per-frame` budget (10 million by
default). It is a coverage probe, not a game-specific gameplay bot.
Partial captures are retained and analyzed when a ROM stops producing frames
after recording at least one observation. Failed ROMs are recorded in the
manifest and do not prevent the remaining corpus from being attempted; the
command exits nonzero only for hard capture/analysis failures or a report
failure.

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
