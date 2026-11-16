// Streaming, LOD and the worker pool: what is loaded around the viewers, at which LOD, and in what order
// the workers mesh it.
#include "voxel_world.h"
#include "voxel_world_state.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <chrono>
#include <cmath>

using namespace godot;

namespace dkv {

void update_viewers(WorldState &st, VoxelWorld &world) {
	const float cs = dkv::CHUNK * st.vol->cell;
	std::vector<dkv::WorldState::Viewer> found;
	if (world.is_inside_tree() && world.get_tree()) {
		TypedArray<Node> group = world.get_tree()->get_nodes_in_group(VIEWER_GROUP);
		for (int i = 0; i < group.size(); i++) {
			VoxelViewer *vv = Object::cast_to<VoxelViewer>(group[i]);
			if (!vv || !vv->is_inside_tree() || !vv->get_enabled() || vv->get_world_3d() != world.get_world_3d()) continue;
			dkv::WorldState::Viewer v;
			v.pos = world.to_local(vv->get_global_position());
			v.chunk = Vector3i((v.pos / cs).floor());
			v.view_distance = vv->get_view_distance();
			v.vertical_view_distance = vv->get_vertical_view_distance();
			v.collision_distance = vv->get_collision_distance();
			found.push_back(v);
		}
	}
	if (found.empty() && world.is_inside_tree() && world.get_viewport())
		if (Camera3D *cam = world.get_viewport()->get_camera_3d()) {
			dkv::WorldState::Viewer v;
			v.pos = world.to_local(cam->get_global_position());
			v.chunk = Vector3i((v.pos / cs).floor());
			v.view_distance = st.lod2_distance + cs;
			v.vertical_view_distance = st.lod2_distance + cs;
			v.collision_distance = st.collision_distance;
			found.push_back(v);
		}
	bool changed = found.size() != st.active_viewers.size();
	for (size_t i = 0; i < found.size() && i < st.active_viewers.size(); i++)
		if (found[i].chunk != st.active_viewers[i].chunk || found[i].view_distance != st.active_viewers[i].view_distance ||
				found[i].vertical_view_distance != st.active_viewers[i].vertical_view_distance ||
				found[i].collision_distance != st.active_viewers[i].collision_distance ||
				found[i].pos.distance_squared_to(st.active_viewers[i].pos) > 4.0f) // 2 m of slack, so LOD and collision follow
			changed = true;
	if (changed) {
		st.active_viewers = std::move(found);
		st.need_scan = st.need_stream = true;
	} else {
		for (size_t i = 0; i < found.size(); i++) st.active_viewers[i].pos = found[i].pos;
	}
}


void start_workers(WorldState &st) {
	if (!st.vol || st.workers.size() || Engine::get_singleton()->is_editor_hint()) return;
	for (int i = 0; i < st.thread_count(); i++) st.workers.emplace_back([&st] { worker(st); });
}


void stop_workers(WorldState &st) {
	{
		std::lock_guard lk(st.qm);
		st.quit = true;
	}
	st.qcv.notify_all();
	for (auto &t : st.workers) t.join();
	st.workers.clear();
	st.quit = false;
	st.jobs = {};
	st.results.clear();
	st.pending.clear();
}


void worker(WorldState &st) {
	for (;;) {
		dkv::WorldState::Result r;
		{
			std::unique_lock lk(st.qm);
			st.qcv.wait(lk, [&] { return st.quit || !st.jobs.empty(); });
			if (st.quit) return;
			r.job = st.jobs.top();
			st.jobs.pop();
		}
		auto t0 = std::chrono::steady_clock::now();
		const dkv::ChunkKey &key = r.job.key;
		if (r.job.is_gen) { // endless: the edited chunk we kept in memory, else the chunk's save file, else generate it
			std::vector<uint8_t> raw;
			bool restored = false;
			if (!r.job.stored.is_empty()) {
				PackedByteArray raw = r.job.stored.decompress((int)r.job.stored_size, FileAccess::COMPRESSION_ZSTD);
				restored = raw.size() == (int64_t)r.job.stored_size && st.vol->deserialize_chunk(key.x, key.y, key.z, (const uint8_t *)raw.ptr(), (size_t)raw.size());
			}
			if (!restored && !r.job.save_dir.is_empty() && read_chunk_file(st, key, r.job.save_dir, r.job.gen_class, raw))
				restored = st.vol->deserialize_chunk(key.x, key.y, key.z, raw.data(), raw.size());
			if (!restored) st.vol->fill_chunk(key.x, key.y, key.z, st.active_generator.get());
			r.has = true;
		} else {
			r.has = st.vol->mesh_chunk(key.x, key.y, key.z, r.job.lod, r.mesh);
		}
		r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		std::lock_guard lk(st.qm);
		st.results.push_back(std::move(r));
	}
}


void stream_endless(WorldState &st) {
	const float cs = dkv::CHUNK * st.vol->cell;
	const float margin = cs;
	std::vector<dkv::WorldState::Job> gen;

	for (int i = (int)st.chunks.size() - 1; i >= 0; i--) {
		dkv::WorldState::Chunk &c = st.chunks[i];
		if (c.gen_queued) continue; // its generate job is still queued; the next scan drops it
		Vector3 center = c.offset + Vector3(0.5, 0.5, 0.5) * cs;
		bool wanted = false;
		for (const dkv::WorldState::Viewer &v : st.active_viewers) {
			float dx = center.x - v.pos.x, dz = center.z - v.pos.z, dy = std::fabs(center.y - v.pos.y);
			float rh = v.view_distance + margin, rv = v.vertical_view_distance + margin;
			if (dx * dx + dz * dz <= rh * rh && dy <= rv) {
				wanted = true;
				break;
			}
		}
		if (wanted) continue;
		store_chunk(st, c.key);
		st.vol->remove_chunk(c.key.x, c.key.y, c.key.z);
		free_chunk(st, c);
		st.chunk_lookup.erase(c.key);
		if (i < (int)st.chunks.size() - 1) { // swap-remove; queued jobs carry a stale index and re-find by key
			st.chunks[i] = std::move(st.chunks.back());
			st.chunk_lookup[st.chunks[i].key] = i;
		}
		st.chunks.pop_back();
	}

	for (const dkv::WorldState::Viewer &v : st.active_viewers) {
		const float rh = v.view_distance + margin, rv = v.vertical_view_distance + margin, rh2 = rh * rh;
		const int x0 = (int)std::floor((v.pos.x - rh) / cs), x1 = (int)std::floor((v.pos.x + rh) / cs);
		const int z0 = (int)std::floor((v.pos.z - rh) / cs), z1 = (int)std::floor((v.pos.z + rh) / cs);
		const int y0 = (int)std::floor((v.pos.y - rv) / cs), y1 = (int)std::floor((v.pos.y + rv) / cs);
		for (int z = z0; z <= z1; z++)
			for (int y = y0; y <= y1; y++)
				for (int x = x0; x <= x1; x++) {
					dkv::ChunkKey key(x, y, z);
					int32_t idx;
					auto it = st.chunk_lookup.find(key);
					if (it != st.chunk_lookup.end()) {
						dkv::WorldState::Chunk &c = st.chunks[it->second];
						if (c.gen_queued || st.vol->has_chunk(x, y, z)) continue;
						c.gen_queued = true;
						idx = it->second;
					} else {
						dkv::Vec3 o = st.vol->chunk_origin(x, y, z);
						Vector3 offset((float)o.x, (float)o.y, (float)o.z), center = offset + Vector3(0.5, 0.5, 0.5) * cs;
						float dx = center.x - v.pos.x, dz = center.z - v.pos.z, dy = center.y - v.pos.y;
						if (dx * dx + dz * dz > rh2 || std::fabs(dy) > rv) continue;
						dkv::WorldState::Chunk nc;
						nc.key = key;
						nc.offset = offset;
						nc.gen_queued = true;
						idx = (int32_t)st.chunks.size();
						st.chunk_lookup[key] = idx;
						st.chunks.push_back(std::move(nc));
					}
					dkv::WorldState::Job j;
					j.gen = st.generation;
					j.is_gen = true;
					j.key = key;
					Vector3 gc = st.chunks[idx].offset + Vector3(0.5, 0.5, 0.5) * cs;
					j.prio = st.active_viewers[0].pos.distance_to(gc); // nearest viewer first
					for (size_t vi = 1; vi < st.active_viewers.size(); vi++) j.prio = MIN(j.prio, st.active_viewers[vi].pos.distance_to(gc));
					j.index = idx;
					auto sit = st.store.find(key);
					if (sit != st.store.end()) {
						j.stored = sit->second.data;
						j.stored_size = sit->second.raw_size;
					}
					j.save_dir = st.save_directory; // the worker reads these, so snapshot them under the queue lock
					j.gen_class = st.generator_class;
					gen.push_back(std::move(j));
				}
	}
	if (gen.empty()) return;
	{
		std::lock_guard lk(st.qm);
		for (dkv::WorldState::Job &j : gen) st.jobs.push(std::move(j));
	}
	st.qcv.notify_all();
}


void scan(WorldState &st) {
	const float cs = dkv::CHUNK * st.vol->cell, hyst = cs * 0.5f;

	if (!st.vol->bounded && st.need_stream) stream_endless(st);
	st.need_stream = false;

	std::vector<dkv::WorldState::Job> add;
	for (int i = 0; i < (int)st.chunks.size(); i++) {
		dkv::WorldState::Chunk &c = st.chunks[i];
		// Not queued = generated: entries start queued and are erased with their volume chunk, so no locked lookup.
		if (c.gen_queued) continue;
		Vector3 center = c.offset + Vector3(0.5, 0.5, 0.5) * cs;
		float d = 0.0f;
		bool in_mesh = st.vol->bounded, want_coll = false;
		for (size_t v = 0; v < st.active_viewers.size(); v++) {
			const dkv::WorldState::Viewer &vw = st.active_viewers[v];
			float dx = center.x - vw.pos.x, dz = center.z - vw.pos.z, dy = center.y - vw.pos.y;
			float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (v == 0 || dist < d) d = dist;
			if (dx * dx + dz * dz <= vw.view_distance * vw.view_distance && std::fabs(dy) <= vw.vertical_view_distance) in_mesh = true;
			// Bodies only exist at LOD0: never ask for one past the LOD1 band, or the chunk is re-queued forever.
			const float coll = MIN(vw.collision_distance, st.lod1_distance - cs);
			if (dist < (c.collision ? coll + hyst : coll)) want_coll = true;
		}
		if (!in_mesh) {
			if (c.lod < 0) continue; // margin ring: loaded so inner chunks have neighbours, never meshed
			want_coll = false;
		}
		int want;
		if (!in_mesh) {
			want = c.lod; // keep what is on screen until the chunk is unloaded
		} else {
			int lo = st.lod_at(d - hyst), hi = st.lod_at(d + hyst); // hysteresis: keep the current LOD while it is plausible
			want = (c.lod == lo || c.lod == hi) ? c.lod : lo;
		}
		if (!want_coll && c.body.is_valid()) free_chunk(st, c, true);
		bool stale = c.applied_version != c.version;
		if (!stale && c.lod == want && (c.collision || !want_coll)) continue;
		if (c.queued && c.queued_version == c.version) continue;
		// Last: 27 locked lookups, so only for the few chunks that would queue a job, never all loaded ones.
		if (!st.vol->neighbors_ready(c.key.x, c.key.y, c.key.z)) continue;
		c.queued = true;
		c.queued_version = c.version;
		bool urgent = stale && c.lod >= 0; // an edit to something already on screen
		dkv::WorldState::Job j;
		j.gen = st.generation;
		j.key = c.key;
		j.prio = urgent ? d - 1e6f : d;
		j.index = i;
		j.lod = (int8_t)want;
		j.collision = want_coll;
		j.urgent = urgent;
		j.version = c.version;
		add.push_back(std::move(j));
	}
	st.need_scan = false;
	if (add.empty()) return;
	{
		std::lock_guard lk(st.qm);
		for (dkv::WorldState::Job &j : add) st.jobs.push(std::move(j));
	}
	st.qcv.notify_all();
}

} // namespace dkv
