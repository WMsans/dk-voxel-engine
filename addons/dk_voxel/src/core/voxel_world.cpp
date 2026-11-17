// VoxelWorld: Godot node around dkv::Volume. Worker threads mesh 16^3-cell chunks nearest-first; the main
// thread uploads results to the RenderingServer and (near the viewer) to static trimesh bodies.
#include "voxel_world.h"

#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <chrono>
#include <cstring>
#include <thread>

using namespace godot;

void VoxelWorld::ensure_volume() {
	if (st->vol) return;
	ERR_FAIL_COND_MSG(st->cell_size <= 0.0f, "VoxelWorld: cell_size must be > 0");
	ERR_FAIL_COND_MSG(st->bounds_size.x < 0 || st->bounds_size.y < 0 || st->bounds_size.z < 0 || st->bounds_size.x > 128 || st->bounds_size.y > 64 || st->bounds_size.z > 128,
			"VoxelWorld: bounds_size must be 0 (endless) or within 1..128 x 1..64 x 1..128");
	const bool bounded = st->bounds_size.x > 0 && st->bounds_size.y > 0 && st->bounds_size.z > 0;
	ERR_FAIL_COND_MSG(!bounded && st->bounds_size != Vector3i(0, 0, 0), "VoxelWorld: bounds_size components must all be zero or all positive");
	if (bounded)
		st->vol = std::make_unique<dkv::Volume>(st->bounds_size.x, st->bounds_size.y, st->bounds_size.z, st->cell_size);
	else
		st->vol = std::make_unique<dkv::Volume>(st->cell_size);
	st->active_generator = st->generator.is_valid() ? st->generator->create_generator() : nullptr;
	st->generator_class = st->generator.is_valid() ? st->generator->get_class() : String(); // save headers must name this generator
	st->chunks.clear();
	st->chunk_lookup.clear();
	if (bounded) {
		st->chunks.assign((size_t)st->vol->chunk_count(), dkv::WorldState::Chunk());
		for (int z = 0; z < st->ncz(); z++)
			for (int y = 0; y < st->ncy(); y++)
				for (int x = 0; x < st->ncx(); x++) {
					int idx = st->chunk_index(x, y, z);
					dkv::Vec3 o = st->vol->chunk_origin(x, y, z);
					st->chunks[idx].key = dkv::ChunkKey(x, y, z);
					st->chunks[idx].offset = Vector3(o.x, o.y, o.z);
				}
		st->vol->fill_all(st->active_generator.get(), st->thread_count());
		if (!st->save_directory.is_empty()) { // bounded: a file from an earlier run replaces the generated chunk
			std::vector<uint8_t> raw;
			for (dkv::WorldState::Chunk &c : st->chunks)
				if (read_chunk_file(*st, c.key, st->save_directory, st->generator_class, raw)) st->vol->deserialize_chunk(c.key.x, c.key.y, c.key.z, raw.data(), raw.size());
		}
		for (dkv::WorldState::Chunk &c : st->chunks) c.version++;
	}
	dkv::create_far(*st);
	dkv::apply_material_rules(*st);
	st->need_scan = st->need_stream = true;
	st->need_vis = ~0u;
}

// The chunk list entry for a chunk coordinate, or null when it is not loaded (endless) or out of bounds.
dkv::WorldState::Chunk *VoxelWorld::find_chunk(int cx, int cy, int cz) {
	if (!st->vol) return nullptr;
	if (st->vol->bounded) return st->vol->in_bounds(cx, cy, cz) ? &st->chunks[st->chunk_index(cx, cy, cz)] : nullptr;
	auto it = st->chunk_lookup.find(dkv::ChunkKey(cx, cy, cz));
	return it == st->chunk_lookup.end() ? nullptr : &st->chunks[it->second];
}

// Viewers sharing our World3D drive LOD, collision and streaming. Without one, the active camera does,
// with the world's own collision distance and a view distance that reaches the last LOD band.

