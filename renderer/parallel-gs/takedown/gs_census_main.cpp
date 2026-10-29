// takedown-gs-census: what a recorded GS stream draws (gs_census.hpp).
//
//   takedown-gs-census <stream> [--frames] [--runs <frame>[,<frame>...]]
//
// <stream> is a TAKEDOWN_DEV_STREAM_DUMP file. Prints totals; --frames adds a
// line per frame, --runs lists every draw run of the given frames in order.
#include "gs_census.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace TakedownRenderer;

namespace
{
struct Options
{
	bool frames = false;
	std::set<uint64_t> runs;
};

std::string declines_string(uint32_t declines)
{
	std::string text;
	for (uint32_t d = 0; d < uint32_t(CensusDecline::Count); d++)
		if (declines & (1u << d))
		{
			if (!text.empty())
				text += ",";
			text += census_decline_name(CensusDecline(d));
		}
	return text;
}

void print_frame(const CensusFrame &f, void *user)
{
	const Options &options = *static_cast<const Options *>(user);
	if (options.frames)
	{
		std::printf("frame %llu: runs %zu", (unsigned long long)f.index, f.runs.size());
		for (size_t c = 0; c < f.primitives.size(); c++)
			std::printf(" %s %llu", census_class_name(CensusClass(c)), (unsigned long long)f.primitives[c]);
		std::printf(" | world hostable %llu", (unsigned long long)f.world_hostable);
		if (f.has_scene)
		{
			const CensusTarget &t = f.targets.at(f.scene_fbp);
			std::printf(" | scene fbp %u (block %u) fbw %u %s", t.fbp, t.fbp * 32u, t.fbw, census_psm_name(t.psm).c_str());
		}
		std::printf(" | targets %zu transfers %zu", f.targets.size(), f.transfers.size());
		if (f.panini)
			std::printf(" panini");
		if (f.native_models)
			std::printf(" native-models %llu", (unsigned long long)f.native_models);
		std::printf("\n");
	}
	if (!options.runs.count(f.index))
		return;
	std::printf("frame %llu targets:\n", (unsigned long long)f.index);
	for (const auto &entry : f.targets)
	{
		const CensusTarget &t = entry.second;
		std::printf("  fbp %u (block %u) fbw %u %s max_y %d prims %llu", t.fbp, t.fbp * 32u, t.fbw,
		            census_psm_name(t.psm).c_str(), t.max_y, (unsigned long long)t.primitives);
		for (size_t c = 0; c < t.by_class.size(); c++)
			if (t.by_class[c])
				std::printf(" %s %llu", census_class_name(CensusClass(c)), (unsigned long long)t.by_class[c]);
		std::printf("\n");
	}
	std::printf("frame %llu runs:\n", (unsigned long long)f.index);
	size_t transfer = 0;
	for (size_t i = 0; i <= f.runs.size(); i++)
	{
		for (; transfer < f.transfers.size() && f.transfers[transfer].before_run == i; transfer++)
		{
			const CensusTransfer &t = f.transfers[transfer];
			static const char *dir[] = { "upload", "download", "copy", "off" };
			std::printf("  -- %s %ux%u src %u/%u %s -> dst %u/%u %s%s%s\n", dir[t.direction & 3], t.width, t.height,
			            t.sbp, t.sbw, census_psm_name(t.spsm).c_str(), t.dbp, t.dbw, census_psm_name(t.dpsm).c_str(),
			            t.bytes ? (" " + std::to_string(t.bytes) + " bytes").c_str() : "",
			            t.destination_is_target ? " INTO-TARGET" : "");
		}
		if (i == f.runs.size())
			break;
		const CensusRun &r = f.runs[i];
		const CensusState &s = r.state;
		std::printf("  %4zu %-5s %6llu prims fb %u/%u %s z %u %s | box %d,%d-%d,%d z %06x-%06x | %s | %s | %s",
		            i, census_class_name(r.cls), (unsigned long long)r.primitives, s.fbp * 32u, s.fbw,
		            census_psm_name(s.fpsm).c_str(), s.zbp * 32u, census_psm_name(s.zpsm).c_str(), r.x0, r.y0, r.x1,
		            r.y1, r.z_min, r.z_max, census_blend_string(s).c_str(), census_test_string(s).c_str(),
		            census_texture_string(s).c_str());
		if (s.tme)
			std::printf(" @%u/%u %ux%u", s.tbp0, s.tbw, 1u << s.tw, 1u << s.th);
		if (s.fbmsk)
			std::printf(" fbmsk %08x", s.fbmsk);
		if (s.fge)
			std::printf(" fog");
		if (r.declines)
			std::printf(" [%s]", declines_string(r.declines).c_str());
		std::printf("\n");
	}
}

void print_histogram(const char *title, const std::map<std::string, uint64_t> &histogram, uint64_t total)
{
	std::vector<std::pair<uint64_t, std::string>> rows;
	for (const auto &entry : histogram)
		rows.emplace_back(entry.second, entry.first);
	std::sort(rows.rbegin(), rows.rend());
	std::printf("%s:\n", title);
	size_t shown = 0;
	for (const auto &row : rows)
	{
		if (shown++ == 24)
		{
			std::printf("  ... %zu more\n", rows.size() - 24);
			break;
		}
		std::printf("  %12llu %5.1f%%  %s\n", (unsigned long long)row.first,
		            total ? 100.0 * double(row.first) / double(total) : 0.0, row.second.c_str());
	}
}
} // namespace

