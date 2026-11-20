// voxel_terrain.gdshader for the cel look (cel_shading.gdshader, outline.gdshader, LeafSeedEffect).
// Lambert and no specular, so the cel ramp sees view-independent light. Every surface writes roughness 1.0
// except the leaves, which write leaf.gdshader's marker 0.25 + 0.5 * bump profile for the scallop pass.
shader_type spatial;
render_mode diffuse_lambert, specular_disabled;

// The material id whose voxels are leaves; demo/world.gd sets it from its LEAVES constant.
uniform int leaf_material = -1;
// Bump radius in screen pixels, shared with leaf_scallop.gdshaderinc.
global uniform float leaf_bump_px;

// leaf.gdshader lays its bumps around each clump's on-screen centre; a voxel canopy has no clumps, so
// bumps sit on a world-space lattice instead: 1 on a lattice point, falling as a semicircle to 0. The
// lattice of 2s is a subset of the lattice of s, so blending power-of-two levels never slides a bump.
float lattice_bumps(vec3 p, float s) {
	// Turned off the axes so the bumps don't line up along the voxel grid.
	const mat3 R = mat3(vec3(0.788, 0.547, -0.282), vec3(-0.488, 0.832, 0.264), vec3(0.379, -0.093, 0.921));
	vec3 q = R * p / s;
	float d = length(q - round(q)) / 0.7;
	return sqrt(max(1.0 - d * d, 0.0));
}

// Bump profile with bumps about 2 * leaf_bump_px apart on screen at this depth.
float leaf_profile(vec3 world, float view_z, mat4 proj, vec2 viewport) {
	float ppm = 0.5 * viewport.y * abs(proj[1][1]) / max(view_z, 0.1); // pixels per metre
	float level = log2(2.0 * leaf_bump_px / ppm);
	float s0 = exp2(floor(level));
	return mix(lattice_bumps(world, s0), lattice_bumps(world, 2.0 * s0), fract(level));
}

#define VOXEL_FRAGMENT_HOOK \
	bool leaf = (t < 0.5 ? ia : ib) == leaf_material; \
	ROUGHNESS = leaf ? 0.25 + 0.5 * leaf_profile(vpos, -VERTEX.z, PROJECTION_MATRIX, VIEWPORT_SIZE) : 1.0; \
	METALLIC = 0.0; \
	RIM = 0.0;

#include "res://addons/dk_voxel/voxel_terrain.gdshaderinc"
