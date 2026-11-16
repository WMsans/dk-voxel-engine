#include "level_terrain.h"

#include <cstring>

namespace dkv {

namespace {

struct Reader {
	const uint8_t *p, *end;
	template <typename T>
	bool get(T &v) {
		if ((size_t)(end - p) < sizeof v) return false;
		memcpy(&v, p, sizeof v); // ponytail: assumes a little-endian host, like every platform Godot ships on
		p += sizeof v;
		return true;
	}
};

constexpr int MAX_SAMPLES = 2048; // per axis; a sanity bound, not a design limit

} // namespace

float LevelTerrain::Part::sample(float x, float y, float z) const {
	const float far = 127.0f / SD_SCALE * cell;
	const float g[3] = {(x - origin[0]) / cell, (y - origin[1]) / cell, (z - origin[2]) / cell};
	int i[3];
	float f[3];
	for (int a = 0; a < 3; a++) {
		if (!(g[a] >= 0.0f && g[a] <= n[a] - 1)) return far;
		i[a] = std::min((int)g[a], n[a] - 2);
		f[a] = g[a] - i[a];
	}
	const size_t sy = n[0], sz = (size_t)n[0] * n[1];
	const int8_t *s = sd.data() + i[0] + i[1] * sy + i[2] * sz;
	auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
	const float c00 = lerp(s[0], s[1], f[0]), c10 = lerp(s[sy], s[sy + 1], f[0]);
	const float c01 = lerp(s[sz], s[sz + 1], f[0]), c11 = lerp(s[sz + sy], s[sz + sy + 1], f[0]);
	return lerp(lerp(c00, c10, f[1]), lerp(c01, c11, f[1]), f[2]) / SD_SCALE * cell;
}

bool LevelTerrain::load(const uint8_t *data, size_t len, std::string &err) {
	Reader r{data, data + len};
	char magic[4];
	uint32_t version = 0, count = 0;
	LevelTerrain lv;
	if (!r.get(magic) || memcmp(magic, "DKVL", 4) != 0) return err = "not a .dkvl level", false;
	if (!r.get(version) || version != 1) return err = "unsupported .dkvl version", false;
	if (!r.get(lv.size) || !r.get(lv.spawn) || !r.get(count)) return err = "truncated header", false;
	for (uint32_t k = 0; k < count; k++) {
		Part pt;
		uint16_t reserved;
		uint32_t n[3];
		if (!r.get(pt.mode) || !r.get(pt.material) || !r.get(reserved) || !r.get(pt.origin) || !r.get(pt.cell) || !r.get(n))
			return err = "truncated part " + std::to_string(k), false;
		if (pt.mode > PAINT) return err = "part " + std::to_string(k) + ": unknown mode", false;
		if (!(pt.cell > 0.0f)) return err = "part " + std::to_string(k) + ": cell must be > 0", false;
		for (int a = 0; a < 3; a++) {
			if (n[a] < 2 || n[a] > MAX_SAMPLES) return err = "part " + std::to_string(k) + ": bad grid size", false;
			pt.n[a] = (int)n[a];
		}
		const size_t total = (size_t)n[0] * n[1] * n[2];
		if ((size_t)(r.end - r.p) < total) return err = "truncated part " + std::to_string(k), false;
		pt.sd.assign((const int8_t *)r.p, (const int8_t *)r.p + total);
		r.p += total;
		lv.parts.push_back(std::move(pt));
	}
	if (r.p != r.end) return err = "trailing bytes after the last part", false;
	memcpy(size, lv.size, sizeof size);
	memcpy(spawn, lv.spawn, sizeof spawn);
	parts = std::move(lv.parts);
	return true;
}

void LevelTerrain::generate_block(const int origin[3], float cell, Voxel out[512]) const {
	// Only the parts whose grid overlaps this block; most blocks touch none and are air.
	const Part *hit[64];
	int nhit = 0;
	for (const Part &p : parts) {
		bool overlap = true;
		for (int a = 0; a < 3; a++) {
			const float lo = origin[a] * cell, hi = (origin[a] + BLOCK - 1) * cell;
			overlap &= hi >= p.origin[a] && lo <= p.origin[a] + (p.n[a] - 1) * p.cell;
		}
		if (overlap && nhit < 64) hit[nhit++] = &p; // ponytail: 64 parts per block; raise if a level stacks more
	}
	for (int i = 0; i < 512; i++) {
		const float x = (origin[0] + (i & 7)) * cell, y = (origin[1] + ((i >> 3) & 7)) * cell, z = (origin[2] + (i >> 6)) * cell;
		float sd = 1e9f;
		uint8_t m = 0;
		for (int k = 0; k < nhit; k++) {
			const Part &p = *hit[k];
			const float s = p.sample(x, y, z);
			if (p.mode == PASTE) {
				if (s < 0.0f || s < sd) m = p.material; // a later part overwrites the material inside it
				sd = std::min(sd, s);
			} else if (p.mode == ERASE) {
				sd = std::max(sd, -s);
			} else if (s < 0.0f) {
				m = p.material;
			}
		}
		out[i] = {sdf_to_density(std::clamp(sd / cell, -2.0f, 2.0f)), m}; // clamp: 1e9 would overflow the int
	}
}

} // namespace dkv
