// nsc_scene.hpp -- reader for the NS-1 native scene recording (.nsc, see native_scene_format.h).
//
// Header-only and independent of the renderer: the file is mapped read-only and every record is
// a pointer into the mapping (no copies), indexed by id. Game-derived data: the recordings stay
// under tmp/. Used by tools/ns2_hybrid.hpp (the hybrid replay) and tools/nsc_scene_info.cpp.
#pragma once
#include "native_scene_format.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace nsc
{
struct MeshRef
{
	const NscMeshHeader *header = nullptr;
	const float *position = nullptr; // vertex_count * 3
	const float *uv = nullptr;       // vertex_count * 2 or null
	const uint8_t *colour = nullptr; // vertex_count * 4 or null
	const float *normal = nullptr;   // vertex_count * 3 or null
	const uint32_t *index = nullptr; // index_count
};
struct TextureRef
{
	const NscTextHeader *header = nullptr;
	const uint8_t *data = nullptr; // every mip level, level 0 first
	size_t level_bytes(uint32_t level) const
	{
		const size_t w = std::max(1u, header->width >> level), h = std::max(1u, header->height >> level);
		return w * h * (header->kind == 0 ? 4u : 1u);
	}
};
struct PaletteRef
{
	const NscPaltHeader *header = nullptr;
	const uint8_t *rgba = nullptr; // count * 4
};
struct FrameRef
{
	const NscFrameHeader *header = nullptr;
	const NscView *views = nullptr;
	const NscDraw *draws = nullptr;
};

class Scene
{
public:
	Scene() = default;
	Scene(const Scene &) = delete;
	Scene &operator=(const Scene &) = delete;
	~Scene()
	{
#ifndef _WIN32
		if (base_)
			munmap(const_cast<uint8_t *>(base_), size_);
#endif
	}

	// Throws std::runtime_error on a malformed or missing file.
	void open(const std::string &path)
	{
#ifdef _WIN32
		throw std::runtime_error("NSC recordings are not supported on Windows: " + path);
#else
		const int fd = ::open(path.c_str(), O_RDONLY);
		if (fd < 0)
			throw std::runtime_error("cannot open " + path);
		struct stat st = {};
		fstat(fd, &st);
		size_ = size_t(st.st_size);
		void *map = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
		close(fd);
		if (map == MAP_FAILED)
			throw std::runtime_error("cannot map " + path);
		base_ = static_cast<const uint8_t *>(map);
		madvise(map, size_, MADV_SEQUENTIAL);
#endif
		if (size_ < sizeof(NscFileHeader))
			throw std::runtime_error("short file");
		const auto *fh = reinterpret_cast<const NscFileHeader *>(base_);
		if (fh->magic != NSC_MAGIC || fh->version != NSC_VERSION)
			throw std::runtime_error("not an NSC1 version 1 file");
		size_t o = sizeof(NscFileHeader);
		while (o + sizeof(NscChunkHeader) <= size_)
		{
			const auto *ch = reinterpret_cast<const NscChunkHeader *>(base_ + o);
			o += sizeof(NscChunkHeader);
			if (o + ch->payload_bytes > size_)
				throw std::runtime_error("truncated chunk");
			const uint8_t *p = base_ + o;
			index_chunk(ch->fourcc, p, size_t(ch->payload_bytes));
			o += size_t(ch->payload_bytes);
		}
#ifndef _WIN32
		madvise(map, size_, MADV_RANDOM);
#endif
	}

	const std::string &meta() const { return meta_; }
	const std::vector<FrameRef> &frames() const { return frames_; }
	const std::unordered_map<uint64_t, MeshRef> &meshes() const { return meshes_; }
	const std::unordered_map<uint64_t, TextureRef> &textures() const { return textures_; }
	const std::unordered_map<uint64_t, PaletteRef> &palettes() const { return palettes_; }
	const std::unordered_map<uint64_t, const NscMaterial *> &materials() const { return materials_; }
	// A car constant block: 52 quadwords (208 floats), by id (low 32 bits of the draw's const_id).
	const std::unordered_map<uint32_t, const float *> &constants() const { return constants_; }

	// A car mesh's two damage influences per vertex (null when the recording has none).
	const uint32_t *damage(uint64_t mesh_id) const { auto it = damage_.find(mesh_id); return it == damage_.end() ? nullptr : it->second; }
	const MeshRef *mesh(uint64_t id) const { auto it = meshes_.find(id); return it == meshes_.end() ? nullptr : &it->second; }
	const TextureRef *texture(uint64_t id) const { auto it = textures_.find(id); return it == textures_.end() ? nullptr : &it->second; }
	const PaletteRef *palette(uint64_t id) const { auto it = palettes_.find(id); return it == palettes_.end() ? nullptr : &it->second; }
	const NscMaterial *material(uint64_t id) const { auto it = materials_.find(id); return it == materials_.end() ? nullptr : it->second; }

private:
	static size_t pad16(size_t n) { return (n + 15u) & ~size_t(15); }

	void index_chunk(uint32_t fourcc, const uint8_t *p, size_t bytes)
	{
		switch (fourcc)
		{
		case NSC_CH_META:
			meta_.assign(reinterpret_cast<const char *>(p), strnlen(reinterpret_cast<const char *>(p), bytes));
			break;
		case NSC_CH_MESH:
		{
			MeshRef m;
			m.header = reinterpret_cast<const NscMeshHeader *>(p);
			const uint32_t nv = m.header->vertex_count, flags = m.header->flags;
			size_t q = pad16(sizeof(NscMeshHeader));
			m.position = reinterpret_cast<const float *>(p + q); q += pad16(size_t(nv) * 12);
			if (flags & NSC_MESH_HAS_UV) { m.uv = reinterpret_cast<const float *>(p + q); q += pad16(size_t(nv) * 8); }
			if (flags & NSC_MESH_HAS_COLOR) { m.colour = p + q; q += pad16(size_t(nv) * 4); }
			if (flags & NSC_MESH_HAS_NORMAL) { m.normal = reinterpret_cast<const float *>(p + q); q += pad16(size_t(nv) * 12); }
			if (flags & NSC_MESH_HAS_DAMAGE) q += pad16(size_t(nv) * 8);
			if (m.header->aux_floats) q += pad16(size_t(nv) * m.header->aux_floats * 4);
			m.index = reinterpret_cast<const uint32_t *>(p + q);
			meshes_[m.header->mesh_id] = m;
			break;
		}
		case NSC_CH_TEXT:
		{
			TextureRef t;
			t.header = reinterpret_cast<const NscTextHeader *>(p);
			t.data = p + sizeof(NscTextHeader);
			textures_[t.header->tex_id] = t;
			break;
		}
		case NSC_CH_PALT:
		{
			PaletteRef t;
			t.header = reinterpret_cast<const NscPaltHeader *>(p);
			t.rgba = p + sizeof(NscPaltHeader);
			palettes_[t.header->pal_id] = t;
			break;
		}
		case NSC_CH_MATL:
		{
			const auto *m = reinterpret_cast<const NscMaterial *>(p);
			materials_[m->material_id] = m;
			break;
		}
		case NSC_CH_CSTB:
		{
			// u64 id, u32 kind, u32 quadwords, then the quadwords.
			uint64_t id;
			std::memcpy(&id, p, 8);
			constants_[uint32_t(id)] = reinterpret_cast<const float *>(p + 16);
			break;
		}
		case NSC_CH_FRAM:
		{
			FrameRef f;
			f.header = reinterpret_cast<const NscFrameHeader *>(p);
			const size_t views = sizeof(NscFrameHeader) + f.header->env_bytes;
			f.views = reinterpret_cast<const NscView *>(p + views);
			f.draws = reinterpret_cast<const NscDraw *>(p + views + size_t(f.header->nviews) * sizeof(NscView));
			frames_.push_back(f);
			break;
		}
		case NSC_CH_DAMG:
		{
			// u64 mesh id, u32 vertex count, u32 0, then uint32 influences[vertex_count][2].
			uint64_t id;
			std::memcpy(&id, p, 8);
			damage_[id] = reinterpret_cast<const uint32_t *>(p + 16);
			break;
		}
		default:
			break; // BLOB: not needed here
		}
	}

	const uint8_t *base_ = nullptr;
	size_t size_ = 0;
	std::string meta_;
	std::vector<FrameRef> frames_;
	std::unordered_map<uint64_t, MeshRef> meshes_;
	std::unordered_map<uint64_t, TextureRef> textures_;
	std::unordered_map<uint64_t, PaletteRef> palettes_;
	std::unordered_map<uint64_t, const NscMaterial *> materials_;
	std::unordered_map<uint32_t, const float *> constants_;
	std::unordered_map<uint64_t, const uint32_t *> damage_;
};
}
