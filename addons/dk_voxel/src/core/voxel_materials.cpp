// Material packing: VoxelMaterialDef/VoxelMaterialSet bindings, and the sampler arrays the shader samples.
#include "voxel_materials.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d_array.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>

using namespace godot;

namespace {

// Every layer of a Texture2DArray shares one size and format.
constexpr int TEXTURE_ARRAY_SIZE = 256;

// The layer a material with no texture gets: a flat colour, sized like the array so packing does not resize
// it. A Texture2DArray needs a value in every slot; nothing here draws a pattern.
Ref<Image> flat_image(const Color &color) {
	Ref<Image> img = Image::create(TEXTURE_ARRAY_SIZE, TEXTURE_ARRAY_SIZE, false, Image::FORMAT_RGBA8);
	img->fill(color);
	return img;
}

// One array layer from a Texture2D, resized to the array's size. Never mutates the game's image.
Ref<Image> layer_image(const Ref<Texture2D> &tex, const Ref<Image> &fallback) {
	Ref<Image> src;
	if (tex.is_valid()) src = tex->get_image();
	if (src.is_valid()) src = src->duplicate();
	if (src.is_null()) src = fallback->duplicate();
	if (src->is_compressed()) src->decompress();
	if (src->get_format() != Image::FORMAT_RGBA8) src->convert(Image::FORMAT_RGBA8);
	if (src->get_width() != TEXTURE_ARRAY_SIZE || src->get_height() != TEXTURE_ARRAY_SIZE)
		src->resize(TEXTURE_ARRAY_SIZE, TEXTURE_ARRAY_SIZE, Image::INTERPOLATE_LANCZOS);
	src->generate_mipmaps();
	return src;
}

// Cheap identity of the set's textures, so a colour-only change does not rebuild four arrays.
uint64_t texture_fingerprint(const Ref<VoxelMaterialSet> &set) {
	uint64_t h = 1469598103934665603ull;
	for (int i = 0; i < dkv::MATERIAL_SLOTS; i++) {
		Ref<VoxelMaterialDef> def = dkv::material_def(set, i);
		Ref<Texture2D> texs[4] = {def->get_albedo(), def->get_normal(), def->get_roughness_map(), def->get_ao_map()};
		for (const Ref<Texture2D> &tex : texs) {
			const int64_t id = tex.is_valid() ? (int64_t)tex->get_instance_id() : 0;
			h = (h ^ (uint64_t)id) * 1099511628211ull;
		}
	}
	return h;
}

} // namespace

Ref<VoxelMaterialDef> dkv::material_def(const Ref<VoxelMaterialSet> &set, int id) {
	TypedArray<VoxelMaterialDef> mats = set.is_valid() ? set->get_materials() : TypedArray<VoxelMaterialDef>();
	Ref<VoxelMaterialDef> m;
	if (id >= 0 && id < (int)mats.size()) m = mats[id];
	if (!m.is_valid() && !mats.is_empty()) m = mats[0]; // missing id (or a null entry): entry 0
	if (!m.is_valid()) m.instantiate(); // no set, or an empty one: grey default
	return m;
}

void dkv::apply_material_rules(WorldState &st) {
	if (!st.vol) return;
	st.vol->expose_inner = st.expose_inner_material;
	memset(st.vol->hardness, 0, sizeof(st.vol->hardness));
	for (int i = 0; i < 256; i++) st.vol->inner[i] = (uint8_t)i;
	if (!st.material_set.is_valid() || st.material_set->get_materials().is_empty()) return;
	for (int i = 0; i < 256; i++) {
		Ref<VoxelMaterialDef> def = material_def(st.material_set, i);
		st.vol->hardness[i] = (uint8_t)def->get_hardness();
		if (def->get_inner_material() >= 0) st.vol->inner[i] = (uint8_t)def->get_inner_material();
	}
}

