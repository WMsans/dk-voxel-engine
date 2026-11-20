// Godot-facing materials: one resource per material id, and the packing that turns their textures into the
// sampler arrays the terrain shader samples.
#pragma once
#include "voxel_world_state.h"

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

using namespace godot;

namespace dkv {
// The shader declares the same number; tests/run_tests.gd fails if the two disagree. The voxel format allows
// 256 ids, but the shader's uniform arrays stop here: an id past it renders as the last slot.
inline constexpr int MATERIAL_SLOTS = 32;
} // namespace dkv

// One entry of a VoxelMaterialSet. hardness: erase power needed, 255 = unbreakable. The two colours are a tint
// ramp over `albedo`; set both to white to show a texture unmodified.
class VoxelMaterialDef : public Resource {
	GDCLASS(VoxelMaterialDef, Resource)

	String name;
	Color color_a = Color(0.5f, 0.5f, 0.5f);
	Color color_b = Color(0.6f, 0.6f, 0.6f);
	float pattern_scale = 0.3f; // tiles per metre
	float roughness = 0.9f;
	float metallic = 0.0f;
	float emission = 0.0f;
	int hardness = 0;
	int inner_material = -1; // id a dig turns this material into (its broken inside); -1 = stays itself
	Ref<Texture2D> albedo; // null = the built-in grain
	Ref<Texture2D> normal; // null = flat
	Ref<Texture2D> roughness_map; // null = 1.0, so the scalar `roughness` alone applies
	Ref<Texture2D> ao_map; // null = 1.0, so the baked voxel AO alone applies

public:
	void set_name(const String &p_name) { name = p_name; emit_changed(); }
	String get_name() const { return name; }
	void set_color_a(const Color &p_color) { color_a = p_color; emit_changed(); }
	Color get_color_a() const { return color_a; }
	void set_color_b(const Color &p_color) { color_b = p_color; emit_changed(); }
	Color get_color_b() const { return color_b; }
	void set_pattern_scale(float p_scale) { pattern_scale = p_scale; emit_changed(); }
	float get_pattern_scale() const { return pattern_scale; }
	void set_roughness(float p_roughness) { roughness = p_roughness; emit_changed(); }
	float get_roughness() const { return roughness; }
	void set_metallic(float p_metallic) { metallic = p_metallic; emit_changed(); }
	float get_metallic() const { return metallic; }
	void set_emission(float p_emission) { emission = p_emission; emit_changed(); }
	float get_emission() const { return emission; }
	void set_hardness(int p_hardness) { hardness = CLAMP(p_hardness, 0, 255); emit_changed(); }
	int get_hardness() const { return hardness; }
	void set_inner_material(int p_id) { inner_material = CLAMP(p_id, -1, 255); emit_changed(); }
	int get_inner_material() const { return inner_material; }
	void set_albedo(const Ref<Texture2D> &p_texture) { albedo = p_texture; emit_changed(); }
	Ref<Texture2D> get_albedo() const { return albedo; }
	void set_normal(const Ref<Texture2D> &p_texture) { normal = p_texture; emit_changed(); }
	Ref<Texture2D> get_normal() const { return normal; }
	void set_roughness_map(const Ref<Texture2D> &p_texture) { roughness_map = p_texture; emit_changed(); }
	Ref<Texture2D> get_roughness_map() const { return roughness_map; }
	void set_ao_map(const Ref<Texture2D> &p_texture) { ao_map = p_texture; emit_changed(); }
	Ref<Texture2D> get_ao_map() const { return ao_map; }

protected:
	static void _bind_methods();
};

// Index = material id. Missing ids read as entry 0. Re-emits its defs' `changed`, so a VoxelWorld holding the
// set re-packs when any entry is edited in place.
class VoxelMaterialSet : public Resource {
	GDCLASS(VoxelMaterialSet, Resource)

	TypedArray<VoxelMaterialDef> materials;

	void _def_changed() { emit_changed(); }

public:
	void set_materials(const TypedArray<VoxelMaterialDef> &p_materials);
	TypedArray<VoxelMaterialDef> get_materials() const { return materials; }

protected:
	static void _bind_methods();
};

namespace dkv {

// Def for a material id: entry `id` if the set has one, else entry 0 (a non-empty set), else a default grey
// zero-hardness def.
Ref<VoxelMaterialDef> material_def(const Ref<VoxelMaterialSet> &set, int id);
// Copies the set's hardness and inner-material tables, and the world's expose_inner flag, into the volume.
void apply_material_rules(WorldState &st);
// Packs each of albedo, normal, roughness and AO into one Texture2DArray (layer = material id) and pushes every
// uniform the terrain shader reads: albedo_tex, normal_tex, roughness_tex, ao_tex, color_a, color_b, params.
void apply_material_set(WorldState &st);

} // namespace dkv
