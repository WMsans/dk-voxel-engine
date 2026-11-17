#include "voxel_volume.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

namespace dkv {

// One 16^3-cell chunk: 2^3 blocks of 8^3 voxels, each either one uniform voxel or 512 of them.
struct Volume::Chunk {
	std::unique_ptr<Block> b[8]; // null = uniform block
	Voxel uniform[8];
	bool modified = false; // edited since it was generated or loaded
};

Volume::Volume(float cell_size) : cell(cell_size), bounded(false), bmin{0, 0, 0}, bmax{0, 0, 0} {
	for (int i = 0; i < 256; i++) inner[i] = (uint8_t)i;
}

Volume::Volume(int chunks_x, int chunks_y, int chunks_z, float cell_size) :
		cell(cell_size), bounded(true), bmin{0, 0, 0}, bmax{chunks_x, chunks_y, chunks_z} {
	for (int i = 0; i < 256; i++) inner[i] = (uint8_t)i;
	for (int z = 0; z < chunks_z; z++)
		for (int y = 0; y < chunks_y; y++)
			for (int x = 0; x < chunks_x; x++) chunks.emplace(ChunkKey(x, y, z), std::make_unique<Chunk>());
}

Volume::~Volume() = default;

bool Volume::in_bounds(int cx, int cy, int cz) const {
	return !bounded || (cx >= bmin[0] && cx < bmax[0] && cy >= bmin[1] && cy < bmax[1] && cz >= bmin[2] && cz < bmax[2]);
}

bool Volume::in_world(int x, int y, int z) const {
	return !bounded || ((unsigned)(x - 1 - bmin[0] * CHUNK) < (unsigned)((bmax[0] - bmin[0]) * CHUNK - 1) &&
			(unsigned)(y - 1 - bmin[1] * CHUNK) < (unsigned)((bmax[1] - bmin[1]) * CHUNK - 1) &&
			(unsigned)(z - 1 - bmin[2] * CHUNK) < (unsigned)((bmax[2] - bmin[2]) * CHUNK - 1));
}

Volume::Chunk *Volume::chunk_at(int cx, int cy, int cz) {
	auto it = chunks.find(ChunkKey(cx, cy, cz));
	return it == chunks.end() ? nullptr : it->second.get();
}

const Volume::Chunk *Volume::chunk_at(int cx, int cy, int cz) const {
	auto it = chunks.find(ChunkKey(cx, cy, cz));
	return it == chunks.end() ? nullptr : it->second.get();
}

const Block *Volume::block_at(int bx, int by, int bz, Voxel &uniform) const {
	const Chunk *c = chunk_at(bx >> 1, by >> 1, bz >> 1);
	if (!c) {
		uniform = {};
		return nullptr;
	}
	int bi = ((bz & 1) << 2) | ((by & 1) << 1) | (bx & 1);
	uniform = c->uniform[bi];
	return c->b[bi].get();
}

bool Volume::has_chunk(int cx, int cy, int cz) const {
	std::shared_lock lk(mtx);
	return chunks.find(ChunkKey(cx, cy, cz)) != chunks.end();
}

bool Volume::neighbors_ready(int cx, int cy, int cz) const {
	std::shared_lock lk(mtx);
	for (int dz = -1; dz <= 1; dz++)
		for (int dy = -1; dy <= 1; dy++)
			for (int dx = -1; dx <= 1; dx++) {
				int x = cx + dx, y = cy + dy, z = cz + dz;
				if (!in_bounds(x, y, z)) continue; // outside the bounds reads as air, so it is ready
				if (chunks.find(ChunkKey(x, y, z)) == chunks.end()) return false;
			}
	return true;
}

std::vector<ChunkKey> Volume::loaded_chunks() const {
	std::shared_lock lk(mtx);
	std::vector<ChunkKey> out;
	out.reserve(chunks.size());
	for (const auto &kv : chunks) out.push_back(kv.first);
	return out;
}

size_t Volume::chunk_count() const {
	std::shared_lock lk(mtx);
	return chunks.size();
}

// Block order as in block_at(): x, then y, then z. The node compresses the result.
std::vector<uint8_t> Volume::serialize_chunk(int cx, int cy, int cz) const {
	std::shared_lock lk(mtx);
	const Chunk *c = chunk_at(cx, cy, cz);
	if (!c) return {};
	std::vector<uint8_t> out;
	out.reserve(9 + 8 * (1024 + 512 * 9));
	out.push_back(1); // format
	for (int bi = 0; bi < 8; bi++) {
		const Block *p = c->b[bi].get();
		out.push_back(p ? (p->hermite ? 2 : 1) : 0);
		if (!p) {
			out.push_back(c->uniform[bi].d);
			out.push_back(c->uniform[bi].m);
			continue;
		}
		const uint8_t *v = reinterpret_cast<const uint8_t *>(p->v);
		out.insert(out.end(), v, v + sizeof(Block::v));
		if (p->hermite) {
			const uint8_t *h = p->hermite.get();
			out.insert(out.end(), h, h + 512 * 9);
		}
	}
	return out;
}

bool Volume::deserialize_chunk(int cx, int cy, int cz, const uint8_t *data, size_t size) {
	if (!in_bounds(cx, cy, cz) || !data) return false;
	size_t at = 0;
	if (size < 9 || data[at++] != 1) return false; // 1 byte format + 8 kinds
	auto c = std::make_unique<Chunk>();
	for (int bi = 0; bi < 8; bi++) {
		const uint8_t kind = data[at++];
		if (kind > 2) return false;
		if (kind == 0) {
			if (size - at < 2) return false;
			c->uniform[bi] = {data[at], data[at + 1]};
			at += 2;
			continue;
		}
		if (size - at < sizeof(Block::v)) return false;
		auto p = std::make_unique<Block>();
		memcpy(p->v, data + at, sizeof(Block::v));
		at += sizeof(Block::v);
		if (kind == 2) {
			if (size - at < 512 * 9) return false;
			p->hermite.reset(new uint8_t[512 * 9]);
			memcpy(p->hermite.get(), data + at, 512 * 9);
			at += 512 * 9;
		}
		c->b[bi] = std::move(p);
	}
	if (at != size) return false; // trailing bytes: not our layout
	std::unique_lock lk(mtx);
	chunks[ChunkKey(cx, cy, cz)] = std::move(c); // replaces; not modified
	return true;
}

void Volume::remove_chunks(const std::vector<ChunkKey> &keys) {
	if (keys.empty()) return;
	std::unique_lock lk(mtx);
	for (const ChunkKey &k : keys) chunks.erase(k);
}

bool Volume::is_modified(int cx, int cy, int cz) const {
	std::shared_lock lk(mtx);
	const Chunk *c = chunk_at(cx, cy, cz);
	return c && c->modified;
}

void Volume::clear_modified(int cx, int cy, int cz) {
	std::unique_lock lk(mtx);
	if (Chunk *c = chunk_at(cx, cy, cz)) c->modified = false;
}

Vec3 Volume::chunk_origin(int cx, int cy, int cz) const {
	return Vec3((float)cx * CHUNK * cell, (float)cy * CHUNK * cell, (float)cz * CHUNK * cell);
}

void Volume::fill_chunk(int cx, int cy, int cz, const Generator *g) {
	std::unique_ptr<Chunk> c = std::make_unique<Chunk>();
	if (g) {
		const int base[3] = {cx * CHUNK, cy * CHUNK, cz * CHUNK};
		for (int bi = 0; bi < 8; bi++) { // block order: x, then y, then z, as in block_at()
			const int origin[3] = {base[0] + (bi & 1) * BLOCK, base[1] + ((bi >> 1) & 1) * BLOCK, base[2] + ((bi >> 2) & 1) * BLOCK};
			Voxel tmp[512];
			g->generate_block(origin, cell, tmp);
			bool same = true;
			for (int i = 1; i < 512 && same; i++) same = tmp[i].d == tmp[0].d && (tmp[i].m == tmp[0].m || tmp[0].d == 0);
			if (same) {
				c->uniform[bi] = tmp[0];
			} else {
				c->b[bi] = std::make_unique<Block>();
				memcpy(c->b[bi]->v, tmp, sizeof(tmp));
			}
		}
	}
	std::unique_lock lk(mtx);
	chunks[ChunkKey(cx, cy, cz)] = std::move(c); // replaces an existing chunk
}

void Volume::fill_all(const Generator *g, int threads) {
	const int nx = bmax[0] - bmin[0], ny = bmax[1] - bmin[1], nz = bmax[2] - bmin[2];
	const int total = nx * ny * nz;
	std::atomic<int> next{0};
	auto work = [&]() {
		for (int i; (i = next.fetch_add(1)) < total;) {
			int x = bmin[0] + i % nx, y = bmin[1] + (i / nx) % ny, z = bmin[2] + i / (nx * ny);
			fill_chunk(x, y, z, g);
		}
	};
	std::vector<std::thread> pool;
	for (int i = 1; i < threads; i++) pool.emplace_back(work);
	work();
	for (auto &t : pool) t.join();
}

Voxel *Volume::writable(int x, int y, int z) {
	if (bounded && ((unsigned)(x - bmin[0] * CHUNK) >= (unsigned)((bmax[0] - bmin[0]) * CHUNK) ||
			   (unsigned)(y - bmin[1] * CHUNK) >= (unsigned)((bmax[1] - bmin[1]) * CHUNK) ||
			   (unsigned)(z - bmin[2] * CHUNK) >= (unsigned)((bmax[2] - bmin[2]) * CHUNK)))
		return nullptr;
	Chunk *c = chunk_at(x >> 4, y >> 4, z >> 4);
	if (!c) return nullptr; // never creates a chunk
	int bi = (((z >> 3) & 1) << 2) | (((y >> 3) & 1) << 1) | ((x >> 3) & 1);
	if (!c->b[bi]) {
		c->b[bi] = std::make_unique<Block>();
		std::fill_n(c->b[bi]->v, 512, c->uniform[bi]);
	}
	c->modified = true;
	return &c->b[bi]->v[vidx(x, y, z)];
}

uint8_t *Volume::hermite_at(int x, int y, int z, bool create) {
	Chunk *c = chunk_at(x >> 4, y >> 4, z >> 4);
	if (!c) return nullptr;
	int bi = (((z >> 3) & 1) << 2) | (((y >> 3) & 1) << 1) | ((x >> 3) & 1);
	Block *p = c->b[bi].get();
	if (!p) {
		if (!create) return nullptr;
		writable(x, y, z);
		p = c->b[bi].get();
	}
	if (!p->hermite) {
		if (!create) return nullptr;
		p->hermite.reset(new uint8_t[512 * 9]());
	}
	return p->hermite.get() + vidx(x, y, z) * 9;
}

Voxel Volume::get(int x, int y, int z) const {
	std::shared_lock lk(mtx);
	return get_unlocked(x, y, z);
}

Voxel Volume::get_unlocked(int x, int y, int z) const {
	if (!in_world(x, y, z)) return {};
	const Chunk *c = chunk_at(x >> 4, y >> 4, z >> 4);
	if (!c) return {};
	int bi = (((z >> 3) & 1) << 2) | (((y >> 3) & 1) << 1) | ((x >> 3) & 1);
	const Block *p = c->b[bi].get();
	return p ? p->v[vidx(x, y, z)] : c->uniform[bi];
}

// ---------------------------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------------------------

EditResult Volume::edit(Shape shape, Mode mode, Vec3 c, Vec3 half, const float B[9], uint8_t material, int power, float strength, bool sharp) {
	EditResult res;
	auto sdf = [&](Vec3 w) -> float { // world units, < 0 inside
		Vec3 d = w - c;
		Vec3 p(d.x * B[0] + d.y * B[3] + d.z * B[6], d.x * B[1] + d.y * B[4] + d.z * B[7], d.x * B[2] + d.y * B[5] + d.z * B[8]);
		if (shape == SPHERE) return p.len() - half.x;
		if (shape == CYLINDER) {
			float dr = std::sqrt(p.x * p.x + p.z * p.z) - half.x, dy = std::fabs(p.y) - half.y;
			return std::min(std::max(dr, dy), 0.0f) + Vec3(std::max(dr, 0.0f), std::max(dy, 0.0f), 0).len();
		}
		Vec3 q(std::fabs(p.x) - half.x, std::fabs(p.y) - half.y, std::fabs(p.z) - half.z);
		return Vec3(std::max(q.x, 0.0f), std::max(q.y, 0.0f), std::max(q.z, 0.0f)).len() + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
	};
	float R = shape == SPHERE ? half.x : shape == CYLINDER ? std::sqrt(half.x * half.x + half.y * half.y) : half.len();
	int lo[3], hi[3];
	for (int i = 0; i < 3; i++) {
		lo[i] = (int)std::floor((c[i] - R) / cell) - 2;
		hi[i] = (int)std::ceil((c[i] + R) / cell) + 2;
		if (bounded) { // a bounded world is closed: never write into the air outside it
			lo[i] = std::max(lo[i], bmin[i] * CHUNK + 1);
			hi[i] = std::min(hi[i], bmax[i] * CHUNK - 2);
		}
		if (lo[i] > hi[i]) return res;
	}
	const int w = hi[0] - lo[0] + 1, h = hi[1] - lo[1] + 1, dpt = hi[2] - lo[2] + 1;
	std::vector<uint8_t> chg((size_t)w * h * dpt, 0), old;
	auto ri = [&](int x, int y, int z) { return ((size_t)(z - lo[2]) * h + (y - lo[1])) * w + (x - lo[0]); };
	auto in_region = [&](int x, int y, int z) { return x >= lo[0] && x <= hi[0] && y >= lo[1] && y <= hi[1] && z >= lo[2] && z <= hi[2]; };

	std::unique_lock lk(mtx);
	if (mode == SMOOTH) {
		old.resize(chg.size());
		for (int z = lo[2]; z <= hi[2]; z++)
			for (int y = lo[1]; y <= hi[1]; y++)
				for (int x = lo[0]; x <= hi[0]; x++) old[ri(x, y, z)] = get_unlocked(x, y, z).d;
	}
	// An erase repaints solid voxels up to 2 cells out, not just the 1-cell density band: a surface cell's far
	// corners sit up to sqrt(3) cells from the cut, and an unpainted corner bleeds the outer material onto it.
	const float reach = mode == ERASE && expose_inner ? 2.0f : 1.0f;
	std::vector<std::array<int, 3>> paint;
	auto old_d = [&](int x, int y, int z) -> float { return in_region(x, y, z) ? old[ri(x, y, z)] : get_unlocked(x, y, z).d; };
	for (int z = lo[2]; z <= hi[2]; z++)
		for (int y = lo[1]; y <= hi[1]; y++)
			for (int x = lo[0]; x <= hi[0]; x++) {
				float sd = sdf(Vec3(x * cell, y * cell, z * cell)) / cell;
				if (sd > reach) continue;
				Voxel v = get_unlocked(x, y, z), nv = v;
				uint8_t sh = sdf_to_density(sd);
				if (mode == BUILD) {
					if (sh > v.d) nv = {sh, v.d < 128 ? material : v.m};
				} else if (hardness[v.m] > power) {
					continue;
				} else if (mode == ERASE) {
					if (255 - sh < v.d) {
						nv.d = 255 - sh;
						if (expose_inner) nv.m = inner[v.m];
					} else if (expose_inner && v.d > 0 && inner[v.m] != v.m) {
						paint.push_back({x, y, z}); // solid near the cut: repainted below if the dig removed anything
						continue;
					}
				} else if (sd < 0) {
					float avg = (old_d(x, y, z) + old_d(x - 1, y, z) + old_d(x + 1, y, z) + old_d(x, y - 1, z) + old_d(x, y + 1, z) + old_d(x, y, z - 1) + old_d(x, y, z + 1)) / 7.0f;
					nv.d = to_u8(v.d + (avg - v.d) * std::clamp(strength * std::min(-sd * 0.5f, 1.0f), 0.0f, 1.0f));
				}
				if (nv == v) continue;
				Voxel *dst = writable(x, y, z);
				if (!dst) continue; // endless: the chunk is not loaded
				if (nv.d < v.d) res.removed[v.m] += (v.d - nv.d) / 255.0f;
				*dst = nv;
				chg[ri(x, y, z)] = 1;
				res.changed = true;
			}
	if (!res.changed) return res;
	for (const auto &p : paint) // no chg mark: density is unchanged, so its Hermite edges stay valid
		if (Voxel *dst = writable(p[0], p[1], p[2])) dst->m = inner[dst->m];

	// Hermite data. Sharp edits store the exact crossing and analytic normal of every edge the shape now
	// defines; everything else that was touched is erased so it falls back to the density field.
	for (int z = lo[2]; z <= hi[2]; z++)
		for (int y = lo[1]; y <= hi[1]; y++)
			for (int x = lo[0]; x <= hi[0]; x++)
				for (int a = 0; a < 3; a++) {
					int e[3] = {x, y, z};
					e[a]++;
					if (!(chg[ri(x, y, z)] || (in_region(e[0], e[1], e[2]) && chg[ri(e[0], e[1], e[2])]))) continue;
					uint8_t *hd = hermite_at(x, y, z, false);
					bool fa = get_unlocked(x, y, z).d >= 128, fb = get_unlocked(e[0], e[1], e[2]).d >= 128;
					Vec3 pa(x * cell, y * cell, z * cell), pb(e[0] * cell, e[1] * cell, e[2] * cell);
					float sa = 0, sb = 0;
					bool ok = sharp && mode != SMOOTH && fa != fb;
					if (ok) { // the shape must be what puts the surface on this edge
						sa = sdf(pa); sb = sdf(pb);
						ok = (mode == BUILD ? sa < 0 : sa >= 0) == fa && (mode == BUILD ? sb < 0 : sb >= 0) == fb;
					}
					if (!ok) {
						if (hd) hd[a * 3] = 0;
						continue;
					}
					float t0 = 0, t1 = 1;
					for (int it = 0; it < 9; it++) { // bisect the analytic surface along the edge
						float tm = (t0 + t1) * 0.5f;
						((sdf(pa + (pb - pa) * tm) < 0) == (sa < 0) ? t0 : t1) = tm;
					}
					float t = (t0 + t1) * 0.5f, eps = cell * 0.02f;
					Vec3 p = pa + (pb - pa) * t;
					Vec3 n = Vec3(sdf(p + Vec3(eps, 0, 0)) - sdf(p - Vec3(eps, 0, 0)), sdf(p + Vec3(0, eps, 0)) - sdf(p - Vec3(0, eps, 0)), sdf(p + Vec3(0, 0, eps)) - sdf(p - Vec3(0, 0, eps))).norm();
					if (mode == ERASE) n = n * -1.0f;
					hd = hermite_at(x, y, z, true);
					if (!hd) continue;
					hd += a * 3;
					hd[0] = (uint8_t)((1 + (int)std::lround(t * 126)) << 1 | (n.z < 0));
					hd[1] = (uint8_t)(int8_t)std::lround(n.x * 127);
					hd[2] = (uint8_t)(int8_t)std::lround(n.y * 127);
				}
	// A chunk reads samples [16c - 2, 16c + 17].
	for (int i = 0; i < 3; i++) {
		res.cmin[i] = floordiv(lo[i] - 3, CHUNK);
		res.cmax[i] = floordiv(hi[i] + 2, CHUNK);
		if (bounded) {
			res.cmin[i] = std::max(res.cmin[i], bmin[i]);
			res.cmax[i] = std::min(res.cmax[i], bmax[i] - 1);
		}
	}
	return res;
}

// ---------------------------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------------------------

float Volume::density(Vec3 p) const {
	std::shared_lock lk(mtx);
	return density_unlocked(p);
}

float Volume::density_unlocked(Vec3 p) const {
	float gx = p.x / cell, gy = p.y / cell, gz = p.z / cell;
	int x = (int)std::floor(gx), y = (int)std::floor(gy), z = (int)std::floor(gz);
	float fx = gx - x, fy = gy - y, fz = gz - z, sum = 0;
	for (int c = 0; c < 8; c++) sum += (c & 1 ? fx : 1 - fx) * (c & 2 ? fy : 1 - fy) * (c & 4 ? fz : 1 - fz) * get_unlocked(x + (c & 1), y + ((c >> 1) & 1), z + (c >> 2)).d;
	return sum;
}

RayHit Volume::raycast(Vec3 o, Vec3 dir, float max_dist) const {
	std::shared_lock lk(mtx);
	return raycast_unlocked(o, dir, max_dist);
}

RayHit Volume::raycast_unlocked(Vec3 o, Vec3 dir, float max_dist) const {
	RayHit r;
	dir = dir.norm();
	const float step = cell * 0.5f;
	float prev = 0;
	for (float t = 0; t <= max_dist; prev = t, t += step) {
		if (density_unlocked(o + dir * t) < 127.5f) continue;
		float a = prev, b = t;
		for (int i = 0; i < 8; i++) {
			float m = (a + b) * 0.5f;
			(density_unlocked(o + dir * m) < 127.5f ? a : b) = m;
		}
		r.hit = true;
		r.pos = o + dir * b;
		float e = cell * 0.5f;
		Vec3 g(density_unlocked(r.pos + Vec3(e, 0, 0)) - density_unlocked(r.pos - Vec3(e, 0, 0)), density_unlocked(r.pos + Vec3(0, e, 0)) - density_unlocked(r.pos - Vec3(0, e, 0)), density_unlocked(r.pos + Vec3(0, 0, e)) - density_unlocked(r.pos - Vec3(0, 0, e)));
		r.normal = g.dot(g) > 1e-6f ? g.norm() * -1.0f : dir * -1.0f;
		int x = (int)std::floor(r.pos.x / cell), y = (int)std::floor(r.pos.y / cell), z = (int)std::floor(r.pos.z / cell);
		Voxel best;
		for (int c = 0; c < 8; c++) {
			Voxel v = get_unlocked(x + (c & 1), y + ((c >> 1) & 1), z + (c >> 2));
			if (v.d >= best.d) best = v;
		}
		r.material = best.m;
		return r;
	}
	return r;
}

} // namespace dkv
