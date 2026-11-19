// Screen-space scalloped leaf edges, shared by cel_shading and outline so both see
// the same grown shape. A non-leaf pixel near a leaf edge "becomes" that leaf (its
// colour and depth) when it falls inside a bump growing out of the edge.
//
// leaf.gdshader writes ROUGHNESS = 0.25 + 0.5 * profile: 0.25..0.75 marks a leaf
// pixel, profile (0..1) is the bump height there, a row of semicircles of radius
// leaf_bump_px along each clump's edge. Other surfaces are 1.0, sky 0.
// LeafSeedEffect (jump flood) provides leaf_seed: for every pixel, the nearest
// leaf pixel in front of it by more than its depth_gap, so the edge distance is
// exact and smooth, and a leaf scallops over farther leaves as well as the sky.

// Bump radius in screen pixels (project setting shader_globals/leaf_bump_px).
// Must stay below LeafSeedEffect's 31 px reach.
global uniform float leaf_bump_px;
global uniform sampler2D leaf_seed;

// Colour and depth come from this far inside the leaf: right at the silhouette
// the surface is grazing and its depth and cel band flicker.
const float SAMPLE_INSET_PX = 3.0;

float leaf_w(sampler2D nr_tex, vec2 uv) {
	// Roughness; Godot decodes the buffer's packing for spatial shaders.
	return texture(nr_tex, uv).w;
}

bool is_leaf(sampler2D nr_tex, vec2 uv) {
	return abs(leaf_w(nr_tex, uv) - 0.5) < 0.26;
}

// The uv whose colour and depth this pixel should show: itself, or the nearest
// leaf pixel in front of it when this pixel lies under that edge point's bump.
// Leaf pixels too: a farther leaf gets covered by a nearer leaf's scallops.
vec2 leaf_source(vec2 uv, sampler2D depth_tex, sampler2D nr_tex) {
	vec2 size = vec2(textureSize(depth_tex, 0));
	ivec2 c = ivec2(uv * size);
	// texelFetch: globals can't take a filter hint, and coordinates mustn't blend.
	vec2 s = texelFetch(leaf_seed, c, 0).xy;
	vec2 o = s - vec2(c);
	float d = length(o);
	if (!(d < leaf_bump_px + 1.0)) {
		return uv;
	}
	vec2 e = (s + 0.5) / size;
	// No leaf in reach, or leaf_seed unbound (no LeafSeedEffect: default texture).
	if (!is_leaf(nr_tex, e)) {
		return uv;
	}
	float profile = clamp((leaf_w(nr_tex, e) - 0.25) / 0.5, 0.0, 1.0);
	if (d >= leaf_bump_px * profile) {
		return uv;
	}
	vec2 q = e + o / max(d, 1e-4) * SAMPLE_INSET_PX / size;
	if (!is_leaf(nr_tex, q)) {
		q = e;
	}
	// Reverse-Z: bigger is nearer. Only grow over farther surfaces.
	return texture(depth_tex, q).r > texture(depth_tex, uv).r ? q : uv;
}
