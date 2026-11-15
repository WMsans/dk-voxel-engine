// dk_voxel core — no Godot dependency (see selftest.cpp).
//
// Technique (see ../README.md for where each piece was observed):
//   * sparse 8^3 blocks of voxels: u8 density (iso 127.5, band = +-1 cell) + u8 material
//   * optional Hermite data per cell: 3 edges x 3 bytes [t:7|sign:1, nx, ny]
//   * 16^3-cell mesh chunks, dual contouring: one QEF vertex per surface cell
//     (Jacobi eigen-solve, mass point, eigenvalues < 0.1 truncated, clamped to the cell)
//   * LOD = octree collapse of cells whose merged QEF error is under a threshold
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <shared_mutex>
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

struct Voxel {
	uint8_t d = 0, m = 0; // density: 0 air .. 255 solid; material id
	bool operator==(Voxel o) const { return d == o.d && m == o.m; }
};

struct Block {
	Voxel v[512];
	std::unique_ptr<uint8_t[]> hermite; // 512 * 9, allocated on first sharp edit
};

struct MeshData {
	std::vector<float> pos, nrm; // xyz in volume space (world units from the volume origin)
	std::vector<uint8_t> col; // r = material A, g = material B, b = blend A->B, a = ambient occlusion
	std::vector<int32_t> idx; // clockwise front faces (Godot convention)
	void clear() { pos.clear(); nrm.clear(); col.clear(); idx.clear(); }
};

enum Shape { SPHERE, BOX, CYLINDER };
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

class Volume {
public:
	Volume(int chunks_x, int chunks_y, int chunks_z, float cell_size);

	const int cx, cy, cz; // size in chunks
	const int nx, ny, nz; // size in voxels
	const float cell;
	float lod_error[3] = {0.0f, 0.02f, 0.12f}; // QEF error budget per LOD (cells^2)
	uint8_t hardness[256] = {}; // erase needs power >= hardness

	// Built-in terrain (hills, caves, ore, bedrock). Blocking; runs on `threads` threads.
	void generate(uint32_t seed, int threads);

	// center/half in world units; basis = 3x3 rotation, shape-local axes in columns. half.x = radius for
	// SPHERE and CYLINDER (half.y = cylinder half height). strength scales SMOOTH. sharp writes Hermite data.
	EditResult edit(Shape s, Mode m, Vec3 center, Vec3 half, const float basis[9], uint8_t material, int power, float strength, bool sharp);

	// Thread-safe against edit(). Returns false (and leaves out empty) when the chunk has no surface.
	bool mesh_chunk(int x, int y, int z, int lod, MeshData &out) const;

	RayHit raycast(Vec3 origin, Vec3 dir, float max_dist) const;
	float density(Vec3 p) const; // trilinear, 0..255
	Voxel get(int x, int y, int z) const {
		// Sample 0 on each axis reads as air too, so the world is closed on every side by edges a chunk owns.
		if ((unsigned)(x - 1) >= (unsigned)(nx - 1) || (unsigned)(y - 1) >= (unsigned)(ny - 1) || (unsigned)(z - 1) >= (unsigned)(nz - 1)) return {};
		size_t b = bidx(x >> 3, y >> 3, z >> 3);
		const Block *p = blocks[b].get();
		return p ? p->v[vidx(x, y, z)] : uniform[b];
	}

private:
	const int bx, by, bz;
	std::vector<std::unique_ptr<Block>> blocks; // null = uniform block
	std::vector<Voxel> uniform;
	mutable std::shared_mutex mtx;

	size_t bidx(int x, int y, int z) const { return ((size_t)z * by + y) * bx + x; }
	static int vidx(int x, int y, int z) { return (z & 7) << 6 | (y & 7) << 3 | (x & 7); }
	Voxel *writable(int x, int y, int z);
	uint8_t *hermite_at(int x, int y, int z, bool create);
};

} // namespace dkv
