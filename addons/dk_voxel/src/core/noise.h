// Noise and caching helpers every generator can use. Godot-free and header-only, so a game's own generator
// includes this and nothing else.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace dkv {

inline uint32_t hash3(int x, int y, int z, uint32_t s) {
	uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u ^ (uint32_t)z * 0xcb1ab31fu ^ s;
	h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
	return h;
}

// Gradient noise, roughly -1..1.
inline float noise3(float x, float y, float z, uint32_t s) {
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

inline float fbm(float x, float y, float z, uint32_t s, int oct) {
	float a = 0.5f, sum = 0;
	for (int i = 0; i < oct; i++, a *= 0.5f, x *= 2, y *= 2, z *= 2) sum += a * noise3(x, y, z, s + i * 1013u);
	return sum * 2.0f;
}

// Caches the last NX x NZ column values for one (tag, x0, z0). A generator supplies `tag` as a hash of the
// settings the columns depend on, so the entry can never be served for different settings — and because the
// key is the origin the values were computed for, the order the engine happens to fill blocks in is
// irrelevant. Blocks are 8x8 columns, so NX = NZ = 8 suits a per-block cache.
template <typename T, int NX, int NZ>
struct ColumnCache {
	T data[NX * NZ]{};
	uint64_t tag = ~0ull;
	int x0 = 0, z0 = 0;

	// Returns the storage. `fresh` is true when it was empty or held another key, so the caller fills it.
	T *acquire(uint64_t key_tag, int key_x0, int key_z0, bool &fresh) {
		fresh = !(key_tag == tag && key_x0 == x0 && key_z0 == z0);
		if (fresh) {
			tag = key_tag;
			x0 = key_x0;
			z0 = key_z0;
		}
		return data;
	}
};

// Packs a generator's settings into a ColumnCache tag, exactly (no float truncation).
inline uint64_t settings_tag(uint32_t seed, float a, float b) {
	uint32_t ab, bb;
	memcpy(&ab, &a, sizeof ab);
	memcpy(&bb, &b, sizeof bb);
	uint64_t h = (uint64_t)seed * 0x100000001b3ull;
	h = (h ^ ab) * 0x100000001b3ull;
	return (h ^ bb) * 0x100000001b3ull;
}

} // namespace dkv
