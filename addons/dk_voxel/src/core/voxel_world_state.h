// The VoxelWorld node's data, in one place. Everything the node schedules, caches or uploads lives here, so
// voxel_world.h can stay a thin public API and the .cpp files can work on one state object.
#pragma once
#include "voxel_volume.h"

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/texture2d_array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector3i.hpp>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace godot;

class VoxelGenerator;
class VoxelMaterialSet;
class VoxelWorld;

namespace dkv {

struct WorldState {
	// A chunk the node tracks: its render/physics RIDs, the version its mesh was built from, and its LOD.
	struct Chunk {
		ChunkKey key;
		RID mesh, instance, body, shape;
		Vector3 offset; // chunk origin in metres, relative to the world node (mesh vertices are chunk-relative)
		int8_t lod = -1; // LOD on screen, -1 = never meshed
		bool collision = false; // the mesh on screen was built with collision
		bool queued = false;
		bool gen_queued = false; // endless: a generate job for this chunk is on the worker queue
		bool visible = true; // the instance's visibility, as last pushed to the RenderingServer
		uint32_t version = 1, queued_version = 0, applied_version = 0; // version bumps on every edit
		int32_t tris = 0;
	};
	// One viewer driving LOD, collision and (endless) streaming, in the world's local space.
	struct Viewer {
		Vector3 pos;
		Vector3i chunk = Vector3i(INT32_MAX, 0, 0);
		float view_distance = 160.0f;
		float vertical_view_distance = 96.0f;
		float collision_distance = 40.0f;
	};
	// An unloaded edited chunk, kept in memory until something generates it again.
	struct StoredChunk {
		uint32_t raw_size = 0;
		PackedByteArray data;
	};
	struct Job {
		uint64_t gen = 0; // the world generation that queued it; results from an older one are dropped
		bool is_gen = false; // endless: fill/restore a chunk instead of meshing it
		int8_t level = 0; // 0 = the near volume, k = far[k - 1]
		ChunkKey key;
		PackedByteArray stored;
		uint32_t stored_size = 0;
		String save_dir, gen_class; // snapshot at queue time: workers must not read the mutable members
		float prio = 0;
		int32_t index = -1;
		int8_t lod = 0;
		bool collision = false, urgent = false;
		uint32_t version = 0;
		bool operator<(const Job &o) const { return prio > o.prio; }
	};
	struct Result {
		Job job;
		bool has;
		MeshData mesh;
		double ms;
	};

	std::unique_ptr<Volume> vol;
	std::vector<Chunk> chunks; // bounded: one per chunk in the bounds, indexed as in the Volume
	std::unordered_map<ChunkKey, int32_t, ChunkKeyHash> chunk_lookup; // endless: key -> index in chunks
	std::unordered_map<ChunkKey, StoredChunk, ChunkKeyHash> store; // endless: unloaded edits
	String save_directory; // empty = edits never reach disk; the unload store differs, the path is the same
	String generator_class; // `generator`'s class name in save headers, empty when there is none
	std::vector<Viewer> active_viewers;
	// Far field (endless only), the way voxy and VoxelLodTerrain do it: level k = 1..far_levels is a coarse copy of the
	// world, cell * 2^k, filled straight from the generator and reaching view_distance * 2^k. A level-k chunk covers
	// exactly 2x2x2 level-(k-1) chunks and hands over to them once all eight are meshed (see update_visibility), so
	// each level costs about the same and the view distance doubles per level. No collision, saves or raycasts.
	struct FarLevel {
		std::unique_ptr<Volume> vol;
		std::unordered_map<ChunkKey, Chunk, ChunkKeyHash> chunks;
		std::vector<Vector3i> streamed_at; // the viewers' chunks at this level when it was last streamed
	};
	std::vector<FarLevel> far; // far[k - 1] = level k
	int far_levels = 6;
	uint32_t need_vis = ~0u; // bit k: level k's visibility may have changed (update_visibility)
	std::shared_ptr<const Generator> active_generator; // rebuilt by ensure_volume(), read by the workers
	Ref<Material> material;
	Ref<VoxelGenerator> generator;
	Ref<VoxelMaterialSet> material_set;
	float cell_size = 0.5f;
	Vector3i bounds_size = Vector3i(0, 0, 0); // chunks from the origin; zero = endless
	int threads = 0; // 0 = cores - 2, min 1
	float lod1_distance = 90, lod2_distance = 170, collision_distance = 40;
	uint32_t collision_layer = 1;
	bool expose_inner_material = true;
	uint64_t material_fingerprint = 0; // the texture identity the packed arrays were built from
	RID material_pushed; // the material the arrays were last pushed to
	// The packed arrays live here, not in the material: the RenderingServer only holds their RIDs, and keeping them out
	// of the ShaderMaterial's properties keeps the generated data out of any scene or .tres the material is saved in.
	Ref<Texture2DArray> material_arrays[4];

