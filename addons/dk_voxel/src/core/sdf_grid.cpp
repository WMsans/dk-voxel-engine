#include "sdf_grid.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace dkv {

float SdfGrid::sample(Vec3 p) const {
	const float far = 127.0f / SD_SCALE * cell;
	float f[3];
	int i[3];
	for (int a = 0; a < 3; a++) {
		const float g = (p[a] - origin[a]) / cell;
		if (!(g >= 0.0f && g <= n[a] - 1)) return far;
		i[a] = std::min((int)g, n[a] - 2);
		f[a] = g - i[a];
	}
	const size_t sy = n[0], sz = (size_t)n[0] * n[1];
	const int8_t *s = sd.data() + i[0] + i[1] * sy + i[2] * sz;
	auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
	const float c00 = lerp(s[0], s[1], f[0]), c10 = lerp(s[sy], s[sy + 1], f[0]);
	const float c01 = lerp(s[sz], s[sz + 1], f[0]), c11 = lerp(s[sz + sy], s[sz + sy + 1], f[0]);
	return lerp(lerp(c00, c10, f[1]), lerp(c01, c11, f[1]), f[2]) / SD_SCALE * cell;
}

namespace {

// Ericson, Real-Time Collision Detection 5.1.5.
Vec3 closest_on_triangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c) {
	const Vec3 ab = b - a, ac = c - a, ap = p - a;
	const float d1 = ab.dot(ap), d2 = ac.dot(ap);
	if (d1 <= 0 && d2 <= 0) return a;
	const Vec3 bp = p - b;
	const float d3 = ab.dot(bp), d4 = ac.dot(bp);
	if (d3 >= 0 && d4 <= d3) return b;
	const float vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
	const Vec3 cp = p - c;
	const float d5 = ab.dot(cp), d6 = ac.dot(cp);
	if (d6 >= 0 && d5 <= d6) return c;
	const float vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
	const float va = d3 * d6 - d5 * d4;
	if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
	const float den = 1.0f / (va + vb + vc);
	return a + ab * (vb * den) + ac * (vc * den);
}

constexpr int PAD = 3; // cells of air around the mesh
constexpr float BAND = 2.0f; // cells of exact distance either side of the surface

} // namespace

