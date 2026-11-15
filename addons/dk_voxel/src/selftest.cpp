// Core self-test: scons selftest && build/selftest
#include "demo_generator.h"
#include "voxel_core.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <tuple>

using namespace dkv;

static const float ID[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
static int failures = 0;
#define CHECK(cond, ...) \
	do { \
		if (!(cond)) { failures++; printf("  FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } \
	} while (0)

static double ms_since(std::chrono::steady_clock::time_point t0) {
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

struct Soup {
	long tris = 0, open_edges = 0, bad_normals = 0;
	double volume = 0, ms = 0;
	std::vector<Vec3> pts;
	float nearest(Vec3 p) const {
		float best = 1e9f;
		for (Vec3 q : pts) best = std::min(best, (q - p).len());
		return best;
	}
};

// Meshes every chunk and checks the union is closed and consistently wound: every directed edge a->b
// must be matched by b->a, across chunk borders and LOD changes. lo/hi are the chunk range to mesh
// (exclusive max), defaulting to the volume's bounds. Mesh positions are chunk-relative, so they are
// offset by the chunk origin before the checks.
static Soup mesh_all(const Volume &v, const std::function<int(int, int, int)> &lod);

static Soup mesh_all(const Volume &v, const std::function<int(int, int, int)> &lod, std::array<int, 3> lo, std::array<int, 3> hi) {
	Soup s;
	typedef std::tuple<float, float, float> P; // exact: seams must be bit-identical
	std::map<std::pair<P, P>, int> edges;
	MeshData m;
	auto t0 = std::chrono::steady_clock::now();
	for (int z = lo[2]; z < hi[2]; z++)
		for (int y = lo[1]; y < hi[1]; y++)
			for (int x = lo[0]; x < hi[0]; x++) {
				if (!v.mesh_chunk(x, y, z, lod(x, y, z), m)) continue;
				Vec3 origin = v.chunk_origin(x, y, z);
				for (size_t i = 0; i < m.idx.size(); i += 3) {
					Vec3 p[3], n[3];
					P k[3];
					for (int c = 0; c < 3; c++) {
						int id = m.idx[i + c];
						p[c] = Vec3(m.pos[id * 3], m.pos[id * 3 + 1], m.pos[id * 3 + 2]) + origin;
						n[c] = Vec3(m.nrm[id * 3], m.nrm[id * 3 + 1], m.nrm[id * 3 + 2]);
						k[c] = P(p[c].x, p[c].y, p[c].z);
						s.pts.push_back(p[c]);
					}
					if (k[0] == k[1] || k[1] == k[2] || k[0] == k[2]) continue;
					for (int c = 0; c < 3; c++) edges[{k[c], k[(c + 1) % 3]}]++;
					Vec3 fn = (p[2] - p[0]).cross(p[1] - p[0]); // clockwise front
					s.volume += p[0].dot(p[2].cross(p[1])) / 6.0;
					if (fn.len() > 1e-9f && fn.norm().dot((n[0] + n[1] + n[2]).norm()) < 0.0f) s.bad_normals++;
					s.tris++;
				}
			}
	s.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	for (auto &e : edges) {
		auto it = edges.find({e.first.second, e.first.first});
		if (it == edges.end() || it->second != e.second) s.open_edges++;
	}
	return s;
}

static Soup mesh_all(const Volume &v, const std::function<int(int, int, int)> &lod) {
	return mesh_all(v, lod, {v.bmin[0], v.bmin[1], v.bmin[2]}, {v.bmax[0], v.bmax[1], v.bmax[2]});
}

int main() {
	const double PI = 3.14159265358979;
	auto lod0 = [](int, int, int) { return 0; };
	auto mixed = [](int x, int y, int z) { return (x + y * 2 + z) % 3; };

	printf("sphere\n");
	{
		Volume v(4, 4, 4, 0.5f);
		v.edit(SPHERE, BUILD, {16, 16, 16}, {10, 0, 0}, ID, 3, 0, 1, false);
		double want = 4.0 / 3.0 * PI * 1000;
		Soup a = mesh_all(v, lod0), b = mesh_all(v, mixed), c = mesh_all(v, [](int, int, int) { return 2; });
		printf("  lod0 %ld tris, mixed %ld, lod2 %ld; volume %.1f / %.1f / %.1f (exact %.1f)\n", a.tris, b.tris, c.tris, a.volume, b.volume, c.volume, want);
		CHECK(a.open_edges == 0 && b.open_edges == 0 && c.open_edges == 0, "open edges %ld %ld %ld", a.open_edges, b.open_edges, c.open_edges);
		CHECK(a.bad_normals == 0 && b.bad_normals < b.tris / 200, "flipped %ld %ld", a.bad_normals, b.bad_normals);
		CHECK(std::fabs(a.volume - want) < want * 0.01, "volume %.1f", a.volume);
		CHECK(std::fabs(c.volume - want) < want * 0.04, "lod2 volume %.1f", c.volume);
		CHECK(c.tris < a.tris * 0.6, "lod2 did not simplify: %ld vs %ld", c.tris, a.tris);

		RayHit h = v.raycast({16, 31, 16}, {0, -1, 0}, 20);
		CHECK(h.hit && std::fabs(h.pos.y - 26) < 0.06f && h.normal.y > 0.99f && h.material == 3, "hit %d y %.3f ny %.3f", h.hit, h.pos.y, h.normal.y);

		EditResult e = v.edit(SPHERE, ERASE, {16, 16, 16}, {5, 0, 0}, ID, 0, 0, 1, false);
		double removed = e.removed[3] * 0.125, dug = 4.0 / 3.0 * PI * 125;
		CHECK(std::fabs(removed - dug) < dug * 0.03, "removed %.1f want %.1f", removed, dug);
		Soup d = mesh_all(v, mixed);
		CHECK(d.open_edges == 0 && std::fabs(d.volume - (want - dug)) < want * 0.03, "hollow: open %ld volume %.1f", d.open_edges, d.volume);

		v.hardness[3] = UNBREAKABLE;
		CHECK(!v.edit(SPHERE, ERASE, {16, 16, 16}, {9, 0, 0}, ID, 0, 3, 1, false).changed, "unbreakable material was dug");
	}

	printf("sharp box (Hermite data)\n");
	{
		Volume s(4, 4, 4, 0.5f), r(4, 4, 4, 0.5f);
		// Faces deliberately off the voxel grid.
		s.edit(BOX, BUILD, {16.1f, 16.2f, 16.3f}, {6, 4, 5}, ID, 7, 0, 1, true);
		r.edit(BOX, BUILD, {16.1f, 16.2f, 16.3f}, {6, 4, 5}, ID, 7, 0, 1, false);
		Soup a = mesh_all(s, lod0), b = mesh_all(r, lod0), c = mesh_all(s, mixed);
		Vec3 corner(22.1f, 20.2f, 21.3f);
		printf("  volume sharp %.2f, density-only %.2f (exact 960); corner miss %.3f m vs %.3f m; lod tris %ld -> %ld\n", a.volume, b.volume, a.nearest(corner), b.nearest(corner), a.tris, c.tris);
		CHECK(a.open_edges == 0 && c.open_edges == 0, "open edges %ld %ld", a.open_edges, c.open_edges);
		CHECK(std::fabs(a.volume - 960) < 960 * 0.004, "sharp volume %.2f", a.volume);
		CHECK(a.nearest(corner) < 0.03f, "corner not reproduced: %.3f", a.nearest(corner));
		CHECK(std::fabs(c.volume - 960) < 960 * 0.01 && c.tris < a.tris * 0.7, "flat faces should collapse for free: %ld tris, volume %.2f", c.tris, c.volume);

		Volume t(4, 4, 4, 0.5f);
		const float cs = std::cos(0.5f), sn = std::sin(0.5f);
		const float rot[9] = {cs, 0, sn, 0, 1, 0, -sn, 0, cs};
		t.edit(BOX, BUILD, {16, 16, 16}, {6, 4, 5}, rot, 7, 0, 1, true);
		t.edit(CYLINDER, ERASE, {16, 16, 16}, {2, 6, 0}, ID, 0, 0, 1, true);
		Soup d = mesh_all(t, lod0);
		double want = 960 - PI * 4 * 8;
		printf("  rotated box minus cylinder: volume %.2f (exact %.2f)\n", d.volume, want);
		CHECK(d.open_edges == 0 && std::fabs(d.volume - want) < want * 0.01, "open %ld volume %.2f", d.open_edges, d.volume);
	}

	printf("endless sphere across the origin\n");
	{
		Volume v(0.5f);
		for (int z = -2; z < 2; z++) for (int y = -2; y < 2; y++) for (int x = -2; x < 2; x++) v.fill_chunk(x, y, z, nullptr);
		v.edit(SPHERE, BUILD, {0.3f, -0.2f, 0.1f}, {7, 0, 0}, ID, 3, 0, 1, false);
		double want = 4.0 / 3.0 * PI * 343;
		Soup a = mesh_all(v, lod0, {-2, -2, -2}, {2, 2, 2}), b = mesh_all(v, mixed, {-2, -2, -2}, {2, 2, 2});
		CHECK(a.open_edges == 0 && b.open_edges == 0, "open edges %ld %ld", a.open_edges, b.open_edges);
		CHECK(std::fabs(a.volume - want) < want * 0.01, "volume %.1f", a.volume);
	}

	printf("generator\n");
	{
		DemoTerrain g; g.seed = 1234; g.height = 32.0f; // = 4 chunks * 16 * 0.5, today's terrain test
		Volume v(8, 4, 8, 0.5f);
		auto t0 = std::chrono::steady_clock::now();
		v.fill_all(&g, 8);
		double gen = ms_since(t0);
		Soup a = mesh_all(v, lod0), b = mesh_all(v, mixed), c = mesh_all(v, [](int, int, int) { return 2; });
		int n = (v.bmax[0] - v.bmin[0]) * (v.bmax[1] - v.bmin[1]) * (v.bmax[2] - v.bmin[2]);
		printf("  generate %.0f ms; %d chunks: lod0 %ld tris %.2f ms/chunk, mixed %ld, lod2 %ld tris %.2f ms/chunk\n", gen, n, a.tris, a.ms / n, b.tris, c.tris, c.ms / n);
		CHECK(a.tris > 10000, "no terrain");
		CHECK(a.open_edges == 0 && b.open_edges == 0 && c.open_edges == 0, "open edges %ld %ld %ld", a.open_edges, b.open_edges, c.open_edges);
		CHECK(a.bad_normals < a.tris / 200, "flipped %ld of %ld", a.bad_normals, a.tris);
		CHECK(std::fabs(c.volume - a.volume) < a.volume * 0.02, "lod2 changed the volume: %.0f vs %.0f", c.volume, a.volume);
		CHECK(gen < 63 * 1.1 * 1.5, "generation slower than baseline: %.0f ms", gen); // generous for noise; print it

		Volume e(0.5f);
		e.fill_chunk(3, 1, -5, &g); Volume f(0.5f); f.fill_chunk(3, 1, -5, &g);
		bool same = true;
		for (int z = 0; z < 16; z++) for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++)
			same &= e.get(48 + x, 16 + y, -80 + z) == f.get(48 + x, 16 + y, -80 + z);
		CHECK(same, "generation not deterministic");
	}

	printf(failures ? "FAILED (%d)\n" : "ALL CORE TESTS PASSED\n", failures);
	return failures ? 1 : 0;
}
