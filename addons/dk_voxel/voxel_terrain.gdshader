// Terrain material for VoxelWorld meshes.
// COLOR.r / .g = the two material ids of the triangle (constant across it), .b = blend, .a = baked AO.
// Patterns are sampled from one seamless 3D noise texture in volume space, so nothing stretches on cliffs
// or cave ceilings and no UVs are needed.
shader_type spatial;

uniform sampler3D noise_tex : repeat_enable, filter_linear_mipmap;
uniform vec3 color_a[16] : source_color;
uniform vec3 color_b[16] : source_color;
uniform vec4 params[16]; // x = pattern scale (1/m), y = roughness, z = metallic, w = emission
uniform float bump = 0.12;
uniform float rim = 0.35;

varying vec3 vpos;

void vertex() {
	vpos = VERTEX;
}

// rgb = albedo, a = height (used to blend materials and to bump the normal)
vec4 surface(int id, vec3 p) {
	float s = params[id].x;
	float broad = texture(noise_tex, p * s * 0.23 + vec3(0.11, 0.37, 0.59)).r;
	float mid = texture(noise_tex, p * s).r;
	float fine = texture(noise_tex, p * s * 3.1 + vec3(0.5, 0.2, 0.8)).r;
	float h = mid * 0.6 + fine * 0.4;
	// Chunky, painted-looking bands rather than a smooth gradient.
	float t = smoothstep(0.42, 0.58, h) * 0.7 + smoothstep(0.35, 0.75, broad) * 0.3;
	return vec4(mix(color_a[id], color_b[id], t) * (0.8 + 0.4 * broad), h);
}

void fragment() {
	int ia = int(COLOR.r * 255.0 + 0.5) & 15;
	int ib = int(COLOR.g * 255.0 + 0.5) & 15;
	vec4 a = surface(ia, vpos);
	vec4 b = ia == ib ? a : surface(ib, vpos);
	// Height blend: the taller material wins near the transition, giving a ragged, natural border.
	float t = ia == ib ? 0.0 : smoothstep(-0.12, 0.12, COLOR.b - 0.5 + (b.a - a.a) * 0.35);
	vec4 pa = params[ia], pb = params[ib];
	vec3 albedo = mix(a.rgb, b.rgb, t);
	float h = mix(a.a, b.a, t);
	float ao = COLOR.a;

	// Bump the normal from screen-space height derivatives (Mikkelsen), no tangents required.
	vec3 n = normalize(NORMAL);
	vec3 ds = dFdx(VERTEX), dt = dFdy(VERTEX);
	vec3 r1 = cross(dt, n), r2 = cross(n, ds);
	float det = dot(ds, r1);
	vec3 g = sign(det) * (dFdx(h) * r1 + dFdy(h) * r2);
	NORMAL = normalize(abs(det) * n - bump * g);

	ALBEDO = albedo * mix(0.7, 1.0, ao);
	ROUGHNESS = mix(pa.y, pb.y, t);
	METALLIC = mix(pa.z, pb.z, t);
	EMISSION = albedo * mix(pa.w, pb.w, t) * (0.4 + h);
	AO = ao;
	AO_LIGHT_AFFECT = 0.5;
	RIM = rim;
	RIM_TINT = 0.6;
}