void dkv::apply_material_set(WorldState &st) {
	ShaderMaterial *sm = Object::cast_to<ShaderMaterial>(st.material.ptr());
	if (!sm) return;
	// Everything goes straight to the RenderingServer, never through set_shader_parameter: these are derived from the
	// material set, and as ShaderMaterial properties they would be saved (megabytes of texture arrays) with the material.
	RenderingServer *rs = RenderingServer::get_singleton();
	const RID rid = sm->get_rid();

	// Colours and parameters describe every slot and are cheap: always push them.
	PackedColorArray ca, cb;
	Array params;
	for (int i = 0; i < MATERIAL_SLOTS; i++) {
		Ref<VoxelMaterialDef> m = material_def(st.material_set, i);
		ca.push_back(m->get_color_a());
		cb.push_back(m->get_color_b());
		params.push_back(Vector4(m->get_pattern_scale(), m->get_roughness(), m->get_metallic(), m->get_emission()));
	}
	rs->material_set_param(rid, "color_a", ca);
	rs->material_set_param(rid, "color_b", cb);
	rs->material_set_param(rid, "params", params);

	const TypedArray<VoxelMaterialDef> mats = st.material_set.is_valid() ? st.material_set->get_materials() : TypedArray<VoxelMaterialDef>();
	if (mats.size() > MATERIAL_SLOTS)
		UtilityFunctions::push_error("VoxelWorld: material set has " + String::num_int64(mats.size()) + " entries but the shader has " +
				String::num_int64(MATERIAL_SLOTS) + " slots; the extra entries are ignored");

	static const char *uniforms[4] = { "albedo_tex", "normal_tex", "roughness_tex", "ao_tex" };
	const uint64_t fingerprint = texture_fingerprint(st.material_set);
	if (fingerprint != st.material_fingerprint || st.material_arrays[0].is_null()) {
		st.material_fingerprint = fingerprint;
		const Ref<Image> grey = flat_image(Color(0.5f, 0.5f, 0.5f)), flat = flat_image(Color(0.5f, 0.5f, 1.0f)),
				one = flat_image(Color(1.0f, 1.0f, 1.0f));
		// One Texture2DArray per texture kind, layer = material id.
		auto pack = [&](int k, Ref<Texture2D> (VoxelMaterialDef::*get)() const, const Ref<Image> &fallback) {
			TypedArray<Ref<Image>> layers;
			for (int i = 0; i < MATERIAL_SLOTS; i++) layers.push_back(layer_image((material_def(st.material_set, i).ptr()->*get)(), fallback));
			Ref<Texture2DArray> array;
			array.instantiate();
			if (array->create_from_images(layers) == OK) st.material_arrays[k] = array;
		};
		pack(0, &VoxelMaterialDef::get_albedo, grey);
		pack(1, &VoxelMaterialDef::get_normal, flat);
		pack(2, &VoxelMaterialDef::get_roughness_map, one);
		pack(3, &VoxelMaterialDef::get_ao_map, one);
	} else if (rid == st.material_pushed) {
		return;
	}
	st.material_pushed = rid;
	for (int k = 0; k < 4; k++)
		if (st.material_arrays[k].is_valid()) rs->material_set_param(rid, uniforms[k], st.material_arrays[k]->get_rid());
}

