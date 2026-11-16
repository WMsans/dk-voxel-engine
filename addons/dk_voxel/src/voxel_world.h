// VoxelWorld: Godot node around dkv::Volume. Worker threads mesh 16^3-cell chunks nearest-first; the main
// thread uploads results to the RenderingServer and (near the viewer) to static trimesh bodies.
#pragma once
#include "voxel_world_state.h"
#include "voxel_resources.h"

#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref.hpp>

#include <memory>

using namespace godot;

class VoxelWorld : public Node3D {
	GDCLASS(VoxelWorld, Node3D)

	std::unique_ptr<dkv::WorldState> st;

	// Edits and queries are legal before the node enters the tree (e.g. while a level is being built).
	Transform3D gx() const { return is_inside_tree() ? get_global_transform() : get_transform(); }

	// Builds the Volume from the properties on first use: first edit, query or _ready. Bounded worlds are
	// generated synchronously; endless worlds start empty (streaming arrives in Task 5).
	void ensure_volume();
	// Def for a material id: entry `id` if the set has one, else entry 0 (a non-empty set), else a default grey zero-hardness def.
	Ref<VoxelMaterialDef> material_def(int id) const;
	void apply_hardness();
	void push_material_uniforms();
	int thread_count() const { return st->threads > 0 ? st->threads : MAX(1, (int)std::thread::hardware_concurrency() - 2); }
	// The wanted LOD for a distance, the one definition scan() and is_area_ready() share.
	int lod_at(float d) const { return d < st->lod1_distance ? 0 : d < st->lod2_distance ? 1 : 2; }
	void start_workers();
	void stop_workers();

	// Collects the VoxelViewers that share our World3D (the active camera when there is none).
	void update_viewers();
	dkv::WorldState::Chunk *find_chunk(int cx, int cy, int cz);
	void worker();

	// Endless streaming: unload what no viewer wants (keeping edits in `store`), then queue generate
	// jobs for the wanted set. Bounded worlds keep every chunk and never call this.
	void stream_endless();
	// Keeps an edited chunk that is about to be unloaded: on disk when save_directory is set, else (or when
	// the write fails) compressed in `store`. `try_disk` is false when a write was just attempted and failed.

	// One zstd file per modified chunk: `<dir>/<x>_<y>_<z>.dkvc` (the directory is passed in: workers read a
	// snapshot taken at queue time). read_chunk_file() is false when there is no file or its header does not
	// match this world (it pushes an error in that case).

	// Pick each chunk's LOD from its distance to the viewer and queue whatever is out of date.
	void scan();

	void apply(dkv::WorldState::Result &r);

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
	Vector3 get_world_size() const;
	int get_pending_count();
	Dictionary get_stats();

	void set_material(const Ref<Material> &m);
	Ref<Material> get_material() const { return st->material; }
	void set_generator(const Ref<VoxelGenerator> &g);
	Ref<VoxelGenerator> get_generator() const { return st->generator; }
	void set_save_directory(const String &v) { st->save_directory = v; }
	String get_save_directory() const { return st->save_directory; }
	void set_material_set(const Ref<VoxelMaterialSet> &s);
	Ref<VoxelMaterialSet> get_material_set() const { return st->material_set; }
	void set_cell_size(float v) { st->cell_size = v; }
	float get_cell_size() const { return st->cell_size; }
	void set_bounds_size(const Vector3i &v) { st->bounds_size = v; }
	Vector3i get_bounds_size() const { return st->bounds_size; }
	void set_threads(int v) { st->threads = v; }
	int get_threads() const { return st->threads; }
	void set_lod1_distance(float v);
	float get_lod1_distance() const { return st->lod1_distance; }
	void set_lod2_distance(float v);
	float get_lod2_distance() const { return st->lod2_distance; }
	void set_collision_distance(float v);
	float get_collision_distance() const { return st->collision_distance; }
	void set_collision_layer(uint32_t v);
	uint32_t get_collision_layer() const { return st->collision_layer; }
};
