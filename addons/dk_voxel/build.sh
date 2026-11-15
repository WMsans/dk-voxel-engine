#!/usr/bin/env bash
# build.sh — build the dk_voxel GDExtension (registers the VoxelWorld node).
#   ./build.sh            template_debug library (the one the editor loads)
#   ./build.sh --release  also template_release
#   ./build.sh --test     also build & run the core self-test (no Godot needed)
#   ./build.sh -j N       job count (default: nproc)
set -euo pipefail
ADDON="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JOBS="$(nproc 2>/dev/null || echo 4)"
RELEASE=0; TEST=0
while [ $# -gt 0 ]; do
	case "$1" in
		--release) RELEASE=1 ;;
		--test) TEST=1 ;;
		-j) shift; JOBS="$1" ;;
		-j*) JOBS="${1#-j}" ;;
		*) echo "build.sh: unknown argument: $1" >&2; exit 2 ;;
	esac
	shift
done
case "$(uname -m)" in x86_64|amd64) ARCH=x86_64 ;; arm64|aarch64) ARCH=arm64 ;; *) echo "unsupported arch" >&2; exit 1 ;; esac
case "$(uname -s)" in Linux) PLATFORM=linux ;; Darwin) PLATFORM=macos ;; *) echo "unsupported OS" >&2; exit 1 ;; esac

# godot-cpp, pinned to the 4.7 API commit. Cloned on first build.
TP="$ADDON/thirdparty"
if [ ! -f "$TP/godot-cpp/SConstruct" ]; then
	mkdir -p "$TP"; : > "$TP/.gdignore"
	git clone https://github.com/godotengine/godot-cpp.git "$TP/godot-cpp"
	git -C "$TP/godot-cpp" checkout 507ed9d840c01a3c5b2a39af8bb4000bfac30bf5
fi

cd "$ADDON"
scons -j"$JOBS" platform=$PLATFORM arch=$ARCH target=template_debug
[ $RELEASE -eq 1 ] && scons -j"$JOBS" platform=$PLATFORM arch=$ARCH target=template_release
if [ $TEST -eq 1 ]; then scons -j"$JOBS" selftest && build/selftest; fi
echo "==> Done."
