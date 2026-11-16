// Save files: one zstd-compressed file per modified chunk, the in-memory store for unloaded edits, and the
// flush that writes both.
#include "voxel_world_state.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>

using namespace godot;

namespace dkv {

// Save file header: magic, format version, the volume's cell size, the generator class name, then the
// core's serialised chunk.
static const uint32_t DKVC_MAGIC = 0x43564B44; // 'DKVC', little-endian
static const uint16_t DKVC_VERSION = 1;

String chunk_file_path(const ChunkKey &key, const String &dir) {
	return dir.path_join(String::num_int64(key.x) + "_" + String::num_int64(key.y) + "_" + String::num_int64(key.z) + ".dkvc");
}

// Main thread only: the path is the world's save_directory, and a writer thread would need it snapshotted.
bool write_chunk_file(WorldState &st, const ChunkKey &key, const uint8_t *data, size_t size) {
	const String path = chunk_file_path(key, st.save_directory);
	String abs = st.save_directory;
	if (abs.begins_with("user://") || abs.begins_with("res://")) abs = ProjectSettings::get_singleton()->globalize_path(abs);
	if (!DirAccess::dir_exists_absolute(abs)) {
		Error e = DirAccess::make_dir_recursive_absolute(abs);
		if (e != OK) {
			UtilityFunctions::push_error("VoxelWorld: cannot create save directory " + abs);
			return false;
		}
	}
	const String tmp = path + String(".tmp");
	{
		Ref<FileAccess> f = FileAccess::open_compressed(tmp, FileAccess::WRITE, FileAccess::COMPRESSION_ZSTD);
		if (f.is_null()) {
			UtilityFunctions::push_error("VoxelWorld: cannot write " + tmp);
			return false;
		}
		f->store_32(DKVC_MAGIC);
		f->store_16(DKVC_VERSION);
		f->store_float(st.vol->cell);
		f->store_pascal_string(st.generator_class);
		f->store_32((uint32_t)size);
		f->store_buffer(data, (uint64_t)size);
	} // closed here: a reader never sees a half-written file
	Error e = DirAccess::rename_absolute(tmp, path);
	if (e != OK) {
		UtilityFunctions::push_error("VoxelWorld: cannot rename " + tmp + " to " + path);
		return false;
	}
	return true;
}

// Reads a chunk's save file into `out`. False when there is no file, or when the header names another
// format, cell size or generator (reported once with the path; the caller falls back to the generator).
// `dir` and `gen_class` come from the caller: workers pass the snapshot taken when their job was queued.
bool read_chunk_file(WorldState &st, const ChunkKey &key, const String &dir, const String &gen_class, std::vector<uint8_t> &out) {
	const String path = chunk_file_path(key, dir);
	if (!FileAccess::file_exists(path)) return false;
	Ref<FileAccess> f = FileAccess::open_compressed(path, FileAccess::READ, FileAccess::COMPRESSION_ZSTD);
	if (f.is_null()) return false;
	if (f->get_32() != DKVC_MAGIC || f->get_16() != DKVC_VERSION || f->get_float() != st.vol->cell || f->get_pascal_string() != gen_class) {
		UtilityFunctions::push_error("VoxelWorld: save file header mismatch (format/cell size/generator): " + path);
		return false;
	}
	const uint32_t n = f->get_32();
	PackedByteArray raw = f->get_buffer((int64_t)n);
	if (raw.size() != (int64_t)n) {
		UtilityFunctions::push_error("VoxelWorld: save file body is short (truncated): " + path);
		return false; // generate instead
	}
	out.assign((const uint8_t *)raw.ptr(), (const uint8_t *)raw.ptr() + raw.size());
	return true;
}

// Keep an edited chunk that is about to be unloaded: on disk when save_directory is set (the file is then
// authoritative), otherwise compressed in memory until something generates it again.
void store_chunk(WorldState &st, const ChunkKey &key, bool try_disk /* = true */) {
	if (!st.vol->is_modified(key.x, key.y, key.z)) return;
	std::vector<uint8_t> raw = st.vol->serialize_chunk(key.x, key.y, key.z);
	if (raw.empty()) return;
	// ponytail: unload writes on the main thread; move to a writer thread if hitches show
	if (try_disk && !st.save_directory.is_empty() && write_chunk_file(st, key, raw.data(), raw.size())) { // on disk: the file is authoritative
		st.vol->clear_modified(key.x, key.y, key.z);
		st.store.erase(key); // a stale store entry would shadow the file we just wrote
		return;
	}
	// no save_directory (or the write failed): compressed in memory until something generates it again
	PackedByteArray data;
	data.resize((int64_t)raw.size());
	memcpy(data.ptrw(), raw.data(), raw.size());
	st.store[key] = {(uint32_t)raw.size(), data.compress(FileAccess::COMPRESSION_ZSTD)};
}

// Writes every modified loaded chunk and every unloaded stored edit, then clears the modified flags so
// the same chunk is not written twice. Empty save_directory: nothing goes to disk.
Error save_world(WorldState &st) {
	if (st.save_directory.is_empty() || !st.vol) return OK;
	Error err = OK;
	std::vector<ChunkKey> written;
	for (const ChunkKey &key : st.vol->loaded_chunks())
		if (st.vol->is_modified(key.x, key.y, key.z)) { // loaded chunks are newer than any store entry
			st.store.erase(key);
			std::vector<uint8_t> raw = st.vol->serialize_chunk(key.x, key.y, key.z);
			if (raw.empty()) continue;
			if (write_chunk_file(st, key, raw.data(), raw.size())) st.vol->clear_modified(key.x, key.y, key.z);
			else err = ERR_FILE_CANT_WRITE;
		}
	for (const auto &kv : st.store) { // unloaded edits: decompressed back to the core's format
		PackedByteArray raw = kv.second.data.decompress((int64_t)kv.second.raw_size, FileAccess::COMPRESSION_ZSTD);
		if (raw.size() != (int64_t)kv.second.raw_size) {
			UtilityFunctions::push_error("VoxelWorld: stored chunk did not decompress");
			err = ERR_FILE_CORRUPT;
			continue;
		}
		if (!write_chunk_file(st, kv.first, (const uint8_t *)raw.ptr(), (size_t)raw.size())) err = ERR_FILE_CANT_WRITE;
		else written.push_back(kv.first); // erased after the loop: the map must not change while iterating
	}
	for (const ChunkKey &key : written) st.store.erase(key);
	return err;
}

} // namespace dkv
