#[compute]
#version 450

// Cel bands, outlines and scalloped leaf edges in one full-screen pass (CelEffect). Reads `src`, a copy of the
// lit image made just before, and writes the result back into the colour buffer.
//
// Cel: the lit image is banded by luminance through `ramp`; hue is kept. Needs view-independent light (Lambert,
// no specular or sky reflections, flat ambient, hard shadows) or the bands flicker and speckle.
// Outline: drawn where the view-space depth's second derivative across the pixel exceeds depth_threshold
// metres, or, with no depth step, on the nearer of two surfaces whose hues differ by more than color_threshold
// (leaves over trunk).
// Leaves: `seeds` (leaf_seed.glsl) holds, per 2x2 cell, the nearest leaf pixel in front of it. A pixel under that
// edge point's bump "becomes" the leaf (its colour and depth), so leaf clumps grow a row of semicircles of
// radius leaf_bump_px along their edges, and the outline follows them.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D src;
layout(rgba16f, set = 0, binding = 1) uniform restrict writeonly image2D dst;
layout(set = 0, binding = 2) uniform sampler2D depth_tex;
layout(set = 0, binding = 3) uniform sampler2D nr_tex;
layout(set = 0, binding = 4) uniform usampler2D seeds;
layout(set = 0, binding = 5) uniform sampler2D ramp;

layout(push_constant, std430) uniform Params {
	vec4 outline_color;
	ivec2 size;
	float thickness; // line width in pixels
	float depth_threshold; // metres
	float color_threshold; // chromaticity distance
	float leaf_bump_px;
	float proj_wz; // projection W.z
	float proj_zz; // projection Z.z
} p;

// Colour and depth come from this far inside the leaf: right at the silhouette the surface is grazing and its
// depth and cel band flicker.
const float SAMPLE_INSET_PX = 3.0;

ivec2 clamp_px(ivec2 c) {
	return clamp(c, ivec2(0), p.size - 1);
}

// Raw buffer: Godot stores roughness * 127/255, mirrored (1 - w) for dynamic instances.
float roughness(ivec2 c) {
	float w = texelFetch(nr_tex, clamp_px(c), 0).w;
	return (w > 0.5 ? 1.0 - w : w) / (127.0 / 255.0);
}

// voxel_terrain_cel.gdshader marks leaves with roughness 0.25 + 0.5 * bump profile; everything else is 1.0.
bool is_leaf(ivec2 c) {
	return abs(roughness(c) - 0.5) < 0.26;
}

float depth(ivec2 c) {
	return texelFetch(depth_tex, clamp_px(c), 0).r;
}

// View distance in metres from the reverse-Z depth buffer; 0 is the far plane (sky).
float view_depth(ivec2 c) {
	float d = depth(c);
	return d <= 0.0 ? 10000.0 : p.proj_wz / (d + p.proj_zz);
}

vec3 color(ivec2 c) {
	return texelFetch(src, clamp_px(c), 0).rgb;
}

// Colour with brightness divided out: lighting and cel bands leave it alone, green leaves and brown bark don't.
vec3 chroma(ivec2 c) {
	vec3 col = color(c);
	return col / max(col.r + col.g + col.b, 1e-4);
}

// The pixel whose colour and depth `c` should show: itself, or the nearest leaf pixel in front of it when `c`
// lies under that edge point's bump. Leaf pixels too: a farther leaf gets covered by a nearer leaf's scallops.
ivec2 leaf_source(ivec2 c) {
	// `seeds` is half resolution: cell h covers pixels 2h .. 2h + 1. Bits 0..11: the seed's offset from pixel 2h,
	// 6 bits an axis (leaf_seed.glsl); 0xfff = none.
	ivec2 h = clamp_px(c) / 2;
	uint sv = texelFetch(seeds, h, 0).r & 0xfffu;
	if (sv == 0xfffu) {
		return c;
	}
	ivec2 s = 2 * h + ivec2(int(sv & 63u) - 32, int((sv >> 6) & 63u) - 32);
	vec2 o = vec2(s - c);
	float d = length(o);
	if (!(d < p.leaf_bump_px + 1.0) || !is_leaf(s)) {
		return c;
	}
	float profile = clamp((roughness(s) - 0.25) / 0.5, 0.0, 1.0);
	if (d >= p.leaf_bump_px * profile) {
		return c;
	}
	ivec2 q = ivec2(floor(vec2(s) + 0.5 + o / max(d, 1e-4) * SAMPLE_INSET_PX));
	if (!is_leaf(q)) {
		q = s;
	}
	// Reverse-Z: bigger is nearer. Only grow over farther surfaces.
	return depth(q) > depth(c) ? q : c;
}