void VoxelWorld::apply(dkv::WorldState::Result &r) {
	if (r.job.gen != st->generation) return; // queued before a reset(): never touch the new world
	st->need_vis |= 3u << r.job.level; // readiness at level k decides what levels k and k + 1 show
	if (r.job.level > 0) {
		if (is_inside_tree()) dkv::apply_far(*st, *this, gx(), r);
		return;
	}
	if (r.job.is_gen) {
		if (dkv::WorldState::Chunk *c = find_chunk(r.job.key.x, r.job.key.y, r.job.key.z)) c->gen_queued = false;
		// A restored chunk keeps its store entry: it is the authoritative copy until an edit overwrites it.
		if (r.has && r.job.stored.is_empty()) st->store.erase(r.job.key);
		st->need_scan = true; // a new chunk may complete a neighbour's 26
		return;
	}
	dkv::WorldState::Chunk *cp = (r.job.index >= 0 && r.job.index < (int)st->chunks.size() && st->chunks[r.job.index].key == r.job.key)
			? &st->chunks[r.job.index]
			: find_chunk(r.job.key.x, r.job.key.y, r.job.key.z);
	if (!cp) return; // unloaded while the job ran
	dkv::WorldState::Chunk &c = *cp;
	if (r.job.version == c.queued_version) c.queued = false;
	if (r.job.version != c.version) st->need_scan = true; // edited again meanwhile
	if (r.job.version < c.applied_version || !is_inside_tree()) return;
	st->stat_ms += r.ms;
	st->stat_jobs++;
	c.applied_version = r.job.version;
	if (!r.has) {
		free_chunk(*st, c);
		c.lod = r.job.lod;
		c.collision = r.job.collision;
		return;
	}
	c.lod = r.job.lod;
	const bool want_body = r.job.collision && r.job.lod == 0;
	upload_chunk(*st, *this, c, r.mesh, st->chunk_transform(gx(), c), want_body);
}

Dictionary VoxelWorld::do_edit(dkv::Shape s, dkv::Mode m, const Transform3D &xf, Vector3 half, int mat, int power, float strength, bool sharp) {
	ensure_volume();
	Dictionary out, removed;
	float total = 0;
	bool changed = false;
	AABB edited; // the shape's local [-half, half] box, in global space
	if (st->vol) {
		Transform3D l = gx().affine_inverse() * xf;
		Basis b = l.basis.orthonormalized();
		const float B[9] = {(float)b[0][0], (float)b[0][1], (float)b[0][2], (float)b[1][0], (float)b[1][1], (float)b[1][2], (float)b[2][0], (float)b[2][1], (float)b[2][2]};
		dkv::EditResult r = st->vol->edit(s, m, {(float)l.origin.x, (float)l.origin.y, (float)l.origin.z}, {(float)half.x, (float)half.y, (float)half.z}, B, (uint8_t)CLAMP(mat, 0, 255), power, strength, sharp);
		for (int z = r.cmin[2]; z <= r.cmax[2]; z++)
			for (int y = r.cmin[1]; y <= r.cmax[1]; y++)
				for (int x = r.cmin[0]; x <= r.cmax[0]; x++)
					if (dkv::WorldState::Chunk *ch = find_chunk(x, y, z)) ch->version++;
		st->need_scan |= r.changed;
		// Mirror the edit into the far levels fine enough to show it, so a crater is still there from afar.
		// ponytail: far chunks regenerate from the generator when they unload, so a far edit lasts until then.
		std::vector<dkv::WorldState::Job> jobs;
		for (int k = 1; r.changed && k <= (int)st->far.size(); k++) {
			dkv::WorldState::FarLevel &L = st->far[k - 1];
			if (L.vol->cell > MAX(half.x, MAX(half.y, half.z))) break; // smaller than a cell: only dents the coarse surface
			memcpy(L.vol->hardness, st->vol->hardness, sizeof L.vol->hardness);
			memcpy(L.vol->inner, st->vol->inner, sizeof L.vol->inner);
			L.vol->expose_inner = st->vol->expose_inner;
			dkv::EditResult fr = L.vol->edit(s, m, {(float)l.origin.x, (float)l.origin.y, (float)l.origin.z}, {(float)half.x, (float)half.y, (float)half.z}, B, (uint8_t)CLAMP(mat, 0, 255), power, strength, sharp);
			for (int z = fr.cmin[2]; z <= fr.cmax[2]; z++)
				for (int y = fr.cmin[1]; y <= fr.cmax[1]; y++)
					for (int x = fr.cmin[0]; x <= fr.cmax[0]; x++) {
						auto it = L.chunks.find(dkv::ChunkKey(x, y, z));
						if (it == L.chunks.end()) continue;
						it->second.version++;
						dkv::queue_far_mesh(*st, k, it->first, jobs);
					}
		}
		dkv::push_jobs(*st, jobs);
		const float vv = st->vol->cell * st->vol->cell * st->vol->cell;
		for (int i = 0; i < 256; i++)
			if (r.removed[i] > 0) {
				removed[i] = r.removed[i] * vv;
				total += r.removed[i] * vv;
			}
		out["changed"] = r.changed;
		if (r.changed) {
			changed = true;
			Vector3 mn(1e20f, 1e20f, 1e20f), mx(-1e20f, -1e20f, -1e20f);
			for (int i = 0; i < 8; i++) {
				Vector3 p = xf.xform(Vector3(i & 1 ? half.x : -half.x, i & 2 ? half.y : -half.y, i & 4 ? half.z : -half.z));
				mn = mn.min(p);
				mx = mx.max(p);
			}
			edited = AABB(mn, mx - mn);
		}
	}
	out["removed"] = removed; // material id -> m^3
	out["volume"] = total;
	if (changed) emit_signal("terrain_edited", edited, out);
	return out;
}

