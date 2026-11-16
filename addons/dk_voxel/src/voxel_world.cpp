// VoxelWorld: Godot node around dkv::Volume. Worker threads mesh 16^3-cell chunks nearest-first; the main
// thread uploads results to the RenderingServer and (near the viewer) to static trimesh bodies.
#include "voxel_world.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
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
	active_generator = generator.is_valid() ? generator->create_generator() : nullptr;
	chunks.clear();
	chunk_lookup.clear();
	if (bounded) {
		chunks.assign((size_t)vol->chunk_count(), Chunk());
		for (int z = 0; z < ncz(); z++)
			for (int y = 0; y < ncy(); y++)
				for (int x = 0; x < ncx(); x++) {
					int idx = chunk_index(x, y, z);
					dkv::Vec3 o = vol->chunk_origin(x, y, z);
					chunks[idx].key = dkv::ChunkKey(x, y, z);
					chunks[idx].offset = Vector3(o.x, o.y, o.z);
				}
		vol->fill_all(active_generator.get(), thread_count());
		for (Chunk &c : chunks) c.version++;
	}
	apply_hardness();
	need_scan = true;
}

// The chunk list entry for a chunk coordinate, or null when it is not loaded (endless) or out of bounds.
VoxelWorld::Chunk *VoxelWorld::find_chunk(int cx, int cy, int cz) {
	if (!vol) return nullptr;
	if (vol->bounded) return vol->in_bounds(cx, cy, cz) ? &chunks[chunk_index(cx, cy, cz)] : nullptr;
	auto it = chunk_lookup.find(dkv::ChunkKey(cx, cy, cz));
	return it == chunk_lookup.end() ? nullptr : &chunks[it->second];
}

