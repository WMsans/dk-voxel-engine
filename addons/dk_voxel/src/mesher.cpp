// Dual contouring mesher: chunk-local mesh data, seams bit-exact across chunk borders and LOD changes.
#include "voxel_core.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>

namespace dkv {

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
	const int o[3] = {ccx * CHUNK, ccy * CHUNK, ccz * CHUNK};
	bool has_h = false;
	{
		std::shared_lock lk(mtx);
		int b0[3], b1[3];
		for (int a = 0; a < 3; a++) { b0[a] = (o[a] - 2) >> 3; b1[a] = (o[a] + CHUNK + 1) >> 3; }
		// Cheap reject: every block the chunk touches is uniform and on the same side of the surface.
		bool in = false, outside = false, dense = false;
		for (int z = b0[2]; z <= b1[2]; z++)
			for (int y = b0[1]; y <= b1[1]; y++)
				for (int x = b0[0]; x <= b1[0]; x++) {
					Voxel u;
					const Block *p = block_at(x, y, z, u);
					if (p) { dense = true; has_h |= (bool)p->hermite; }
					else (u.d >= 128 ? in : outside) = true;
				}
		if (!dense && !(in && outside)) return false;
		// Samples -2..17, copied block by block.
		for (int z = b0[2]; z <= b1[2]; z++)
			for (int y = b0[1]; y <= b1[1]; y++)
				for (int x = b0[0]; x <= b1[0]; x++) {
					Voxel u;
					const Block *p = block_at(x, y, z, u);
					int x0 = std::max(x * 8, o[0] - 2), x1 = std::min(x * 8 + 7, o[0] + CHUNK + 1);
					int y0 = std::max(y * 8, o[1] - 2), y1 = std::min(y * 8 + 7, o[1] + CHUNK + 1);
					int z0 = std::max(z * 8, o[2] - 2), z1 = std::min(z * 8 + 7, o[2] + CHUNK + 1);
					for (int vz = z0; vz <= z1; vz++)
						for (int vy = y0; vy <= y1; vy++)
							for (int vx = x0; vx <= x1; vx++) {
								Voxel v = p ? p->v[vidx(vx, vy, vz)] : u;
								int li = LI(vx - o[0], vy - o[1], vz - o[2]);
								S.D[li] = v.d;
								S.M[li] = v.m;
							}
				}
		in = outside = false;
		for (int i = 0; i < G * G * G; i++) (S.D[i] >= 128 ? in : outside) = true;
		if (!(in && outside)) return false;
		if (bounded) { // the closed-world rule: the outer half-voxel of the world reads as air (see get())
			for (int z = -2; z < CHUNK + 2; z++)
				for (int y = -2; y < CHUNK + 2; y++)
					for (int x = -2; x < CHUNK + 2; x++)
						if (!in_world(o[0] + x, o[1] + y, o[2] + z)) {
							int li = LI(x, y, z);
							S.D[li] = 0;
							S.M[li] = 0;
						}
		}
		if (has_h) {
			S.H.assign((size_t)HN * HN * HN * 9, 0);
			for (int z = b0[2]; z <= b1[2]; z++)
				for (int y = b0[1]; y <= b1[1]; y++)
					for (int x = b0[0]; x <= b1[0]; x++) {
						Voxel u;
						const Block *p = block_at(x, y, z, u);
						if (!p || !p->hermite) continue;
						int x0 = std::max(x * 8, o[0] - 1), x1 = std::min(x * 8 + 7, o[0] + CHUNK);
						int y0 = std::max(y * 8, o[1] - 1), y1 = std::min(y * 8 + 7, o[1] + CHUNK);
						int z0 = std::max(z * 8, o[2] - 1), z1 = std::min(z * 8 + 7, o[2] + CHUNK);
						for (int vz = z0; vz <= z1; vz++)
							for (int vy = y0; vy <= y1; vy++)
								for (int vx = x0; vx <= x1; vx++) {
									if (bounded && !in_world(vx, vy, vz)) continue; // no Hermite outside the world
									memcpy(&S.H[HI(vx - o[0], vy - o[1], vz - o[2])], p->hermite.get() + vidx(vx, vy, vz) * 9, 9);
								}
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
		// Chunk-relative, snapped to 1/4096 of a cell. A border vertex is computed from a different local cell
	// in each of the two chunks that see it; the snap is a power-of-two grid the chunk shift (16 cells) is an
	// exact multiple of, so local + chunk origin rebuilds the same float from either chunk and the seam stays
	// bit-exact. 1/4096 of a cell is far below anything visible.
		out.pos.insert(out.pos.end(), {(v.o[0] + std::round(v.f.x * 4096.0f) / 4096.0f) * cell, (v.o[1] + std::round(v.f.y * 4096.0f) / 4096.0f) * cell, (v.o[2] + std::round(v.f.z * 4096.0f) / 4096.0f) * cell});
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

} // namespace dkv