void VoxelWorld::_bind_methods() {
	ADD_SIGNAL(MethodInfo("terrain_edited", PropertyInfo(Variant::AABB, "aabb"), PropertyInfo(Variant::DICTIONARY, "result")));
	ClassDB::bind_method(D_METHOD("reset"), &VoxelWorld::reset);
	ClassDB::bind_method(D_METHOD("save"), &VoxelWorld::save);
	ClassDB::bind_method(D_METHOD("dig_sphere", "center", "radius", "power"), &VoxelWorld::dig_sphere, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("build_sphere", "center", "radius", "material"), &VoxelWorld::build_sphere);
	ClassDB::bind_method(D_METHOD("smooth_sphere", "center", "radius", "strength", "power"), &VoxelWorld::smooth_sphere, DEFVAL(1.0), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("build_box", "transform", "half_extents", "material", "sharp"), &VoxelWorld::build_box, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("dig_box", "transform", "half_extents", "power", "sharp"), &VoxelWorld::dig_box, DEFVAL(0), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("build_cylinder", "transform", "radius", "half_height", "material", "sharp"), &VoxelWorld::build_cylinder, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("dig_cylinder", "transform", "radius", "half_height", "power", "sharp"), &VoxelWorld::dig_cylinder, DEFVAL(0), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("raycast", "origin", "direction", "max_distance"), &VoxelWorld::raycast);
	ClassDB::bind_method(D_METHOD("get_density", "position"), &VoxelWorld::get_density);
	ClassDB::bind_method(D_METHOD("is_area_ready", "aabb", "need_collision"), &VoxelWorld::is_area_ready, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("set_lod_error", "lod", "error"), &VoxelWorld::set_lod_error);
	ClassDB::bind_method(D_METHOD("get_world_size"), &VoxelWorld::get_world_size);
	ClassDB::bind_method(D_METHOD("get_material_array", "kind"), &VoxelWorld::get_material_array);
	ClassDB::bind_method(D_METHOD("get_pending_count"), &VoxelWorld::get_pending_count);
	ClassDB::bind_method(D_METHOD("get_stats"), &VoxelWorld::get_stats);
#define PROP(type, name) \
	ClassDB::bind_method(D_METHOD("set_" #name, "v"), &VoxelWorld::set_##name); \
	ClassDB::bind_method(D_METHOD("get_" #name), &VoxelWorld::get_##name); \
	ADD_PROPERTY(PropertyInfo(type, #name), "set_" #name, "get_" #name);
	PROP(Variant::FLOAT, cell_size)
	PROP(Variant::VECTOR3I, bounds_size)
	PROP(Variant::INT, threads)
	PROP(Variant::FLOAT, lod1_distance)
	PROP(Variant::FLOAT, lod2_distance)
	PROP(Variant::FLOAT, collision_distance)
	PROP(Variant::INT, collision_layer)
	PROP(Variant::INT, far_levels)
	PROP(Variant::STRING, save_directory)
	PROP(Variant::BOOL, expose_inner_material)
#undef PROP
	ClassDB::bind_method(D_METHOD("set_material", "material"), &VoxelWorld::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &VoxelWorld::get_material);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "Material"), "set_material", "get_material");
	ClassDB::bind_method(D_METHOD("set_generator", "generator"), &VoxelWorld::set_generator);
	ClassDB::bind_method(D_METHOD("get_generator"), &VoxelWorld::get_generator);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "generator", PROPERTY_HINT_RESOURCE_TYPE, "VoxelGenerator"), "set_generator", "get_generator");
	ClassDB::bind_method(D_METHOD("set_material_set", "material_set"), &VoxelWorld::set_material_set);
	ClassDB::bind_method(D_METHOD("get_material_set"), &VoxelWorld::get_material_set);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material_set", PROPERTY_HINT_RESOURCE_TYPE, "VoxelMaterialSet"), "set_material_set", "get_material_set");
}

void VoxelWorld::_ready() {
	ensure_volume();
	dkv::start_workers(*st);
}

void VoxelWorld::_notification(int what) {
	if (what == NOTIFICATION_TRANSFORM_CHANGED || what == NOTIFICATION_VISIBILITY_CHANGED) {
		if (!is_inside_tree()) return;
		RenderingServer *rs = RenderingServer::get_singleton();
		auto place = [&](dkv::WorldState::Chunk &c) {
			Transform3D xf = st->chunk_transform(gx(), c);
			if (c.instance.is_valid()) rs->instance_set_transform(c.instance, xf);
			if (c.body.is_valid()) PhysicsServer3D::get_singleton()->body_set_state(c.body, PhysicsServer3D::BODY_STATE_TRANSFORM, xf);
		};
		for (dkv::WorldState::Chunk &c : st->chunks) place(c);
		for (dkv::WorldState::FarLevel &L : st->far)
			for (auto &[key, c] : L.chunks) place(c);
		st->need_vis = ~0u;
	} else if (what == NOTIFICATION_EXIT_WORLD) { // RIDs belong to the world we are leaving; remesh on re-entry
		for (dkv::WorldState::Chunk &c : st->chunks) {
			free_chunk(*st, c);
			c.applied_version = 0;
		}
		dkv::free_far(*st); // restreamed on re-entry
		st->need_scan = st->need_stream = true;
	}
}

VoxelWorld::VoxelWorld() {
	st = std::make_unique<dkv::WorldState>();
	set_notify_transform(true);
}
VoxelWorld::~VoxelWorld() {
	dkv::save_world(*st); // flush what this world edited; never deletes files
	dkv::stop_workers(*st);
	for (dkv::WorldState::Chunk &c : st->chunks) free_chunk(*st, c);
	dkv::free_far(*st);
}

Error VoxelWorld::save() { return dkv::save_world(*st); }

void VoxelWorld::reset() {
	st->generation++; // results still in flight belong to the world we are dropping
	dkv::stop_workers(*st);
	if (dkv::save_world(*st) == OK) {
		st->store.clear(); // everything reached disk (or save_directory is empty, so there is nothing to keep)
	} else { // do not discard edits a later save() could still write
		UtilityFunctions::push_error("VoxelWorld: reset could not write every edited chunk; keeping them for a later save()");
		for (const dkv::ChunkKey &key : st->vol->loaded_chunks())
			if (st->vol->is_modified(key.x, key.y, key.z)) store_chunk(*st, key, false); // the flush just failed: keep in memory
	}
	for (dkv::WorldState::Chunk &c : st->chunks) free_chunk(*st, c);
	dkv::free_far(*st);
	st->far.clear();
	st->chunks.clear();
	st->chunk_lookup.clear();
	st->vol.reset();
	st->active_viewers.clear();
	ensure_volume();
	dkv::start_workers(*st);
}

void VoxelWorld::_process(double) {
	if (!st->vol || st->workers.empty()) return;
	dkv::update_viewers(*st, *this);
	if (st->need_scan) dkv::scan(*st);
	std::vector<dkv::WorldState::Result> rs;
	{
		std::lock_guard lk(st->qm);
		rs.swap(st->results);
	}
	for (dkv::WorldState::Result &r : rs) {
		if (r.job.urgent) apply(r); // edits show up the frame they finish
		else st->pending.push_back(std::move(r));
	}
	const uint64_t t0 = Time::get_singleton()->get_ticks_usec();
	while (!st->pending.empty() && Time::get_singleton()->get_ticks_usec() - t0 < 4000) { // streaming: 4 ms a frame
		apply(st->pending.front());
		st->pending.pop_front();
	}
	dkv::update_visibility(*st, is_visible_in_tree());
}

Dictionary VoxelWorld::dig_sphere(Vector3 c, float r, int power) { return do_edit(dkv::SPHERE, dkv::ERASE, Transform3D(Basis(), c), Vector3(r, r, r), 0, power, 1, false); }
Dictionary VoxelWorld::build_sphere(Vector3 c, float r, int mat) { return do_edit(dkv::SPHERE, dkv::BUILD, Transform3D(Basis(), c), Vector3(r, r, r), mat, 0, 1, false); }
Dictionary VoxelWorld::smooth_sphere(Vector3 c, float r, float strength, int power) { return do_edit(dkv::SPHERE, dkv::SMOOTH, Transform3D(Basis(), c), Vector3(r, r, r), 0, power, strength, false); }
Dictionary VoxelWorld::build_box(const Transform3D &xf, Vector3 half, int mat, bool sharp) { return do_edit(dkv::BOX, dkv::BUILD, xf, half, mat, 0, 1, sharp); }
Dictionary VoxelWorld::dig_box(const Transform3D &xf, Vector3 half, int power, bool sharp) { return do_edit(dkv::BOX, dkv::ERASE, xf, half, 0, power, 1, sharp); }
Dictionary VoxelWorld::build_cylinder(const Transform3D &xf, float r, float hh, int mat, bool sharp) { return do_edit(dkv::CYLINDER, dkv::BUILD, xf, Vector3(r, hh, r), mat, 0, 1, sharp); }
Dictionary VoxelWorld::dig_cylinder(const Transform3D &xf, float r, float hh, int power, bool sharp) { return do_edit(dkv::CYLINDER, dkv::ERASE, xf, Vector3(r, hh, r), 0, power, 1, sharp); }

// Marches the voxel field itself, so it works before (and beyond where) collision exists.
Dictionary VoxelWorld::raycast(Vector3 origin, Vector3 dir, float max_dist) {
	ensure_volume();
	Dictionary d;
	if (!st->vol) return d;
	Vector3 o = gx().xform_inv(origin), dl = gx().basis.xform_inv(dir);
	dkv::RayHit h = st->vol->raycast({(float)o.x, (float)o.y, (float)o.z}, {(float)dl.x, (float)dl.y, (float)dl.z}, max_dist);
	if (!h.hit) return d;
	d["position"] = gx().xform(Vector3(h.pos.x, h.pos.y, h.pos.z));
	d["normal"] = gx().basis.xform(Vector3(h.normal.x, h.normal.y, h.normal.z));
	d["material"] = h.material;
	return d;
}
Vector3 VoxelWorld::get_world_size() const {
	return st->vol ? Vector3(st->vol->bmax[0] - st->vol->bmin[0], st->vol->bmax[1] - st->vol->bmin[1], st->vol->bmax[2] - st->vol->bmin[2]) * (dkv::CHUNK * st->vol->cell) : Vector3();
}

float VoxelWorld::get_density(Vector3 p) {
	ensure_volume();
	p = gx().xform_inv(p);
	return st->vol ? st->vol->density({(float)p.x, (float)p.y, (float)p.z}) / 255.0f : 0.0f;
}

bool VoxelWorld::is_area_ready(const AABB &aabb, bool need_collision) {
	ensure_volume();
	if (!st->vol || st->workers.empty() || st->active_viewers.empty()) return false;
	if (st->need_scan) dkv::scan(*st);
	Transform3D inv = gx().affine_inverse();
	// The world node may be rotated: transform all 8 corners, as do_edit() does for terrain_edited.
	Vector3 lmin(1e20f, 1e20f, 1e20f), lmax(-1e20f, -1e20f, -1e20f);
	for (int i = 0; i < 8; i++) {
		Vector3 p = inv.xform(aabb.position + Vector3(i & 1 ? aabb.size.x : 0.0f, i & 2 ? aabb.size.y : 0.0f, i & 4 ? aabb.size.z : 0.0f));
		lmin = lmin.min(p);
		lmax = lmax.max(p);
	}
	const float cs = dkv::CHUNK * st->vol->cell;
	int cmin[3], cmax[3];
	for (int i = 0; i < 3; i++) {
		cmin[i] = (int)std::floor(lmin[i] / cs);
		cmax[i] = (int)std::floor((lmax[i] - 1e-4f) / cs);
		if (st->vol->bounded) {
			cmin[i] = MAX(cmin[i], st->vol->bmin[i]);
			cmax[i] = MIN(cmax[i], st->vol->bmax[i] - 1);
		}
	}
	const float hyst = cs * 0.5f;
	for (int z = cmin[2]; z <= cmax[2]; z++)
		for (int y = cmin[1]; y <= cmax[1]; y++)
			for (int x = cmin[0]; x <= cmax[0]; x++) {
				if (!st->vol->has_chunk(x, y, z)) return false;
				dkv::WorldState::Chunk *c = find_chunk(x, y, z);
				if (!c || c->gen_queued || c->queued || c->applied_version != c->version || c->lod < 0) return false;
				Vector3 center = c->offset + Vector3(0.5, 0.5, 0.5) * cs;
				float d = 0.0f;
				for (size_t v = 0; v < st->active_viewers.size(); v++) {
					float dist = st->active_viewers[v].pos.distance_to(center);
					if (v == 0 || dist < d) d = dist;
				}
				int lo = st->lod_at(d - hyst), hi = st->lod_at(d + hyst);
				int want = (c->lod == lo || c->lod == hi) ? c->lod : lo;
				if (c->lod != want) return false;
				if (need_collision && c->tris > 0 && (!c->collision || !c->body.is_valid())) return false;
			}
	return true;
}
void VoxelWorld::set_lod_error(int lod, float e) {
	if (st->vol && lod >= 1 && lod <= 2) st->vol->lod_error[lod] = e;
}
int VoxelWorld::get_pending_count() {
	std::lock_guard lk(st->qm);
	return (int)(st->jobs.size() + st->results.size() + st->pending.size());
}
Dictionary VoxelWorld::get_stats() {
	Dictionary d;
	d["triangles"] = st->stat_tris;
	d["meshes"] = st->stat_meshes;
	d["bodies"] = st->stat_bodies;
	d["chunks"] = (int64_t)(st->vol ? st->vol->chunk_count() : 0);
	int64_t far_chunks = 0;
	for (const dkv::WorldState::FarLevel &L : st->far) far_chunks += (int64_t)L.chunks.size();
	d["far_chunks"] = far_chunks;
	d["pending"] = get_pending_count();
	d["mesh_ms"] = st->stat_jobs ? st->stat_ms / st->stat_jobs : 0.0;
	d["threads"] = (int)st->workers.size();
	return d;
}

void VoxelWorld::set_material(const Ref<Material> &m) {
	st->material = m;
	for (dkv::WorldState::Chunk &c : st->chunks)
		if (c.mesh.is_valid()) RenderingServer::get_singleton()->mesh_surface_set_material(c.mesh, 0, m.is_valid() ? m->get_rid() : RID());
	dkv::apply_material_set(*st);
}
void VoxelWorld::set_generator(const Ref<VoxelGenerator> &g) {
	st->generator = g;
	st->generator_class = g.is_valid() ? g->get_class() : String();
}
void VoxelWorld::_materials_changed() {
	dkv::apply_material_rules(*st);
	dkv::apply_material_set(*st);
}

void VoxelWorld::set_material_set(const Ref<VoxelMaterialSet> &s) {
	if (st->material_set.is_valid()) st->material_set->disconnect("changed", callable_mp(this, &VoxelWorld::_materials_changed));
	st->material_set = s;
	if (st->material_set.is_valid()) st->material_set->connect("changed", callable_mp(this, &VoxelWorld::_materials_changed));
	_materials_changed();
}
void VoxelWorld::set_lod1_distance(float v) { st->lod1_distance = v; st->need_scan = true; }
void VoxelWorld::set_lod2_distance(float v) { st->lod2_distance = v; st->need_scan = true; }
void VoxelWorld::set_collision_distance(float v) { st->collision_distance = v; st->need_scan = true; }
void VoxelWorld::set_collision_layer(uint32_t v) {
	st->collision_layer = v;
	for (dkv::WorldState::Chunk &c : st->chunks)
		if (c.body.is_valid()) PhysicsServer3D::get_singleton()->body_set_collision_layer(c.body, v);
}
