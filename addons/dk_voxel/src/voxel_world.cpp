// VoxelWorld: Godot node around dkv::Volume. Worker threads mesh 16^3-cell chunks nearest-first; the main
// thread uploads results to the RenderingServer and (near the viewer) to static trimesh bodies.
#include "voxel_world.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>

#include <chrono>
#include <thread>

using namespace godot;

// The shader samples 32 materials; missing ids read as entry 0.
static const int MATERIAL_SLOTS = 32;

void VoxelWorld::ensure_volume() {
	if (vol) return;
	ERR_FAIL_COND_MSG(cell_size <= 0.0f, "VoxelWorld: cell_size must be > 0");
	ERR_FAIL_COND_MSG(bounds_size.x < 0 || bounds_size.y < 0 || bounds_size.z < 0 || bounds_size.x > 128 || bounds_size.y > 64 || bounds_size.z > 128,
			"VoxelWorld: bounds_size must be 0 (endless) or within 1..128 x 1..64 x 1..128");
	const bool bounded = bounds_size.x > 0 && bounds_size.y > 0 && bounds_size.z > 0;
	ERR_FAIL_COND_MSG(!bounded && bounds_size != Vector3i(0, 0, 0), "VoxelWorld: bounds_size components must all be zero or all positive");
	if (bounded)
		vol = std::make_unique<dkv::Volume>(bounds_size.x, bounds_size.y, bounds_size.z, cell_size);
	else
		vol = std::make_unique<dkv::Volume>(cell_size);
	chunks.assign((size_t)vol->chunk_count(), Chunk());
	for (int z = 0; z < ncz(); z++)
		for (int y = 0; y < ncy(); y++)
			for (int x = 0; x < ncx(); x++) {
				dkv::Vec3 o = vol->chunk_origin(x, y, z);
				chunks[chunk_index(x, y, z)].offset = Vector3(o.x, o.y, o.z);
			}
	apply_hardness();
	if (bounded) {
		std::shared_ptr<const dkv::Generator> gen = generator.is_valid() ? generator->create_generator() : nullptr;
		vol->fill_all(gen.get(), thread_count());
		for (Chunk &c : chunks) c.version++;
	}
	need_scan = true;
}

Ref<VoxelMaterialDef> VoxelWorld::material_def(int id) const {
	TypedArray<VoxelMaterialDef> mats = material_set.is_valid() ? material_set->get_materials() : TypedArray<VoxelMaterialDef>();
	Ref<VoxelMaterialDef> m;
	if (id >= 0 && id < (int)mats.size()) m = mats[id];
	if (!m.is_valid() && !mats.is_empty()) m = mats[0]; // missing id (or a null entry): entry 0
	if (!m.is_valid()) m.instantiate(); // no set, or an empty one: grey default
	return m;
}

void VoxelWorld::apply_hardness() {
	if (!vol) return;
	memset(vol->hardness, 0, sizeof(vol->hardness));
	if (!material_set.is_valid() || material_set->get_materials().is_empty()) return;
	for (int i = 0; i < 256; i++) vol->hardness[i] = (uint8_t)material_def(i)->get_hardness();
}

void VoxelWorld::push_material_uniforms() {
	ShaderMaterial *sm = Object::cast_to<ShaderMaterial>(material.ptr());
	if (!sm) return;
	PackedColorArray ca, cb;
	Array params;
	for (int i = 0; i < MATERIAL_SLOTS; i++) {
		Ref<VoxelMaterialDef> m = material_def(i);
		ca.push_back(m->get_color_a());
		cb.push_back(m->get_color_b());
		params.push_back(Vector4(m->get_pattern_scale(), m->get_roughness(), m->get_metallic(), m->get_emission()));
	}
	sm->set_shader_parameter("color_a", ca);
	sm->set_shader_parameter("color_b", cb);
	sm->set_shader_parameter("params", params);
}

