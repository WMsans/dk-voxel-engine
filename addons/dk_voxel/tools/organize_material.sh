#!/usr/bin/env bash
# Organizes one generated material folder (file names like brick_albedo_00002_.png) into the layout the
# demo uses: <out>/<name>/<name>_<map>.png, 256x256 RGBA, newest numbered version of each map.
#
#   addons/dk_voxel/tools/organize_material.sh ~/Pictures/slops/terrain/brick
#   addons/dk_voxel/tools/organize_material.sh ~/Pictures/slops/terrain/brick /some/other/textures
#
# Output defaults to demo/textures. albedo, normal, roughness and ao are required; height is copied when
# present (the engine does not sample it yet). The texture/preview/junctions composites and the metalness
# map are skipped: VoxelMaterialDef has a scalar metallic, not a map.
set -euo pipefail

src=${1:?usage: $0 SOURCE_DIR [OUTPUT_TEXTURES_DIR]}
out=${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/demo/textures}
size=256
name=$(basename "${src%/}")

[ -d "$src" ] || { echo "no such folder: $src" >&2; exit 1; }
magick=$(command -v magick || command -v convert) || { echo "ImageMagick not found" >&2; exit 1; }

# Newest <name>_<map>_<number>_.png, or an already organized <name>_<map>.png; empty when absent.
newest() {
	local map=$1 f n best="" best_n=-1
	for f in "$src/${name}_${map}_"*.png "$src/${name}_${map}.png"; do
		[ -f "$f" ] || continue
		n=${f##*/}; n=${n%.png}; n=${n%_}; n=${n##*_}
		[[ $n =~ ^[0-9]+$ ]] || n=0
		if [ "$n" -gt "$best_n" ]; then best_n=$n; best=$f; fi
	done
	printf '%s' "$best"
}

declare -A picks
for map in albedo normal roughness ao height; do
	picks[$map]=$(newest "$map")
	if [ -z "${picks[$map]}" ] && [ "$map" != height ]; then
		echo "missing ${name}_${map} in $src" >&2
		exit 1
	fi
done

mkdir -p "$out/$name"
for map in albedo normal roughness ao height; do
	f=${picks[$map]}
	[ -n "$f" ] || continue
	dest="$out/$name/${name}_${map}.png"
	"$magick" "$f" -resize "${size}x${size}!" -alpha set -colorspace sRGB -strip PNG32:"$dest"
	[ "$("$magick" identify -format '%wx%h' "$dest")" = "${size}x${size}" ] || { echo "resize failed: $dest" >&2; exit 1; }
	echo "$(basename "$f") -> ${dest#"$PWD"/}"
done
