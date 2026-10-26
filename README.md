# MoveEhr

Animated 3D models in Resolume Arena. MoveEhr is a set of four FFGL plugins that load a model file (.fbx, .gltf, .glb, .obj),
play its animations, chain movements, morph between up to four models, and render it as surfaces, voxels, wireframe or points,
live, on the GPU.

| Plugin | Type | What it is for |
|---|---|---|
| **MoveEhr** | source | renders the model over transparency |
| **MoveEhr FX** | effect | the same, with the layer below as background, skin or reflection (+ per-part skins from the input) |
| **MoveEhr Wow** | source | the same renderer with the showpiece looks: Swarm, Lava, Caustics |
| **MoveEhr Wow FX** | effect | Wow as an effect |

There are four plugins rather than one because Arena shows at most 512 parameters per FFGL plugin, so the controls are split to
keep each plugin under that limit (400 / 411 / 338 / 349 parameters).

There is no built-in model library. A plugin shows nothing until you point a Model slot at a file. Some free models to start
with are in `models/` (see below).

Public domain: see `LICENSE` (The Unlicense). Use the code however you want. Third-party libraries keep their own licences
(`THIRD_PARTY.md`); the bundled models are CC0 or our own (`models/CREDITS.md`).

## Requirements

- macOS 11 or later (built as a universal binary: Apple Silicon + Intel). Tested on Apple Silicon (M4 Max).
- Resolume Arena 7 (tested with **Arena 7.28.1**).
- To build: Xcode Command Line Tools (`xcode-select --install`), CMake 3.20+, git.
- Optional, only for the model scripts: Python 3; Blender 4.x (the FBX conversions and the movement split run it headless).

## Build

```sh
git clone --recurse-submodules https://github.com/AmritusG/MoveEhr-public.git
cd MoveEhr-public
# (cloned without --recurse-submodules? run: git submodule update --init)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This produces `build/MoveEhr.bundle`, `build/MoveEhrFX.bundle`, `build/MoveEhrWow.bundle`, `build/MoveEhrWowFX.bundle`, plus the
offline test host `build/loadehr_harness` and a few diagnostic probes you do not need to install.

## Install

Copy the bundles into Arena's Extra Effects folder, sign them ad-hoc (Apple Silicon needs locally built code to carry a
signature) and clear the quarantine flag. Then restart Arena, which only picks up new or replaced plugins when it starts:

```sh
EE="$HOME/Documents/Resolume Arena/Extra Effects"
mkdir -p "$EE"
for b in MoveEhr MoveEhrFX MoveEhrWow MoveEhrWowFX; do
  rm -rf "$EE/$b.bundle"
  cp -Rp "build/$b.bundle" "$EE/"
  codesign --force --sign - "$EE/$b.bundle"
  xattr -cr "$EE/$b.bundle"
