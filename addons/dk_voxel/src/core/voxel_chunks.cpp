// Chunk RID lifecycle: uploading a meshed chunk to the RenderingServer and, near the viewer, to
// PhysicsServer3D, and freeing both again.
#include "voxel_world.h"
#include "voxel_world_state.h"

#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/world3d.hpp>

#include <algorithm>
#include <cstring>

using namespace godot;

namespace dkv {

namespace {

int pow2_at_least(int n, int lo) {
	int v = lo;
	while (v < n) v <<= 1;
	return v;
}
int log2i(int v) {
	int l = 0;
	while ((1 << l) < v) l++;
	return l;
}
uint64_t pool_key(int vcap, int icap) { return (uint64_t)log2i(vcap) << 32 | (uint32_t)icap; }

// A mesh with one surface of `vcap` vertices and `icap` indices, all zero, and an instance drawing it. The only
// place a chunk's GPU buffers are allocated: allocation can stall the frame for milliseconds, so chunks recycle
// these through st.mesh_pool and rewrite them in place.
WorldState::PooledMesh create_pooled(WorldState &st, VoxelWorld &world, int vcap, int icap) {
	RenderingServer *rs = RenderingServer::get_singleton();
	PackedVector3Array v, n;
	PackedColorArray col;
	PackedInt32Array idx;
	v.resize(vcap);
	n.resize(vcap);
	col.resize(vcap);
	idx.resize(icap);
	n.fill(Vector3(0, 1, 0));
	col.fill(Color());
	idx.fill(0);
	Array arrays;
	arrays.resize(RenderingServer::ARRAY_MAX);
	arrays[RenderingServer::ARRAY_VERTEX] = v;
	arrays[RenderingServer::ARRAY_NORMAL] = n;
	arrays[RenderingServer::ARRAY_COLOR] = col;
	arrays[RenderingServer::ARRAY_INDEX] = idx;
	WorldState::PooledMesh pm;
	pm.mesh = rs->mesh_create();
	rs->mesh_add_surface_from_arrays(pm.mesh, RenderingServer::PRIMITIVE_TRIANGLES, arrays);
	// Godot picks the stored format (it adds a tangent stream, for one); the byte layout is read back from it.
	if (st.mesh_format == 0) st.mesh_format = (uint64_t)(int64_t)rs->mesh_get_surface(pm.mesh, 0)["format"];
	pm.instance = rs->instance_create2(pm.mesh, world.get_world_3d()->get_scenario());
	pm.vcap = vcap;
	pm.icap = icap;
	return pm;
}

} // namespace

void free_mesh_pool(WorldState &st) {
	RenderingServer *rs = RenderingServer::get_singleton();
	for (auto &[key, list] : st.mesh_pool)
		for (WorldState::PooledMesh &pm : list) {
			rs->free_rid(pm.instance);
			rs->free_rid(pm.mesh);
		}
	st.mesh_pool.clear();
	st.mesh_pool_size = 0;
}

// Puts a chunk's mesh and instance back in the pool (hidden, buffers kept), or frees them when the pool is full.
static void release_mesh(WorldState &st, WorldState::Chunk &c) {
	if (!c.mesh.is_valid()) return;
	RenderingServer *rs = RenderingServer::get_singleton();
	st.stat_meshes--;
	if (st.mesh_pool_size < WorldState::MESH_POOL_MAX) {
		rs->instance_set_visible(c.instance, false);
		st.mesh_pool[pool_key(c.vcap, c.icap)].push_back({c.mesh, c.instance, c.vcap, c.icap});
		st.mesh_pool_size++;
	} else {
		rs->free_rid(c.instance);
		rs->free_rid(c.mesh);
	}
	c.instance = c.mesh = RID();
	c.vcap = c.icap = 0;
}

void free_chunk(WorldState &st, WorldState::Chunk &c, bool body_only /* = false */) {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (c.body.is_valid()) {
		ps->free_rid(c.body);
		ps->free_rid(c.shape);
		c.body = c.shape = RID();
		st.stat_bodies--;
	}
	c.collision = false;
	c.coll_dirty = false;
	c.coll_faces = PackedVector3Array();
	if (body_only) return;
	release_mesh(st, c);
	st.stat_tris -= c.tris;
	c.tris = 0;
	c.lod = -1;
}

// Collision is the LOD0 mesh itself. ponytail: only near the viewer, so bodies far from the camera have
// nothing to land on; add per-body collision anchors if that matters.
void upload_chunk(WorldState &st, VoxelWorld &world, WorldState::Chunk &c, const MeshData &m, const Transform3D &xf, bool collision) {
	RenderingServer *rs = RenderingServer::get_singleton();
	const int nv = (int)m.pos.size() / 3, ni = (int)m.idx.size();
	// Vertex capacity is a power of two: unused vertices cost memory only. Every index is drawn, padding included
	// (degenerate triangles), so index capacity steps by an eighth of a power of two: at most 1/8 padding. A mesh is
	// rewritten in place while the new one fits, with up to 1/4 padding; otherwise it swaps for a pooled mesh of
	// the right size, or on a miss a new one.
	const int tris = (ni + 2) / 3, step = std::max(32, pow2_at_least(tris, 1) / 8);
	const int vneed = pow2_at_least(nv, 256), ineed = 3 * ((tris + step - 1) / step * step);
	if (c.mesh.is_valid() && (c.vcap < nv || c.vcap > 4 * vneed || c.icap < ni || c.icap > ineed + ineed / 4)) release_mesh(st, c);
	if (!c.mesh.is_valid()) {
		WorldState::PooledMesh pm;
		bool found = false;
		for (int dv = 0; dv <= 1 && !found; dv++) { // the exact index class, vertices exact or one step bigger
			auto it = st.mesh_pool.find(pool_key(vneed << dv, ineed));
			if (it == st.mesh_pool.end() || it->second.empty()) continue;
			pm = it->second.back();
			it->second.pop_back();
			st.mesh_pool_size--;
			found = true;
		}
		if (!found) pm = create_pooled(st, world, vneed, ineed);
		c.mesh = pm.mesh;
		c.instance = pm.instance;
		c.vcap = pm.vcap;
		c.icap = pm.icap;
		st.stat_meshes++;
		rs->instance_set_transform(c.instance, xf);
		rs->instance_set_visible(c.instance, true);
		c.visible = true;
	}
	if (st.material.is_valid()) rs->mesh_surface_set_material(c.mesh, 0, st.material->get_rid());

	// Rewrite the buffers in Godot's own layout: positions, then normal + tangent (octahedral, 16 bits a
	// component; terrain shaders never read the tangent), colours in the attribute stream, and indices padded
	// with degenerate triangles up to the capacity.
	const int64_t fmt = (int64_t)st.mesh_format;
	const int vstride = rs->mesh_surface_get_format_vertex_stride(fmt, c.vcap);
	const int ntstride = rs->mesh_surface_get_format_normal_tangent_stride(fmt, c.vcap);
	const int noff = rs->mesh_surface_get_format_offset(fmt, c.vcap, RenderingServer::ARRAY_NORMAL);
	const int astride = rs->mesh_surface_get_format_attribute_stride(fmt, c.vcap);
	const int coff = rs->mesh_surface_get_format_offset(fmt, c.vcap, RenderingServer::ARRAY_COLOR);
	const int istride = rs->mesh_surface_get_format_index_stride(fmt, c.vcap);
	AABB aabb;
	if (nv > 0) {
		PackedByteArray pos, nt, attr;
		pos.resize((int64_t)nv * vstride);
		nt.resize((int64_t)nv * ntstride);
		attr.resize((int64_t)nv * astride);
		uint8_t *pw = pos.ptrw(), *nw = nt.ptrw(), *aw = attr.ptrw();
		memset(nw, 0, nt.size());
		Vector3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
		for (int i = 0; i < nv; i++) {
			const Vector3 p(m.pos[i * 3], m.pos[i * 3 + 1], m.pos[i * 3 + 2]);
			memcpy(pw + (size_t)i * vstride, &p, sizeof(float) * 3);
			mn = mn.min(p);
			mx = mx.max(p);
			const Vector2 o = Vector3(m.nrm[i * 3], m.nrm[i * 3 + 1], m.nrm[i * 3 + 2]).octahedron_encode();
			const uint16_t oct[2] = {(uint16_t)CLAMP(o.x * 65535, 0, 65535), (uint16_t)CLAMP(o.y * 65535, 0, 65535)};
			memcpy(nw + (size_t)i * ntstride, oct, 4);
			memcpy(aw + (size_t)i * astride + coff, &m.col[i * 4], 4);
		}
		aabb = AABB(mn, mx - mn);
		rs->mesh_surface_update_vertex_region(c.mesh, 0, 0, pos);
		rs->mesh_surface_update_vertex_region(c.mesh, 0, noff, nt);
		rs->mesh_surface_update_attribute_region(c.mesh, 0, 0, attr);
	}
	PackedByteArray ib;
	ib.resize((int64_t)c.icap * istride);
	uint8_t *iw = ib.ptrw();
	memset(iw, 0, ib.size()); // index 0 three times: a degenerate triangle the GPU drops
	if (istride == 2)
		for (int i = 0; i < ni; i++) ((uint16_t *)iw)[i] = (uint16_t)m.idx[i];
	else
		memcpy(iw, m.idx.data(), (size_t)ni * 4);
	rs->mesh_surface_update_index_region(c.mesh, 0, 0, ib);
	rs->mesh_set_custom_aabb(c.mesh, aabb);
	st.stat_tris += ni / 3 - c.tris;
	c.tris = ni / 3;

	if (!collision) {
		free_chunk(st, c, true);
		return;
	}
	const int nc = (int)std::min(m.idx.size(), m.skirt_from); // skirts are for the eye only
	c.coll_faces.resize(nc);
	Vector3 *fw = c.coll_faces.ptrw();
	const Vector3 *vr = (const Vector3 *)m.pos.data(); // float xyz triples, as Vector3 (real_t = float)
	for (int i = 0; i < nc; i++) fw[i] = vr[m.idx[i]];
	if (!c.coll_dirty) st.coll_queue.push_back(c.key);
	c.coll_dirty = true;
	c.collision = true;
}

static void build_collision(WorldState &st, VoxelWorld &world, WorldState::Chunk &c) {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	Dictionary data;
	data["faces"] = c.coll_faces;
	data["backface_collision"] = false;
	if (!c.body.is_valid()) {
		c.shape = ps->concave_polygon_shape_create();
		ps->shape_set_data(c.shape, data);
		c.body = ps->body_create();
		ps->body_set_mode(c.body, PhysicsServer3D::BODY_MODE_STATIC);
		ps->body_add_shape(c.body, c.shape);
		ps->body_set_collision_layer(c.body, st.collision_layer);
		ps->body_set_collision_mask(c.body, 0);
		ps->body_attach_object_instance_id(c.body, world.get_instance_id());
		ps->body_set_state(c.body, PhysicsServer3D::BODY_STATE_TRANSFORM, st.chunk_transform(world.is_inside_tree() ? world.get_global_transform() : world.get_transform(), c));
		ps->body_set_space(c.body, world.get_world_3d()->get_space());
		st.stat_bodies++;
	} else {
		ps->shape_set_data(c.shape, data);
	}
	c.coll_faces = PackedVector3Array();
	c.coll_dirty = false;
}

int build_collisions(WorldState &st, VoxelWorld &world, uint64_t start_usec, uint64_t budget_usec) {
	if (st.coll_queue.empty()) return 0;
	auto chunk = [&](const ChunkKey &key) -> WorldState::Chunk * {
		if (st.vol->bounded) return st.vol->in_bounds(key.x, key.y, key.z) ? &st.chunks[st.chunk_index(key.x, key.y, key.z)] : nullptr;
		auto it = st.chunk_lookup.find(key);
		return it == st.chunk_lookup.end() ? nullptr : &st.chunks[it->second];
	};
	// Drop entries whose chunk was unloaded or freed meanwhile, then nearest first (the queue is short: an edit's
	// chunks plus what streamed into the collision radius).
	const float cs = CHUNK * st.vol->cell;
	std::vector<std::pair<float, ChunkKey>> order;
	for (const ChunkKey &key : st.coll_queue) {
		WorldState::Chunk *c = chunk(key);
		if (!c || !c->coll_dirty) continue;
		bool seen = false; // freed and uploaded again while queued: one entry per chunk
		for (const auto &o : order) seen |= o.second == key;
		if (seen) continue;
		const Vector3 ctr = c->offset + Vector3(0.5f, 0.5f, 0.5f) * cs;
		float d = 1e30f;
		for (const WorldState::Viewer &v : st.active_viewers) d = std::min(d, v.pos.distance_squared_to(ctr));
		order.push_back({d, key});
	}
	std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
	Time *time = Time::get_singleton();
	size_t i = 0;
	for (; i < order.size(); i++) {
		if (i > 0 && time->get_ticks_usec() - start_usec >= budget_usec) break;
		build_collision(st, world, *chunk(order[i].second));
	}
	st.coll_queue.clear();
	for (; i < order.size(); i++) st.coll_queue.push_back(order[i].second);
	return (int)st.coll_queue.size();
}

} // namespace dkv
