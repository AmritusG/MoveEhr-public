#!/bin/sh
# Smoke render: the four bundles (build/), each loads a shipped CC0 character (models/quaternius/characters/FBX/Knight_Male.fbx),
# plays its first clip for half a second and renders a PNG to <out>/ (default smoke_out/). Passes when every bundle's last frame
# covers pixels and its Status A reports the load. Needs only a Release build; no Arena.
cd "$(dirname "$0")/.."
OUT="${1:-smoke_out}"; mkdir -p "$OUT"
MODEL="$(pwd)/models/quaternius/characters/FBX/Knight_Male.fbx"
fail=0
for b in MoveEhr MoveEhrFX MoveEhrWow MoveEhrWowFX; do
	{ printf 'bundle build/%s.bundle\nsize 640 360\n' "$b"
	  case $b in *FX) echo 'input bars';; esac
	  printf 'instance\nmark\ntext Model_A %s\nwait_loaded 60\nset Anim Clip A 1\nset Anim Play 1\nframes 30\nmeans 1\nget Status A\nshot %s/%s.png\n' "$MODEL" "$OUT" "$b"
	} > "$OUT/$b.txt"
	./build/loadehr_harness "$OUT/$b.txt" > "$OUT/$b.log" 2>&1
	px=$(grep '^means frame' "$OUT/$b.log" | tail -1 | sed -E 's/.*\| ([0-9]+) px$/\1/')
	st=$(grep '^get Status A' "$OUT/$b.log")
	if [ -n "$px" ] && [ "$px" -gt 0 ] && [ -s "$OUT/$b.png" ]; then echo "smoke ok   $b: $px px covered; $st"
	else echo "smoke FAIL $b: covered '$px'; $st (log: $OUT/$b.log)"; fail=1; fi
done
exit $fail
