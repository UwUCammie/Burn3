#include "gs_dump_parser.hpp"
#include "gs_interface.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ParallelGS
{
namespace
{
// Budgets of the Takedown records. The native model limit mirrors
// TakedownNative::model_packet_limit (takedown/render/native_model_protocol.hpp);
// the other two are the limits of their producers.
constexpr uint32_t kNativeModelPacketLimit = 32U * 1024U * 1024U;
constexpr uint32_t kHostPresentationLimit = 64U * 1024U;
constexpr uint32_t kHostUiLimit = 512U * 1024U;
constexpr uint32_t kPaniniWarpPayload = 6U;
constexpr uint32_t kNativeCmdRecordLimit = 8U * 1024U * 1024U;
}

bool GSDumpParser::restart()
{
	if (!file)
		return false;

	// A pipe cannot be rewound, so there is nothing to replay from.
	if (is_raw && !seekable)
		return false;

	rewind(file.get());
	eof = feof(file.get()) != 0;
	truncated = false;
	vsync_count = 0;

	if (is_raw)
		return !eof;

	// FakeCRC
	if (read_u32() != UINT32_MAX)
		return false;

	// Parse header
	DumpHeader header = {};
	uint32_t header_size = read_u32();
	if (header_size < sizeof(header))
		return false;
	read_data(&header, sizeof(header));
	if (header.version < STATE_VERSION_MIN || header.version > STATE_VERSION)
		return false;

	// Skip over serial and screenshot data.
	read_skip(header_size - sizeof(header));

	// Context registers
	read_register_state();

	// VRAM
	read_data(iface->map_vram_write(0, vram_size), vram_size);
	iface->end_vram_write(0, vram_size);
	iface->write_register(RegisterAddr::TEXFLUSH, uint64_t(0));

	// GIFPaths
	for (uint32_t i = 0; i < 4; i++)
	{
		auto &gif_path = iface->get_gif_path(i);
		read_data(&gif_path.tag, sizeof(gif_path.tag));
		// The dump format doesn't have current loop counter, so assuming that real implementation
		// decrements loop counter after every iteration?
		gif_path.loop = 0;
		gif_path.reg = read_u32();
	}

	// InternalQ
	iface->get_register_state().internal_q = read_f32();

	// PrivRegisterState
	read_data(&iface->get_priv_register_state(), sizeof(iface->get_priv_register_state()));

	iface->clobber_register_state();

	return !eof;
}

bool GSDumpParser::open_raw_file(FILE *file_, uint32_t vram_size_, GSInterface *iface_)
{
	iface = iface_;
	vram_size = vram_size_;
	file.reset(file_);
	is_raw = true;
	eof = false;
	truncated = false;
	vsync_count = 0;
	native_models.clear();
	seekable = file && fseek(file.get(), 0, SEEK_CUR) == 0;
	return file != nullptr;
}

bool GSDumpParser::open_raw(const char *path, uint32_t vram_size_, GSInterface *iface_)
{
	return open_raw_file(fopen(path, "rb"), vram_size_, iface_);
}

bool GSDumpParser::open(const char *path, uint32_t vram_size_, GSInterface *iface_)
{
	iface = iface_;
	vram_size = vram_size_;
	file.reset(fopen(path, "rb"));
	if (!file)
		return false;

	is_raw = false;
	return restart();
}

void GSDumpParser::set_panini_callback(std::function<void(const PaniniRecord &)> callback)
{
	panini_callback = std::move(callback);
}

void GSDumpParser::set_native_cmd_callback(std::function<void(const uint8_t *, size_t)> callback)
{
	native_cmd_callback = std::move(callback);
}

std::vector<std::vector<uint8_t>> GSDumpParser::take_native_models()
{
	std::vector<std::vector<uint8_t>> result;
	std::swap(result, native_models);
	return result;
}

void GSDumpParser::read_register_state()
{
	auto &regs = iface->get_register_state();
	uint32_t version = read_u32(); // STATE_VERSION
	read_reg(regs.prim);
	read_reg(regs.prmodecont);
	read_reg(regs.texclut);
	read_reg(regs.scanmsk);
	read_reg(regs.texa);
	read_reg(regs.fogcol);
	read_reg(regs.dimx);
	read_reg(regs.dthe);
	read_reg(regs.colclamp);
	read_reg(regs.pabe);
	read_reg(regs.bitbltbuf);
	read_reg(regs.trxdir);
	read_reg(regs.trxpos);
	read_reg(regs.trxreg);
	read_u64(); // Dummy value

	for (auto &ctx : regs.ctx)
	{
		read_reg(ctx.xyoffset);
		read_reg(ctx.tex0);
		read_reg(ctx.tex1);
		read_reg(ctx.clamp);
		read_reg(ctx.miptbl_1_3);
		read_reg(ctx.miptbl_4_6);
		read_reg(ctx.scissor);
		read_reg(ctx.alpha);
		read_reg(ctx.test);
		read_reg(ctx.fba);
		read_reg(ctx.frame);
		read_reg(ctx.zbuf);
	}

	read_reg(regs.rgbaq);
	read_reg(regs.st);
	regs.uv.words[0] = read_u32();
	regs.fog.words[0] = read_u32();
	read_u64(); // Dummy XYZ

	read_u32(); // Dummy GIFReg
	read_u32();

	// Dummy transfer X/Y
	read_u32();
	read_u32();

	if (version >= 9)
	{
		// Skip dummy state.
		read_skip(61);
	}
}

bool GSDumpParser::iterate_until_vsync(bool high_res_scanout, bool conservative_crtc)
{
	if (!file)
		return false;

	bool has_transfer = false;

	while (!eof)
	{
		auto type = GSDumpPacketType(read_u8());
		// End of stream at a packet boundary: clean, not truncated.
		if (eof)
			break;

		switch (type)
		{
		case GSDumpPacketType::Transfer:
		{
			auto path = read_u8();
			auto size = read_u32();
			if (eof)
				break;
			// gif_transfer() works in whole quadwords; a partial one cannot be sized or consumed.
			if (size % sizeof(GIFTagBits) != 0)
				throw std::runtime_error("malformed GIF transfer record");
			auto num_words = size / sizeof(GIFTagBits);
			if (num_words > gif_tag_buffer.size())
				gif_tag_buffer.resize(num_words);
			read_data(gif_tag_buffer.data(), size);
			if (!eof)
				iface->gif_transfer(path, gif_tag_buffer.data(), size);
			has_transfer = true;
			break;
		}

		case GSDumpPacketType::Vsync:
		{
			VSyncInfo vsync = {};
			vsync.phase = read_u8();
			if (eof)
				break;
			vsync_count++;
			vsync.dst_layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
			vsync.dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			vsync.dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
			vsync.adapt_to_internal_horizontal_resolution = true;

			if (conservative_crtc)
			{
				vsync.skip_deinterlace = true;
				vsync.crtc_offsets = true;
				vsync.overscan = true;
			}
			else
			{
				vsync.high_resolution_scanout = high_res_scanout;
				vsync.scanout_scale = scanout_scale;
				vsync.force_progressive = true;
				vsync.anti_blur = true;
			}

			iface->flush();
			vsync_result = iface->vsync(vsync);
			if (has_transfer || conservative_crtc)
				return true;
			break;
		}

		case GSDumpPacketType::PrivRegisters:
			read_data(&iface->get_priv_register_state(), sizeof(iface->get_priv_register_state()));
			break;

		case GSDumpPacketType::NativeModel:
		{
			auto size = read_u32();
			if (eof)
				break;
			if (size > kNativeModelPacketLimit)
				throw std::runtime_error("native model stream record exceeds budget");
			std::vector<uint8_t> payload;
			read_payload(payload, size);
			if (!eof)
				native_models.push_back(std::move(payload));
			break;
		}

		case GSDumpPacketType::HostPresentation:
		{
			auto size = read_u32();
			if (eof)
				break;
			if (size > kHostPresentationLimit)
				throw std::runtime_error("host presentation record exceeds budget");
			std::vector<uint8_t> payload;
			read_payload(payload, size);
			if (!eof)
			{
				host_presentation = std::move(payload);
				host_presentation_serial++;
			}
			break;
		}

		case GSDumpPacketType::PaniniWarp:
		{
			auto size = read_u32();
			if (eof)
				break;
			if (size != kPaniniWarpPayload)
				throw std::runtime_error("malformed Panini warp record");
			PaniniRecord record = {};
			record.tangent = read_u16();
			record.strength = read_u8();
			record.flags = read_u8();
			record.half_width = read_u16();
			if (eof)
				break;
			if (panini_callback)
				panini_callback(record);
			break;
		}

		case GSDumpPacketType::HostUi:
		{
			auto size = read_u32();
			if (eof)
				break;
			if (size > kHostUiLimit)
				throw std::runtime_error("host UI record exceeds budget");
			std::vector<uint8_t> payload;
			read_payload(payload, size);
			if (!eof)
			{
				host_ui = std::move(payload);
				host_ui_serial++;
			}
			break;
		}

		case GSDumpPacketType::NativeCmd:
		{
			auto size = read_u32();
			if (eof)
				break;
			// Live play: a record this renderer cannot use is skipped by its length, never fatal (a stream error would stop the
			// renderer reading and the game would block on the pipe). The native commands it carried are not drawn.
			static bool warned = false;
			if (size > kNativeCmdRecordLimit || !native_cmd_callback)
			{
				if (!warned)
				{
					warned = true;
					LOGW("Skipping a native command record of %u bytes (%s).\n", size,
					     size > kNativeCmdRecordLimit ? "over the 8 MiB limit" : "no native command consumer");
				}
				// Not read_skip: that seeks, and a live pipe cannot.
				uint8_t scratch[16384];
				for (uint32_t left = size; left && !eof;)
				{
					const uint32_t n = std::min<uint32_t>(left, sizeof(scratch));
					read_data(scratch, n);
					left -= n;
				}
				break;
			}
			std::vector<uint8_t> payload;
			read_payload(payload, size);
			if (!eof)
			{
				try
				{
					native_cmd_callback(payload.data(), payload.size());
				}
				catch (const std::exception &e)
				{
					if (!warned)
					{
						warned = true;
						LOGW("Native command record dropped: %s\n", e.what());
					}
				}
			}
			break;
		}

		case GSDumpPacketType::ReadFIFO:
		{
			// Unimplemented.
			LOGW("Unimplemented ReadFIFO\n");
			read_u32(); // Size of the unimplemented payload.
			if (eof)
				break;
			return true;
		}

		default:
			throw std::runtime_error("unknown stream record kind " + std::to_string(unsigned(type)));
		}

		if (eof)
			truncated = true;
	}

	return !eof;
}

ScanoutResult GSDumpParser::consume_vsync_result()
{
	ScanoutResult result = {};
	std::swap(result, vsync_result);
	return result;
}

uint8_t GSDumpParser::read_u8()
{
	uint8_t v = 0;
	if (fread(&v, sizeof(v), 1, file.get()) != 1)
		eof = true;
	return v;
}

uint16_t GSDumpParser::read_u16()
{
	uint16_t v = 0;
	if (fread(&v, sizeof(v), 1, file.get()) != 1)
		eof = true;
	return v;
}

uint32_t GSDumpParser::read_u32()
{
	uint32_t v = 0;
	if (fread(&v, sizeof(v), 1, file.get()) != 1)
		eof = true;
	return v;
}

float GSDumpParser::read_f32()
{
	float v = 0;
	if (fread(&v, sizeof(v), 1, file.get()) != 1)
		eof = true;
	return v;
}

uint64_t GSDumpParser::read_u64()
{
	uint64_t v = 0;
	if (fread(&v, sizeof(v), 1, file.get()) != 1)
		eof = true;
	return v;
}

template <typename T>
void GSDumpParser::read_reg(T &reg)
{
	reg.bits = read_u64();
}

void GSDumpParser::read_skip(size_t size)
{
	fseek(file.get(), long(size), SEEK_CUR);
}

void GSDumpParser::read_data(void *data, size_t size)
{
	if (fread(data, 1, size, file.get()) != size)
		eof = true;
}

void GSDumpParser::read_payload(std::vector<uint8_t> &payload, uint32_t size)
{
	payload.resize(size);
	if (size != 0)
		read_data(payload.data(), size);
}
}
