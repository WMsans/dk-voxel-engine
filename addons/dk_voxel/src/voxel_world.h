// VoxelWorld: Godot node around dkv::Volume. Worker threads mesh 16^3-cell chunks nearest-first; the main
// thread uploads results to the RenderingServer and (near the viewer) to static trimesh bodies.
#pragma once
#include "core/voxel_volume.h"
#include "voxel_resources.h"

#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace godot;

class VoxelWorld : public Node3D {
	GDCLASS(VoxelWorld, Node3D)

	struct Chunk {
		dkv::ChunkKey key;
		RID mesh, instance, body, shape;
		Vector3 offset; // chunk origin in metres, relative to the world node (mesh vertices are chunk-relative)
		int8_t lod = -1; // LOD on screen, -1 = never meshed
		bool collision = false; // the mesh on screen was built with collision
		bool queued = false;
		bool gen_queued = false; // endless: a generate job for this chunk is on the worker queue
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
	// An unloaded edited chunk, kept in memory until something generates it again (Task 6 writes it to disk).
	struct StoredChunk {
		uint32_t raw_size = 0;
		PackedByteArray data;
	};
	struct Job {
		uint64_t gen = 0; // the world generation that queued it; results from an older one are dropped
		bool is_gen = false; // endless: fill/restore a chunk instead of meshing it
		dkv::ChunkKey key;
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
		dkv::MeshData mesh;
		double ms;
	};

	std::unique_ptr<dkv::Volume> vol;
	std::vector<Chunk> chunks; // bounded: one per chunk in the bounds, indexed as in the Volume
	std::unordered_map<dkv::ChunkKey, int32_t, dkv::ChunkKeyHash> chunk_lookup; // endless: key -> index in chunks
	std::unordered_map<dkv::ChunkKey, StoredChunk, dkv::ChunkKeyHash> store; // endless: unloaded edits
	String save_directory; // empty = edits never reach disk; the unload store differs, the path is the same
	String generator_class; // `generator`'s class name in save headers, empty when there is none
	std::vector<Viewer> active_viewers;
	std::shared_ptr<const dkv::Generator> active_generator; // rebuilt by ensure_volume(), read by the workers
	Ref<Material> material;
	Ref<VoxelGenerator> generator;
	Ref<VoxelMaterialSet> material_set;
	float cell_size = 0.5f;
	Vector3i bounds_size = Vector3i(0, 0, 0); // chunks from the origin; zero = endless
	int threads = 0; // 0 = cores - 2, min 1
	float lod1_distance = 90, lod2_distance = 170, collision_distance = 40;
	uint32_t collision_layer = 1;

	std::priority_queue<Job> jobs;
	std::vector<Result> results;
	std::deque<Result> pending;
	std::mutex qm;
	std::condition_variable qcv;
	std::vector<std::thread> workers;
	bool quit = false;

	uint64_t generation = 0; // bumped by reset(): results queued before it are never applied
	bool need_scan = true;
	int64_t stat_tris = 0;
	int stat_meshes = 0, stat_bodies = 0, stat_jobs = 0;
	double stat_ms = 0;

	// Edits and queries are legal before the node enters the tree (e.g. while a level is being built).
	Transform3D gx() const { return is_inside_tree() ? get_global_transform() : get_transform(); }

	// Chunk index <-> chunk coordinate, for the bounded world ensure_volume() builds.
	int ncx() const { return vol->bmax[0] - vol->bmin[0]; }
	int ncy() const { return vol->bmax[1] - vol->bmin[1]; }
	int ncz() const { return vol->bmax[2] - vol->bmin[2]; }
	int chunk_index(int x, int y, int z) const { return ((int)((size_t)(z - vol->bmin[2]) * ncy() + (y - vol->bmin[1])) * ncx() + (x - vol->bmin[0])); }
	Transform3D chunk_transform(Chunk &c) const { return gx() * Transform3D(Basis(), c.offset); }

	// Builds the Volume from the properties on first use: first edit, query or _ready. Bounded worlds are
	// generated synchronously; endless worlds start empty (streaming arrives in Task 5).
	void ensure_volume();
	// Def for a material id: entry `id` if the set has one, else entry 0 (a non-empty set), else a default grey zero-hardness def.
	Ref<VoxelMaterialDef> material_def(int id) const;
	void apply_hardness();
	void push_material_uniforms();
	int thread_count() const { return threads > 0 ? threads : MAX(1, (int)std::thread::hardware_concurrency() - 2); }
	// The wanted LOD for a distance, the one definition scan() and is_area_ready() share.
	int lod_at(float d) const { return d < lod1_distance ? 0 : d < lod2_distance ? 1 : 2; }
	void start_workers();
	void stop_workers();

