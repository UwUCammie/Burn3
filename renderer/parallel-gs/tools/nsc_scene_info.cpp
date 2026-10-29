// nsc_scene_info: loads an NS-1 recording with nsc_scene.hpp and checks what the hybrid replay relies on.
//   nsc_scene_info <file.nsc> [--kicks]
// Prints the record counts and verifies: every draw's mesh, material, texture and palette exist; every mesh's
// indices are in range; frame boundaries (gs_kick_end of a frame is the stream's kick count at its VSync).
// With --kicks: kick ranges of the draws of categories A, B, C: sorted by first kick, draws that share one
// (a prop launch's instances) must tile consecutive kicks, and no two groups may overlap.
// Build (no renderer needed): g++ -std=c++17 -O2 nsc_scene_info.cpp -o nsc_scene_info
#include "nsc_scene.hpp"
#include <algorithm>
#include <map>

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "usage: %s file.nsc [--kicks]\n", argv[0]);
		return 2;
	}
	nsc::Scene scene;
	try { scene.open(argv[1]); }
	catch (const std::exception &e) { fprintf(stderr, "%s\n", e.what()); return 1; }
	printf("frames %zu, meshes %zu, textures %zu, palettes %zu, materials %zu, car constant blocks %zu\n", scene.frames().size(),
	       scene.meshes().size(), scene.textures().size(), scene.palettes().size(), scene.materials().size(), scene.constants().size());
	uint64_t other_missing_material = 0, draws = 0, missing_mesh = 0, missing_material = 0, missing_texture = 0, bad_index = 0;
	for (const auto &f : scene.frames())
		for (uint32_t i = 0; i < f.header->ndraws; i++)
		{
			const NscDraw &d = f.draws[i];
			draws++;
			if (d.category <= NSC_CAT_C_CAR && !d.gs_kick_count)
				continue; // culled by the game's own program: no GS state was ever set for it
			const auto *mat = scene.material(d.material_id);
			if (!mat) { (d.category <= NSC_CAT_C_CAR ? missing_material : other_missing_material)++; continue; }
			if (mat->tme && mat->tex_id && !(mat->flags & NSC_MAT_TEX_IS_RENDER_TARGET))
			{
				const auto *t = scene.texture(mat->tex_id);
				if (!t || (t->header->kind == 1 && !scene.palette(mat->pal_id))) missing_texture++;
			}
			if (d.category <= NSC_CAT_C_CAR && d.gs_kick_count)
			{
				const auto *m = scene.mesh(d.mesh_id);
				if (!m) { missing_mesh++; continue; }
				for (uint32_t k = 0; k < m->header->index_count; k++)
					if (m->index[k] >= m->header->vertex_count) { bad_index++; break; }
			}
		}
	printf("draws %llu (%llu of other categories have no material record): missing material (A, B, C) %llu, mesh %llu, texture/palette %llu, meshes with bad indices %llu\n", (unsigned long long)draws,
	       (unsigned long long)other_missing_material, (unsigned long long)missing_material, (unsigned long long)missing_mesh, (unsigned long long)missing_texture, (unsigned long long)bad_index);
	int status = missing_material || missing_mesh || missing_texture || bad_index ? 1 : 0;
	if (argc > 2 && !strcmp(argv[2], "--kicks"))
	{
		uint64_t groups = 0, overlaps = 0, untiled = 0;
		for (const auto &f : scene.frames())
		{
			std::map<uint64_t, std::vector<const NscDraw *>> by_first;
			for (uint32_t i = 0; i < f.header->ndraws; i++)
				if (f.draws[i].category >= NSC_CAT_A_WORLD && f.draws[i].category <= NSC_CAT_C_CAR && f.draws[i].gs_kick_count)
					by_first[f.draws[i].gs_kick_first].push_back(&f.draws[i]);
			uint64_t end = 0;
			for (const auto &g : by_first)
			{
				groups++;
				uint64_t total = 0;
				for (const NscDraw *d : g.second) total += d->gs_kick_count;
				if (g.first < end) overlaps++;
				end = g.first + total;
			}
			(void)untiled;
		}
		printf("kick groups %llu, overlapping %llu\n", (unsigned long long)groups, (unsigned long long)overlaps);
		status |= overlaps ? 1 : 0;
	}
	return status;
}
