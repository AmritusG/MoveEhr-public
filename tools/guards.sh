#!/bin/sh
# MoveEhr hard-rule guards (inherited from LoadEhr; W1: G8 unique names, G9 exact counts) (part of tools/regress.sh). Exit 1 on any failure.
#  G1 no param named Library* or Laser* in any of the four bundles
#  G2 no code path that opens a directory listing for models: the only directory API in src/ is the texture lookup next to an
#     already chosen model file (ModelLoader.cpp FolderIndex: the model's own folder + its textures/ subfolder, textures only; MoveEhr W8 B:
#     with Movement Source + Same Folder, that same folder's movement files of the model's format, read for their takes)
#  G3 no model file inside any built bundle
#  G4 nothing loaded = transparent: a fresh instance of each bundle covers 0 pixels; every Model slot is empty; Status A says so
#  G5 no laser / library source files, no library tools
#  G6 (W2) the mirror room is gone: no param named Mirror Room / Mirror Count / Box Size / LED*, no Mirror* source, no MIRROR program
#     variant (Mirror Floor in the Floor group is not the mirror room and stays)
#  G7 (W2) libtess2 is gone (SGI Free Software License B: not on the licence list): no libtess2 / tesselator / SGI-B in the tree;
#     (W3) no LGPL file anywhere in the tree (submodules included; docs/handoffs are prose and may quote a tag): no LGPL SPDX tag,
#     no GNU Lesser / Library GPL licence text,
#     no COPYING.LESSER / COPYING.LIB file
#  G8 (MoveEhr W1) every param name is unique within each bundle (Arena crashes at instantiation on a duplicate, FFGL-SPINOUT-L001)
#  G9 (MoveEhr W1; W3 +5 voxel part sizes; W7 + Point Shading; W8 + Movement Source + the Movement Chain group) each bundle carries exactly
#     the planned count (docs/PLAN.md section 12: 400 / 411 / 338 / 349)
cd "$(dirname "$0")/.."
fail=0
say() { printf '%s\n' "$*"; }
T=$(mktemp -d)
for b in MoveEhr MoveEhrFX MoveEhrWow MoveEhrWowFX; do
	printf 'bundle build/%s.bundle\nsize 320 180\ninstance\nparams\nframes 3\nmeans 2\nget Model A\nget Model B\nget Model C\nget Model D\nget Status A\n' "$b" > "$T/g_$b.txt"
	./build/loadehr_harness "$T/g_$b.txt" > "$T/g_$b.out" 2>/dev/null
	n=$(grep -c -E '^\s+[0-9]+ \|' "$T/g_$b.out")
	bad=$(grep -E '^\s+[0-9]+ \|' "$T/g_$b.out" | awk -F'|' '{print $3}' | grep -i -E '^\s*(library|laser)' | tr '\n' ',')
	if [ -n "$bad" ]; then say "G1 FAIL $b: $bad"; fail=1; else say "G1 ok   $b: $n params, none named Library* / Laser*"; fi
	room=$(grep -E '^\s+[0-9]+ \|' "$T/g_$b.out" | awk -F'|' '{print $3}' | grep -E '^\s*(Mirror Room|Mirror Count|Box Size|Box Depth|LED )' | tr '\n' ',')
	if [ -n "$room" ]; then say "G6 FAIL $b: $room"; fail=1; else say "G6 ok   $b: no Mirror Room / Mirror Count / Box Size / LED* param"; fi
	dup=$(grep -E '^\s+[0-9]+ \|' "$T/g_$b.out" | awk -F'|' '{gsub(/^ +| +$/,"",$3); print $3}' | sort | uniq -d | tr '\n' ',')
	if [ -n "$dup" ]; then say "G8 FAIL $b: duplicate names $dup"; fail=1; else say "G8 ok   $b: $n names, all unique"; fi
	case $b in MoveEhr) want=400;; MoveEhrFX) want=411;; MoveEhrWow) want=338;; MoveEhrWowFX) want=349;; esac
	if [ "$n" = "$want" ]; then say "G9 ok   $b: $n params = the plan's $want"; else say "G9 FAIL $b: $n params, the plan says $want"; fail=1; fi
	[ "$n" -le 512 ] || { say "COUNT FAIL $b: $n params > 512 (Arena drops every param past index 511)"; fail=1; }
	px=$(grep '^means frame' "$T/g_$b.out" | sed -E 's/.*\| ([0-9]+) px$/\1/' | sort -u | tr '\n' ' ')
	empty=$(grep -c -E "^get Model [A-D] = ''$" "$T/g_$b.out")
	st=$(grep '^get Status A' "$T/g_$b.out")
	if [ "$px" = "0 " ] && [ "$empty" = 4 ] && echo "$st" | grep -q 'transparent'; then say "G4 ok   $b: covered pixels $px; 4 empty slots; $st"
	else say "G4 FAIL $b: covered '$px', empty slots $empty, $st"; fail=1; fi
