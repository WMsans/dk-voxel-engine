// dk_voxel core — no Godot dependency (see selftest.cpp).
//
// Technique (see ../README.md for where each piece was observed):
//   * sparse 8^3 blocks of voxels: u8 density (iso 127.5, band = +-1 cell) + u8 material
//   * optional Hermite data per cell: 3 edges x 3 bytes [t:7|sign:1, nx, ny]
//   * 16^3-cell mesh chunks, dual contouring: one QEF vertex per surface cell
//     (Jacobi eigen-solve, mass point, eigenvalues < 0.1 truncated, clamped to the cell)
//   * LOD = octree collapse of cells whose merged QEF error is under a threshold
//   * chunks live in a map keyed by chunk coordinate: a bounded volume loads them all, an endless one
//     fills and drops them as viewers move
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace dkv {

struct Vec3 {
	float x = 0, y = 0, z = 0;
	Vec3() = default;
	Vec3(float x, float y, float z) : x(x), y(y), z(z) {}
	Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
	Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
	Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
	float dot(Vec3 o) const { return x * o.x + y * o.y + z * o.z; }
	Vec3 cross(Vec3 o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
	float len() const { return std::sqrt(dot(*this)); }
	Vec3 norm() const { float l = len(); return l > 1e-12f ? *this * (1.0f / l) : Vec3(0, 1, 0); }
	float &operator[](int i) { return i == 0 ? x : i == 1 ? y : z; }
	float operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
};

constexpr int BLOCK = 8; // voxels per data-block edge
constexpr int CHUNK = 16; // cells per mesh-chunk edge
constexpr int UNBREAKABLE = 255;

// Density helpers shared by the editor and the demo generator.
inline uint8_t to_u8(float f) { return (uint8_t)std::clamp((int)std::lround(f), 0, 255); }
// Signed distance in cells -> density. The band is +-1 cell, like the shipped SDF grids.
inline uint8_t sdf_to_density(float cells) { return to_u8(127.5f - cells * 127.5f); }

// Floor division, correct for negative operands (chunk of a voxel, chunk of an edit bound).
inline int floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

struct Voxel {
	uint8_t d = 0, m = 0; // density: 0 air .. 255 solid; material id
	bool operator==(Voxel o) const { return d == o.d && m == o.m; }
};

struct Block {
	Voxel v[512];
	std::unique_ptr<uint8_t[]> hermite; // 512 * 9, allocated on first sharp edit
};

struct ChunkKey {
	int x = 0, y = 0, z = 0;
	ChunkKey() = default;
	ChunkKey(int x, int y, int z) : x(x), y(y), z(z) {}
	bool operator==(const ChunkKey &o) const { return x == o.x && y == o.y && z == o.z; }
};

struct ChunkKeyHash {
	size_t operator()(const ChunkKey &k) const {
		uint64_t h = (uint32_t)k.x * 0x9e3779b97f4a7c15ull ^ (uint32_t)k.y * 0xc2b2ae3d27d4eb4full ^ (uint32_t)k.z * 0x165667b19e3779f9ull;
		h ^= h >> 29; h *= 0xbf58476d1ce4e5b9ull; h ^= h >> 32;
		return (size_t)h;
	}
};

struct MeshData {
	std::vector<float> pos, nrm; // xyz relative to the chunk origin, in world units
	std::vector<uint8_t> col; // r = material A, g = material B, b = blend A->B, a = ambient occlusion
	std::vector<int32_t> idx; // clockwise front faces (Godot convention)
	size_t skirt_from = SIZE_MAX; // idx from here on are skirts (add_skirts): drawn, never collided with
	void clear() { pos.clear(); nrm.clear(); col.clear(); idx.clear(); skirt_from = SIZE_MAX; }
};

// Folds every open edge of a chunk mesh (its border with the next chunk) `depth` into the surface, so a
// neighbour meshed at another resolution meets a strip instead of a crack. voxel_mesher.cpp.
void add_skirts(MeshData &m, float depth);

enum Shape { SPHERE, BOX, CYLINDER, MESH }; // MESH samples an SdfGrid (sdf_grid.h)
enum Mode { ERASE, BUILD, SMOOTH };

struct EditResult {
	std::array<float, 256> removed{}; // solid volume removed per material, in voxels
	int cmin[3] = {0, 0, 0}, cmax[3] = {-1, -1, -1}; // inclusive range of chunks to remesh
	bool changed = false;
};

struct RayHit {
	bool hit = false;
	Vec3 pos, normal;
	uint8_t material = 0;
};

} // namespace dkv
