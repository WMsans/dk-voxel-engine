// Core self-test: scons selftest && build/selftest
#include "terrain/demo_terrain.h"
#include "terrain/level_terrain.h"
#include "core/noise.h"
#include "core/voxel_generator.h"
#include "core/voxel_volume.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <thread>
#include <tuple>

using namespace dkv;

static const float ID[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
static const uint64_t DEMO_WORLD_HASH = 0xc7cee77b1f580a40ull; // without FP contraction (see SConscript); the old
// 0x6312024949a38932 was recorded by a clang -march=native build, where FMA changes the noise
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

// FNV-1a over every chunk's serialised bytes. Any change to generated terrain, or to the block layout the
// core picks, moves this number.
static uint64_t hash_volume(const Volume &v) {
	uint64_t h = 1469598103934665603ull;
	for (int z = v.bmin[2]; z < v.bmax[2]; z++)
		for (int y = v.bmin[1]; y < v.bmax[1]; y++)
			for (int x = v.bmin[0]; x < v.bmax[0]; x++)
				for (uint8_t c : v.serialize_chunk(x, y, z)) { h ^= c; h *= 1099511628211ull; }
	return h;
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

	printf("inner material\n");
	{
		Volume v(4, 4, 4, 0.5f);
		v.edit(BOX, BUILD, {16, 16, 16}, {10, 10, 10}, ID, 1, 0, 1, false);
		v.inner[1] = 2; v.inner[2] = 3; // grass -> torn turf -> soil
		v.edit(SPHERE, ERASE, {16, 26, 16}, {3.2f, 0, 0}, ID, 0, 0, 1, false);
		Voxel dug = v.get(32, 45, 32), below = v.get(32, 38, 32), far = v.get(52, 52, 52);
		CHECK(dug.d >= 128 && dug.m == 2, "dug surface voxel: d %d m %d", dug.d, dug.m);
		CHECK(below.m == 1 && far.m == 1, "untouched voxels changed: %d %d", below.m, far.m);
		CHECK(v.get(32, 44, 32).m == 2, "solid 1.6 cells from the cut should be repainted: %d", v.get(32, 44, 32).m);
		CHECK(!v.edit(SPHERE, ERASE, {10, 28.6f, 10}, {2, 0, 0}, ID, 0, 0, 1, false).changed && v.get(20, 52, 20).m == 1, "a dig in the air repainted the ground");
		v.edit(SPHERE, ERASE, {16, 26, 16}, {3.4f, 0, 0}, ID, 0, 0, 1, false);
		CHECK(v.get(32, 45, 32).m == 3, "second dig should advance the chain: %d", v.get(32, 45, 32).m);
		v.expose_inner = false;
		v.edit(SPHERE, ERASE, {24, 26, 24}, {3.2f, 0, 0}, ID, 0, 0, 1, false);
		CHECK(v.get(48, 45, 48).m == 1, "expose_inner off still swapped: %d", v.get(48, 45, 48).m);
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
		printf("  world hash %016llx\n", (unsigned long long)hash_volume(v));
		double gen = ms_since(t0);
		Soup a = mesh_all(v, lod0), b = mesh_all(v, mixed), c = mesh_all(v, [](int, int, int) { return 2; });
		CHECK(hash_volume(v) == DEMO_WORLD_HASH, "world hash %016llx, want %016llx",
				(unsigned long long)hash_volume(v), (unsigned long long)DEMO_WORLD_HASH);
		int n = (v.bmax[0] - v.bmin[0]) * (v.bmax[1] - v.bmin[1]) * (v.bmax[2] - v.bmin[2]);
		printf("  generate %.0f ms; %d chunks: lod0 %ld tris %.2f ms/chunk, mixed %ld, lod2 %ld tris %.2f ms/chunk\n", gen, n, a.tris, a.ms / n, b.tris, c.tris, c.ms / n);
		CHECK(a.tris > 10000, "no terrain");
		CHECK(a.open_edges == 0 && b.open_edges == 0 && c.open_edges == 0, "open edges %ld %ld %ld", a.open_edges, b.open_edges, c.open_edges);
		CHECK(a.bad_normals < a.tris / 200, "flipped %ld of %ld", a.bad_normals, a.tris);
		CHECK(std::fabs(c.volume - a.volume) < a.volume * 0.02, "lod2 changed the volume: %.0f vs %.0f", c.volume, a.volume);
		CHECK(gen < 63 * 1.1 * 1.5, "generation slower than baseline: %.0f ms", gen); // generous for noise; print it

		Volume e(0.5f);
		e.fill_chunk(3, 1, -5, &g);
		DemoTerrain low; low.seed = g.seed; low.height = 4.0f; // same seed and cell, different height
		Volume r(0.5f); // reference for low: generated on a fresh thread, so its column cache is cold
		std::thread([&] { r.fill_chunk(3, 1, -5, &low); }).join();
		Volume lo(0.5f); lo.fill_chunk(3, 1, -5, &low); // this thread already cached g's columns
		Volume f(0.5f); f.fill_chunk(3, 1, -5, &g);
		bool same = true, stable = true;
		for (int z = 0; z < 16; z++) for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
			same &= e.get(48 + x, 16 + y, -80 + z) == f.get(48 + x, 16 + y, -80 + z);
			stable &= r.get(48 + x, 16 + y, -80 + z) == lo.get(48 + x, 16 + y, -80 + z);
		}
		CHECK(same, "generation not deterministic");
		CHECK(stable, "column cache ignored height: settings are part of the cache key");
	}

	printf("serialise\n");
	{
		Volume v(0.5f);
		for (int z = -1; z <= 1; z++) for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) v.fill_chunk(x, y, z, nullptr);
		v.edit(BOX, BUILD, {1.1f, 2.2f, 0.3f}, {3, 2, 2.5f}, ID, 7, 0, 1, true); // sharp: Hermite present
		CHECK(v.is_modified(0, 0, 0), "edit did not mark the chunk");
		MeshData before; v.mesh_chunk(0, 0, 0, 0, before);
		auto bytes = v.serialize_chunk(0, 0, 0);
		v.remove_chunk(0, 0, 0);
		CHECK(!v.has_chunk(0, 0, 0) && !v.neighbors_ready(0, 0, 0), "remove");
		CHECK(v.deserialize_chunk(0, 0, 0, bytes.data(), bytes.size()) && !v.is_modified(0, 0, 0), "restore");
		MeshData after; v.mesh_chunk(0, 0, 0, 0, after);
		CHECK(before.pos == after.pos && before.idx == after.idx, "round trip changed the mesh");
		CHECK(!v.deserialize_chunk(0, 0, 0, bytes.data(), bytes.size() - 1), "truncated data accepted");
	}
	printf("edit clips at unloaded chunks\n");
	{
		Volume v(0.5f);
		v.fill_chunk(0, 0, 0, nullptr);                                   // only one chunk loaded
		EditResult r = v.edit(SPHERE, BUILD, {8, 4, 4}, {6, 0, 0}, ID, 3, 0, 1, false); // reaches into x = 1
		CHECK(r.changed && v.chunk_count() == 1 && !v.has_chunk(1, 0, 0), "edit created a chunk");
		EditResult e = v.edit(SPHERE, ERASE, {8, 4, 4}, {6, 0, 0}, ID, 0, 0, 1, false);
		double m3 = 0; for (float f : e.removed) m3 += f * 0.125;  // removed is in voxels; 0.5^3 m3 each
		CHECK(m3 > 50 && m3 < 0.5 * 4.0 / 3.0 * 3.14159 * 216, "removed %.0f m3: should be only the part inside chunk 0 (< half sphere)", m3);
	}

	printf("concurrent fill and query\n");
	{
		// Workers insert chunks under the unique lock while the main thread reads the map. Without the
		// shared_lock in get()/density()/raycast() this is a data race on the chunk map (a rehash under a
		// reader), so it is a smoke test for the locking, not a proof of it: build with -fsanitize=thread.
		Volume v(0.5f);
		const int N = 256; // 8 x 8 x 4 chunks, all distinct
		std::atomic<int> next{0};
		auto writer = [&] {
			for (;;) {
				int i = next.fetch_add(1);
				if (i >= N) return;
				v.fill_chunk(i % 8 - 4, (i / 8) % 8 - 4, (i / 64) % 4 - 2, nullptr);
			}
		};
		std::vector<std::thread> pool;
		for (int t = 0; t < 4; t++) pool.emplace_back(writer);
		for (int i = 0; i < 200000; i++) { // read the map while it grows
			v.get(i % 64 - 32, i / 64 % 16 - 8, i / 1024 % 64 - 32);
			if ((i & 4095) == 0) { v.density({0, 0, 0}); v.raycast({0, 100, 0}, {0, -1, 0}, 200); }
		}
		for (auto &t : pool) t.join();
		bool all = v.chunk_count() == (size_t)N;
		for (int i = 0; i < N; i++) all &= v.has_chunk(i % 8 - 4, (i / 8) % 8 - 4, (i / 64) % 4 - 2);
		CHECK(all, "concurrent fill lost chunks: %zu", v.chunk_count());
	}

	printf("column cache\n");
	{
		ColumnCache<float, 8, 8> c;
		bool fresh = false;
		float *a = c.acquire(1, 0, 0, fresh);
		CHECK(fresh, "an empty cache reports fresh");
		for (int i = 0; i < 64; i++) a[i] = (float)i;
		float *b = c.acquire(1, 0, 0, fresh);
		CHECK(!fresh && b[63] == 63.0f, "the same key hits");
		c.acquire(2, 0, 0, fresh); // another generator's settings
		CHECK(fresh, "a different settings tag misses");
		c.acquire(1, 8, 0, fresh); // another column, same settings
		CHECK(fresh, "a different origin misses");
		c.acquire(1, 0, 0, fresh);
		CHECK(fresh, "a key that was overwritten is not served stale");
		ColumnCache<float, 8, 8> other;
		CHECK(other.acquire(1, 0, 0, fresh) != nullptr && fresh, "a second cache on the same thread starts cold");
	}

	printf("demo material ids\n");
	{
		DemoTerrain g; g.seed = 1234; g.height = 32.0f;
		Volume v(4, 4, 4, 0.5f);
		v.fill_all(&g, 4);
		bool known = true;
		for (int i = 0; i < 4; i++)
			for (int y = 0; y < 64; y++) {
				uint8_t m = v.get(i * 16 + 8, y, 8).m;
				known &= m == 0 || (m >= GRASS && m <= CRYSTAL);
			}
		CHECK(known, "generated terrain uses only named materials");
	}

	printf("level profile (.dkvl)\n");
	{
		// A hollow ball: paste a sphere (r 6, rock), erase one inside it (r 3), repaint the top half gold.
		std::vector<uint8_t> f;
		auto put = [&](const void *p, size_t n) { f.insert(f.end(), (const uint8_t *)p, (const uint8_t *)p + n); };
		auto sphere = [&](uint8_t mode, uint8_t mat, float r, float y0) {
			const float c = 16, cell = 0.5f, org[3] = {c - 8, c - 8, c - 8};
			const uint32_t n[3] = {33, 33, 33};
			uint8_t hdr[4] = {mode, mat, 0, 0};
			put(hdr, 4); put(org, 12); put(&cell, 4); put(n, 12);
			for (uint32_t z = 0; z < 33; z++) for (uint32_t y = 0; y < 33; y++) for (uint32_t x = 0; x < 33; x++) {
				Vec3 p(org[0] + x * cell, org[1] + y * cell, org[2] + z * cell);
				float d = (p - Vec3(c, c, c)).len() - r;
				if (y0 > 0) d = c - p.y; // the paint part: everything above the centre
				f.push_back((uint8_t)(int8_t)std::clamp((int)std::lround(d / cell * LevelTerrain::SD_SCALE), -127, 127));
			}
		};
		const float size[3] = {32, 32, 32}, spawn[3] = {16, 23, 16};
		const uint32_t count = 3;
		put("DKVL", 4); uint32_t ver = 1; put(&ver, 4); put(size, 12); put(spawn, 12); put(&count, 4);
		sphere(LevelTerrain::PASTE, 3, 6, 0);
		sphere(LevelTerrain::ERASE, 0, 3, 0);
		sphere(LevelTerrain::PAINT, 5, 0, 1);

		LevelTerrain g;
		std::string err;
		CHECK(g.load(f.data(), f.size(), err) && g.parts.size() == 3 && g.spawn[1] == 23, "load: %s", err.c_str());
		LevelTerrain bad;
		CHECK(!bad.load(f.data(), f.size() - 1, err), "truncated level accepted");
		f.push_back(0);
		CHECK(!bad.load(f.data(), f.size(), err), "trailing bytes accepted");

		Volume v(4, 4, 4, 0.5f);
		v.fill_all(&g, 4);
		double want = 4.0 / 3.0 * PI * (216 - 27);
		Soup a = mesh_all(v, lod0);
		printf("  hollow ball: %ld tris, volume %.1f (exact %.1f)\n", a.tris, a.volume, want);
		CHECK(a.open_edges == 0 && std::fabs(a.volume - want) < want * 0.03, "open %ld volume %.1f", a.open_edges, a.volume);
		CHECK(v.get(32, 32, 32).d < 128 && v.get(32, 24, 32).d > 128, "erase did not hollow the ball");
		CHECK(v.get(32, 40, 32).m == 5 && v.get(32, 24, 32).m == 3, "paint: top %d bottom %d", v.get(32, 40, 32).m, v.get(32, 24, 32).m);
		CHECK(v.get(2, 2, 2).d == 0, "outside every part is air");
	}

	printf("skirts\n");
	{ // a closed surface inside one chunk gets none; a sphere cut by chunk borders gets a strip on each cut edge
		Volume v(2, 2, 2, 0.5f);
		v.edit(SPHERE, BUILD, {4, 4, 4}, {2.5f, 0, 0}, ID, 3, 0, 1, false); // inside chunk (0,0,0): 0..8 m
		MeshData m;
		v.mesh_chunk(0, 0, 0, 0, m);
		size_t ni = m.idx.size();
		add_skirts(m, 1.0f);
		CHECK(ni > 0 && m.idx.size() == ni && m.skirt_from == ni, "closed mesh got skirts: %zu -> %zu", ni, m.idx.size());
		v.edit(SPHERE, BUILD, {8, 8, 8}, {3, 0, 0}, ID, 3, 0, 1, false); // straddles all 8 chunks
		v.mesh_chunk(0, 0, 0, 0, m);
		ni = m.idx.size();
		add_skirts(m, 1.0f);
		CHECK(m.skirt_from == ni && m.idx.size() > ni && (m.idx.size() - ni) % 6 == 0, "cut mesh: %zu tris, %zu skirt", ni / 3, (m.idx.size() - ni) / 3);
		CHECK(m.pos.size() / 3 == m.nrm.size() / 3 && m.col.size() == m.pos.size() / 3 * 4, "skirt vertex arrays disagree");
	}

	printf(failures ? "FAILED (%d)\n" : "ALL CORE TESTS PASSED\n", failures);
	return failures ? 1 : 0;
}
