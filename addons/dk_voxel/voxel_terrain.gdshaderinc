// The body of voxel_terrain.gdshader, for shader variants. A variant declares shader_type and its own
// render_mode, may #define VOXEL_FRAGMENT_HOOK as statements to run at the end of fragment() (they see
// ia, ib = the two material ids, t = the blend between them, vpos = world position), then includes this.

// Must match dk_voxel::MATERIAL_SLOTS; tests/run_tests.gd fails if the two disagree.
const int MATERIAL_SLOTS = 32;

uniform sampler2DArray albedo_tex : source_color, filter_linear_mipmap, repeat_enable;
// No source_color here: a normal map is linear data, and an sRGB decode would bend it.
uniform sampler2DArray normal_tex : filter_linear_mipmap, repeat_enable;
// Data maps, so no source_color either: 1.0 leaves the scalar/vertex value untouched.
uniform sampler2DArray roughness_tex : filter_linear_mipmap, repeat_enable;
uniform sampler2DArray ao_tex : filter_linear_mipmap, repeat_enable;
uniform vec3 color_a[32] : source_color;
uniform vec3 color_b[32] : source_color;
uniform vec4 params[32]; // x = tiles per metre, y = roughness, z = metallic, w = emission
uniform float normal_strength = 1.0;
uniform float rim = 0.35;

varying vec3 vpos;
varying vec3 vnrm;

void vertex() {
	// World space: VERTEX is chunk-relative, and a chunk-local texture phase seams at every border.
	vpos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	// The projection must be chosen from a world-space normal too. fragment()'s NORMAL is in view space:
	// choosing from it picks the side projections for a floor seen from above, which smears the texture.
	vnrm = (MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz;
}

// Sharpened triplanar weights, so a face mostly uses one projection. A projection under TRI_SKIP is dropped
// and the rest renormalised: most pixels then fetch one projection instead of three.
const float TRI_SKIP = 0.02;

vec3 tri_weights(vec3 n) {
	vec3 w = pow(abs(n), vec3(4.0));
	w /= max(w.x + w.y + w.z, 1e-4);
	w *= step(TRI_SKIP, w);
	return w / max(w.x + w.y + w.z, 1e-4);
}

// The fetches below sit in branches that can diverge inside a pixel quad, so they take explicit gradients:
// dpx, dpy = the screen derivatives of the world position, taken once in uniform control flow.
vec4 tri_sample(sampler2DArray tex, int id, vec3 p, vec3 dpx, vec3 dpy, vec3 w, float scale) {
	float layer = float(id);
	vec4 c = vec4(0.0);
	if (w.x > 0.0) c += textureGrad(tex, vec3(p.zy * scale, layer), dpx.zy * scale, dpy.zy * scale) * w.x;
	if (w.y > 0.0) c += textureGrad(tex, vec3(p.xz * scale, layer), dpx.xz * scale, dpy.xz * scale) * w.y;
	if (w.z > 0.0) c += textureGrad(tex, vec3(p.xy * scale, layer), dpx.xy * scale, dpy.xy * scale) * w.z;
	return c;
}

// Decodes each projection's tangent-space normal and swizzles it back into world space.
vec3 tri_normal(int id, vec3 p, vec3 dpx, vec3 dpy, vec3 n, vec3 w, float scale) {
	float layer = float(id);
	vec3 r = vec3(0.0);
	if (w.x > 0.0) {
		vec3 t = textureGrad(normal_tex, vec3(p.zy * scale, layer), dpx.zy * scale, dpy.zy * scale).xyz * 2.0 - 1.0;
		r += vec3(t.z * sign(n.x), t.y, t.x) * w.x;
	}
	if (w.y > 0.0) {
		vec3 t = textureGrad(normal_tex, vec3(p.xz * scale, layer), dpx.xz * scale, dpy.xz * scale).xyz * 2.0 - 1.0;
		r += vec3(t.x, t.z * sign(n.y), t.y) * w.y;
	}
	if (w.z > 0.0) {
		vec3 t = textureGrad(normal_tex, vec3(p.xy * scale, layer), dpx.xy * scale, dpy.xy * scale).xyz * 2.0 - 1.0;
		r += vec3(t.x, t.y, t.z * sign(n.z)) * w.z;
	}
	return normalize(r);
}

void fragment() {
	vec3 n = normalize(vnrm);
	vec3 w = tri_weights(n);
	vec3 dpx = dFdx(vpos), dpy = dFdy(vpos);
	int ia = clamp(int(COLOR.r * 255.0 + 0.5), 0, MATERIAL_SLOTS - 1);
	int ib = clamp(int(COLOR.g * 255.0 + 0.5), 0, MATERIAL_SLOTS - 1);
	vec4 pa = params[ia], pb = params[ib];
	float t = ia == ib ? 0.0 : smoothstep(0.35, 0.65, COLOR.b);
	// A material is only sampled where it shows: across most of a two-material quad the blend is all one side.
	bool use_a = t < 1.0, use_b = t > 0.0;

	vec4 a = use_a ? tri_sample(albedo_tex, ia, vpos, dpx, dpy, w, pa.x) : vec4(0.0);
	vec4 b = use_b ? tri_sample(albedo_tex, ib, vpos, dpx, dpy, w, pb.x) : a;
	a = use_a ? a : b;
	// The texture's own luminance drives the two-colour ramp, so a colour-only material still looks mottled.
	vec3 tint_a = mix(color_a[ia], color_b[ia], clamp(dot(a.rgb, vec3(0.3333)), 0.0, 1.0));
	vec3 tint_b = mix(color_a[ib], color_b[ib], clamp(dot(b.rgb, vec3(0.3333)), 0.0, 1.0));
	vec3 albedo = mix(a.rgb * tint_a, b.rgb * tint_b, t);

	vec3 na = use_a ? tri_normal(ia, vpos, dpx, dpy, n, w, pa.x) : n;
	vec3 nb = use_b ? tri_normal(ib, vpos, dpx, dpy, n, w, pb.x) : na;
	na = use_a ? na : nb;
	NORMAL = normalize((VIEW_MATRIX * vec4(mix(n, normalize(mix(na, nb, t)), normal_strength), 0.0)).xyz); // back to view space

	float ra = use_a ? tri_sample(roughness_tex, ia, vpos, dpx, dpy, w, pa.x).r : 1.0;
	float rb = use_b ? tri_sample(roughness_tex, ib, vpos, dpx, dpy, w, pb.x).r : ra;
	ra = use_a ? ra : rb;
	float oa = use_a ? tri_sample(ao_tex, ia, vpos, dpx, dpy, w, pa.x).r : 1.0;
	float ob = use_b ? tri_sample(ao_tex, ib, vpos, dpx, dpy, w, pb.x).r : oa;
	oa = use_a ? oa : ob;

	float ao = COLOR.a * mix(oa, ob, t);
	ALBEDO = albedo * mix(0.7, 1.0, ao);
	ROUGHNESS = clamp(mix(pa.y * ra, pb.y * rb, t), 0.0, 1.0);
	METALLIC = mix(pa.z, pb.z, t);
	EMISSION = albedo * mix(pa.w, pb.w, t);
	AO = ao;
	AO_LIGHT_AFFECT = 0.5;
	RIM = rim;
	RIM_TINT = 0.6;
#ifdef VOXEL_FRAGMENT_HOOK
	VOXEL_FRAGMENT_HOOK
#endif
}