// Viewers sharing our World3D drive LOD, collision and streaming. Without one, the active camera does,
// with the world's own collision distance and a view distance that reaches the last LOD band.
void VoxelWorld::update_viewers() {
	const float cs = dkv::CHUNK * vol->cell;
	std::vector<Viewer> found;
	if (is_inside_tree() && get_tree()) {
		TypedArray<Node> group = get_tree()->get_nodes_in_group(VIEWER_GROUP);
		for (int i = 0; i < group.size(); i++) {
			VoxelViewer *vv = Object::cast_to<VoxelViewer>(group[i]);
			if (!vv || !vv->is_inside_tree() || !vv->get_enabled() || vv->get_world_3d() != get_world_3d()) continue;
			Viewer v;
			v.pos = to_local(vv->get_global_position());
			v.chunk = Vector3i((v.pos / cs).floor());
			v.view_distance = vv->get_view_distance();
			v.vertical_view_distance = vv->get_vertical_view_distance();
			v.collision_distance = vv->get_collision_distance();
			found.push_back(v);
		}
	}
	if (found.empty() && is_inside_tree() && get_viewport())
		if (Camera3D *cam = get_viewport()->get_camera_3d()) {
			Viewer v;
			v.pos = to_local(cam->get_global_position());
			v.chunk = Vector3i((v.pos / cs).floor());
			v.view_distance = lod2_distance + cs;
			v.vertical_view_distance = lod2_distance + cs;
			v.collision_distance = collision_distance;
			found.push_back(v);
		}
	bool changed = found.size() != active_viewers.size();
	for (size_t i = 0; i < found.size() && i < active_viewers.size(); i++)
		if (found[i].chunk != active_viewers[i].chunk || found[i].view_distance != active_viewers[i].view_distance ||
				found[i].vertical_view_distance != active_viewers[i].vertical_view_distance ||
				found[i].collision_distance != active_viewers[i].collision_distance ||
				found[i].pos.distance_squared_to(active_viewers[i].pos) > 4.0f) // 2 m of slack, so LOD and collision follow
			changed = true;
	if (changed) {
		active_viewers = std::move(found);
		need_scan = true;
	} else {
		for (size_t i = 0; i < found.size(); i++) active_viewers[i].pos = found[i].pos;
	}
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
		auto t0 = std::chrono::steady_clock::now();
		const dkv::ChunkKey &key = r.job.key;
		if (r.job.is_gen) { // endless: put back the edited chunk we kept in memory, else generate it
			bool restored = false;
			if (!r.job.stored.is_empty()) {
				PackedByteArray raw = r.job.stored.decompress((int)r.job.stored_size, FileAccess::COMPRESSION_ZSTD);
				restored = raw.size() == (int64_t)r.job.stored_size && vol->deserialize_chunk(key.x, key.y, key.z, (const uint8_t *)raw.ptr(), (size_t)raw.size());
			}
			if (!restored) vol->fill_chunk(key.x, key.y, key.z, active_generator.get());
			r.has = true;
		} else {
			r.has = vol->mesh_chunk(key.x, key.y, key.z, r.job.lod, r.mesh);
		}
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

// Keep an edited chunk that is about to be unloaded: compressed in memory (Task 6 writes it to disk).
void VoxelWorld::store_chunk(const dkv::ChunkKey &key) {
	if (!vol->is_modified(key.x, key.y, key.z)) return;
	std::vector<uint8_t> raw = vol->serialize_chunk(key.x, key.y, key.z);
	if (raw.empty()) return;
	PackedByteArray data;
	data.resize((int64_t)raw.size());
	memcpy(data.ptrw(), raw.data(), raw.size());
	store[key] = {(uint32_t)raw.size(), data.compress(FileAccess::COMPRESSION_ZSTD)};
}

// Endless streaming: unload what no viewer wants, then queue a generate job for every wanted chunk that
// is not loaded. The extra chunk of margin is loaded so the chunks viewers see have all 26 neighbours.
void VoxelWorld::stream_endless() {
	const float cs = dkv::CHUNK * vol->cell;
	const float margin = cs;
	std::vector<Job> gen;

	for (int i = (int)chunks.size() - 1; i >= 0; i--) {
		Chunk &c = chunks[i];
		if (c.gen_queued) continue; // its generate job is still queued; the next scan drops it
		Vector3 center = c.offset + Vector3(0.5, 0.5, 0.5) * cs;
		bool wanted = false;
		for (const Viewer &v : active_viewers) {
			float dx = center.x - v.pos.x, dz = center.z - v.pos.z, dy = std::fabs(center.y - v.pos.y);
			float rh = v.view_distance + margin, rv = v.vertical_view_distance + margin;
			if (dx * dx + dz * dz <= rh * rh && dy <= rv) {
				wanted = true;
				break;
			}
		}
		if (wanted) continue;
		store_chunk(c.key);
		vol->remove_chunk(c.key.x, c.key.y, c.key.z);
		free_chunk(c);
		chunk_lookup.erase(c.key);
		if (i < (int)chunks.size() - 1) { // swap-remove; queued jobs carry a stale index and re-find by key
			chunks[i] = std::move(chunks.back());
			chunk_lookup[chunks[i].key] = i;
		}
		chunks.pop_back();
	}

	for (const Viewer &v : active_viewers) {
		const float rh = v.view_distance + margin, rv = v.vertical_view_distance + margin, rh2 = rh * rh;
		const int x0 = (int)std::floor((v.pos.x - rh) / cs), x1 = (int)std::floor((v.pos.x + rh) / cs);
		const int z0 = (int)std::floor((v.pos.z - rh) / cs), z1 = (int)std::floor((v.pos.z + rh) / cs);
		const int y0 = (int)std::floor((v.pos.y - rv) / cs), y1 = (int)std::floor((v.pos.y + rv) / cs);
		for (int z = z0; z <= z1; z++)
			for (int y = y0; y <= y1; y++)
				for (int x = x0; x <= x1; x++) {
					dkv::ChunkKey key(x, y, z);
					int32_t idx;
					auto it = chunk_lookup.find(key);
					if (it != chunk_lookup.end()) {
						Chunk &c = chunks[it->second];
						if (c.gen_queued || vol->has_chunk(x, y, z)) continue;
						c.gen_queued = true;
						idx = it->second;
					} else {
						dkv::Vec3 o = vol->chunk_origin(x, y, z);
						Vector3 offset((float)o.x, (float)o.y, (float)o.z), center = offset + Vector3(0.5, 0.5, 0.5) * cs;
						float dx = center.x - v.pos.x, dz = center.z - v.pos.z, dy = center.y - v.pos.y;
						if (dx * dx + dz * dz > rh2 || std::fabs(dy) > rv) continue;
						Chunk nc;
						nc.key = key;
						nc.offset = offset;
						nc.gen_queued = true;
						idx = (int32_t)chunks.size();
						chunk_lookup[key] = idx;
						chunks.push_back(std::move(nc));
					}
					Job j;
					j.gen = generation;
					j.is_gen = true;
					j.key = key;
					Vector3 gc = chunks[idx].offset + Vector3(0.5, 0.5, 0.5) * cs;
					j.prio = active_viewers[0].pos.distance_to(gc); // nearest viewer first
					for (size_t vi = 1; vi < active_viewers.size(); vi++) j.prio = MIN(j.prio, active_viewers[vi].pos.distance_to(gc));
					j.index = idx;
					auto sit = store.find(key);
					if (sit != store.end()) {
						j.stored = sit->second.data;
						j.stored_size = sit->second.raw_size;
					}
					gen.push_back(std::move(j));
				}
	}
	if (gen.empty()) return;
	{
		std::lock_guard lk(qm);
		for (Job &j : gen) jobs.push(std::move(j));
	}
	qcv.notify_all();
}

// Pick each chunk's LOD from its distance to the nearest viewer and queue whatever is out of date.
void VoxelWorld::scan() {
	const float cs = dkv::CHUNK * vol->cell, hyst = cs * 0.5f;
	auto lod_at = [&](float d) { return d < lod1_distance ? 0 : d < lod2_distance ? 1 : 2; };

	if (!vol->bounded) stream_endless();

	std::vector<Job> add;
	for (int i = 0; i < (int)chunks.size(); i++) {
		Chunk &c = chunks[i];
		if (c.gen_queued || !vol->has_chunk(c.key.x, c.key.y, c.key.z)) continue;
		Vector3 center = c.offset + Vector3(0.5, 0.5, 0.5) * cs;
		float d = 0.0f;
		bool in_mesh = vol->bounded, want_coll = false;
		for (size_t v = 0; v < active_viewers.size(); v++) {
			const Viewer &vw = active_viewers[v];
			float dx = center.x - vw.pos.x, dz = center.z - vw.pos.z, dy = center.y - vw.pos.y;
			float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (v == 0 || dist < d) d = dist;
			if (dx * dx + dz * dz <= vw.view_distance * vw.view_distance && std::fabs(dy) <= vw.vertical_view_distance) in_mesh = true;
			if (dist < (c.collision ? vw.collision_distance + hyst : vw.collision_distance)) want_coll = true;
		}
		if (!in_mesh) {
			if (c.lod < 0) continue; // margin ring: loaded so inner chunks have neighbours, never meshed
			want_coll = false;
		}
		if (!vol->neighbors_ready(c.key.x, c.key.y, c.key.z)) continue;
		int want;
		if (!in_mesh) {
			want = c.lod; // keep what is on screen until the chunk is unloaded
		} else {
			int lo = lod_at(d - hyst), hi = lod_at(d + hyst); // hysteresis: keep the current LOD while it is plausible
			want = (c.lod == lo || c.lod == hi) ? c.lod : lo;
		}
		if (!want_coll && c.body.is_valid()) free_chunk(c, true);
		bool stale = c.applied_version != c.version;
		if (!stale && c.lod == want && (c.collision || !want_coll)) continue;
		if (c.queued && c.queued_version == c.version) continue;
		c.queued = true;
		c.queued_version = c.version;
		bool urgent = stale && c.lod >= 0; // an edit to something already on screen
		Job j;
		j.gen = generation;
		j.key = c.key;
		j.prio = urgent ? d - 1e6f : d;
		j.index = i;
		j.lod = (int8_t)want;
		j.collision = want_coll;
		j.urgent = urgent;
		j.version = c.version;
		add.push_back(std::move(j));
	}
	need_scan = false;
	if (add.empty()) return;
	{
		std::lock_guard lk(qm);
		for (Job &j : add) jobs.push(std::move(j));
	}
	qcv.notify_all();
}

void VoxelWorld::apply(Result &r) {
	if (r.job.gen != generation) return; // queued before a reset(): never touch the new world
	if (r.job.is_gen) {
		if (Chunk *c = find_chunk(r.job.key.x, r.job.key.y, r.job.key.z)) c->gen_queued = false;
		if (r.has && !r.job.stored.is_empty()) store.erase(r.job.key); // loaded again: drop our copy
		need_scan = true; // a new chunk may complete a neighbour's 26
		return;
	}
	Chunk *cp = (r.job.index >= 0 && r.job.index < (int)chunks.size() && chunks[r.job.index].key == r.job.key)
			? &chunks[r.job.index]
			: find_chunk(r.job.key.x, r.job.key.y, r.job.key.z);
	if (!cp) return; // unloaded while the job ran
	Chunk &c = *cp;
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
	bool changed = false;
	AABB edited; // the shape's local [-half, half] box, in global space
	if (vol) {
		Transform3D l = gx().affine_inverse() * xf;
		Basis b = l.basis.orthonormalized();
		const float B[9] = {(float)b[0][0], (float)b[0][1], (float)b[0][2], (float)b[1][0], (float)b[1][1], (float)b[1][2], (float)b[2][0], (float)b[2][1], (float)b[2][2]};
		dkv::EditResult r = vol->edit(s, m, {(float)l.origin.x, (float)l.origin.y, (float)l.origin.z}, {(float)half.x, (float)half.y, (float)half.z}, B, (uint8_t)CLAMP(mat, 0, 255), power, strength, sharp);
		for (int z = r.cmin[2]; z <= r.cmax[2]; z++)
			for (int y = r.cmin[1]; y <= r.cmax[1]; y++)
				for (int x = r.cmin[0]; x <= r.cmax[0]; x++)
					if (Chunk *ch = find_chunk(x, y, z)) ch->version++;
		need_scan |= r.changed;
		const float vv = vol->cell * vol->cell * vol->cell;
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
	generation++; // results still in flight belong to the world we are dropping
	stop_workers();
	for (Chunk &c : chunks) free_chunk(c);
	chunks.clear();
	chunk_lookup.clear();
	store.clear();
	vol.reset();
	active_viewers.clear();
	ensure_volume();
	start_workers();
}

void VoxelWorld::_process(double) {
	if (!vol || workers.empty()) return;
	update_viewers();
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

bool VoxelWorld::is_area_ready(const AABB &aabb, bool need_collision) {
	ensure_volume();
	if (!vol || workers.empty() || active_viewers.empty()) return false;
	if (need_scan) scan();
	Transform3D inv = gx().affine_inverse();
	Vector3 p0 = inv.xform(aabb.position), p1 = inv.xform(aabb.position + aabb.size); // the world node may be rotated
	Vector3 lmin = p0.min(p1), lmax = p0.max(p1);
	const float cs = dkv::CHUNK * vol->cell;
	int cmin[3], cmax[3];
	for (int i = 0; i < 3; i++) {
		cmin[i] = (int)std::floor(lmin[i] / cs);
		cmax[i] = (int)std::floor((lmax[i] - 1e-4f) / cs);
		if (vol->bounded) {
			cmin[i] = MAX(cmin[i], vol->bmin[i]);
			cmax[i] = MIN(cmax[i], vol->bmax[i] - 1);
		}
	}
	const float hyst = cs * 0.5f;
	auto lod_at = [&](float d) { return d < lod1_distance ? 0 : d < lod2_distance ? 1 : 2; };
	for (int z = cmin[2]; z <= cmax[2]; z++)
		for (int y = cmin[1]; y <= cmax[1]; y++)
			for (int x = cmin[0]; x <= cmax[0]; x++) {
				if (!vol->has_chunk(x, y, z)) return false;
				Chunk *c = find_chunk(x, y, z);
				if (!c || c->gen_queued || c->queued || c->applied_version != c->version || c->lod < 0) return false;
				Vector3 center = c->offset + Vector3(0.5, 0.5, 0.5) * cs;
				float d = 0.0f;
				for (size_t v = 0; v < active_viewers.size(); v++) {
					float dist = active_viewers[v].pos.distance_to(center);
					if (v == 0 || dist < d) d = dist;
				}
				int lo = lod_at(d - hyst), hi = lod_at(d + hyst);
				int want = (c->lod == lo || c->lod == hi) ? c->lod : lo;
				if (c->lod != want) return false;
				if (need_collision && c->tris > 0 && (!c->collision || !c->body.is_valid())) return false;
			}
	return true;
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
	d["chunks"] = (int64_t)(vol ? vol->chunk_count() : 0);
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