	std::priority_queue<Job> jobs;
	std::vector<Result> results;
	std::deque<Result> pending;
	std::mutex qm;
	std::condition_variable qcv;
	std::vector<std::thread> workers;
	bool quit = false;

	uint64_t generation = 0; // bumped by reset(): results queued before it are never applied
	bool need_scan = true;
	bool need_stream = true; // endless: viewers moved, so load/unload; a finished job alone only needs the mesh pass
	int64_t stat_tris = 0;
	int stat_meshes = 0, stat_bodies = 0, stat_jobs = 0;
	double stat_ms = 0;

	int thread_count() const { return threads > 0 ? threads : MAX(1, (int)std::thread::hardware_concurrency() - 2); }
	// The wanted LOD for a distance, the one definition scan() and is_area_ready() share.
	int lod_at(float d) const { return d < lod1_distance ? 0 : d < lod2_distance ? 1 : 2; }

	// Chunk index <-> chunk coordinate, for the bounded world ensure_volume() builds.
	int ncx() const { return vol->bmax[0] - vol->bmin[0]; }
	int ncy() const { return vol->bmax[1] - vol->bmin[1]; }
	int ncz() const { return vol->bmax[2] - vol->bmin[2]; }
	int chunk_index(int x, int y, int z) const { return ((int)((size_t)(z - vol->bmin[2]) * ncy() + (y - vol->bmin[1])) * ncx() + (x - vol->bmin[0])); }
	Transform3D chunk_transform(const Transform3D &gx, const Chunk &c) const { return gx * Transform3D(Basis(), c.offset); }
};

// voxel_chunks.cpp: chunk RID lifecycle.
void free_chunk(WorldState &st, WorldState::Chunk &c, bool body_only = false);
void upload_chunk(WorldState &st, VoxelWorld &world, WorldState::Chunk &c, const MeshData &m, const Transform3D &xf, bool collision);

// voxel_stream.cpp: viewers, streaming, LOD scanning and the worker pool.
void update_viewers(WorldState &st, VoxelWorld &world);
void scan(WorldState &st);
void stream_endless(WorldState &st);
void worker(WorldState &st);
void start_workers(WorldState &st);
void stop_workers(WorldState &st);
// voxel_far.cpp: the far-field levels and which level's chunk is on screen.
void create_far(WorldState &st);
void free_far(WorldState &st);
void stream_far(WorldState &st);
void queue_far_mesh(WorldState &st, int k, const ChunkKey &key, std::vector<WorldState::Job> &out);
void push_jobs(WorldState &st, std::vector<WorldState::Job> &jobs);
void apply_far(WorldState &st, VoxelWorld &world, const Transform3D &gx, WorldState::Result &r);
void update_visibility(WorldState &st, bool world_visible);

// voxel_save.cpp: one file per modified chunk, the in-memory store, and the flush.
String chunk_file_path(const ChunkKey &key, const String &dir);
bool write_chunk_file(WorldState &st, const ChunkKey &key, const uint8_t *data, size_t size);
bool read_chunk_file(WorldState &st, const ChunkKey &key, const String &dir, const String &gen_class, std::vector<uint8_t> &out);
void store_chunk(WorldState &st, const ChunkKey &key, bool try_disk = true);
Error save_world(WorldState &st);

} // namespace dkv
