// The demo terrain, written as rules: each function below answers one question about a place in the world,
// and generate_block only composes them. Tune the constants, or replace a rule, to change the world.
//
// Everything must stay deterministic in (seed, height, cell) and safe to call from any worker thread.
#include "demo_terrain.h"

#include "core/noise.h"

#include <algorithm>

namespace dkv {

namespace {

constexpr int COLUMNS = 8; // columns per block edge: one cache entry covers a whole block
constexpr float SKY_MARGIN = 9.0f; // metres above the tallest column that a block is pure air

// Height: a continent term, a ridge term that adds crests where its noise crosses zero, and flat mesas.
constexpr float CONTINENT_SCALE = 150.0f; // metres per noise unit
constexpr float CONTINENT_LIFT = 0.08f; // fraction of `height`
constexpr float RIDGE_SCALE = 75.0f;
constexpr float RIDGE_SHARPNESS = 1.6f;
constexpr float MOUNTAIN_GAIN = 2.2f;
constexpr float MOUNTAIN_BIAS = 0.45f;
constexpr float MOUNTAIN_FRACTION = 0.45f;
constexpr float MESA_SCALE = 60.0f;
constexpr float MESA_GAIN = 5.0f;
constexpr float MESA_OFFSET = 0.5f;
constexpr float MESA_FRACTION = 0.12f;
constexpr float HEIGHT_FLOOR = 0.17f; // fraction of `height` that is never eroded

// Overhangs: added to the surface distance up high, so cliffs lean out.
constexpr float OVERHANG_ENTRY = 8.0f; // only near the surface
constexpr float OVERHANG_SCALE_XZ = 16.0f;
constexpr float OVERHANG_SCALE_Y = 11.0f;
constexpr float OVERHANG_AMPLITUDE = 7.0f;
constexpr float OVERHANG_RAMP_FROM = 0.7f; // fraction of `height` where overhangs start
constexpr float OVERHANG_RAMP_SLOPE = 3.0f;

// Caves: tunnels where two noise sheets cross, plus one blob field for caverns.
constexpr float CAVE_ENTRY = 1.0f; // metres below the surface where carving starts
constexpr float TUNNEL_SCALE_XZ = 26.0f;
constexpr float TUNNEL_SCALE_Y = 20.0f;
constexpr float TUNNEL_WIDTH = 0.075f;
constexpr float TUNNEL_GAIN = 30.0f;
constexpr float CAVERN_SCALE_XZ = 44.0f;
constexpr float CAVERN_SCALE_Y = 30.0f;
constexpr float CAVERN_THRESHOLD = 0.42f;
constexpr float CAVERN_GAIN = 40.0f;
constexpr float BEDROCK_FLOOR = 1.5f; // metres above y = 0 that is always solid

// Surface layers: grass, bare rock or sand, over a soil band, over rock.
constexpr float SURFACE_DEPTH = 0.7f; // metres of the top layer
constexpr float SOIL_DEPTH = 3.5f;
constexpr float SOIL_WOBBLE = 2.0f;
constexpr float LOWLAND_FRACTION = 0.17f; // of `height`: below this there is sand, not grass
constexpr float HIGHLAND_FRACTION = 0.52f; // above this the surface is bare rock
constexpr float HIGHLAND_WOBBLE = 4.0f;
constexpr float WOBBLE_SCALE = 6.0f; // the band that makes layer borders ragged
constexpr float BEDROCK_WOBBLE = 0.6f;

// Ore, decided by hash noise inside rock only.
constexpr float GOLD_SCALE = 7.0f;
constexpr float GOLD_THRESHOLD = 0.5f;
constexpr float CRYSTAL_SCALE = 9.0f;
constexpr float CRYSTAL_THRESHOLD = 0.56f;

// Seed offsets: each rule gets its own stream so tuning one does not reshuffle the others.
constexpr uint32_t SEED_RIDGE = 77, SEED_MESA = 191, SEED_OVERHANG = 5, SEED_TUNNEL_A = 11, SEED_TUNNEL_B = 23,
				   SEED_CAVERN = 31, SEED_WOBBLE = 41, SEED_GOLD = 53, SEED_CRYSTAL = 67;

float column_height(const DemoTerrain &g, float wx, float wz) {
	const float cont = fbm(wx / CONTINENT_SCALE, 0.5f, wz / CONTINENT_SCALE, g.seed, 3);
	const float ridge = 1.0f - std::fabs(fbm(wx / RIDGE_SCALE, 7.5f, wz / RIDGE_SCALE, g.seed + SEED_RIDGE, 4)) * RIDGE_SHARPNESS;
	const float mount = std::clamp(cont * MOUNTAIN_GAIN + MOUNTAIN_BIAS, 0.0f, 1.0f);
	const float mesa = std::clamp(fbm(wx / MESA_SCALE, 3.5f, wz / MESA_SCALE, g.seed + SEED_MESA, 2) * MESA_GAIN - MESA_OFFSET, 0.0f, 1.0f);
	return g.height * (HEIGHT_FLOOR + cont * CONTINENT_LIFT + mount * std::max(ridge, 0.0f) * ridge * MOUNTAIN_FRACTION + mesa * MESA_FRACTION);
}

// Signed distance to the surface, in metres (negative inside), after the overhang term.
float add_overhangs(float sd, float wx, float wy, float wz, const DemoTerrain &g) {
	if (sd >= OVERHANG_ENTRY) return sd;
	const float ramp = std::clamp(wy / g.height * OVERHANG_RAMP_SLOPE - OVERHANG_RAMP_FROM, 0.0f, 1.0f);
	return sd + fbm(wx / OVERHANG_SCALE_XZ, wy / OVERHANG_SCALE_Y, wz / OVERHANG_SCALE_XZ, g.seed + SEED_OVERHANG, 2) * OVERHANG_AMPLITUDE * ramp;
}

// Signed distance after carving tunnels and caverns. Only called close under the surface.
float carve_caves(float sd, float wx, float wy, float wz, const DemoTerrain &g) {
	const float n1 = noise3(wx / TUNNEL_SCALE_XZ, wy / TUNNEL_SCALE_Y, wz / TUNNEL_SCALE_XZ, g.seed + SEED_TUNNEL_A);
	const float n2 = noise3(wx / TUNNEL_SCALE_XZ, wy / TUNNEL_SCALE_Y, wz / TUNNEL_SCALE_XZ, g.seed + SEED_TUNNEL_B);
	sd = std::max(sd, (TUNNEL_WIDTH - std::max(std::fabs(n1), std::fabs(n2))) * TUNNEL_GAIN);
	const float blob = noise3(wx / CAVERN_SCALE_XZ, wy / CAVERN_SCALE_Y, wz / CAVERN_SCALE_XZ, g.seed + SEED_CAVERN);
	return std::max(sd, (blob - CAVERN_THRESHOLD) * CAVERN_GAIN);
}

// The material at a point given its depth below the surface: bedrock, then sand/rock/grass, then soil.
// `deep` says the point is below the soil band, which is the only place ore may replace rock — bare rock at
// the surface is already rock and stays rock.
uint8_t surface_material(float depth, float h, float wy, float wobble, float height, bool &deep) {
	deep = false;
	if (wy < BEDROCK_FLOOR + wobble * BEDROCK_WOBBLE) return BEDROCK;
	if (depth < SURFACE_DEPTH) {
		if (h < height * LOWLAND_FRACTION) return SAND;
		return wy > height * HIGHLAND_FRACTION + wobble * HIGHLAND_WOBBLE ? ROCK : GRASS;
	}
	if (depth < SOIL_DEPTH + wobble * SOIL_WOBBLE) return h < height * LOWLAND_FRACTION ? SAND : SOIL;
	deep = true;
	return ROCK;
}

// Ore replaces rock only. `base` is what surface_material chose.
uint8_t ore_material(float wx, float wy, float wz, const DemoTerrain &g, uint8_t base) {
	if (base != ROCK) return base;
	if (noise3(wx / GOLD_SCALE, wy / GOLD_SCALE, wz / GOLD_SCALE, g.seed + SEED_GOLD) > GOLD_THRESHOLD) return GOLD;
	if (noise3(wx / CRYSTAL_SCALE, wy / CRYSTAL_SCALE, wz / CRYSTAL_SCALE, g.seed + SEED_CRYSTAL) > CRYSTAL_THRESHOLD) return CRYSTAL;
	return base;
}

} // namespace

void DemoTerrain::generate_block(const int origin[3], float cell, Voxel out[512]) const {
	// One cache per thread; the key is the settings plus the columns' origin, so any call order is correct.
	static thread_local ColumnCache<float, COLUMNS, COLUMNS> cache;
	bool fresh = false;
	float *heights = cache.acquire(settings_tag(seed, cell, height), origin[0], origin[2], fresh);
	if (fresh)
		for (int i = 0; i < COLUMNS * COLUMNS; i++)
			heights[i] = column_height(*this, (origin[0] + (i & 7)) * cell, (origin[2] + (i >> 3)) * cell);

	float top = -1e9f;
	for (int i = 0; i < COLUMNS * COLUMNS; i++) top = std::max(top, heights[i]);
	if (origin[1] * cell > top + SKY_MARGIN) {
		std::fill_n(out, 512, Voxel{});
		return;
	}

	for (int i = 0; i < 512; i++) {
		const int lx = i & 7, ly = (i >> 3) & 7, lz = i >> 6;
		const float wx = (origin[0] + lx) * cell, wy = (origin[1] + ly) * cell, wz = (origin[2] + lz) * cell;
		const float h = heights[lz * COLUMNS + lx], depth = h - wy;
		float sd = -depth;
		sd = add_overhangs(sd, wx, wy, wz, *this);
		if (sd < CAVE_ENTRY) sd = carve_caves(sd, wx, wy, wz, *this);
		sd = std::min(sd, wy - BEDROCK_FLOOR);
		const float wobble = noise3(wx / WOBBLE_SCALE, wy / WOBBLE_SCALE, wz / WOBBLE_SCALE, seed + SEED_WOBBLE);
		bool deep = false;
		uint8_t m = surface_material(depth, h, wy, wobble, height, deep);
		if (deep) m = ore_material(wx, wy, wz, *this, m);
		out[i] = {sdf_to_density(sd / cell), m};
	}
}

} // namespace dkv