void VoxelWorld::start_workers() {
	if (!vol || workers.size() || Engine::get_singleton()->is_editor_hint()) return;
	for (int i = 0; i < thread_count(); i++) workers.emplace_back([this] { worker(); });
}

void VoxelWorld::stop_workers() {
	{
		std::lock_guard lk(qm);
		quit = true;
	}
	qcv.notify_all();
	for (auto &t : workers) t.join();
	workers.clear();
	quit = false;
	jobs = {};
	results.clear();
	pending.clear();
}

void VoxelWorld::worker() {
	for (;;) {
		Result r;
		{
			std::unique_lock lk(qm);
			qcv.wait(lk, [&] { return quit || !jobs.empty(); });
			if (quit) return;
			r.job = jobs.top();
			jobs.pop();
		}
		int x, y, z;
		chunk_coord(r.job.index, x, y, z);
		auto t0 = std::chrono::steady_clock::now();
		r.has = vol->mesh_chunk(x, y, z, r.job.lod, r.mesh);
		r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		std::lock_guard lk(qm);
		results.push_back(std::move(r));
	}
}

void VoxelWorld::free_chunk(Chunk &c, bool body_only /* = false */) {
	RenderingServer *rs = RenderingServer::get_singleton();
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (c.body.is_valid()) {
		ps->free_rid(c.body);
		ps->free_rid(c.shape);
		c.body = c.shape = RID();
		stat_bodies--;
	}
	c.collision = false;
	if (body_only) return;
	if (c.instance.is_valid()) rs->free_rid(c.instance);
	if (c.mesh.is_valid()) {
		rs->free_rid(c.mesh);
		stat_meshes--;
	}
	c.instance = c.mesh = RID();
	stat_tris -= c.tris;
	c.tris = 0;
	c.lod = -1;
}

// Pick each chunk's LOD from its distance to the viewer and queue whatever is out of date.
void VoxelWorld::scan() {
	const float cs = dkv::CHUNK * vol->cell, hyst = cs * 0.5f;
	const float coll = MIN(collision_distance, lod1_distance - cs);
	auto lod_at = [&](float d) { return d < lod1_distance ? 0 : d < lod2_distance ? 1 : 2; };
	std::vector<Job> add;
	for (int i = 0; i < (int)chunks.size(); i++) {
		Chunk &c = chunks[i];
		Vector3 center = c.offset + Vector3(0.5, 0.5, 0.5) * cs;
		float d = viewer.distance_to(center);
		int lo = lod_at(d - hyst), hi = lod_at(d + hyst); // hysteresis: keep the current LOD while it is plausible
		int want = (c.lod == lo || c.lod == hi) ? c.lod : lo;
		bool want_coll = d < (c.collision ? coll + hyst : coll);
		if (!want_coll && c.body.is_valid()) free_chunk(c, true);
		bool stale = c.applied_version != c.version;
		if (!stale && c.lod == want && (c.collision || !want_coll)) continue;
		if (c.queued && c.queued_version == c.version) continue;
		c.queued = true;
		c.queued_version = c.version;
		bool urgent = stale && c.lod >= 0; // an edit to something already on screen
		add.push_back({urgent ? d - 1e6f : d, i, (int8_t)want, want_coll, urgent, c.version});
	}
	need_scan = false;
	if (add.empty()) return;
	{
		std::lock_guard lk(qm);
		for (const Job &j : add) jobs.push(j);
	}
	qcv.notify_all();
}

