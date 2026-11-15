// VoxelWorld: Godot node around dkv::Volume. Worker threads mesh 16^3-cell chunks nearest-first; the main
// thread uploads results to the RenderingServer and (near the viewer) to static trimesh bodies.
#pragma once
#include "voxel_core.h"
#include "voxel_resources.h"

#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref.hpp>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

using namespace godot;

class VoxelWorld : public Node3D {
	GDCLASS(VoxelWorld, Node3D)

	struct Chunk {
		RID mesh, instance, body, shape;
		Vector3 offset; // chunk origin in metres, relative to the world node (mesh vertices are chunk-relative)
		int8_t lod = -1; // LOD on screen, -1 = never meshed
		bool collision = false; // the mesh on screen was built with collision
		bool queued = false;
		uint32_t version = 1, queued_version = 0, applied_version = 0; // version bumps on every edit
		int32_t tris = 0;
	};
	struct Job {
		float prio;
		int32_t index;
		int8_t lod;
		bool collision, urgent;
		uint32_t version;
		bool operator<(const Job &o) const { return prio > o.prio; }
	};
	struct Result {
		Job job;
		bool has;
		dkv::MeshData mesh;
		double ms;
	};

	std::unique_ptr<dkv::Volume> vol;
	std::vector<Chunk> chunks;
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

	Vector3 viewer;
	Vector3i viewer_chunk = Vector3i(INT32_MAX, 0, 0);
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
	void chunk_coord(int i, int &x, int &y, int &z) const {
		x = vol->bmin[0] + i % ncx();
		y = vol->bmin[1] + i / ncx() % ncy();
		z = vol->bmin[2] + i / (ncx() * ncy());
	}
	Transform3D chunk_transform(Chunk &c) const { return gx() * Transform3D(Basis(), c.offset); }

	// Builds the Volume from the properties on first use: first edit, query or _ready. Bounded worlds are
	// generated synchronously; endless worlds start empty (streaming arrives in Task 5).
	void ensure_volume();
	void apply_hardness();
	void push_material_uniforms();
	int thread_count() const { return threads > 0 ? threads : MAX(1, (int)std::thread::hardware_concurrency() - 2); }
	void start_workers();
	void stop_workers();

	void worker();
	void free_chunk(Chunk &c, bool body_only = false);

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
	void set_lod_error(int lod, float e);
	Vector3 get_world_size() const { return vol ? Vector3(vol->bmax[0] - vol->bmin[0], vol->bmax[1] - vol->bmin[1], vol->bmax[2] - vol->bmin[2]) * (dkv::CHUNK * vol->cell) : Vector3(); }
	int get_pending_count();
	Dictionary get_stats();

	void set_material(const Ref<Material> &m);
	Ref<Material> get_material() const { return material; }
	void set_generator(const Ref<VoxelGenerator> &g);
	Ref<VoxelGenerator> get_generator() const { return generator; }
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