done
hits=$(grep -n -E 'opendir|readdir|directory_iterator|contentsOfDirectory|enumeratorAtPath|scandir|glob\(|fts_open|NSFileManager|std::filesystem' src/*.cpp src/*.h src/*.mm src/*.inc)
if [ "$(echo "$hits" | grep -c .)" = 2 ] && [ "$(echo "$hits" | grep -c 'src/ModelLoader.cpp')" = 2 ] && grep -q 'struct FolderIndex' src/ModelLoader.cpp; then
	say "G2 ok   the only directory API: ModelLoader.cpp FolderIndex (textures next to the chosen model file):"; echo "$hits" | sed 's/^/        /'
else say "G2 FAIL directory APIs:"; echo "$hits"; fail=1; fi
models=$(find build/MoveEhr*.bundle -type f \( -iname '*.obj' -o -iname '*.fbx' -o -iname '*.gltf' -o -iname '*.glb' -o -iname '*.bin' -o -iname '*.mtl' -o -iname '*.png' -o -iname '*.jpg' \) | grep -v Probe)
if [ -z "$models" ]; then say "G3 ok   no model / texture file inside build/MoveEhr*.bundle ($(find build/MoveEhr.bundle build/MoveEhrFX.bundle build/MoveEhrWow.bundle build/MoveEhrWowFX.bundle -type f | wc -l | tr -d ' ') files: binaries + Info.plist)"
else say "G3 FAIL: $models"; fail=1; fi
gone=$(ls src/Laser* src/Library* tools/w19_* tools/prune.sh tools/w17_library.py 2>/dev/null)
if [ -z "$gone" ]; then say "G5 ok   no Laser* / Library* sources, no library tools"; else say "G5 FAIL: $gone"; fail=1; fi
mir=$(ls src/Mirror* tools/w27_mirror* 2>/dev/null; grep -l -E 'define MIRROR|ifdef MIRROR|MirrorShaders|kMirror(Lib|MarchLib|PointLib)|MirrorClip|MirrorCell|MirrorFold\(' src/* CMakeLists.txt 2>/dev/null)
if [ -z "$mir" ]; then say "G6 ok   no Mirror* source, no MIRROR program variant"; else say "G6 FAIL: $mir"; fail=1; fi
tess=$(ls -d third_party/libtess2 2>/dev/null; grep -rl -i -E 'libtess|tesselator\.h|tessNewTess|SGI FREE SOFTWARE|Free Software License B' src third_party CMakeLists.txt 2>/dev/null)
if [ -z "$tess" ]; then say "G7 ok   no libtess2 / tesselator / SGI-B in src, third_party, CMakeLists.txt"; else say "G7 FAIL: $tess"; fail=1; fi
lgpl=$( { git ls-files --recurse-submodules -z -- . ":!tools/guards.sh" ":!docs/handoffs" | xargs -0 grep -l -I -E 'SPDX-License-Identifier:.*LGPL|GNU (LESSER|LIBRARY) GENERAL PUBLIC LICENSE|GNU (Lesser|Library) General Public License' 2>/dev/null
	git ls-files --recurse-submodules | grep -i -E '(^|/)(COPYING\.(LESSER|LIB)|LGPL[^/]*)$'; } | sort -u)
nf=$(git ls-files --recurse-submodules | wc -l | tr -d ' ')
if [ -z "$lgpl" ]; then say "G7 ok   no LGPL file in the tree ($nf tracked files, submodules included)"; else say "G7 FAIL LGPL: $lgpl"; fail=1; fi
rm -rf "$T"
exit $fail