void VoxelWorld::apply(Result &r) {
	Chunk &c = chunks[r.job.index];
	if (r.job.version == c.queued_version) c.queued = false;
	if (r.job.version != c.version) need_scan = true; // edited again meanwhile
	if (r.job.version < c.applied_version || !is_inside_tree()) return;
	stat_ms += r.ms;
	stat_jobs++;
	c.applied_version = r.job.version;
	if (!r.has) {
		free_chunk(c);
		c.lod = r.job.lod;
		c.collision = r.job.collision;
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	const int nv = (int)r.mesh.pos.size() / 3, ni = (int)r.mesh.idx.size();
	PackedVector3Array verts, normals;
	PackedColorArray colors;
	PackedInt32Array indices;
	verts.resize(nv);
	normals.resize(nv);
	colors.resize(nv);
	indices.resize(ni);
	memcpy(verts.ptrw(), r.mesh.pos.data(), nv * sizeof(Vector3));
	memcpy(normals.ptrw(), r.mesh.nrm.data(), nv * sizeof(Vector3));
	memcpy(indices.ptrw(), r.mesh.idx.data(), ni * sizeof(int32_t));
	Color *cw = colors.ptrw();
	for (int i = 0; i < nv; i++) cw[i] = Color(r.mesh.col[i * 4] / 255.0f, r.mesh.col[i * 4 + 1] / 255.0f, r.mesh.col[i * 4 + 2] / 255.0f, r.mesh.col[i * 4 + 3] / 255.0f);
	Array arrays;
	arrays.resize(RenderingServer::ARRAY_MAX);
	arrays[RenderingServer::ARRAY_VERTEX] = verts;
	arrays[RenderingServer::ARRAY_NORMAL] = normals;
	arrays[RenderingServer::ARRAY_COLOR] = colors;
	arrays[RenderingServer::ARRAY_INDEX] = indices;
	if (c.mesh.is_valid()) {
		rs->mesh_clear(c.mesh);
	} else {
		c.mesh = rs->mesh_create();
		stat_meshes++;
	}
	rs->mesh_add_surface_from_arrays(c.mesh, RenderingServer::PRIMITIVE_TRIANGLES, arrays);
	if (material.is_valid()) rs->mesh_surface_set_material(c.mesh, 0, material->get_rid());
	if (!c.instance.is_valid()) {
		c.instance = rs->instance_create2(c.mesh, get_world_3d()->get_scenario());
		rs->instance_set_transform(c.instance, chunk_transform(c));
	}
	stat_tris += ni / 3 - c.tris;
	c.tris = ni / 3;
	c.lod = r.job.lod;

	// Collision is the LOD0 mesh itself. ponytail: only near the viewer, so bodies far from the camera
	// have nothing to land on; add per-body collision anchors if that matters.
	if (!r.job.collision || r.job.lod != 0) {
		free_chunk(c, true);
		return;
	}
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	PackedVector3Array faces;
	faces.resize(ni);
	Vector3 *fw = faces.ptrw();
	const Vector3 *vr = verts.ptr();
	for (int i = 0; i < ni; i++) fw[i] = vr[r.mesh.idx[i]];
	Dictionary data;
	data["faces"] = faces;
	data["backface_collision"] = false;
	if (!c.body.is_valid()) {
		c.shape = ps->concave_polygon_shape_create();
		ps->shape_set_data(c.shape, data);
		c.body = ps->body_create();
		ps->body_set_mode(c.body, PhysicsServer3D::BODY_MODE_STATIC);
		ps->body_add_shape(c.body, c.shape);
		ps->body_set_collision_layer(c.body, collision_layer);
		ps->body_set_collision_mask(c.body, 0);
		ps->body_attach_object_instance_id(c.body, get_instance_id());
		ps->body_set_state(c.body, PhysicsServer3D::BODY_STATE_TRANSFORM, chunk_transform(c));
		ps->body_set_space(c.body, get_world_3d()->get_space());
		stat_bodies++;
	} else {
		ps->shape_set_data(c.shape, data);
	}
	c.collision = true;
}

Dictionary VoxelWorld::do_edit(dkv::Shape s, dkv::Mode m, const Transform3D &xf, Vector3 half, int mat, int power, float strength, bool sharp) {
	ensure_volume();
	Dictionary out, removed;
	float total = 0;
	if (vol) {
		Transform3D l = gx().affine_inverse() * xf;
		Basis b = l.basis.orthonormalized();
		const float B[9] = {(float)b[0][0], (float)b[0][1], (float)b[0][2], (float)b[1][0], (float)b[1][1], (float)b[1][2], (float)b[2][0], (float)b[2][1], (float)b[2][2]};
		dkv::EditResult r = vol->edit(s, m, {(float)l.origin.x, (float)l.origin.y, (float)l.origin.z}, {(float)half.x, (float)half.y, (float)half.z}, B, (uint8_t)CLAMP(mat, 0, 255), power, strength, sharp);
		if (vol->bounded) { // endless worlds have no chunk list to invalidate yet (Task 5)
			for (int z = r.cmin[2]; z <= r.cmax[2]; z++)
				for (int y = r.cmin[1]; y <= r.cmax[1]; y++)
					for (int x = r.cmin[0]; x <= r.cmax[0]; x++) chunks[chunk_index(x, y, z)].version++;
		}
		need_scan |= r.changed;
		const float vv = vol->cell * vol->cell * vol->cell;
		for (int i = 0; i < 256; i++)
			if (r.removed[i] > 0) {
				removed[i] = r.removed[i] * vv;
				total += r.removed[i] * vv;
			}
		out["changed"] = r.changed;
	}
	out["removed"] = removed; // material id -> m^3
	out["volume"] = total;
	return out;
}

void VoxelWorld::_bind_methods() {
	ClassDB::bind_method(D_METHOD("reset"), &VoxelWorld::reset);
	ClassDB::bind_method(D_METHOD("dig_sphere", "center", "radius", "power"), &VoxelWorld::dig_sphere, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("build_sphere", "center", "radius", "material"), &VoxelWorld::build_sphere);
	ClassDB::bind_method(D_METHOD("smooth_sphere", "center", "radius", "strength", "power"), &VoxelWorld::smooth_sphere, DEFVAL(1.0), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("build_box", "transform", "half_extents", "material", "sharp"), &VoxelWorld::build_box, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("dig_box", "transform", "half_extents", "power", "sharp"), &VoxelWorld::dig_box, DEFVAL(0), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("build_cylinder", "transform", "radius", "half_height", "material", "sharp"), &VoxelWorld::build_cylinder, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("dig_cylinder", "transform", "radius", "half_height", "power", "sharp"), &VoxelWorld::dig_cylinder, DEFVAL(0), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("raycast", "origin", "direction", "max_distance"), &VoxelWorld::raycast);
	ClassDB::bind_method(D_METHOD("get_density", "position"), &VoxelWorld::get_density);
	ClassDB::bind_method(D_METHOD("set_lod_error", "lod", "error"), &VoxelWorld::set_lod_error);
	ClassDB::bind_method(D_METHOD("get_world_size"), &VoxelWorld::get_world_size);
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
	start_workers();
}

void VoxelWorld::_notification(int what) {
	if (what == NOTIFICATION_TRANSFORM_CHANGED || what == NOTIFICATION_VISIBILITY_CHANGED) {
		if (!is_inside_tree()) return;
		RenderingServer *rs = RenderingServer::get_singleton();
		for (Chunk &c : chunks) {
			Transform3D xf = chunk_transform(c);
			if (c.instance.is_valid()) {
				rs->instance_set_transform(c.instance, xf);
				rs->instance_set_visible(c.instance, is_visible_in_tree());
			}
			if (c.body.is_valid()) PhysicsServer3D::get_singleton()->body_set_state(c.body, PhysicsServer3D::BODY_STATE_TRANSFORM, xf);
		}
	} else if (what == NOTIFICATION_EXIT_WORLD) { // RIDs belong to the world we are leaving; remesh on re-entry
		for (Chunk &c : chunks) {
			free_chunk(c);
			c.applied_version = 0;
		}
		need_scan = true;
	}
}

VoxelWorld::VoxelWorld() { set_notify_transform(true); }
VoxelWorld::~VoxelWorld() {
	stop_workers();
	for (Chunk &c : chunks) free_chunk(c);
}

void VoxelWorld::reset() {
	stop_workers();
	for (Chunk &c : chunks) free_chunk(c);
	chunks.clear();
	vol.reset();
	ensure_volume();
	start_workers();
}

void VoxelWorld::_process(double) {
	if (!vol || workers.empty()) return;
	if (Camera3D *cam = get_viewport()->get_camera_3d()) viewer = to_local(cam->get_global_position());
	Vector3i vc = Vector3i((viewer / (dkv::CHUNK * vol->cell)).floor());
	if (vc != viewer_chunk) {
		viewer_chunk = vc;
		need_scan = true;
	}
	if (need_scan) scan();
	std::vector<Result> rs;
	{
		std::lock_guard lk(qm);
		rs.swap(results);
	}
	for (Result &r : rs) {
		if (r.job.urgent) apply(r); // edits show up the frame they finish
		else pending.push_back(std::move(r));
	}
	const uint64_t t0 = Time::get_singleton()->get_ticks_usec();
	while (!pending.empty() && Time::get_singleton()->get_ticks_usec() - t0 < 4000) { // streaming: 4 ms a frame
		apply(pending.front());
		pending.pop_front();
	}
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
	if (!vol) return d;
	Vector3 o = gx().xform_inv(origin), dl = gx().basis.xform_inv(dir);
	dkv::RayHit h = vol->raycast({(float)o.x, (float)o.y, (float)o.z}, {(float)dl.x, (float)dl.y, (float)dl.z}, max_dist);
	if (!h.hit) return d;
	d["position"] = gx().xform(Vector3(h.pos.x, h.pos.y, h.pos.z));
	d["normal"] = gx().basis.xform(Vector3(h.normal.x, h.normal.y, h.normal.z));
	d["material"] = h.material;
	return d;
}
float VoxelWorld::get_density(Vector3 p) {
	ensure_volume();
	p = gx().xform_inv(p);
	return vol ? vol->density({(float)p.x, (float)p.y, (float)p.z}) / 255.0f : 0.0f;
}
void VoxelWorld::set_lod_error(int lod, float e) {
	if (vol && lod >= 1 && lod <= 2) vol->lod_error[lod] = e;
}
int VoxelWorld::get_pending_count() {
	std::lock_guard lk(qm);
	return (int)(jobs.size() + results.size() + pending.size());
}
Dictionary VoxelWorld::get_stats() {
	Dictionary d;
	d["triangles"] = stat_tris;
	d["meshes"] = stat_meshes;
	d["bodies"] = stat_bodies;
	d["pending"] = get_pending_count();
	d["mesh_ms"] = stat_jobs ? stat_ms / stat_jobs : 0.0;
	d["threads"] = (int)workers.size();
	return d;
}

void VoxelWorld::set_material(const Ref<Material> &m) {
	material = m;
	for (Chunk &c : chunks)
		if (c.mesh.is_valid()) RenderingServer::get_singleton()->mesh_surface_set_material(c.mesh, 0, m.is_valid() ? m->get_rid() : RID());
	push_material_uniforms();
}
void VoxelWorld::set_generator(const Ref<VoxelGenerator> &g) { generator = g; }
void VoxelWorld::set_material_set(const Ref<VoxelMaterialSet> &s) {
	material_set = s;
	apply_hardness();
	push_material_uniforms();
}
void VoxelWorld::set_lod1_distance(float v) { lod1_distance = v; need_scan = true; }
void VoxelWorld::set_lod2_distance(float v) { lod2_distance = v; need_scan = true; }
void VoxelWorld::set_collision_distance(float v) { collision_distance = v; need_scan = true; }
void VoxelWorld::set_collision_layer(uint32_t v) {
	collision_layer = v;
	for (Chunk &c : chunks)
		if (c.body.is_valid()) PhysicsServer3D::get_singleton()->body_set_collision_layer(c.body, v);
}