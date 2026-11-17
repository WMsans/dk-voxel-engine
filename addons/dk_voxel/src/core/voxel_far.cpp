// The far field: nested coarse copies of an endless world, so the view distance doubles with every level while
// each level costs about the same (the clipmap / octree-LOD scheme of voxy, Distant Horizons and godot_voxel's
// VoxelLodTerrain). Level 0 is the near volume with collision and saves; level k >= 1 has cell * 2^k, chunks of
// 16 * 2^k cells, and reaches view_distance * 2^k. Which level is on screen is decided per chunk in
// update_visibility(): a chunk hands over to its 8 children only once they are all in reach and meshed, so the
// world never shows a hole while the finer level streams in.
#include "voxel_world.h"
#include "voxel_world_state.h"

#include <godot_cpp/classes/rendering_server.hpp>

#include <cmath>

using namespace godot;

namespace dkv {

namespace {

constexpr int FAR_LOD = 1; // mesher LOD for far chunks: a level-k cell already spans 2^k near cells

float level_size(const WorldState &st, int k) { return CHUNK * st.vol->cell * (float)(1 << k); }

Vector3 center(const WorldState &st, int k, const ChunkKey &key) {
	return Vector3(key.x + 0.5f, key.y + 0.5f, key.z + 0.5f) * level_size(st, k);
}

ChunkKey parent(const ChunkKey &k) { return ChunkKey(floordiv(k.x, 2), floordiv(k.y, 2), floordiv(k.z, 2)); }
ChunkKey child(const ChunkKey &k, int i) { return ChunkKey(k.x * 2 + (i & 1), k.y * 2 + (i >> 1 & 1), k.z * 2 + (i >> 2)); }

// Level k reaches view_distance * 2^k across and vertical_view_distance + 2 chunks up and down: enough that when all
// 8 children of a chunk are in reach of level k - 1, all of its siblings are in reach of level k, so the hand-over
// never skips a level. Level 0 is exactly the near field scan() meshes.
bool in_reach(const WorldState &st, int k, Vector3 c, float margin = 0.0f) {
	const float s = level_size(st, k);
	for (const WorldState::Viewer &v : st.active_viewers) {
		const float r = v.view_distance * (float)(1 << k) + margin, h = v.vertical_view_distance + (k ? 2.0f * s : 0.0f) + margin;
		const float dx = c.x - v.pos.x, dz = c.z - v.pos.z;
		if (dx * dx + dz * dz <= r * r && std::fabs(c.y - v.pos.y) <= h) return true;
	}
	return false;
}

const WorldState::Chunk *find(const WorldState &st, int k, const ChunkKey &key) {
	if (k == 0) {
		auto it = st.chunk_lookup.find(key);
		return it == st.chunk_lookup.end() ? nullptr : &st.chunks[it->second];
	}
	auto it = st.far[k - 1].chunks.find(key);
	return it == st.far[k - 1].chunks.end() ? nullptr : &it->second;
}

// Inner chunks of a level are only seen once the viewer walks away, but they are queued last.
float priority(const WorldState &st, int k, Vector3 c) {
	float d = 1e30f;
	for (const WorldState::Viewer &v : st.active_viewers) d = MIN(d, v.pos.distance_to(c));
	return MAX(d, st.active_viewers.empty() ? 0.0f : st.active_viewers[0].view_distance * (float)(1 << (k - 1)) - level_size(st, k));
}

} // namespace

void create_far(WorldState &st) {
	st.far.clear();
	if (!st.vol || st.vol->bounded) return; // a bounded world is loaded whole
	st.far.resize((size_t)CLAMP(st.far_levels, 0, 12));
	for (size_t i = 0; i < st.far.size(); i++) st.far[i].vol = std::make_unique<Volume>(st.vol->cell * (float)(2 << i));
}

void free_far(WorldState &st) {
	for (WorldState::FarLevel &L : st.far) {
		for (auto &[key, c] : L.chunks) free_chunk(st, c);
		L.chunks.clear();
		L.streamed_at.clear();
	}
}

void push_jobs(WorldState &st, std::vector<WorldState::Job> &jobs) {
	if (jobs.empty()) return;
	{
		std::lock_guard lk(st.qm);
		for (WorldState::Job &j : jobs) st.jobs.push(std::move(j));
	}
	st.qcv.notify_all();
	jobs.clear();
}

void queue_far_mesh(WorldState &st, int k, const ChunkKey &key, std::vector<WorldState::Job> &out) {
	WorldState::FarLevel &L = st.far[k - 1];
	auto it = L.chunks.find(key);
	if (it == L.chunks.end()) return;
	WorldState::Chunk &c = it->second;
	if (c.gen_queued || (c.queued && c.queued_version == c.version) || (c.lod >= 0 && c.applied_version == c.version)) return;
	const Vector3 ctr = center(st, k, key);
	if (!in_reach(st, k, ctr) || !L.vol->neighbors_ready(key.x, key.y, key.z)) return;
	c.queued = true;
	c.queued_version = c.version;
	WorldState::Job j;
	j.gen = st.generation;
	j.level = (int8_t)k;
	j.key = key;
	j.lod = FAR_LOD;
	j.version = c.version;
	j.prio = priority(st, k, ctr);
	out.push_back(std::move(j));
}

void stream_far(WorldState &st) {
	std::vector<WorldState::Job> jobs;
	for (int k = 1; k <= (int)st.far.size(); k++) {
		WorldState::FarLevel &L = st.far[k - 1];
		const float s = level_size(st, k);
		std::vector<Vector3i> at;
		for (const WorldState::Viewer &v : st.active_viewers) at.push_back(Vector3i((v.pos / s).floor()));
		if (at == L.streamed_at) continue; // coarse levels move rarely: 2^k times less often than the near field
		L.streamed_at = at;
		for (auto it = L.chunks.begin(); it != L.chunks.end();) {
			WorldState::Chunk &c = it->second;
			if (c.gen_queued || in_reach(st, k, center(st, k, it->first), s)) { // the margin ring feeds the mesher's border
				++it;
				continue;
			}
			L.vol->remove_chunk(c.key.x, c.key.y, c.key.z);
			free_chunk(st, c);
			it = L.chunks.erase(it);
		}
		for (const WorldState::Viewer &v : st.active_viewers) {
			const float r = v.view_distance * (float)(1 << k) + s, h = v.vertical_view_distance + 3.0f * s;
			const int x0 = (int)std::floor((v.pos.x - r) / s), x1 = (int)std::floor((v.pos.x + r) / s);
			const int z0 = (int)std::floor((v.pos.z - r) / s), z1 = (int)std::floor((v.pos.z + r) / s);
			const int y0 = (int)std::floor((v.pos.y - h) / s), y1 = (int)std::floor((v.pos.y + h) / s);
			for (int z = z0; z <= z1; z++)
				for (int y = y0; y <= y1; y++)
					for (int x = x0; x <= x1; x++) {
						const ChunkKey key(x, y, z);
						const Vector3 ctr = center(st, k, key);
						if (L.chunks.count(key) || !in_reach(st, k, ctr, s)) continue;
						WorldState::Chunk nc;
						nc.key = key;
						nc.offset = ctr - Vector3(0.5f, 0.5f, 0.5f) * s;
						nc.gen_queued = true;
						L.chunks.emplace(key, std::move(nc));
						WorldState::Job j;
						j.gen = st.generation;
						j.is_gen = true;
						j.level = (int8_t)k;
						j.key = key;
						j.prio = priority(st, k, ctr);
						jobs.push_back(std::move(j));
					}
		}
		for (auto &[key, c] : L.chunks) queue_far_mesh(st, k, key, jobs); // margin chunks that came into reach
	}
	push_jobs(st, jobs);
}

void apply_far(WorldState &st, VoxelWorld &world, const Transform3D &gx, WorldState::Result &r) {
	const int k = r.job.level;
	if (k > (int)st.far.size()) return;
	WorldState::FarLevel &L = st.far[k - 1];
	auto it = L.chunks.find(r.job.key);
	if (it == L.chunks.end()) return; // unloaded while the job ran
	WorldState::Chunk &c = it->second;
	std::vector<WorldState::Job> jobs;
	if (r.job.is_gen) {
		c.gen_queued = false;
		for (int i = 0; i < 27; i++) // this chunk may complete its own or a neighbour's 26
			queue_far_mesh(st, k, ChunkKey(c.key.x + i % 3 - 1, c.key.y + i / 3 % 3 - 1, c.key.z + i / 9 - 1), jobs);
	} else if (!c.gen_queued) { // a mesh of a chunk that was unloaded and re-queued for generation is stale
		if (r.job.version == c.queued_version) c.queued = false;
		if (r.job.version >= c.applied_version) {
			c.applied_version = r.job.version;
			if (r.has) upload_chunk(st, world, c, r.mesh, st.chunk_transform(gx, c), false);
			else free_chunk(st, c);
			c.lod = r.job.lod;
		}
		queue_far_mesh(st, k, c.key, jobs); // edited again while it was meshing
	}
	push_jobs(st, jobs);
}

void update_visibility(WorldState &st, bool world_visible) {
	if (!st.vol || !st.need_vis) return;
	const uint32_t dirty = st.need_vis;
	st.need_vis = 0;
	const int K = (int)st.far.size();
	std::vector<std::unordered_map<ChunkKey, bool, ChunkKeyHash>> memo((size_t)K + 1);
	// Level k's chunk hands over to level k - 1: all 8 children are in reach and meshed.
	auto handed_over = [&](int k, const ChunkKey &key) {
		if (k == 0) return false;
		auto [it, fresh] = memo[k].try_emplace(key, true);
		if (fresh)
			for (int i = 0; i < 8 && it->second; i++) {
				const ChunkKey ch = child(key, i);
				const WorldState::Chunk *c = find(st, k - 1, ch);
				it->second = c && !c->gen_queued && c->lod >= 0 && in_reach(st, k - 1, center(st, k - 1, ch));
			}
		return it->second;
	};
	RenderingServer *rs = RenderingServer::get_singleton();
	auto show = [&](int k, WorldState::Chunk &c) {
		if (!c.instance.is_valid()) return;
		bool v = world_visible;
		if (v && !st.vol->bounded)
			v = (k == K ? in_reach(st, K, center(st, K, c.key)) : handed_over(k + 1, parent(c.key))) && !handed_over(k, c.key);
		if (v != c.visible) rs->instance_set_visible(c.instance, c.visible = v);
	};
	if (dirty & 1u)
		for (WorldState::Chunk &c : st.chunks) show(0, c);
	for (int k = 1; k <= K; k++)
		if (dirty >> k & 1u)
			for (auto &[key, c] : st.far[k - 1].chunks) show(k, c);
}

} // namespace dkv
