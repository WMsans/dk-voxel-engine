// Terrain material for VoxelWorld meshes.
// COLOR.r / .g = the two material ids of the triangle (constant across it), .b = blend, .a = baked AO.
// Albedo and normal come from one Texture2DArray each, layer = material id, triplanar-projected in world
// space, so nothing stretches on cliffs or cave ceilings, no UVs are needed, and the projection runs on
// across chunk borders (mesh vertices are chunk-relative, so VERTEX alone would restart the texture at
// every chunk).
// The two colours are a tint ramp over the albedo: set both to white to show a texture unmodified.
shader_type spatial;

// Must match dk_voxel::MATERIAL_SLOTS; tests/run_tests.gd fails if the two disagree.
const int MATERIAL_SLOTS = 32;

uniform sampler2DArray albedo_tex : source_color, filter_linear_mipmap, repeat_enable;
// No source_color here: a normal map is linear data, and an sRGB decode would bend it.
uniform sampler2DArray normal_tex : filter_linear_mipmap, repeat_enable;
uniform vec3 color_a[32] : source_color;
uniform vec3 color_b[32] : source_color;
uniform vec4 params[32]; // x = tiles per metre, y = roughness, z = metallic, w = emission
uniform float normal_strength = 1.0;
uniform float rim = 0.35;

varying vec3 vpos;

void vertex() {
	// World space: VERTEX is chunk-relative, and a chunk-local texture phase seams at every border.
	vpos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

// Sharpened triplanar weights, so a face mostly uses one projection.
vec3 tri_weights(vec3 n) {
	vec3 w = pow(abs(n), vec3(4.0));
	return w / max(w.x + w.y + w.z, 1e-4);
}

vec4 tri_albedo(int id, vec3 p, vec3 w, float scale) {
	float layer = float(id);
	return texture(albedo_tex, vec3(p.zy * scale, layer)) * w.x
			+ texture(albedo_tex, vec3(p.xz * scale, layer)) * w.y
			+ texture(albedo_tex, vec3(p.xy * scale, layer)) * w.z;
}

// Decodes the three projections and swizzles each tangent-space normal back into world space.
vec3 tri_normal(int id, vec3 p, vec3 n, vec3 w, float scale) {
	float layer = float(id);
	vec3 tx = texture(normal_tex, vec3(p.zy * scale, layer)).xyz * 2.0 - 1.0;
	vec3 ty = texture(normal_tex, vec3(p.xz * scale, layer)).xyz * 2.0 - 1.0;
	vec3 tz = texture(normal_tex, vec3(p.xy * scale, layer)).xyz * 2.0 - 1.0;
	vec3 wx = vec3(tx.z * sign(n.x), tx.y, tx.x);
	vec3 wy = vec3(ty.x, ty.z * sign(n.y), ty.y);
	vec3 wz = vec3(tz.x, tz.y, tz.z * sign(n.z));
	return normalize(wx * w.x + wy * w.y + wz * w.z);
}

void fragment() {
	vec3 n = normalize(NORMAL);
	vec3 w = tri_weights(n);
	int ia = clamp(int(COLOR.r * 255.0 + 0.5), 0, MATERIAL_SLOTS - 1);
	int ib = clamp(int(COLOR.g * 255.0 + 0.5), 0, MATERIAL_SLOTS - 1);
	vec4 pa = params[ia], pb = params[ib];

	vec4 a = tri_albedo(ia, vpos, w, pa.x);
	vec4 b = ia == ib ? a : tri_albedo(ib, vpos, w, pb.x);
	// The texture's own luminance drives the two-colour ramp, so a colour-only material still looks mottled.
	vec3 tint_a = mix(color_a[ia], color_b[ia], clamp(dot(a.rgb, vec3(0.3333)), 0.0, 1.0));
	vec3 tint_b = mix(color_a[ib], color_b[ib], clamp(dot(b.rgb, vec3(0.3333)), 0.0, 1.0));
	float t = ia == ib ? 0.0 : smoothstep(0.35, 0.65, COLOR.b);
	vec3 albedo = mix(a.rgb * tint_a, b.rgb * tint_b, t);

	vec3 na = tri_normal(ia, vpos, n, w, pa.x);
	vec3 nb = ia == ib ? na : tri_normal(ib, vpos, n, w, pb.x);
	NORMAL = normalize(mix(n, normalize(mix(na, nb, t)), normal_strength));

	float ao = COLOR.a;
	ALBEDO = albedo * mix(0.7, 1.0, ao);
	ROUGHNESS = mix(pa.y, pb.y, t);
	METALLIC = mix(pa.z, pb.z, t);
	EMISSION = albedo * mix(pa.w, pb.w, t);
	AO = ao;
	AO_LIGHT_AFFECT = 0.5;
	RIM = rim;
	RIM_TINT = 0.6;
}