int main(int argc, char **argv)
{
	Options options;
	const char *path = nullptr;
	for (int i = 1; i < argc; i++)
	{
		if (!std::strcmp(argv[i], "--frames"))
			options.frames = true;
		else if (!std::strcmp(argv[i], "--runs") && i + 1 < argc)
		{
			std::string list = argv[++i];
			size_t start = 0;
			while (start < list.size())
			{
				const size_t comma = list.find(',', start);
				options.runs.insert(std::strtoull(list.substr(start, comma - start).c_str(), nullptr, 10));
				start = comma == std::string::npos ? list.size() : comma + 1;
			}
		}
		else if (!path && argv[i][0] != '-')
			path = argv[i];
		else
		{
			std::fprintf(stderr, "usage: %s <stream> [--frames] [--runs <frame>[,<frame>...]]\n", argv[0]);
			return 2;
		}
	}
	if (!path)
	{
		std::fprintf(stderr, "usage: %s <stream> [--frames] [--runs <frame>[,<frame>...]]\n", argv[0]);
		return 2;
	}
	FILE *file = std::fopen(path, "rb");
	if (!file)
	{
		std::perror(path);
		return 1;
	}
	GsCensus census;
	census.set_frame_callback(print_frame, &options);
	std::vector<uint8_t> buffer(1u << 20);
	size_t read;
	while ((read = std::fread(buffer.data(), 1, buffer.size(), file)) > 0)
		census.feed(buffer.data(), read);
	std::fclose(file);
	census.finish();

	const CensusTotals &t = census.totals();
	uint64_t all = 0;
	for (uint64_t n : t.primitives)
		all += n;
	const uint64_t world = t.primitives[size_t(CensusClass::World)];
	std::printf("\nframes %llu (with world %llu), primitives %llu\n", (unsigned long long)t.frames,
	            (unsigned long long)t.frames_with_world, (unsigned long long)all);
	for (size_t c = 0; c < t.primitives.size(); c++)
		std::printf("  %-6s %12llu %5.1f%%\n", census_class_name(CensusClass(c)), (unsigned long long)t.primitives[c],
		            all ? 100.0 * double(t.primitives[c]) / double(all) : 0.0);
	std::printf("world primitives a host rasterizer could draw (no hard decline): %llu of %llu (%.1f%%)\n",
	            (unsigned long long)t.world_hostable, (unsigned long long)world,
	            world ? 100.0 * double(t.world_hostable) / double(world) : 0.0);
	std::printf("declines (all classes, primitives):\n");
	for (size_t d = 0; d < t.declines.size(); d++)
		if (t.declines[d])
			std::printf("  %-18s %12llu\n", census_decline_name(CensusDecline(d)), (unsigned long long)t.declines[d]);
	std::printf("transfers: %llu uploads (%llu bytes), %llu local copies, %llu into render targets\n",
	            (unsigned long long)t.transfers_upload, (unsigned long long)t.upload_bytes,
	            (unsigned long long)t.transfers_local, (unsigned long long)t.transfers_into_targets);
	print_histogram("world prim types", t.world_prim, world);
	print_histogram("world blend", t.world_blend, world);
	print_histogram("world depth/alpha test", t.world_test, world);
	print_histogram("world texture", t.world_texture, world);
	print_histogram("render-target reads (primitives)", t.reads_kinds, t.primitives[size_t(CensusClass::Reads)]);
	if (t.parse_errors)
		std::printf("PARSE ERRORS: %llu (stream out of sync)\n", (unsigned long long)t.parse_errors);
	return t.parse_errors ? 1 : 0;
}