done
```

The same steps apply to the prebuilt bundles from the GitHub release. They are ad-hoc signed, not notarised: after unzipping,
run `xattr -cr` on them or macOS will refuse to load them.

The sources appear under Sources, the FX versions under Effects. The plugins write a log to `~/Library/Logs/MoveEhr/MoveEhr.log`.

## Quick start

1. Drop **MoveEhr** on a clip. It is transparent: no model yet.
2. Set **Model A** to `models/quaternius/characters/FBX/Knight_Male.fbx`. **Status A** shows the triangles, parts and clips.
3. In **Animation**, pick **Anim Clip A** and set **Anim Play** to Loop.
4. Fill Model B (C, D) with other files and move **Transition** (or set **Transition Play**) to morph between them.
5. To chain movements stored in separate files: set Model A to `models/moves/Knight.fbx` and **Movement Source** to + Same
   Folder, then in **Movement Chain** turn on **Chain On** and pick a clip for each step.

## The controls, in plain words

Every parameter of each plugin, with ranges and defaults: `docs/PARAMS.md`.

- **Model**: four model slots (A..D), each with a Status line and Reload. The filled slots form a chain A -> B -> C -> D;
  **Transition** walks it with 17 transition styles (Dissolve, Shatter, Dust, Liquid, Points, Voxel, Melt, Glitch, ...), a style
  order, ease / stagger / spread, and **Transition Play** to run it by itself. **Model Source** can also turn text or a logo
  (SVG / PDF / PNG) into a 3D model.
- **Animation**: per slot, which animation clip plays (or the bind pose); play mode (Loop, Ping-Pong, Once On Trigger, Scrub),
  speed, cross-fade between clips, BPM lock, root motion (in place or follow), and where movements come from (the model file
  only, or also the other animation files in the same folder).
- **Movement Chain**: up to 8 steps, each a clip with its own length and speed, played in order, ping-pong, once, random or
  shuffled, timed in seconds or beats; Next / Prev / Restart buttons. Reverse Clips plays ping-pong steps backwards on the way back.
- **Text / Logo / Extrude / Letters**: the text or logo model, its depth and bevel, per-letter animation.
- **Transform / Camera / Motion**: rotation, spin (with glide), position, scale; camera field of view, distance and orbit;
  wobble, beat bounce / nod and audio-driven scale, glow and explode.
- **Material / Surface / Surface Look / Parts**: tint, hue, metal, roughness, normal and height strength, emissive and rim glow;
  surfaces like oil film, glitter, liquid chrome, glass, hologram, heat glow and animated patterns; surface looks (audio spikes,
  skin of copies, x-ray, thermal, night vision, LED matrix); up to 8 parts with their own visibility, hue, glow and surface, and
  (FX) a skin from the input layer.
- **Shape**: shatter (with physics and bounce), slices, low poly with tumbling facets, blow to dust, contour lines, and
  **voxels**: the model rebuilt from cubes, with a block size per body part (head, torso, arms, hands, legs).
- **Deform**: taper, twist, bend, squash, shear, bulge, spherize (toward a sphere or cube), noise warp, ripple, melt / drip,
  glitch, mirror fold, jelly, audio-driven deform.
- **Wireframe**: wire, **points only** or points + wire; point shape, size variation, motion, twinkle, glow and bloom, and how
  points are lit (fully per pixel, or once per point, which is much faster on a million points).
- **Lines / Build / Cut**: plexus, light-paint and neon-tube lines; slit-scan, print-on, exploded and black-hole build-ups;
  cross-sections, reveal boxes / spheres and windows.
- **Lighting**: a key light, ambient, exposure and tonemapping, and an environment for reflections (studio, the input layer in
  the FX plugins, or an .hdr file, optionally shown as the backdrop).
- **Post / Floor / Output / Audio**: bloom and lens flare; a shadow-catching floor or wall and a mirror floor; background
  colour, anti-aliasing (MSAA) and Reset All; the audio input.
- **FX / Input**: explode, dissolve, toon steps, outline, depth fog; and (effect plugins) how the incoming layer is used:
  background or skin, and how it is projected.
- **Swarm / Lava / Caustics** (Wow plugins only): firefly swarms released from the surface, spreading lava cracks with heat haze,
  caustic light through glass.
- **Debug**: debug views (albedo, normals, UVs, part IDs, ...), a log switch, built-in test shapes.

## Bundled models (`models/`)

- `quaternius/`: 61 animated low-poly characters, mechs and animals by Quaternius, CC0 (public domain).
- `moves/`: a knight split into one movement per file (for the Movement Chain); `moves_glb/`: the same as .glb; `walk/`: a walk
  with forward travel. All derived from the CC0 files.
- `generated/`: our own test models, HDR environment maps and test shapes, made by the scripts in `tools/`.

Where every file comes from, with source pages and licences: `models/CREDITS.md`.

## Tools

| Script | What it does |
|---|---|
| `tools/smoke.sh [out]` | renders a CC0 character with each of the four built bundles, headless (no Arena needed) |
| `tools/guards.sh` | checks the built bundles and the tree: parameter counts, unique names, nothing rendered when empty, no model inside a bundle, no non-permissive third-party code |
| `tools/params_doc.py` | regenerates `docs/PARAMS.md` from the built bundles |
| `build/loadehr_harness <script>` | the offline host behind the scripts above: loads a bundle, sets parameters by name, renders PNGs (commands listed at the top of `tools/harness/harness.cpp`) |
| `tools/gen_test_models.py`, `tools/gen_anim_models.py`, `tools/gen_hdri.py`, `tools/gen_sphere_1m.py`, `tools/gen_test_logos.py` | regenerate the files in `models/generated/` and `test_logos/` (output goes to `test_models/gen/`, which git ignores) |
| `tools/split_moves.py`, `tools/make_root_walk.py` | the headless Blender scripts that made `models/moves/` and `models/walk/` |

The plugins were developed against a larger private regression suite (reference renders made on one machine, Arena
automation) that is not part of this repository. The tools above are the ones that run from this tree alone.

## Known limits

- macOS only (OpenGL via FFGL, AVFoundation for video, Apple Vision for face tracking in the FX builds).
- Arena drops every FFGL parameter past index 511. That is why there are four plugins, and why some groups exist only in some of
  them (see `docs/PARAMS.md`).
- Arena shows the plugin name from the FFGL info, not the bundle name. Replacing a plugin needs an Arena restart.
- Model formats: .fbx, .gltf, .glb, .obj. Textures are found next to the model file (and in a `textures/` folder beside it).
- Very heavy models (a million vertices and up) load in the background and can cost frame rate with the heavier looks.

## Credits

Written by Amrit Rosell. Models by Quaternius (CC0). Libraries: ufbx, cgltf, stb, FFGL SDK, meshoptimizer, Clipper2,
earcut.hpp, NanoSVG, DXVKit (see `THIRD_PARTY.md`).
