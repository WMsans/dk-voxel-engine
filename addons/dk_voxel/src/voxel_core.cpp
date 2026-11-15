#include "voxel_core.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace dkv {

static inline uint8_t to_u8(float f) { return (uint8_t)std::clamp((int)std::lround(f), 0, 255); }
static inline int floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
// Signed distance in cells -> density. The band is +-1 cell, like the shipped SDF grids.
static inline uint8_t sdf_to_density(float cells) { return to_u8(127.5f - cells * 127.5f); }

Volume::Volume(int a, int b, int c, float cs) :
		cx(a), cy(b), cz(c), nx(a * CHUNK), ny(b * CHUNK), nz(c * CHUNK), cell(cs), bx(a * 2), by(b * 2), bz(c * 2) {
	blocks.resize((size_t)bx * by * bz);
	uniform.resize(blocks.size());
}

Voxel *Volume::writable(int x, int y, int z) {
	if ((unsigned)x >= (unsigned)nx || (unsigned)y >= (unsigned)ny || (unsigned)z >= (unsigned)nz) return nullptr;
	size_t b = bidx(x >> 3, y >> 3, z >> 3);
	if (!blocks[b]) {
		blocks[b] = std::make_unique<Block>();
		std::fill_n(blocks[b]->v, 512, uniform[b]);
	}
	return &blocks[b]->v[vidx(x, y, z)];
}

