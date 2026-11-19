shader_type spatial;
render_mode unshaded, cull_disabled, depth_test_disabled, depth_draw_never, fog_disabled;

// Full-screen post-process. Drawn as the last transparent object, so it composites
// over the finished image. Needs a QuadMesh of size 2 in the node carrying it.
uniform vec4 outline_color : source_color = vec4(0.05, 0.05, 0.08, 1.0);
// Line width in screen pixels.
uniform float thickness = 1.5;
// Depth gap between two surfaces, in metres, below which no line is drawn.
uniform float depth_threshold = 0.5;
// Hue difference (chromaticity distance) that draws a line on the nearer surface
// even with no depth gap, e.g. leaves pressed against the trunk.
uniform float color_threshold = 0.08;

uniform sampler2D depth_texture : hint_depth_texture, filter_nearest;
// Opaque-only copy, before cel shading. Cel shading keeps hue, so this is fine.
uniform sampler2D screen_texture : hint_screen_texture, filter_nearest;
uniform sampler2D normal_roughness_texture : hint_normal_roughness_texture, filter_nearest;

#include "res://demo/cel/leaf_scallop.gdshaderinc"

// View-space distance of the opaque surface at uv. Godot 4 uses a reverse-Z buffer,
// so depth 0 is the far plane: sky, or nothing drawn there. inv_proj has to be
// handed in because stage built-ins are only visible inside the entry points.
float view_depth(vec2 uv, mat4 inv_proj) {
	// texture() on the depth sampler is available in helper functions.
	float d = texture(depth_texture, uv).r;
	if (d <= 0.0) {
		return 10000.0;
	}
	vec4 view = inv_proj * vec4(uv * 2.0 - 1.0, d, 1.0);
	return -view.z / view.w;
}

// Colour with brightness divided out: lighting and cel bands leave it alone,
// green leaves and brown bark don't.
vec3 chroma(vec2 uv) {
	vec3 c = texture(screen_texture, uv).rgb;
	return c / max(c.r + c.g + c.b, 1e-4);
}

// Hue step towards a neighbour, counted only when this pixel is in front of it,
// so the line lands on the occluder (the leaf) and not on the trunk behind.
float hue_step(vec3 cc, float c, vec2 uv, float n) {
	return n > c ? distance(cc, chroma(uv)) : 0.0;
}

void vertex() {
	POSITION = vec4(VERTEX.xy, 1.0, 1.0);
}

void fragment() {
	vec2 o = thickness / vec2(textureSize(depth_texture, 0));
	// Every tap goes through leaf_source, so lines follow the grown leaf edges.
	vec2 tl = SCREEN_UV - vec2(o.x, 0.0);
	vec2 tr = SCREEN_UV + vec2(o.x, 0.0);
	vec2 tu = SCREEN_UV - vec2(0.0, o.y);
	vec2 td = SCREEN_UV + vec2(0.0, o.y);
	vec2 m = leaf_source(SCREEN_UV, depth_texture, normal_roughness_texture);
	vec2 l = leaf_source(tl, depth_texture, normal_roughness_texture);
	vec2 r = leaf_source(tr, depth_texture, normal_roughness_texture);
	vec2 u = leaf_source(tu, depth_texture, normal_roughness_texture);
	vec2 dn = leaf_source(td, depth_texture, normal_roughness_texture);
	float c = view_depth(m, INV_PROJECTION_MATRIX);
	float zl = view_depth(l, INV_PROJECTION_MATRIX);
	float zr = view_depth(r, INV_PROJECTION_MATRIX);
	float zu = view_depth(u, INV_PROJECTION_MATRIX);
	float zd = view_depth(dn, INV_PROJECTION_MATRIX);
	// Second derivative, not a plain difference: a surface receding from the camera
	// has a large depth difference across it but no line, and this cancels that out.
	// Across a step between two objects it equals the step in metres, which is
	// exactly what the threshold is about.
	float lap = abs(zl + zr - 2.0 * c) + abs(zu + zd - 2.0 * c);
	float edge = smoothstep(depth_threshold, depth_threshold * 2.0, lap);

	vec3 cc = chroma(m);
	float hue = max(max(hue_step(cc, c, l, zl), hue_step(cc, c, r, zr)),
			max(hue_step(cc, c, u, zu), hue_step(cc, c, dn, zd)));
	edge = max(edge, smoothstep(color_threshold, color_threshold * 2.0, hue));

	// Grown leaf pixels borrow depth from slightly different leaf points, which the
	// second derivative above amplifies into noise inside the grown band. Where any
	// tap is grown, use a plain step instead: line on the nearer side wherever a
	// tap is farther by more than the threshold (sky, ground, a farther leaf), so
	// the line follows the scallops and not the mesh edge underneath.
	if (m != SCREEN_UV || l != tl || r != tr || u != tu || dn != td) {
		float gap = max(max(zl, zr), max(zu, zd)) - c;
		edge = smoothstep(depth_threshold, depth_threshold * 2.0, gap);
	}

	ALBEDO = outline_color.rgb;
	ALPHA = edge * outline_color.a;
}
