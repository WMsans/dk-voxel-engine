// Chunk RID lifecycle: uploading a meshed chunk to the RenderingServer and, near the viewer, to
// PhysicsServer3D, and freeing both again.
#include "voxel_world.h"
#include "voxel_world_state.h"

#include <godot_cpp/classes/physics_server3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/world3d.hpp>

#include <algorithm>
#include <cstring>

using namespace godot;

namespace dkv {

void free_chunk(WorldState &st, WorldState::Chunk &c, bool body_only /* = false */) {
	RenderingServer *rs = RenderingServer::get_singleton();
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (c.body.is_valid()) {
		ps->free_rid(c.body);
		ps->free_rid(c.shape);
		c.body = c.shape = RID();
		st.stat_bodies--;
	}
	c.collision = false;
	if (body_only) return;
	if (c.instance.is_valid()) rs->free_rid(c.instance);
	if (c.mesh.is_valid()) {
		rs->free_rid(c.mesh);
		st.stat_meshes--;
	}
	c.instance = c.mesh = RID();
	st.stat_tris -= c.tris;
	c.tris = 0;
	c.lod = -1;
}

// Collision is the LOD0 mesh itself. ponytail: only near the viewer, so bodies far from the camera have
// nothing to land on; add per-body collision anchors if that matters.
void upload_chunk(WorldState &st, VoxelWorld &world, WorldState::Chunk &c, const MeshData &m, const Transform3D &xf, bool collision) {
	RenderingServer *rs = RenderingServer::get_singleton();
	const int nv = (int)m.pos.size() / 3, ni = (int)m.idx.size();
	PackedVector3Array verts, normals;
	PackedColorArray colors;
	PackedInt32Array indices;
	verts.resize(nv);
	normals.resize(nv);
	colors.resize(nv);
	indices.resize(ni);
	memcpy(verts.ptrw(), m.pos.data(), nv * sizeof(Vector3));
	memcpy(normals.ptrw(), m.nrm.data(), nv * sizeof(Vector3));
	memcpy(indices.ptrw(), m.idx.data(), ni * sizeof(int32_t));
	Color *cw = colors.ptrw();
	for (int i = 0; i < nv; i++) cw[i] = Color(m.col[i * 4] / 255.0f, m.col[i * 4 + 1] / 255.0f, m.col[i * 4 + 2] / 255.0f, m.col[i * 4 + 3] / 255.0f);
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
		st.stat_meshes++;
	}
	rs->mesh_add_surface_from_arrays(c.mesh, RenderingServer::PRIMITIVE_TRIANGLES, arrays);
	if (st.material.is_valid()) rs->mesh_surface_set_material(c.mesh, 0, st.material->get_rid());
	if (!c.instance.is_valid()) {
		c.instance = rs->instance_create2(c.mesh, world.get_world_3d()->get_scenario());
		rs->instance_set_transform(c.instance, xf);
		c.visible = true;
	}
	st.stat_tris += ni / 3 - c.tris;
	c.tris = ni / 3;

	if (!collision) {
		free_chunk(st, c, true);
		return;
	}
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	PackedVector3Array faces;
	const int nc = (int)std::min(m.idx.size(), m.skirt_from); // skirts are for the eye only
	faces.resize(nc);
	Vector3 *fw = faces.ptrw();
	const Vector3 *vr = verts.ptr();
	for (int i = 0; i < nc; i++) fw[i] = vr[m.idx[i]];
	Dictionary data;
	data["faces"] = faces;
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
		ps->body_set_state(c.body, PhysicsServer3D::BODY_STATE_TRANSFORM, xf);
		ps->body_set_space(c.body, world.get_world_3d()->get_space());
		st.stat_bodies++;
	} else {
		ps->shape_set_data(c.shape, data);
	}
	c.collision = true;
}

} // namespace dkv