uint8_t *Volume::hermite_at(int x, int y, int z, bool create) {
	if ((unsigned)x >= (unsigned)nx || (unsigned)y >= (unsigned)ny || (unsigned)z >= (unsigned)nz) return nullptr;
	size_t b = bidx(x >> 3, y >> 3, z >> 3);
	if (!blocks[b]) {
		if (!create) return nullptr;
		writable(x, y, z);
	}
	Block *p = blocks[b].get();
	if (!p->hermite) {
		if (!create) return nullptr;
		p->hermite.reset(new uint8_t[512 * 9]());
	}
	return p->hermite.get() + vidx(x, y, z) * 9;
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
	std::unique_lock lk(mtx);
	const float H = ny * cell; // world height in metres
	std::atomic<int> next{0};
	auto work = [&]() {
		float height[64];
		for (int zb; (zb = next.fetch_add(1)) < bz;) {
			for (int xb = 0; xb < bx; xb++) {
				float hmax = -1e9f;
				for (int i = 0; i < 64; i++) {
					float wx = (xb * 8 + (i & 7)) * cell, wz = (zb * 8 + (i >> 3)) * cell;
					float cont = fbm(wx / 150.0f, 0.5f, wz / 150.0f, seed, 3);
					float ridge = 1.0f - std::fabs(fbm(wx / 75.0f, 7.5f, wz / 75.0f, seed + 77, 4)) * 1.6f; // crests where the noise crosses zero
					float mount = std::clamp(cont * 2.2f + 0.45f, 0.0f, 1.0f); // lowlands -> mountains
					float mesa = std::clamp(fbm(wx / 60.0f, 3.5f, wz / 60.0f, seed + 191, 2) * 5.0f - 0.5f, 0.0f, 1.0f);
					height[i] = H * (0.17f + cont * 0.08f + mount * std::max(ridge, 0.0f) * ridge * 0.45f + mesa * 0.12f);
					hmax = std::max(hmax, height[i]);
				}
				for (int yb = 0; yb < by; yb++) {
					size_t b = bidx(xb, yb, zb);
					blocks[b].reset();
					if (yb * 8 * cell > hmax + 9.0f) { // well above the terrain: air
						uniform[b] = {};
						continue;
					}
					Voxel tmp[512];
					for (int i = 0; i < 512; i++) {
						int lx = i & 7, ly = (i >> 3) & 7, lz = i >> 6;
						float wx = (xb * 8 + lx) * cell, wy = (yb * 8 + ly) * cell, wz = (zb * 8 + lz) * cell;
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
						uniform[b] = tmp[0];
					} else {
						blocks[b] = std::make_unique<Block>();
						memcpy(blocks[b]->v, tmp, sizeof(tmp));
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
// Dual contouring
// ---------------------------------------------------------------------------------------------

namespace {

struct Qef {
	float a[6] = {}, b[3] = {}, c = 0, m[3] = {}; // AtA (xx xy xz yy yz zz), Atb, btb, mass-point sum
	int n = 0;
	void add(Vec3 p, Vec3 nr) {
		a[0] += nr.x * nr.x; a[1] += nr.x * nr.y; a[2] += nr.x * nr.z; a[3] += nr.y * nr.y; a[4] += nr.y * nr.z; a[5] += nr.z * nr.z;
		float d = nr.dot(p);
		b[0] += nr.x * d; b[1] += nr.y * d; b[2] += nr.z * d;
		c += d * d;
		m[0] += p.x; m[1] += p.y; m[2] += p.z;
		n++;
	}
	void add(const Qef &o, Vec3 d) { // o expressed in a frame offset by d from ours
		Vec3 ad(o.a[0] * d.x + o.a[1] * d.y + o.a[2] * d.z, o.a[1] * d.x + o.a[3] * d.y + o.a[4] * d.z, o.a[2] * d.x + o.a[4] * d.y + o.a[5] * d.z);
		for (int i = 0; i < 6; i++) a[i] += o.a[i];
		for (int i = 0; i < 3; i++) b[i] += o.b[i] + ad[i], m[i] += o.m[i] + d[i] * o.n;
		c += o.c + 2 * (d.x * o.b[0] + d.y * o.b[1] + d.z * o.b[2]) + d.dot(ad);
		n += o.n;
	}
	float error(Vec3 x) const {
		Vec3 ax(a[0] * x.x + a[1] * x.y + a[2] * x.z, a[1] * x.x + a[3] * x.y + a[4] * x.z, a[2] * x.x + a[4] * x.y + a[5] * x.z);
		return x.dot(ax) - 2 * (x.x * b[0] + x.y * b[1] + x.z * b[2]) + c;
	}
	// Minimiser clamped to [lo, hi]; returns the residual. Jacobi eigen-decomposition of AtA (5 sweeps),
	// pseudo-inverse dropping eigenvalues under 0.1, solved about the mass point.
	float solve(Vec3 lo, Vec3 hi, Vec3 &x, uint8_t &rank) const {
		float A[3][3] = {{a[0], a[1], a[2]}, {a[1], a[3], a[4]}, {a[2], a[4], a[5]}};
		float V[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
		static const int P[3] = {0, 0, 1}, Q[3] = {1, 2, 2};
		for (int sweep = 0; sweep < 5; sweep++) {
			for (int r = 0; r < 3; r++) {
				int p = P[r], q = Q[r];
				if (std::fabs(A[p][q]) < 1e-3f) continue;
				float th = (A[q][q] - A[p][p]) / (2 * A[p][q]);
				float t = (th >= 0 ? 1.0f : -1.0f) / (std::fabs(th) + std::sqrt(th * th + 1));
				float cs = 1 / std::sqrt(t * t + 1), sn = t * cs;
				for (int k = 0; k < 3; k++) {
					float kp = A[k][p], kq = A[k][q];
					A[k][p] = cs * kp - sn * kq; A[k][q] = sn * kp + cs * kq;
				}
				for (int k = 0; k < 3; k++) {
					float pk = A[p][k], qk = A[q][k];
					A[p][k] = cs * pk - sn * qk; A[q][k] = sn * pk + cs * qk;
				}
				for (int k = 0; k < 3; k++) {
					float kp = V[k][p], kq = V[k][q];
					V[k][p] = cs * kp - sn * kq; V[k][q] = sn * kp + cs * kq;
				}
			}
		}
		Vec3 mp(m[0] / n, m[1] / n, m[2] / n);
		float r[3] = {b[0] - (a[0] * mp.x + a[1] * mp.y + a[2] * mp.z), b[1] - (a[1] * mp.x + a[3] * mp.y + a[4] * mp.z), b[2] - (a[2] * mp.x + a[4] * mp.y + a[5] * mp.z)};
		x = mp;
		rank = 0;
		for (int i = 0; i < 3; i++) {
			if (std::fabs(A[i][i]) < 0.1f) continue;
			rank++;
			float d = (V[0][i] * r[0] + V[1][i] * r[1] + V[2][i] * r[2]) / A[i][i];
			x.x += V[0][i] * d; x.y += V[1][i] * d; x.z += V[2][i] * d;
		}
		for (int i = 0; i < 3; i++) x[i] = std::clamp(x[i], lo[i], hi[i]);
		return error(x);
	}
};

struct MatAcc { // tiny material -> weight map
	uint8_t m[8];
	float w[8];
	int n = 0;
	void add(uint8_t mat, float wt) {
		if (wt <= 0) return;
		for (int i = 0; i < n; i++)
			if (m[i] == mat) { w[i] += wt; return; }
		if (n < 8) { m[n] = mat; w[n++] = wt; }
	}
	float get(uint8_t mat) const {
		for (int i = 0; i < n; i++)
			if (m[i] == mat) return w[i];
		return 0;
	}
	void top2(uint8_t om[2], float ow[2]) const {
		int a = -1, b = -1;
		for (int i = 0; i < n; i++) {
			if (a < 0 || w[i] > w[a]) { b = a; a = i; }
			else if (b < 0 || w[i] > w[b]) b = i;
		}
		om[0] = a < 0 ? 0 : m[a]; ow[0] = a < 0 ? 0 : w[a];
		om[1] = b < 0 ? om[0] : m[b]; ow[1] = b < 0 ? 0 : w[b];
	}
};

// Position and QEF live in the frame of the vertex's own cell (or collapsed node), origin o. A border cell
// is therefore solved with identical arithmetic by both chunks that see it, and the seam is bit-exact.
struct CellVert {
	Vec3 f, n; // offset from o, in cells; n = summed Hermite normals
	int o[3] = {0, 0, 0}; // chunk-local cell coordinates
	Qef q;
	Vec3 p() const { return Vec3(o[0] + f.x, o[1] + f.y, o[2] + f.z); }
	uint8_t rank = 0, m[2] = {0, 0};
	float w[2] = {0, 0}, ao = 1;
};

constexpr int G = CHUNK + 4, CN = CHUNK + 1, HN = CHUNK + 2; // samples -2..17, cells -1..15, Hermite corners -1..16
inline int LI(int x, int y, int z) { return ((z + 2) * G + (y + 2)) * G + (x + 2); }
inline int CI(int x, int y, int z) { return ((z + 1) * CN + (y + 1)) * CN + (x + 1); }
inline int HI(int x, int y, int z) { return (((z + 1) * HN + (y + 1)) * HN + (x + 1)) * 9; }

struct Scratch {
	uint8_t D[G * G * G], M[G * G * G];
	std::vector<uint8_t> H;
	int32_t cellv[CN * CN * CN], rep[CN * CN * CN];
	std::vector<CellVert> verts;
	std::vector<int32_t> state[4];
	std::unordered_map<uint64_t, int32_t> vmap;
};

} // namespace

bool Volume::mesh_chunk(int ccx, int ccy, int ccz, int lod, MeshData &out) const {
	out.clear();
	static thread_local std::unique_ptr<Scratch> scratch;
	if (!scratch) scratch = std::make_unique<Scratch>();
	Scratch &S = *scratch;
	const int ox = ccx * CHUNK, oy = ccy * CHUNK, oz = ccz * CHUNK;
	bool has_h = false;
	{
		std::shared_lock lk(mtx);
		// Cheap reject: every block the chunk touches is uniform and on the same side of the surface.
		bool in = false, outside = false, dense = false;
		for (int z = (oz - 2) >> 3; z <= (oz + CHUNK + 1) >> 3; z++)
			for (int y = (oy - 2) >> 3; y <= (oy + CHUNK + 1) >> 3; y++)
				for (int x = (ox - 2) >> 3; x <= (ox + CHUNK + 1) >> 3; x++) {
					if ((unsigned)x >= (unsigned)bx || (unsigned)y >= (unsigned)by || (unsigned)z >= (unsigned)bz) { outside = true; continue; }
					size_t b = bidx(x, y, z);
					if (blocks[b]) { dense = true; has_h |= (bool)blocks[b]->hermite; }
					else (uniform[b].d >= 128 ? in : outside) = true;
				}
		if (!dense && !(in && outside)) return false;
		in = outside = false;
		for (int z = -2; z < CHUNK + 2; z++)
			for (int y = -2; y < CHUNK + 2; y++)
				for (int x = -2; x < CHUNK + 2; x++) {
					Voxel v = get(ox + x, oy + y, oz + z);
					S.D[LI(x, y, z)] = v.d; S.M[LI(x, y, z)] = v.m;
					(v.d >= 128 ? in : outside) = true;
				}
		if (!(in && outside)) return false;
		if (has_h) {
			S.H.assign((size_t)HN * HN * HN * 9, 0);
			for (int z = -1; z <= CHUNK; z++)
				for (int y = -1; y <= CHUNK; y++)
					for (int x = -1; x <= CHUNK; x++) {
						int gx = ox + x, gy = oy + y, gz = oz + z;
						if ((unsigned)gx >= (unsigned)nx || (unsigned)gy >= (unsigned)ny || (unsigned)gz >= (unsigned)nz) continue;
						const Block *p = blocks[bidx(gx >> 3, gy >> 3, gz >> 3)].get();
						if (p && p->hermite) memcpy(&S.H[HI(x, y, z)], p->hermite.get() + vidx(gx, gy, gz) * 9, 9);
					}
		}
	}
	const uint8_t *D = S.D, *M = S.M;
	auto grad = [&](int x, int y, int z) {
		return Vec3((float)D[LI(x + 1, y, z)] - D[LI(x - 1, y, z)], (float)D[LI(x, y + 1, z)] - D[LI(x, y - 1, z)], (float)D[LI(x, y, z + 1)] - D[LI(x, y, z - 1)]);
	};

	// 1. One QEF vertex per surface cell, for cells -1..15 (the -1 layer belongs to the neighbours and is
	//    only read; it is what stitches chunks together without cracks).
	S.verts.clear();
	for (int k = -1; k < CHUNK; k++)
		for (int j = -1; j < CHUNK; j++)
			for (int i = -1; i < CHUNK; i++) {
				int ci = CI(i, j, k);
				S.cellv[ci] = S.rep[ci] = -1;
				int mask = 0;
				for (int c = 0; c < 8; c++) mask |= (D[LI(i + (c & 1), j + ((c >> 1) & 1), k + (c >> 2))] >= 128) << c;
				if (mask == 0 || mask == 255) continue;
				CellVert cv;
				cv.o[0] = i; cv.o[1] = j; cv.o[2] = k;
				for (int a = 0; a < 3; a++) {
					int u = (a + 1) % 3, v = (a + 2) % 3;
					for (int e = 0; e < 4; e++) {
						int c0[3] = {i, j, k};
						c0[u] += e & 1; c0[v] += e >> 1;
						int c1[3] = {c0[0], c0[1], c0[2]};
						c1[a]++;
						float da = D[LI(c0[0], c0[1], c0[2])], db = D[LI(c1[0], c1[1], c1[2])];
						if ((da >= 128) == (db >= 128)) continue;
						float t;
						Vec3 nr;
						const uint8_t *h = has_h ? &S.H[HI(c0[0], c0[1], c0[2]) + a * 3] : nullptr;
						if (h && h[0] >= 2) { // authored Hermite sample: exact crossing + normal
							t = ((h[0] >> 1) - 1) / 126.0f;
							nr.x = (int8_t)h[1] / 127.0f; nr.y = (int8_t)h[2] / 127.0f;
							nr.z = std::sqrt(std::max(0.0f, 1 - nr.x * nr.x - nr.y * nr.y)) * (h[0] & 1 ? -1.0f : 1.0f);
						} else { // fall back to the density field
							t = (127.5f - da) / (db - da);
							Vec3 g0 = grad(c0[0], c0[1], c0[2]), g1 = grad(c1[0], c1[1], c1[2]);
							Vec3 g = g0 + (g1 - g0) * t;
							if (g.dot(g) < 1e-6f) g[a] = db - da;
							nr = g.norm() * -1.0f;
						}
						Vec3 p((float)(c0[0] - i), (float)(c0[1] - j), (float)(c0[2] - k));
						p[a] += t;
						cv.q.add(p, nr);
						cv.n = cv.n + nr;
					}
				}
				cv.q.solve(Vec3(0.05f, 0.05f, 0.05f), Vec3(0.95f, 0.95f, 0.95f), cv.f, cv.rank);
				// Materials: trilinear corner weights x corner density, keep the two heaviest.
				float fx = cv.f.x, fy = cv.f.y, fz = cv.f.z;
				MatAcc acc;
				for (int c = 0; c < 8; c++) {
					int li = LI(i + (c & 1), j + ((c >> 1) & 1), k + (c >> 2));
					acc.add(M[li], (c & 1 ? fx : 1 - fx) * (c & 2 ? fy : 1 - fy) * (c & 4 ? fz : 1 - fz) * D[li]);
				}
				acc.top2(cv.m, cv.w);
				// Ambient occlusion from the solid fraction of the surrounding 4^3 samples.
				int sum = 0;
				for (int z = -1; z <= 2; z++)
					for (int y = -1; y <= 2; y++)
						for (int x = -1; x <= 2; x++) sum += D[LI(i + x, j + y, k + z)];
				cv.ao = std::clamp(1.1f - (sum / (64 * 255.0f) - 0.5f) * 1.8f, 0.35f, 1.0f);
				S.cellv[ci] = S.rep[ci] = (int32_t)S.verts.size();
				S.verts.push_back(cv);
			}
	if (S.verts.empty()) return false;

	// 2. LOD: octree collapse. A node whose children all collapsed (or are empty) merges their QEFs; if the
	//    merged minimiser stays under the error budget the node becomes one vertex. Nodes touching the +faces
	//    never collapse, because the neighbouring chunk reads those cells at full resolution.
	lod = std::clamp(lod, 0, 2);
	for (int L = 1; L <= lod; L++) {
		int n = CHUNK >> L, size = 1 << L;
		S.state[L].assign((size_t)n * n * n, -1); // -1 empty, -2 mixed, >= 0 vertex
		for (int c = 0; c < n; c++)
			for (int b = 0; b < n; b++)
				for (int a = 0; a < n; a++) {
					int32_t kids[8];
					int nk = 0;
					bool mixed = false;
					for (int ch = 0; ch < 8; ch++) {
						int x = a * 2 + (ch & 1), y = b * 2 + ((ch >> 1) & 1), z = c * 2 + (ch >> 2);
						int32_t s = L == 1 ? S.cellv[CI(x, y, z)] : S.state[L - 1][((size_t)z * (n * 2) + y) * (n * 2) + x];
						if (s == -2) mixed = true;
						else if (s >= 0) kids[nk++] = s;
					}
					int32_t &st = S.state[L][((size_t)c * n + b) * n + a];
					if (nk == 0 && !mixed) continue;
					st = -2;
					if (mixed || a == n - 1 || b == n - 1 || c == n - 1) continue;
					if (nk == 1) { st = kids[0]; continue; }
					CellVert cv;
					MatAcc acc;
					cv.ao = 0;
					cv.o[0] = a * size; cv.o[1] = b * size; cv.o[2] = c * size;
					for (int q = 0; q < nk; q++) {
						const CellVert &k = S.verts[kids[q]];
						cv.q.add(k.q, Vec3((float)(k.o[0] - cv.o[0]), (float)(k.o[1] - cv.o[1]), (float)(k.o[2] - cv.o[2])));
						cv.n = cv.n + k.n.norm();
						acc.add(k.m[0], k.w[0]); acc.add(k.m[1], k.w[1]);
						cv.ao += k.ao / nk;
					}
					Vec3 navg = cv.n.norm();
					bool sheets = false; // two surfaces facing each other must not be welded together
					for (int q = 0; q < nk; q++) sheets |= S.verts[kids[q]].n.norm().dot(navg) < 0.35f;
					if (sheets) continue;
					if (cv.q.solve(Vec3(0.05f, 0.05f, 0.05f), Vec3(size - 0.05f, size - 0.05f, size - 0.05f), cv.f, cv.rank) > lod_error[lod] * cv.q.n) continue;
					acc.top2(cv.m, cv.w);
					st = (int32_t)S.verts.size();
					S.verts.push_back(cv);
					for (int z = c * size; z < (c + 1) * size; z++)
						for (int y = b * size; y < (b + 1) * size; y++)
							for (int x = a * size; x < (a + 1) * size; x++)
								if (S.cellv[CI(x, y, z)] >= 0) S.rep[CI(x, y, z)] = st;
				}
	}

	// 3. Emit one quad per sign-changing edge owned by this chunk, joining the four cells around it.
	S.vmap.clear();
	auto emit = [&](int32_t r, uint8_t ma, uint8_t mb, Vec3 face_n) -> int32_t {
		const CellVert &v = S.verts[r];
		Vec3 n = v.n.norm();
		uint64_t key = (uint64_t)r | (uint64_t)ma << 20 | (uint64_t)mb << 28;
		if (v.rank >= 2 && n.dot(face_n) < 0.92f) { // sharp feature: split the vertex, shade it flat
			n = face_n;
			key |= (uint64_t)(1 + ((int)((n.x + 1) * 31.5f) | (int)((n.y + 1) * 31.5f) << 6 | (int)((n.z + 1) * 31.5f) << 12)) << 36;
		}
		auto it = S.vmap.find(key);
		if (it != S.vmap.end()) return it->second;
		int32_t id = (int32_t)(out.pos.size() / 3);
		S.vmap.emplace(key, id);
		out.pos.insert(out.pos.end(), {((float)(ox + v.o[0]) + v.f.x) * cell, ((float)(oy + v.o[1]) + v.f.y) * cell, ((float)(oz + v.o[2]) + v.f.z) * cell});
		out.nrm.insert(out.nrm.end(), {n.x, n.y, n.z});
		float wa = v.m[0] == ma ? v.w[0] : v.m[1] == ma ? v.w[1] : 0, wb = v.m[0] == mb ? v.w[0] : v.m[1] == mb ? v.w[1] : 0;
		out.col.insert(out.col.end(), {ma, mb, ma == mb ? (uint8_t)0 : to_u8(wa + wb > 0 ? wb / (wa + wb) * 255 : 127.5f), to_u8(v.ao * 255)});
		return id;
	};
	for (int k = 0; k < CHUNK; k++)
		for (int j = 0; j < CHUNK; j++)
			for (int i = 0; i < CHUNK; i++) {
				bool s0 = D[LI(i, j, k)] >= 128;
				for (int a = 0; a < 3; a++) {
					int c1[3] = {i, j, k};
					c1[a]++;
					if ((D[LI(c1[0], c1[1], c1[2])] >= 128) == s0) continue;
					// The four cells around the edge, counter-clockwise seen from +axis.
					int u = (a + 1) % 3, v = (a + 2) % 3;
					static const int DU[4] = {-1, 0, 0, -1}, DV[4] = {-1, -1, 0, 0};
					int32_t r[4];
					int nr = 0;
					for (int q = 0; q < 4; q++) {
						int c[3] = {i, j, k};
						c[u] += DU[q]; c[v] += DV[q];
						int32_t id = S.rep[CI(c[0], c[1], c[2])];
						bool dup = false;
						for (int p = 0; p < nr; p++) dup |= r[p] == id;
						if (!dup) r[nr++] = id;
					}
					if (nr < 3) continue; // collapsed away at this LOD
					MatAcc acc;
					for (int q = 0; q < nr; q++) {
						acc.add(S.verts[r[q]].m[0], S.verts[r[q]].w[0]);
						acc.add(S.verts[r[q]].m[1], S.verts[r[q]].w[1]);
					}
					uint8_t pm[2];
					float pw[2];
					acc.top2(pm, pw);
					uint8_t ma = std::min(pm[0], pm[1]), mb = std::max(pm[0], pm[1]);
					const Vec3 p0 = S.verts[r[0]].p(), p1 = S.verts[r[1]].p(), p2 = S.verts[r[2]].p(), p3 = S.verts[r[nr - 1]].p();
					Vec3 fn = nr == 4 ? (p2 - p0).cross(p3 - p1) : (p1 - p0).cross(p2 - p0);
					fn = fn.norm() * (s0 ? 1.0f : -1.0f);
					int32_t id[4];
					for (int q = 0; q < nr; q++) id[q] = emit(r[q], ma, mb, fn);
					int tri[6] = {0, 1, 2, 0, 2, 3};
					if (nr == 4 && (p3 - p1).len() < (p2 - p0).len()) { tri[0] = 1; tri[1] = 2; tri[2] = 3; tri[3] = 1; tri[4] = 3; tri[5] = 0; }
					for (int t = 0; t < (nr - 2) * 3; t += 3) {
						// Godot front faces are clockwise: flip when the surface faces +axis.
						out.idx.push_back(id[tri[t]]);
						out.idx.push_back(id[tri[t + (s0 ? 2 : 1)]]);
						out.idx.push_back(id[tri[t + (s0 ? 1 : 2)]]);
					}
				}
			}
	return !out.idx.empty();
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
	const int N[3] = {nx, ny, nz};
	for (int i = 0; i < 3; i++) {
		lo[i] = std::max(0, (int)std::floor((c[i] - R) / cell) - 2);
		hi[i] = std::min(N[i] - 1, (int)std::ceil((c[i] + R) / cell) + 2);
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
				if (nv.d < v.d) res.removed[v.m] += (v.d - nv.d) / 255.0f;
				*writable(x, y, z) = nv;
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
					hd = hermite_at(x, y, z, true) + a * 3;
					hd[0] = (uint8_t)((1 + (int)std::lround(t * 126)) << 1 | (n.z < 0));
					hd[1] = (uint8_t)(int8_t)std::lround(n.x * 127);
					hd[2] = (uint8_t)(int8_t)std::lround(n.y * 127);
				}
	// A chunk reads samples [16c - 2, 16c + 17].
	const int C[3] = {cx, cy, cz};
	for (int i = 0; i < 3; i++) {
		res.cmin[i] = std::max(0, floordiv(lo[i] - 3, CHUNK));
		res.cmax[i] = std::min(C[i] - 1, floordiv(hi[i] + 2, CHUNK));
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
