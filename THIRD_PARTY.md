# Third-party code

MoveEhr's own code is public domain (see `LICENSE`). The libraries below are vendored (or, for DXVKit, a git submodule) under
their own licences, which all allow redistribution in source and binary form. Each library's licence file sits next to its code
and must stay there; binary releases carry this file and the licence texts in `licenses/` inside the release zip.

| Library | Version | Path | Licence | Required notice |
|---|---|---|---|---|
| ufbx (FBX loader) | 26a482a | `third_party/ufbx` | MIT **or** public domain (Unlicense), your choice | none under the public-domain option; under MIT keep `third_party/ufbx/LICENSE` |
| cgltf (glTF loader) | 85cd623 | `third_party/cgltf` | MIT | keep the copyright + permission notice (`third_party/cgltf/LICENSE`, Copyright (c) 2018-2021 Johannes Kuhlmann) |
| stb_image, stb_image_write | stb_image 2.30, stb_image_write 1.16 (2c980bb) | `third_party/stb` | MIT **or** public domain (Unlicense), your choice | none under the public-domain option; under MIT keep `third_party/stb/LICENSE` |
| FFGL SDK (FreeFrame GL) | 5b6e078 | `third_party/ffgl` | BSD 3-clause | keep the copyright notice, conditions and disclaimer in source copies and in the documentation of binary copies (`third_party/ffgl/LICENSE.md`, Copyright 2023 FreeFrame); do not use FreeFrame's name to endorse derived products |
| meshoptimizer (simplifier + allocator only) | 1.3 (4c20343) | `third_party/meshoptimizer` | MIT | keep `third_party/meshoptimizer/LICENSE.md` (Copyright (c) 2016-2026 Arseny Kapoulkine) |
| Clipper2 (engine + offset) | 2.0.1 (f9c5eb6) | `third_party/clipper2` | Boost Software License 1.0 | keep the licence text with source copies (`third_party/clipper2/LICENSE`); not needed for machine-code-only copies |
| earcut.hpp | 3.2.4 (92acda9) | `third_party/earcut` | ISC | keep the copyright + permission notice (`third_party/earcut/LICENSE`, Copyright (c) 2015, Mapbox) |
| NanoSVG (parser only) | 239e102 | `third_party/nanosvg` | zlib | don't misrepresent the origin; mark altered versions; keep `third_party/nanosvg/LICENSE.txt` (Copyright (c) 2013-14 Mikko Mononen) in source copies |
| DXVKit (DXV video decoder, C core) | 1.0.0 (2f5256a) | `third_party/dxvkit` (git submodule, https://github.com/AmritusG/dxvkit) | MIT | keep `third_party/dxvkit/LICENSE` (Copyright (c) 2026 Amrit Rosell) |

`third_party/VERSIONS.txt` records the exact upstream commits.

Not in this repository: no LGPL/GPL code, no libtess2 / SGI Free Software License B code (it was replaced by Clipper2 + earcut
before this release; `tools/guards.sh` check G7 fails the build check if it ever comes back).

The macOS frameworks MoveEhr links (OpenGL, AVFoundation, CoreMedia, CoreVideo, CoreText, CoreGraphics, Vision, ...) are part of
the operating system and are not redistributed.
