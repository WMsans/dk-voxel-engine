#!/usr/bin/env bash
# Organizes one generated material folder (file names like brick_albedo_00002_.png) into the layout the
# demo uses: <out>/<name>/<name>_<map>.png, TEXTURE_ARRAY_SIZE RGBA, newest numbered version of each map.
#
#   addons/dk_voxel/tools/organize_material.sh ~/Pictures/slops/terrain/brick
#   addons/dk_voxel/tools/organize_material.sh ~/Pictures/slops/terrain/grass /some/other/textures slop_grass
#
# NAME defaults to the source folder's name; pass one when the material needs a different folder. Vol2-sourced
# materials come from demo/textures/make_textures.py instead.
#
# Output defaults to demo/textures. albedo, normal, roughness and ao are required; height is copied when
# present (the engine does not sample it yet). The texture/preview/junctions composites and the metalness
# map are skipped: VoxelMaterialDef has a scalar metallic, not a map.
set -euo pipefail

src=${1:?usage: $0 SOURCE_DIR [OUTPUT_TEXTURES_DIR] [NAME]}
out=${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/demo/textures}
size=1024 # must match TEXTURE_ARRAY_SIZE in addons/dk_voxel/src/core/voxel_materials.cpp
srcname=$(basename "${src%/}")
name=${3:-$srcname}

[ -d "$src" ] || { echo "no such folder: $src" >&2; exit 1; }
magick=$(command -v magick || command -v convert) || { echo "ImageMagick not found" >&2; exit 1; }

# Newest <src>_<map>_<number>_.png, or an already organized <src>_<map>.png; empty when absent.
newest() {
	local map=$1 f n best="" best_n=-1
	for f in "$src/${srcname}_${map}_"*.png "$src/${srcname}_${map}.png"; do
		[ -f "$f" ] || continue
		n=${f##*/}; n=${n%.png}; n=${n%_}; n=${n##*_}
		[[ $n =~ ^[0-9]+$ ]] || n=0
		if [ "$n" -gt "$best_n" ]; then best_n=$n; best=$f; fi
	done
	printf '%s' "$best"
}

mkdir -p "$out/$name"
for map in albedo normal roughness ao height; do
	f=$(newest "$map")
	if [ -z "$f" ] && [ "$map" != height ]; then
		echo "missing ${srcname}_${map} in $src" >&2
		exit 1
	fi
	[ -n "$f" ] || continue
	dest="$out/$name/${name}_${map}.png"
	"$magick" "$f" -resize "${size}x${size}!" -alpha set -colorspace sRGB -strip PNG32:"$dest"
	[ "$("$magick" identify -format '%wx%h' "$dest")" = "${size}x${size}" ] || { echo "resize failed: $dest" >&2; exit 1; }
	echo "$(basename "$f") -> ${dest#"$PWD"/}"
done
