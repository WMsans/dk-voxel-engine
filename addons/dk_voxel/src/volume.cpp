#include "voxel_core.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

namespace dkv {

static inline uint8_t to_u8(float f) { return (uint8_t)std::clamp((int)std::lround(f), 0, 255); }
// Signed distance in cells -> density. The band is +-1 cell, like the shipped SDF grids.
static inline uint8_t sdf_to_density(float cells) { return to_u8(127.5f - cells * 127.5f); }

// One 16^3-cell chunk: 2^3 blocks of 8^3 voxels, each either one uniform voxel or 512 of them.
struct Volume::Chunk {
	std::unique_ptr<Block> b[8]; // null = uniform block
	Voxel uniform[8];
	bool modified = false; // edited since it was generated or loaded
};

Volume::Volume(float cell_size) : cell(cell_size), bounded(false), bmin{0, 0, 0}, bmax{0, 0, 0} {}

Volume::Volume(int chunks_x, int chunks_y, int chunks_z, float cell_size) :
		cell(cell_size), bounded(true), bmin{0, 0, 0}, bmax{chunks_x, chunks_y, chunks_z} {
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

Vec3 Volume::chunk_origin(int cx, int cy, int cz) const {
	return Vec3((float)cx * CHUNK * cell, (float)cy * CHUNK * cell, (float)cz * CHUNK * cell);
}

void Volume::fill_chunk(int cx, int cy, int cz, const Generator *g) {
	(void)g; // Task 2 generates the 8 blocks here; until then only air exists.
	std::unique_lock lk(mtx);
	chunks[ChunkKey(cx, cy, cz)] = std::make_unique<Chunk>();
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
	if (!in_world(x, y, z)) return {};
	const Chunk *c = chunk_at(x >> 4, y >> 4, z >> 4);
	if (!c) return {};
	int bi = (((z >> 3) & 1) << 2) | (((y >> 3) & 1) << 1) | ((x >> 3) & 1);
	const Block *p = c->b[bi].get();
	return p ? p->v[vidx(x, y, z)] : c->uniform[bi];
}

// ---------------------------------------------------------------------------------------------
// Terrain generator
// ---------------------------------------------------------------------------------------------

static inline uint32_t hash3(int x, int y, int z, uint32_t s) {
	uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u ^ (uint32_t)z * 0xcb1ab31fu ^ s;
	h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
	return h;
}

// Gradient noise, roughly -1..1.
static float noise3(float x, float y, float z, uint32_t s) {
	int xi = (int)std::floor(x), yi = (int)std::floor(y), zi = (int)std::floor(z);
	float fx = x - xi, fy = y - yi, fz = z - zi;
	float u = fx * fx * fx * (fx * (fx * 6 - 15) + 10), v = fy * fy * fy * (fy * (fy * 6 - 15) + 10), w = fz * fz * fz * (fz * (fz * 6 - 15) + 10);
	float c[8];
	for (int i = 0; i < 8; i++) {
		int dx = i & 1, dy = (i >> 1) & 1, dz = i >> 2;
		uint32_t h = hash3(xi + dx, yi + dy, zi + dz, s);
		float gx = (int)(h & 0xff) - 127.5f, gy = (int)((h >> 8) & 0xff) - 127.5f, gz = (int)((h >> 16) & 0xff) - 127.5f;
		c[i] = (gx * (fx - dx) + gy * (fy - dy) + gz * (fz - dz)) * (1.0f / 110.0f);
	}
	float x00 = c[0] + (c[1] - c[0]) * u, x10 = c[2] + (c[3] - c[2]) * u, x01 = c[4] + (c[5] - c[4]) * u, x11 = c[6] + (c[7] - c[6]) * u;
	float y0 = x00 + (x10 - x00) * v, y1 = x01 + (x11 - x01) * v;
	return y0 + (y1 - y0) * w;
}

static float fbm(float x, float y, float z, uint32_t s, int oct) {
	float a = 0.5f, sum = 0;
	for (int i = 0; i < oct; i++, a *= 0.5f, x *= 2, y *= 2, z *= 2) sum += a * noise3(x, y, z, s + i * 1013u);
	return sum * 2.0f;
}

void Volume::generate(uint32_t seed, int threads) {
	const int B0[3] = {bmin[0] * 2, bmin[1] * 2, bmin[2] * 2}, B1[3] = {bmax[0] * 2, bmax[1] * 2, bmax[2] * 2};
	const float H = bmax[1] * CHUNK * cell; // world height in metres
	std::unique_lock lk(mtx);
	std::atomic<int> next{0};
	auto work = [&]() {
		float height[64];
		for (int zb; (zb = next.fetch_add(1)) < B1[2] - B0[2];) {
			for (int xb = B0[0]; xb < B1[0]; xb++) {
				float hmax = -1e9f;
				for (int i = 0; i < 64; i++) {
					float wx = (xb * 8 + (i & 7)) * cell, wz = ((B0[2] + zb) * 8 + (i >> 3)) * cell;
					float cont = fbm(wx / 150.0f, 0.5f, wz / 150.0f, seed, 3);
					float ridge = 1.0f - std::fabs(fbm(wx / 75.0f, 7.5f, wz / 75.0f, seed + 77, 4)) * 1.6f; // crests where the noise crosses zero
					float mount = std::clamp(cont * 2.2f + 0.45f, 0.0f, 1.0f); // lowlands -> mountains
					float mesa = std::clamp(fbm(wx / 60.0f, 3.5f, wz / 60.0f, seed + 191, 2) * 5.0f - 0.5f, 0.0f, 1.0f);
					height[i] = H * (0.17f + cont * 0.08f + mount * std::max(ridge, 0.0f) * ridge * 0.45f + mesa * 0.12f);
					hmax = std::max(hmax, height[i]);
				}
				for (int yb = B0[1]; yb < B1[1]; yb++) {
					int gz = B0[2] + zb;
					Chunk *c = chunk_at(xb >> 1, yb >> 1, gz >> 1);
					int bi = ((gz & 1) << 2) | ((yb & 1) << 1) | (xb & 1);
					c->b[bi].reset();
					if (yb * 8 * cell > hmax + 9.0f) { // well above the terrain: air
						c->uniform[bi] = {};
						continue;
					}
					Voxel tmp[512];
					for (int i = 0; i < 512; i++) {
						int lx = i & 7, ly = (i >> 3) & 7, lz = i >> 6;
						float wx = (xb * 8 + lx) * cell, wy = (yb * 8 + ly) * cell, wz = (gz * 8 + lz) * cell;
						float h = height[lz * 8 + lx], depth = h - wy;
						float sd = -depth; // > 0 = air, metres
						if (sd < 8.0f) {
							sd += fbm(wx / 16.0f, wy / 11.0f, wz / 16.0f, seed + 5, 2) * 7.0f * std::clamp(wy / H * 3.0f - 0.7f, 0.0f, 1.0f); // cliffs and overhangs up high
							if (sd < 1.0f) { // tunnels: intersection of two noise sheets; caverns: one blob field
								float n1 = noise3(wx / 26.0f, wy / 20.0f, wz / 26.0f, seed + 11), n2 = noise3(wx / 26.0f, wy / 20.0f, wz / 26.0f, seed + 23);
								sd = std::max(sd, (0.075f - std::max(std::fabs(n1), std::fabs(n2))) * 30.0f);
								sd = std::max(sd, (noise3(wx / 44.0f, wy / 30.0f, wz / 44.0f, seed + 31) - 0.42f) * 40.0f);
							}
						}
						sd = std::min(sd, wy - 1.5f); // bedrock floor
						uint8_t m = 3; // rock
						float wob = noise3(wx / 6.0f, wy / 6.0f, wz / 6.0f, seed + 41);
						if (wy < 1.5f + wob * 0.6f) m = 6; // bedrock
						else if (depth < 0.7f) m = h < H * 0.17f ? 4 : wy > H * 0.52f + wob * 4.0f ? 3 : 1; // sand low, bare rock high, grass between
						else if (depth < 3.5f + wob * 2.0f) m = h < H * 0.17f ? 4 : 2; // soil
						else if (noise3(wx / 7.0f, wy / 7.0f, wz / 7.0f, seed + 53) > 0.5f) m = 5; // gold
						else if (noise3(wx / 9.0f, wy / 9.0f, wz / 9.0f, seed + 67) > 0.56f) m = 8; // crystal
						tmp[i] = {sdf_to_density(sd / cell), m};
					}
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
		}
	};
	std::vector<std::thread> pool;
	for (int i = 1; i < threads; i++) pool.emplace_back(work);
	work();
	for (auto &t : pool) t.join();
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
				for (int x = lo[0]; x <= hi[0]; x++) old[ri(x, y, z)] = get(x, y, z).d;
	}
	auto old_d = [&](int x, int y, int z) -> float { return in_region(x, y, z) ? old[ri(x, y, z)] : get(x, y, z).d; };
	for (int z = lo[2]; z <= hi[2]; z++)
		for (int y = lo[1]; y <= hi[1]; y++)
			for (int x = lo[0]; x <= hi[0]; x++) {
				float sd = sdf(Vec3(x * cell, y * cell, z * cell)) / cell;
				if (sd > 1.0f) continue;
				Voxel v = get(x, y, z), nv = v;
				uint8_t sh = sdf_to_density(sd);
				if (mode == BUILD) {
					if (sh > v.d) nv = {sh, v.d < 128 ? material : v.m};
				} else if (hardness[v.m] > power) {
					continue;
				} else if (mode == ERASE) {
					if (255 - sh < v.d) nv.d = 255 - sh;
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
					bool fa = get(x, y, z).d >= 128, fb = get(e[0], e[1], e[2]).d >= 128;
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
	float gx = p.x / cell, gy = p.y / cell, gz = p.z / cell;
	int x = (int)std::floor(gx), y = (int)std::floor(gy), z = (int)std::floor(gz);
	float fx = gx - x, fy = gy - y, fz = gz - z, sum = 0;
	for (int c = 0; c < 8; c++) sum += (c & 1 ? fx : 1 - fx) * (c & 2 ? fy : 1 - fy) * (c & 4 ? fz : 1 - fz) * get(x + (c & 1), y + ((c >> 1) & 1), z + (c >> 2)).d;
	return sum;
}

RayHit Volume::raycast(Vec3 o, Vec3 dir, float max_dist) const {
	RayHit r;
	dir = dir.norm();
	const float step = cell * 0.5f;
	float prev = 0;
	for (float t = 0; t <= max_dist; prev = t, t += step) {
		if (density(o + dir * t) < 127.5f) continue;
		float a = prev, b = t;
		for (int i = 0; i < 8; i++) {
			float m = (a + b) * 0.5f;
			(density(o + dir * m) < 127.5f ? a : b) = m;
		}
		r.hit = true;
		r.pos = o + dir * b;
		float e = cell * 0.5f;
		Vec3 g(density(r.pos + Vec3(e, 0, 0)) - density(r.pos - Vec3(e, 0, 0)), density(r.pos + Vec3(0, e, 0)) - density(r.pos - Vec3(0, e, 0)), density(r.pos + Vec3(0, 0, e)) - density(r.pos - Vec3(0, 0, e)));
		r.normal = g.dot(g) > 1e-6f ? g.norm() * -1.0f : dir * -1.0f;
		int x = (int)std::floor(r.pos.x / cell), y = (int)std::floor(r.pos.y / cell), z = (int)std::floor(r.pos.z / cell);
		Voxel best;
		for (int c = 0; c < 8; c++) {
			Voxel v = get(x + (c & 1), y + ((c >> 1) & 1), z + (c >> 2));
			if (v.d >= best.d) best = v;
		}
		r.material = best.m;
		return r;
	}
	return r;
}

} // namespace dkv