// A neighbour `off` pixels away, rounded the way a nearest-filtered uv offset is.
ivec2 tap(ivec2 c, vec2 off) {
	return ivec2(floor(vec2(c) + 0.5 + off));
}

// Hue step towards a neighbour, counted only when this pixel is in front of it, so the line lands on the
// occluder (the leaf) and not on the trunk behind.
float hue_step(vec3 cc, float z, ivec2 n, float zn) {
	return zn > z ? distance(cc, chroma(n)) : 0.0;
}

vec3 srgb_to_linear(vec3 c) {
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}

void main() {
	ivec2 c = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(c, p.size))) {
		return;
	}
	ivec2 m = leaf_source(c);

	// Cel. The image is linear; the ramp is looked up with gamma luminance so its stops read like picker values.
	vec3 col = color(m);
	if (depth(m) > 0.0) { // sky stays smooth
		float lum = max(dot(col, vec3(0.2126, 0.7152, 0.0722)), 1e-4);
		float x = clamp(pow(lum, 1.0 / 2.2), 0.0, 1.0);
		int w = textureSize(ramp, 0).x;
		col = col / lum * srgb_to_linear(texelFetch(ramp, ivec2(min(int(x * float(w)), w - 1), 0), 0).rgb);
	}

	// Outline. Every tap goes through leaf_source, so lines follow the grown leaf edges.
	ivec2 tl = tap(c, vec2(-p.thickness, 0.0)), tr = tap(c, vec2(p.thickness, 0.0));
	ivec2 tu = tap(c, vec2(0.0, -p.thickness)), td = tap(c, vec2(0.0, p.thickness));
	ivec2 l = leaf_source(tl), r = leaf_source(tr), u = leaf_source(tu), dn = leaf_source(td);
	float z = view_depth(m);
	float zl = view_depth(l), zr = view_depth(r), zu = view_depth(u), zd = view_depth(dn);
	// Second derivative, not a plain difference: a surface receding from the camera has a large depth difference
	// across it but no line. Across a step between two objects it equals the step in metres.
	float lap = abs(zl + zr - 2.0 * z) + abs(zu + zd - 2.0 * z);
	float edge = smoothstep(p.depth_threshold, p.depth_threshold * 2.0, lap);
	vec3 cc = chroma(m);
	float hue = max(max(hue_step(cc, z, l, zl), hue_step(cc, z, r, zr)), max(hue_step(cc, z, u, zu), hue_step(cc, z, dn, zd)));
	edge = max(edge, smoothstep(p.color_threshold, p.color_threshold * 2.0, hue));
	// Grown leaf pixels borrow depth from slightly different leaf points, which the second derivative amplifies
	// into noise inside the grown band. Where any tap is grown, use a plain step on both sides instead, so the
	// line follows the scallops and not the mesh edge underneath.
	if (m != c || l != tl || r != tr || u != tu || dn != td) {
		float gap = max(max(abs(zl - z), abs(zr - z)), max(abs(zu - z), abs(zd - z)));
		edge = smoothstep(p.depth_threshold, p.depth_threshold * 2.0, gap);
	}

	imageStore(dst, c, vec4(mix(col, p.outline_color.rgb, edge * p.outline_color.a), 1.0));
}
