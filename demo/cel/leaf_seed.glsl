#[compute]
#version 450

// Jump flood over leaf pixels (roughness 0.25..0.75, see leaf.gdshader). Each
// pixel stores two seeds (pixel coordinates, NONE if none in reach):
//   BA: the nearest leaf pixel of any depth. Plain JFA; it carries every leaf's
//       own pixels outward so they can be found at all.
//   RG: the nearest leaf pixel that is in front of this pixel by more than
//       depth_gap metres. This is the result: a leaf grows its scallops over
//       anything behind it, sky, ground, trunk, or a farther leaf.
// step 0 seeds, step > 0 is one jump pass.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D nr_tex;
layout(rgba32f, set = 0, binding = 1) uniform restrict readonly image2D src;
layout(rgba32f, set = 0, binding = 2) uniform restrict writeonly image2D dst;
layout(set = 0, binding = 3) uniform sampler2D depth_tex;

layout(push_constant, std430) uniform Params {
	ivec2 size;
	int step;
	int pad;
	float proj_wz; // projection W.z
	float proj_zz; // projection Z.z
	float depth_gap;
	float pad2;
} p;

const vec2 NONE = vec2(-1e6);

bool is_leaf(ivec2 c) {
	// Raw buffer: Godot stores roughness * 127/255, mirrored (1 - w) for dynamic
	// instances. Spatial shaders get this decoded for them; compute doesn't.
	float w = texelFetch(nr_tex, c, 0).w;
	float r = (w > 0.5 ? 1.0 - w : w) / (127.0 / 255.0);
	return abs(r - 0.5) < 0.26;
}

// View distance in metres from the reverse-Z depth buffer (0 = far plane, sky).
float view_z(ivec2 c) {
	return p.proj_wz / (texelFetch(depth_tex, c, 0).r + p.proj_zz);
}

void main() {
	ivec2 c = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(c, p.size))) {
		return;
	}
	if (p.step == 0) {
		imageStore(dst, c, vec4(NONE, is_leaf(c) ? vec2(c) : NONE));
		return;
	}
	float z = view_z(c);
	vec2 near_seed = NONE; // RG
	float near_d = 1e9;
	vec2 any_seed = NONE; // BA
	float any_d = 1e9;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 n = c + ivec2(x, y) * p.step;
			if (any(lessThan(n, ivec2(0))) || any(greaterThanEqual(n, p.size))) {
				continue;
			}
			vec4 v = imageLoad(src, n);
			for (int k = 0; k < 2; k++) {
				vec2 s = k == 0 ? v.xy : v.zw;
				if (s.x < 0.0) {
					continue;
				}
				float d = distance(s, vec2(c));
				if (k == 1 && d < any_d) {
					any_d = d;
					any_seed = s;
				}
				if (d < near_d && view_z(ivec2(s)) < z - p.depth_gap) {
					near_d = d;
					near_seed = s;
				}
			}
		}
	}
	imageStore(dst, c, vec4(near_seed, any_seed));
}
