#include "demo_generator.h"

#include <algorithm>

namespace dkv {

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

void DemoTerrain::generate_block(const int origin[3], float cell, Voxel out[512]) const {
	const float H = height;
	// The 8x8 columns of this block. Blocks are generated in x, y, z order, so a chunk revisits 4 columns;
	// the cache is keyed by the column's parity, which covers that order (and anything aligned like it).
	struct Col { uint32_t seed = 0; float cell = 0; int x = 0, z = 0; float hmax = 0; float h[64]; };
	static thread_local Col col[4];
	Col &c = col[((origin[0] >> 3) & 1) | (((origin[2] >> 3) & 1) << 1)];
	if (c.seed != seed || c.cell != cell || c.x != origin[0] || c.z != origin[2]) {
		c.hmax = -1e9f;
		for (int i = 0; i < 64; i++) {
			float wx = (origin[0] + (i & 7)) * cell, wz = (origin[2] + (i >> 3)) * cell;
			float cont = fbm(wx / 150.0f, 0.5f, wz / 150.0f, seed, 3);
			float ridge = 1.0f - std::fabs(fbm(wx / 75.0f, 7.5f, wz / 75.0f, seed + 77, 4)) * 1.6f; // crests where the noise crosses zero
			float mount = std::clamp(cont * 2.2f + 0.45f, 0.0f, 1.0f); // lowlands -> mountains
			float mesa = std::clamp(fbm(wx / 60.0f, 3.5f, wz / 60.0f, seed + 191, 2) * 5.0f - 0.5f, 0.0f, 1.0f);
			c.h[i] = H * (0.17f + cont * 0.08f + mount * std::max(ridge, 0.0f) * ridge * 0.45f + mesa * 0.12f);
			c.hmax = std::max(c.hmax, c.h[i]);
		}
		c.seed = seed; c.cell = cell; c.x = origin[0]; c.z = origin[2];
	}
	if (origin[1] * cell > c.hmax + 9.0f) { // well above the terrain: air
		std::fill_n(out, 512, Voxel{});
		return;
	}
	for (int i = 0; i < 512; i++) {
		int lx = i & 7, ly = (i >> 3) & 7, lz = i >> 6;
		float wx = (origin[0] + lx) * cell, wy = (origin[1] + ly) * cell, wz = (origin[2] + lz) * cell;
		float h = c.h[lz * 8 + lx], depth = h - wy;
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
		out[i] = {sdf_to_density(sd / cell), m};
	}
}

} // namespace dkv