SdfGrid voxelize_triangles(const std::vector<Vec3> &tris, float cell) {
	SdfGrid g;
	g.cell = cell;
	if (tris.size() < 3) return g;
	int lo[3], hi[3];
	for (int a = 0; a < 3; a++) {
		float mn = tris[0][a], mx = mn;
		for (const Vec3 &v : tris) mn = std::min(mn, v[a]), mx = std::max(mx, v[a]);
		lo[a] = (int)std::floor(mn / cell) - PAD;
		hi[a] = (int)std::ceil(mx / cell) + PAD;
		g.n[a] = hi[a] - lo[a] + 1;
		g.origin[a] = lo[a] * cell;
	}
	const size_t total = (size_t)g.n[0] * g.n[1] * g.n[2];
	auto at = [&](const int i[3]) { return i[0] + (size_t)g.n[0] * (i[1] + (size_t)g.n[1] * i[2]); };
	auto grid = [&](float x, int a) { return (x - g.origin[a]) / cell; }; // metres -> fractional sample index

	// Inside: along each axis, every row of samples is one ray. A triangle's crossing adds +-1 to the winding
	// of the samples past it (sign = which way it faces), so overlapping shells add up instead of cancelling
	// like plain parity would. Three axes vote, so a ray slipping through a small hole cannot flood a row.
	std::vector<uint8_t> votes(total, 0);
	std::vector<std::vector<std::pair<float, int>>> rows;
	for (int a = 0; a < 3; a++) {
		const int u = (a + 1) % 3, v = (a + 2) % 3;
		rows.assign((size_t)g.n[u] * g.n[v], {});
		for (size_t t = 0; t < tris.size(); t += 3) {
			const Vec3 &A = tris[t], &B = tris[t + 1], &C = tris[t + 2];
			const float au = grid(A[u], u), av = grid(A[v], v), bu = grid(B[u], u), bv = grid(B[v], v), cu = grid(C[u], u), cv = grid(C[v], v);
			const float area = (bu - au) * (cv - av) - (bv - av) * (cu - au);
			if (area == 0.0f) continue; // edge-on to this axis: no crossing
			const int u0 = std::max(0, (int)std::ceil(std::min({au, bu, cu}))), u1 = std::min(g.n[u] - 1, (int)std::floor(std::max({au, bu, cu})));
			const int v0 = std::max(0, (int)std::ceil(std::min({av, bv, cv}))), v1 = std::min(g.n[v] - 1, (int)std::floor(std::max({av, bv, cv})));
			for (int iv = v0; iv <= v1; iv++)
				for (int iu = u0; iu <= u1; iu++) {
					// Nudged off the lattice so a ray never runs exactly through a shared edge or vertex.
					const float pu = iu + 1.3e-4f, pv = iv + 0.7e-4f;
					const float w0 = ((cu - bu) * (pv - bv) - (cv - bv) * (pu - bu)) / area;
					const float w1 = ((au - cu) * (pv - cv) - (av - cv) * (pu - cu)) / area;
					const float w2 = 1.0f - w0 - w1;
					if (w0 < 0 || w1 < 0 || w2 < 0) continue;
					const float x = grid(A[a] * w0 + B[a] * w1 + C[a] * w2, a);
					rows[(size_t)iv * g.n[u] + iu].push_back({x, area > 0 ? 1 : -1});
				}
		}
		for (int iv = 0; iv < g.n[v]; iv++)
			for (int iu = 0; iu < g.n[u]; iu++) {
				auto &r = rows[(size_t)iv * g.n[u] + iu];
				if (r.empty()) continue;
				std::sort(r.begin(), r.end());
				int i[3], wind = 0;
				size_t k = 0;
				i[u] = iu, i[v] = iv;
				for (i[a] = 0; i[a] < g.n[a]; i[a]++) {
					for (; k < r.size() && r[k].first < i[a]; k++) wind += r[k].second;
					votes[at(i)] += wind != 0;
				}
			}
	}

	// Distance: each triangle only reaches the samples within BAND cells of it.
	std::vector<float> dist(total, BAND * cell);
	for (size_t t = 0; t < tris.size(); t += 3) {
		int s0[3], s1[3];
		for (int a = 0; a < 3; a++) {
			const float mn = std::min({tris[t][a], tris[t + 1][a], tris[t + 2][a]}), mx = std::max({tris[t][a], tris[t + 1][a], tris[t + 2][a]});
			s0[a] = std::max(0, (int)std::ceil(grid(mn, a) - BAND));
			s1[a] = std::min(g.n[a] - 1, (int)std::floor(grid(mx, a) + BAND));
		}
		int i[3];
		for (i[2] = s0[2]; i[2] <= s1[2]; i[2]++)
			for (i[1] = s0[1]; i[1] <= s1[1]; i[1]++)
				for (i[0] = s0[0]; i[0] <= s1[0]; i[0]++) {
					const Vec3 p(g.origin[0] + i[0] * cell, g.origin[1] + i[1] * cell, g.origin[2] + i[2] * cell);
					float &d = dist[at(i)];
					d = std::min(d, (closest_on_triangle(p, tris[t], tris[t + 1], tris[t + 2]) - p).len());
				}
	}
	g.sd.resize(total);
	for (size_t k = 0; k < total; k++) {
		const float s = (votes[k] >= 2 ? -dist[k] : dist[k]) / cell * SdfGrid::SD_SCALE;
		g.sd[k] = (int8_t)std::clamp((int)std::lround(s), -127, 127);
	}
	return g;
}

} // namespace dkv
