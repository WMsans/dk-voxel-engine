// The far field: nested coarse copies of an endless world, so the view distance doubles with every level while
// each level costs about the same (the clipmap / octree-LOD scheme of voxy, Distant Horizons and godot_voxel's
// VoxelLodTerrain). Level 0 is the near volume with collision and saves; level k >= 1 has cell * 2^(k+1), chunks
// of 16 such cells, and reaches view_distance * 2^k. Which level is on screen is decided per chunk: a chunk is
// split, and draws its children instead of itself, only once they are all meshed, so the world never shows
// a hole while the finer level streams in. chunk_changed() keeps that up to date from the two events that can
// change it, a chunk's first mesh and its unloading, so the cost follows what streams, not what is loaded.
#include "voxel_world.h"
#include "voxel_world_state.h"

#include <godot_cpp/classes/rendering_server.hpp>

#include <cmath>

using namespace godot;

namespace dkv {

namespace {

// Children of a level-k chunk along one axis. Far chunks are twice as wide as a plain octree's (cell * 2^(k+1),
// every cell meshed), so the same reach takes an eighth of the chunks to generate, stream and draw: a level-1
// chunk covers 4x4x4 near chunks, and every coarser one 2x2x2 of the level below.
int fan(int k) { return k == 1 ? 4 : 2; }

float level_size(const WorldState &st, int k) { return CHUNK * st.vol->cell * (float)(k ? 2 << k : 1); }

Vector3 center(const WorldState &st, int k, const ChunkKey &key) {
	return Vector3(key.x + 0.5f, key.y + 0.5f, key.z + 0.5f) * level_size(st, k);
}

// The level-(k + 1) chunk over a level-k chunk, and the i-th of the fan(k)^3 level-(k - 1) chunks under one.
ChunkKey parent(int k, const ChunkKey &key) {
	const int f = fan(k + 1);
	return ChunkKey(floordiv(key.x, f), floordiv(key.y, f), floordiv(key.z, f));
}
ChunkKey child(int k, const ChunkKey &key, int i) {
	const int f = fan(k);
	return ChunkKey(key.x * f + i % f, key.y * f + i / f % f, key.z * f + i / (f * f));
}
int children(int k) { return fan(k) * fan(k) * fan(k); }

// Level k reaches view_distance * 2^k across and vertical_view_distance + 2 chunks up and down: enough that the
// chunks around one that splits are meshed too, so the level above can split in turn. Level 0 is exactly the near
// field scan() meshes.
bool in_reach(const WorldState &st, int k, Vector3 c, float margin = 0.0f) {
	const float s = level_size(st, k);
	for (const WorldState::Viewer &v : st.active_viewers) {
		const float r = v.view_distance * (float)(1 << k) + margin, h = v.vertical_view_distance + (k ? 2.0f * s : 0.0f) + margin;
		const float dx = c.x - v.pos.x, dz = c.z - v.pos.z;
		if (dx * dx + dz * dz <= r * r && std::fabs(c.y - v.pos.y) <= h) return true;
	}
	return false;
}

WorldState::Chunk *find(WorldState &st, int k, const ChunkKey &key) {
	if (k == 0) {
		auto it = st.chunk_lookup.find(key);
		return it == st.chunk_lookup.end() ? nullptr : &st.chunks[it->second];
	}
	auto it = st.far[k - 1].chunks.find(key);
	return it == st.far[k - 1].chunks.end() ? nullptr : &it->second;
}

// Every child of a level-k chunk is meshed (an empty mesh counts), so they can stand in for it.
bool children_meshed(WorldState &st, int k, const ChunkKey &key) {
	if (k == 0) return false;
	for (int i = 0, n = children(k); i < n; i++) {
		const WorldState::Chunk *c = find(st, k - 1, child(k, key, i));
		if (!c || c->lod < 0) return false;
	}
	return true;
}

// Every ancestor of a level-k chunk is split, so its level is the one drawn there.
bool active(WorldState &st, int k, ChunkKey key) {
	if (!st.shown) return false;
	for (; k < (int)st.far.size(); k++) {
		key = parent(k, key);
		const WorldState::Chunk *p = find(st, k + 1, key);
		if (!p || !p->split) return false;
	}
	return true;
}

void show(WorldState::Chunk &c, bool v) {
	if (c.instance.is_valid() && v != c.visible) RenderingServer::get_singleton()->instance_set_visible(c.instance, c.visible = v);
}

// Draws the subtree of a level-k chunk: the chunk itself, or its children where it is split. Chunks below one
// that is not split are hidden already, so this only walks what is or was on screen.
void refresh(WorldState &st, int k, const ChunkKey &key, bool on) {
	WorldState::Chunk *c = find(st, k, key);
	if (!c) return;
	show(*c, on && !c->split);
	if (c->split)
		for (int i = 0, n = children(k); i < n; i++) refresh(st, k - 1, child(k, key, i), on);
}

// Splits or merges a level-k chunk after one of its children was meshed or unloaded.
void resplit(WorldState &st, int k, const ChunkKey &key) {
	WorldState::Chunk *c = find(st, k, key);
	if (!c) return;
	const bool split = children_meshed(st, k, key);
	if (split == c->split) return;
	c->split = split;
	const bool on = active(st, k, key);
	show(*c, on && !split);
	for (int i = 0, n = children(k); i < n; i++) refresh(st, k - 1, child(k, key, i), on && split);
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
	for (size_t i = 0; i < st.far.size(); i++) st.far[i].vol = std::make_unique<Volume>(level_size(st, (int)i + 1) / CHUNK);
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

static void queue_far_mesh(WorldState &st, int k, WorldState::Chunk &c, std::vector<WorldState::Job> &out) {
	WorldState::FarLevel &L = st.far[k - 1];
	const ChunkKey &key = c.key;
	if (c.gen_queued || (c.queued && c.queued_version == c.version) || (c.lod >= 0 && c.applied_version == c.version)) return;
	const Vector3 ctr = center(st, k, key);
	c.out_of_reach = !in_reach(st, k, ctr);
	if (c.out_of_reach || !L.vol->neighbors_ready(key.x, key.y, key.z)) return; // a neighbour's generate result retries
	c.queued = true;
	c.queued_version = c.version;
	WorldState::Job j;
	j.gen = st.generation;
	j.level = (int8_t)k;
	j.key = key;
	j.lod = 0; // the level's coarse cell is the level of detail
	j.version = c.version;
	j.prio = priority(st, k, ctr);
	out.push_back(std::move(j));
}

void queue_far_mesh(WorldState &st, int k, const ChunkKey &key, std::vector<WorldState::Job> &out) {
	auto it = st.far[k - 1].chunks.find(key);
	if (it != st.far[k - 1].chunks.end()) queue_far_mesh(st, k, it->second, out);
}

void chunk_changed(WorldState &st, int k, const ChunkKey &key, WorldState::Chunk *c) {
	if (k && c && !c->split) resplit(st, k, key); // its children may all have been meshed before it was loaded
	if (c) show(*c, !c->split && active(st, k, key));
	if (k == (int)st.far.size()) return;
	// A meshed chunk can only complete its parent's split and a missing one can only undo it: no recount otherwise.
	const ChunkKey pk = parent(k, key);
	const WorldState::Chunk *p = find(st, k + 1, pk);
	if (p && p->split != (c && c->lod >= 0)) resplit(st, k + 1, pk);
}

void refresh_visibility(WorldState &st) {
	if (st.far.empty()) {
		for (WorldState::Chunk &c : st.chunks) show(c, st.shown);
		return;
	}
	for (auto &[key, c] : st.far.back().chunks) refresh(st, (int)st.far.size(), key, st.shown);
}

int visibility_errors(WorldState &st) {
	const int K = (int)st.far.size();
	int bad = 0;
	auto check = [&](int k, const WorldState::Chunk &c) {
		if (!c.instance.is_valid()) return;
		bool want = st.shown && !children_meshed(st, k, c.key);
		ChunkKey key = c.key;
		for (int l = k; want && l < K; l++) {
			key = parent(l, key);
			want = find(st, l + 1, key) && children_meshed(st, l + 1, key);
		}
		bad += want != c.visible;
	};
	for (const WorldState::Chunk &c : st.chunks) check(0, c);
	for (int k = 1; k <= K; k++)
		for (const auto &[key, c] : st.far[k - 1].chunks) check(k, c);
	return bad;
}

bool stream_far(WorldState &st) {
	for (int k = 1; k <= (int)st.far.size(); k++) {
		WorldState::FarLevel &L = st.far[k - 1];
		const float s = level_size(st, k);
		std::vector<Vector3i> at;
		for (const WorldState::Viewer &v : st.active_viewers) at.push_back(Vector3i((v.pos / s).floor()));
		if (at == L.streamed_at) continue; // coarse levels move rarely: 2^k times less often than the near field
		L.streamed_at = at;
		std::vector<WorldState::Job> jobs;
		std::vector<ChunkKey> gone;
		for (auto it = L.chunks.begin(); it != L.chunks.end();) {
			WorldState::Chunk &c = it->second;
			if (c.gen_queued || in_reach(st, k, center(st, k, it->first), s)) { // the margin ring feeds the mesher's border
				if (c.out_of_reach) queue_far_mesh(st, k, c, jobs); // a margin chunk that may have come into reach
				++it;
				continue;
			}
			gone.push_back(it->first);
			free_chunk(st, c);
			it = L.chunks.erase(it);
			chunk_changed(st, k, gone.back());
		}
		L.vol->remove_chunks(gone);
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
						if (!in_reach(st, k, ctr, s)) continue;
						auto [it, fresh] = L.chunks.try_emplace(key);
						if (!fresh) continue;
						WorldState::Chunk &nc = it->second;
						nc.key = key;
						nc.offset = ctr - Vector3(0.5f, 0.5f, 0.5f) * s;
						nc.gen_queued = true;
						WorldState::Job j;
						j.gen = st.generation;
						j.is_gen = true;
						j.level = (int8_t)k;
						j.key = key;
						j.prio = priority(st, k, ctr);
						jobs.push_back(std::move(j));
					}
		}
		push_jobs(st, jobs);
		return true; // one level a frame: aligned chunk borders make several levels restream at once
	}
	return false;
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
			chunk_changed(st, k, c.key, &c);
		}
		queue_far_mesh(st, k, c.key, jobs); // edited again while it was meshing
	}
	push_jobs(st, jobs);
}

} // namespace dkv
