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
#include <shared_mutex>
#include <unordered_map>
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
	void clear() { pos.clear(); nrm.clear(); col.clear(); idx.clear(); }
};

enum Shape { SPHERE, BOX, CYLINDER };
enum Mode { ERASE, BUILD, SMOOTH };

struct Generator {
	virtual ~Generator() = default;
	// origin = the block's cell coordinates; index = vidx(x & 7, y & 7, z & 7). Must be const, thread-safe
	// and deterministic in (settings, origin): an unloaded chunk is regenerated exactly.
	virtual void generate_block(const int origin[3], float cell, Voxel out[512]) const = 0;
};

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

class Volume {
public:
	explicit Volume(float cell_size); // endless: no chunk bounds, starts empty
	Volume(int chunks_x, int chunks_y, int chunks_z, float cell_size); // bounded [0, n) in chunks, all air

	~Volume();

	const float cell;
	const bool bounded;
	const int bmin[3], bmax[3]; // chunk bounds, exclusive max; unused when endless
	float lod_error[3] = {0.0f, 0.02f, 0.12f}; // QEF error budget per LOD (cells^2)
	uint8_t hardness[256] = {}; // erase needs power >= hardness

	bool in_bounds(int cx, int cy, int cz) const; // always true when endless
	bool has_chunk(int cx, int cy, int cz) const;
	bool neighbors_ready(int cx, int cy, int cz) const; // chunk + 26 neighbours present, or outside bounds
	std::vector<ChunkKey> loaded_chunks() const;
	size_t chunk_count() const;

	// Serialise one chunk: u8 format = 1, then per block u8 kind (0 uniform, 1 dense, 2 dense + Hermite)
	// and the uniform voxel (2 bytes) or 1024 voxel bytes or 1024 + 512*9 Hermite bytes. Empty if missing.
	std::vector<uint8_t> serialize_chunk(int cx, int cy, int cz) const;
	bool deserialize_chunk(int cx, int cy, int cz, const uint8_t *data, size_t size); // false on bad data
	void remove_chunk(int cx, int cy, int cz);
	bool is_modified(int cx, int cy, int cz) const;
	void clear_modified(int cx, int cy, int cz);

	// Fill (or replace) one chunk: g == nullptr = air. Thread-safe: generates without the lock, inserts
	// under the unique lock.
	void fill_chunk(int cx, int cy, int cz, const Generator *g);
	// Fill (or replace) every chunk in the bounds on `threads` threads. Endless volumes have no bounds.
	void fill_all(const Generator *g, int threads);

	// center/half in world units; basis = 3x3 rotation, shape-local axes in columns. half.x = radius for
	// SPHERE and CYLINDER (half.y = cylinder half height). strength scales SMOOTH. sharp writes Hermite data.
	EditResult edit(Shape s, Mode m, Vec3 center, Vec3 half, const float basis[9], uint8_t material, int power, float strength, bool sharp);

	// Thread-safe against edit(). Returns false (and leaves out empty) when the chunk has no surface.
	bool mesh_chunk(int x, int y, int z, int lod, MeshData &out) const;

	Vec3 chunk_origin(int cx, int cy, int cz) const; // metres: (c * CHUNK) * cell
	RayHit raycast(Vec3 origin, Vec3 dir, float max_dist) const; // locks mtx for the whole march
	float density(Vec3 p) const; // trilinear, 0..255, locks mtx
	Voxel get(int x, int y, int z) const; // missing chunk or outside bounds = air, locked against fill_chunk

private:
	struct Chunk; // defined in volume.cpp
	std::unordered_map<ChunkKey, std::unique_ptr<Chunk>, ChunkKeyHash> chunks;
	mutable std::shared_mutex mtx; // ponytail: one global lock; per-chunk locks if workers contend.

	// The unlocked queries: the caller holds mtx (shared or unique), so a traversal of many voxels
	// locks once instead of per voxel. Workers insert under the unique lock, so nothing may read the
	// chunk map without it.
	Voxel get_unlocked(int x, int y, int z) const;
	float density_unlocked(Vec3 p) const;
	RayHit raycast_unlocked(Vec3 origin, Vec3 dir, float max_dist) const;

	// Callers must hold mtx. Null when the chunk is missing.
	Chunk *chunk_at(int cx, int cy, int cz);
	const Chunk *chunk_at(int cx, int cy, int cz) const;
	// The closed-world rule: on a bounded volume the outer half-voxel reads as air, so the surface closes
	// on every side. Always true when endless.
	bool in_world(int x, int y, int z) const;
	// Resolves a block: null when its chunk is missing, and uniform is a uniform block's value.
	const Block *block_at(int bx, int by, int bz, Voxel &uniform) const;
	Voxel *writable(int x, int y, int z);
	uint8_t *hermite_at(int x, int y, int z, bool create);
	static int vidx(int x, int y, int z) { return (z & 7) << 6 | (y & 7) << 3 | (x & 7); }
};

} // namespace dkv
