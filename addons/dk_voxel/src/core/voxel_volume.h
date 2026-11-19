// Chunk storage, edits, queries and per-chunk serialisation. Godot-free (see selftest.cpp).
#pragma once
#include "sdf_grid.h"
#include "voxel_generator.h"

#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace dkv {

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
	// Bananza's InnerMaterial: an erase rewrites every voxel it carves into to inner[m], so a dug surface shows
	// its broken inside (grass -> torn turf, rock -> broken rock). Identity = no change. Chains (a -> b -> c)
	// advance one step per dig over the same voxels.
	uint8_t inner[256];
	bool expose_inner = true; // false while authoring (ruins, level dressing): carve without the broken look

	bool in_bounds(int cx, int cy, int cz) const; // always true when endless
	bool has_chunk(int cx, int cy, int cz) const;
	bool neighbors_ready(int cx, int cy, int cz) const; // chunk + 26 neighbours present, or outside bounds
	std::vector<ChunkKey> loaded_chunks() const;
	size_t chunk_count() const;

	// Serialise one chunk: u8 format = 1, then per block u8 kind (0 uniform, 1 dense, 2 dense + Hermite)
	// and the uniform voxel (2 bytes) or 1024 voxel bytes or 1024 + 512*9 Hermite bytes. Empty if missing.
	std::vector<uint8_t> serialize_chunk(int cx, int cy, int cz) const;
	bool deserialize_chunk(int cx, int cy, int cz, const uint8_t *data, size_t size); // false on bad data
	void remove_chunks(const std::vector<ChunkKey> &keys); // one lock for all: a writer waits for every meshing worker
	bool is_modified(int cx, int cy, int cz) const;
	void clear_modified(int cx, int cy, int cz);

	// Fill (or replace) one chunk: g == nullptr = air. Thread-safe: generates without the lock, inserts
	// under the unique lock.
	void fill_chunk(int cx, int cy, int cz, const Generator *g);
	// Fill (or replace) every chunk in the bounds on `threads` threads. Endless volumes have no bounds.
	void fill_all(const Generator *g, int threads);

	// center/half in world units; basis = 3x3 rotation, shape-local axes in columns. half.x = radius for
	// SPHERE and CYLINDER (half.y = cylinder half height). strength scales SMOOTH. sharp writes Hermite data.
	// MESH reads `grid` in volume coordinates and ignores the basis; center/half = the grid's box.
	EditResult edit(Shape s, Mode m, Vec3 center, Vec3 half, const float basis[9], uint8_t material, int power, float strength, bool sharp, const SdfGrid *grid = nullptr);

	// Thread-safe against edit(). Returns false (and leaves out empty) when the chunk has no surface.
	bool mesh_chunk(int x, int y, int z, int lod, MeshData &out) const;

	Vec3 chunk_origin(int cx, int cy, int cz) const; // metres: (c * CHUNK) * cell
	RayHit raycast(Vec3 origin, Vec3 dir, float max_dist) const; // locks mtx for the whole march
	float density(Vec3 p) const; // trilinear, 0..255, locks mtx
	Voxel get(int x, int y, int z) const; // missing chunk or outside bounds = air, locked against fill_chunk

private:
	struct Chunk; // defined in voxel_volume.cpp
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