void VoxelMaterialDef::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_name", "v"), &VoxelMaterialDef::set_name);
	ClassDB::bind_method(D_METHOD("get_name"), &VoxelMaterialDef::get_name);
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "name"), "set_name", "get_name");
	ClassDB::bind_method(D_METHOD("set_color_a", "v"), &VoxelMaterialDef::set_color_a);
	ClassDB::bind_method(D_METHOD("get_color_a"), &VoxelMaterialDef::get_color_a);
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color_a"), "set_color_a", "get_color_a");
	ClassDB::bind_method(D_METHOD("set_color_b", "v"), &VoxelMaterialDef::set_color_b);
	ClassDB::bind_method(D_METHOD("get_color_b"), &VoxelMaterialDef::get_color_b);
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color_b"), "set_color_b", "get_color_b");
	ClassDB::bind_method(D_METHOD("set_pattern_scale", "v"), &VoxelMaterialDef::set_pattern_scale);
	ClassDB::bind_method(D_METHOD("get_pattern_scale"), &VoxelMaterialDef::get_pattern_scale);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pattern_scale"), "set_pattern_scale", "get_pattern_scale");
	ClassDB::bind_method(D_METHOD("set_roughness", "v"), &VoxelMaterialDef::set_roughness);
	ClassDB::bind_method(D_METHOD("get_roughness"), &VoxelMaterialDef::get_roughness);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roughness"), "set_roughness", "get_roughness");
	ClassDB::bind_method(D_METHOD("set_metallic", "v"), &VoxelMaterialDef::set_metallic);
	ClassDB::bind_method(D_METHOD("get_metallic"), &VoxelMaterialDef::get_metallic);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "metallic"), "set_metallic", "get_metallic");
	ClassDB::bind_method(D_METHOD("set_emission", "v"), &VoxelMaterialDef::set_emission);
	ClassDB::bind_method(D_METHOD("get_emission"), &VoxelMaterialDef::get_emission);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "emission"), "set_emission", "get_emission");
	ClassDB::bind_method(D_METHOD("set_hardness", "v"), &VoxelMaterialDef::set_hardness);
	ClassDB::bind_method(D_METHOD("get_hardness"), &VoxelMaterialDef::get_hardness);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "hardness"), "set_hardness", "get_hardness");
	ClassDB::bind_method(D_METHOD("set_inner_material", "v"), &VoxelMaterialDef::set_inner_material);
	ClassDB::bind_method(D_METHOD("get_inner_material"), &VoxelMaterialDef::get_inner_material);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "inner_material", PROPERTY_HINT_RANGE, "-1,255"), "set_inner_material", "get_inner_material");
	ClassDB::bind_method(D_METHOD("set_albedo", "v"), &VoxelMaterialDef::set_albedo);
	ClassDB::bind_method(D_METHOD("get_albedo"), &VoxelMaterialDef::get_albedo);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "albedo", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_albedo", "get_albedo");
	ClassDB::bind_method(D_METHOD("set_normal", "v"), &VoxelMaterialDef::set_normal);
	ClassDB::bind_method(D_METHOD("get_normal"), &VoxelMaterialDef::get_normal);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "normal", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_normal", "get_normal");
	ClassDB::bind_method(D_METHOD("set_roughness_map", "v"), &VoxelMaterialDef::set_roughness_map);
	ClassDB::bind_method(D_METHOD("get_roughness_map"), &VoxelMaterialDef::get_roughness_map);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "roughness_map", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_roughness_map", "get_roughness_map");
	ClassDB::bind_method(D_METHOD("set_ao_map", "v"), &VoxelMaterialDef::set_ao_map);
	ClassDB::bind_method(D_METHOD("get_ao_map"), &VoxelMaterialDef::get_ao_map);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "ao_map", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_ao_map", "get_ao_map");
}

void VoxelMaterialSet::set_materials(const TypedArray<VoxelMaterialDef> &p_materials) {
	for (int i = 0; i < materials.size(); i++) {
		Ref<VoxelMaterialDef> old = materials[i];
		if (old.is_valid()) old->disconnect("changed", callable_mp(this, &VoxelMaterialSet::_def_changed));
	}
	materials = p_materials;
	for (int i = 0; i < materials.size(); i++) {
		Ref<VoxelMaterialDef> def = materials[i];
		if (def.is_valid()) def->connect("changed", callable_mp(this, &VoxelMaterialSet::_def_changed));
	}
	emit_changed();
}

void VoxelMaterialSet::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_materials", "v"), &VoxelMaterialSet::set_materials);
	ClassDB::bind_method(D_METHOD("get_materials"), &VoxelMaterialSet::get_materials);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "materials", PROPERTY_HINT_TYPE_STRING, "24/17:VoxelMaterialDef"), "set_materials", "get_materials");
}
