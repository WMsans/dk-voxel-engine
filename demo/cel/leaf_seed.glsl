#[compute]
#version 450

// Jump flood over leaf pixels (roughness 0.25..0.75, see voxel_terrain_cel.gdshader), on a half-resolution
// grid: cell h stands for full-resolution pixels 2h .. 2h + 1. The flood is bound by memory traffic, so a
// quarter of the pixels is most of the saving; seeds stay exact full-resolution leaf pixels, so
// cel_outline.glsl still measures each pixel's distance to its seed exactly.
// Each cell stores two seeds as offsets from its pixel 2h, 6 bits per axis ([-32, 31]: the flood's reach is
// 32 px), packed in one uint (4 bytes a cell):
//   bits 12..23: the nearest leaf pixel of any depth. Plain JFA; it carries every leaf's own pixels outward
//                so they can be found at all.
//   bits 0..11:  the nearest leaf pixel that is in front of the cell by more than depth_gap metres. This is
//                the result: a leaf grows its scallops over anything behind it, sky, ground, trunk, or a
//                farther leaf.
// step 0 runs at full resolution: it snapshots the lit image for cel_outline.glsl (which writes the colour
// buffer it reads) and seeds each cell from its 2x2 pixels. step > 0 is one jump pass over the cells.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D nr_tex;
layout(r32ui, set = 0, binding = 1) uniform restrict readonly uimage2D src;
layout(r32ui, set = 0, binding = 2) uniform restrict writeonly uimage2D dst;
layout(set = 0, binding = 3) uniform sampler2D depth_tex;
layout(rgba16f, set = 0, binding = 4) uniform restrict readonly image2D color_in;
layout(rgba16f, set = 0, binding = 5) uniform restrict writeonly image2D color_copy;

layout(push_constant, std430) uniform Params {
	ivec2 size; // full resolution
	int step; // in cells
	int pad;
	float proj_wz; // projection W.z
	float proj_zz; // projection Z.z
	float depth_gap;
	float pad2;
} p;

const uint NONE = 0xfffu; // a seed slot with no seed; cel_outline.glsl decodes the same way

uint pack_offset(ivec2 o) {
	return uint(o.x + 32) | (uint(o.y + 32) << 6);
}

ivec2 unpack_offset(uint v) {
	return ivec2(int(v & 63u) - 32, int((v >> 6) & 63u) - 32);
}

ivec2 clamp_px(ivec2 c) {
	return clamp(c, ivec2(0), p.size - 1);
}

bool is_leaf(ivec2 c) {
	// Raw buffer: Godot stores roughness * 127/255, mirrored (1 - w) for dynamic
	// instances. Spatial shaders get this decoded for them; compute doesn't.
	float w = texelFetch(nr_tex, clamp_px(c), 0).w;
	float r = (w > 0.5 ? 1.0 - w : w) / (127.0 / 255.0);
	return abs(r - 0.5) < 0.26;
}

// View distance in metres from the reverse-Z depth buffer (0 = far plane, sky).
float view_z(ivec2 c) {
	return p.proj_wz / (texelFetch(depth_tex, clamp_px(c), 0).r + p.proj_zz);
}

void main() {
	ivec2 cells = (p.size + 1) / 2;
	if (p.step == 0) {
		ivec2 c = ivec2(gl_GlobalInvocationID.xy);
		if (any(greaterThanEqual(c, p.size))) {
			return;
		}
		imageStore(color_copy, c, imageLoad(color_in, c));
		if (any(notEqual(c & 1, ivec2(0)))) {
			return; // one thread per cell seeds it
		}
		uint any_seed = NONE;
		for (int i = 0; i < 4 && any_seed == NONE; i++) {
			ivec2 o = ivec2(i & 1, i >> 1);
			if (is_leaf(c + o)) {
				any_seed = pack_offset(o);
			}
		}
		imageStore(dst, c / 2, uvec4(NONE | (any_seed << 12)));
		return;
	}
	ivec2 h = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(h, cells))) {
		return;
	}
	// The cell's farthest pixel: a seed in front of it by the gap is in front of every pixel the cell covers,
	// or close to it; cel_outline.glsl still checks each pixel against its seed's depth.
	ivec2 base = 2 * h;
	float z = max(max(view_z(base), view_z(base + ivec2(1, 0))), max(view_z(base + ivec2(0, 1)), view_z(base + ivec2(1))));
	vec2 centre = vec2(0.5); // the cell's centre, from pixel 2h
	uint near_seed = NONE;
	float near_d = 1e9;
	uint any_seed = NONE;
	float any_d = 1e9;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 n = h + ivec2(x, y) * p.step;
			if (any(lessThan(n, ivec2(0))) || any(greaterThanEqual(n, cells))) {
				continue;
			}
			uint v = imageLoad(src, n).r;
			for (int k = 0; k < 2; k++) {
				uint sv = (v >> (12 * k)) & NONE;
				if (sv == NONE) {
					continue;
				}
				ivec2 o = 2 * (n - h) + unpack_offset(sv); // the seed, relative to pixel 2h
				if (any(lessThan(o, ivec2(-32))) || any(greaterThan(o, ivec2(31)))) {
					continue; // out of reach
				}
				float d = distance(vec2(o), centre);
				uint so = pack_offset(o);
				if (k == 1 && d < any_d) {
					any_d = d;
					any_seed = so;
				}
				if (d < near_d && view_z(base + o) < z - p.depth_gap) {
					near_d = d;
					near_seed = so;
				}
			}
		}
	}
	imageStore(dst, h, uvec4(near_seed | (any_seed << 12)));
}