	// Collects the VoxelViewers that share our World3D (the active camera when there is none).
	void update_viewers();
	Chunk *find_chunk(int cx, int cy, int cz);
	void worker();
	void free_chunk(Chunk &c, bool body_only = false);

	// Endless streaming: unload what no viewer wants (keeping edits in `store`), then queue generate
	// jobs for the wanted set. Bounded worlds keep every chunk and never call this.
	void stream_endless();
	// Keeps an edited chunk that is about to be unloaded: on disk when save_directory is set, else (or when
	// the write fails) compressed in `store`. `try_disk` is false when a write was just attempted and failed.
	void store_chunk(const dkv::ChunkKey &key, bool try_disk = true);

	// One zstd file per modified chunk: `<dir>/<x>_<y>_<z>.dkvc` (the directory is passed in: workers read a
	// snapshot taken at queue time). read_chunk_file() is false when there is no file or its header does not
	// match this world (it pushes an error in that case).
	String chunk_file_path(const dkv::ChunkKey &key, const String &dir) const;
	bool write_chunk_file(const dkv::ChunkKey &key, const uint8_t *data, size_t size); // main thread only
	bool read_chunk_file(const dkv::ChunkKey &key, const String &dir, const String &gen_class, std::vector<uint8_t> &out);

	// Pick each chunk's LOD from its distance to the viewer and queue whatever is out of date.
	void scan();

	void apply(Result &r);

	Dictionary do_edit(dkv::Shape s, dkv::Mode m, const Transform3D &xf, Vector3 half, int mat, int power, float strength, bool sharp);

protected:
	static void _bind_methods();

	void _notification(int what);

public:
	VoxelWorld();
	~VoxelWorld();

	void _ready() override;
	void _process(double) override;

	// Rebuild the volume from the current properties: applies the generator and the material set again.
	void reset();

	// Writes every modified loaded chunk and every unloaded stored edit to save_directory, then clears the
	// modified flags. A no-op returning OK when save_directory is empty.
	Error save();

	Dictionary dig_sphere(Vector3 c, float r, int power);
	Dictionary build_sphere(Vector3 c, float r, int mat);
	Dictionary smooth_sphere(Vector3 c, float r, float strength, int power);
	Dictionary build_box(const Transform3D &xf, Vector3 half, int mat, bool sharp);
	Dictionary dig_box(const Transform3D &xf, Vector3 half, int power, bool sharp);
	Dictionary build_cylinder(const Transform3D &xf, float r, float hh, int mat, bool sharp);
	Dictionary dig_cylinder(const Transform3D &xf, float r, float hh, int power, bool sharp);

	// Marches the voxel field itself, so it works before (and beyond where) collision exists.
	Dictionary raycast(Vector3 origin, Vector3 dir, float max_dist);
	float get_density(Vector3 p);
	// True when every chunk under `aabb` (global) is loaded and meshed at its wanted LOD, and, with
	// `need_collision`, has a body unless it has no surface.
	bool is_area_ready(const AABB &aabb, bool need_collision = true);
	void set_lod_error(int lod, float e);
	Vector3 get_world_size() const { return vol ? Vector3(vol->bmax[0] - vol->bmin[0], vol->bmax[1] - vol->bmin[1], vol->bmax[2] - vol->bmin[2]) * (dkv::CHUNK * vol->cell) : Vector3(); }
	int get_pending_count();
	Dictionary get_stats();

	void set_material(const Ref<Material> &m);
	Ref<Material> get_material() const { return material; }
	void set_generator(const Ref<VoxelGenerator> &g);
	Ref<VoxelGenerator> get_generator() const { return generator; }
	void set_save_directory(const String &v) { save_directory = v; }
	String get_save_directory() const { return save_directory; }
	void set_material_set(const Ref<VoxelMaterialSet> &s);
	Ref<VoxelMaterialSet> get_material_set() const { return material_set; }
	void set_cell_size(float v) { cell_size = v; }
	float get_cell_size() const { return cell_size; }
	void set_bounds_size(const Vector3i &v) { bounds_size = v; }
	Vector3i get_bounds_size() const { return bounds_size; }
	void set_threads(int v) { threads = v; }
	int get_threads() const { return threads; }
	void set_lod1_distance(float v);
	float get_lod1_distance() const { return lod1_distance; }
	void set_lod2_distance(float v);
	float get_lod2_distance() const { return lod2_distance; }
	void set_collision_distance(float v);
	float get_collision_distance() const { return collision_distance; }
	void set_collision_layer(uint32_t v);
	uint32_t get_collision_layer() const { return collision_layer; }
};
