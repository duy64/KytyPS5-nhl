#include "graphics/host_gpu/renderer/frameDump.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"

#include <cerrno>
#include <set>
#include <algorithm>
#include <array>
#include <bit>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBIWDEF static inline
#include "stb_image_write.h"

namespace Libs::Graphics::FrameDump {

std::atomic<bool> g_active {false};

namespace {

constexpr uint32_t   kMaxImages       = 400;
constexpr uint32_t   kMinSize         = 8;
constexpr uint64_t   kMaxPendingBytes = 3ull << 30u;

struct Target {
	vk::Image            image = nullptr;
	vk::Format           format = vk::Format::eUndefined;
	vk::ImageAspectFlags full_aspect = vk::ImageAspectFlagBits::eColor;
	vk::ImageAspectFlags copy_aspect = vk::ImageAspectFlagBits::eColor;
	uint32_t             mip = 0, layer = 0, w = 0, h = 0, samples = 1;
	vk::ImageLayout      layout = vk::ImageLayout::eUndefined;
	bool                 depth = false;
	uint32_t             slot = 0;
	uint64_t             addr = 0;
	bool                 dirty = false;
	uint32_t             seq = 0;
	uint32_t             slices = 1; // >1: 3D volume, all z slices are read back
	bool                 img3d = false; // 3D image bound to a KYTY_DBG_FRAME_DUMP_BUF_PS draw: raw .bin + IMG3D line
	bool                 relabelled = false; // format holds the registered video-out format, not the creation format
	vk::Format           created_format = vk::Format::eUndefined; // cache creation format, kept when relabelled
};

struct Pending {
	vk::Buffer    buffer = nullptr;
	VmaAllocation alloc  = nullptr;
	void*         mapped = nullptr;
	uint64_t      size   = 0;
	Target        target;
	uint32_t      seq = 0;
	std::string   label;
	GraphicContext* graphics = nullptr;
};

std::recursive_mutex                    g_mutex;
std::unordered_map<uint64_t, Target>    g_targets;
std::vector<Pending>                    g_pending;
std::string                             g_log;
std::string                             g_notes;
uint32_t                                g_seq           = 0;
uint32_t                                g_images        = 0;
uint64_t                                g_pending_bytes = 0;
std::string                             g_dir;
uint32_t                                g_frame = 0;
bool                                    g_capturing = false;
std::atomic<bool>                       g_request {false};
std::atomic<uint64_t>                   g_trace_frame {0};

// ---- buffer / volume content capture (lighting investigation) ----
struct BufReq {
	vk::Buffer  buf = nullptr;
	uint64_t    offset = 0, size = 0;
	uint32_t    seq = 0;
	std::string label;
	uint64_t    addr = 0;
	std::shared_ptr<Buffer> retained;
};
struct BufPending {
	vk::Buffer      buffer = nullptr;
	VmaAllocation   alloc  = nullptr;
	void*           mapped = nullptr;
	BufReq          req;
	GraphicContext* graphics = nullptr;
};
struct VolReq {
	Target   target;
	uint32_t seq = 0;
};
std::vector<BufReq>      g_bufreq;
std::vector<VolReq>      g_volreq;
std::vector<BufPending>  g_bufpending;
std::string              g_buflog;
std::set<std::pair<uint64_t, uint32_t>> g_seen_vol; // (address, draw seq)
std::set<std::string>    g_seen_buf;

// ---- guest sampled-texture readback (NHL27GUESTTEX) ----
// Sampled textures are never read back from the GPU (the dump saves render
// targets and written storage images only), so what a shader actually sampled
// is the dump's blind spot. For every unique sampled binding of the captured
// frame, the download worker copies the RAW GUEST BYTES out of guest memory
// (LibKernel::Memory::TryReadBacking, which fails cleanly on unmapped/short
// ranges) into a .bin next to the PNGs, indexed by guesttex.txt. That is what
// lets a capture answer "what bytes does the game hold at this texture's
// address" (e.g. the Bc7 logo textures) offline. The copies run in the worker,
// not on the game thread; guest memory may have moved on by then, which is
// acceptable for the streamed/static assets this targets. Separate byte budget
// from the GPU readback staging: these are plain host file writes, no staging
// allocations are held.
//
// Two collection blind spots are covered on top of the in-frame bindings:
//  - Textures uploaded BEFORE the captured frame (e.g. a hub photo uploaded
//    while a menu loads, then only composited by later frames): OnDraw and
//    OnDispatch run only during a capture (their call sites gate on
//    FrameDump::Active()), so bindings alone can never see them. Instead the
//    texture cache reports every guest texture upload to a session record
//    (NoteTextureUpload), and FinishCapture merges the record into the dump.
//    Merged entries keep seq 0: no draw of the captured frame bound them.
//    The record lives for the whole session (a dump does NOT clear it) and
//    holds descriptors only, never bytes; the worker reads the guest bytes
//    at dump time exactly like for in-frame bindings.
//  - Late-frame starvation: collection order is draw order, so a byte budget
//    spent first-come on early multi-megabyte textures used to cut off the
//    small UI textures bound late (leaders-board logos at draws ~4500-6300).
//    Collection therefore takes every eligible descriptor; the worker
//    admits in-frame candidates SMALLEST FIRST, then session-record
//    candidates in first-upload order, under the byte/count budgets,
//    writing the rest as SKIPPED (over guesttex budget) index lines.
struct GuestTexReq {
	uint64_t    addr      = 0;
	uint64_t    size      = 0;
	uint32_t    seq       = 0; // first draw/dispatch that bound it
	uint32_t    guest_fmt = 0;
	std::string vk_fmt;
	uint32_t    w = 0, h = 0, d = 0;
	uint32_t    levels = 0, layers = 0;
	uint32_t    tile_mode = 0;
	uint32_t    upload_order = 0; // session record: order of the first upload at this address
};
std::vector<GuestTexReq>                g_guesttex;
std::set<std::pair<uint64_t, uint64_t>> g_seen_guesttex; // (address, size)
uint64_t                                g_guesttex_bytes = 0;
constexpr uint64_t kGuestTexMaxSize    = 64ull << 20;  // per texture
constexpr uint64_t kGuestTexMaxBytes   = 512ull << 20; // per capture (worker admission)
constexpr size_t   kGuestTexMaxCount   = 4096;         // files per capture (worker admission)
constexpr size_t   kGuestTexMaxCollect = 8192;         // candidate descriptors per capture

// Session record of guest texture uploads (descriptors only), keyed by guest
// address: a re-upload at the same address replaces the entry but keeps its
// first-upload order (GuestTexReq::upload_order), which the worker's
// admission uses to reach early uploads (e.g. a hub photo taken during a
// menu load) before the thousands of small uploads that follow them. Fed by
// NoteTextureUpload under its own mutex — the texture cache calls it with the
// cache lock held, so it must never wait on g_mutex. Once the record is full,
// existing addresses can still be replaced but new ones are dropped (logged
// once), so the record favours the textures seen first in the session.
std::map<uint64_t, GuestTexReq> g_guesttex_session;
std::mutex                      g_guesttex_session_mutex;
constexpr size_t                kGuestTexSessionMax       = 16384;
bool                            g_guesttex_session_full   = false; // under g_guesttex_session_mutex
uint32_t                        g_guesttex_session_uploads = 0;    // under g_guesttex_session_mutex

// ---- async download: finished captures are handed to a worker thread ----
// The flip thread records a capture and waits only for the GPU copies
// (FinishCapture); the worker thread does the per-pixel decode, the PNG
// writes, the frame.txt/buffers.txt assembly, and the done line.
struct CaptureJob {
	std::vector<Pending>    pending;    // staging buffers, moved out of g_pending
	std::vector<BufPending> bufpending; // moved out of g_bufpending
	std::vector<GuestTexReq> guesttex;  // moved out of g_guesttex (NHL27GUESTTEX)
	size_t                  guesttex_session = 0; // guesttex entries merged from the session record
	std::string             draws;      // moved out of g_log (draw/dispatch lines)
	std::string             buflog;     // moved out of g_buflog
	std::string             op_ring;    // KYTY_OPRING snapshot (TextureCache::DumpOpRingText)
	std::string             dir;        // copied from g_dir
	uint32_t                seq = 0;    // g_seq at handoff (draws/dispatches count)
	uint32_t                images = 0; // g_pending.size() at handoff
	size_t                  bufreq_leftover = 0; // g_bufreq.size() at handoff
};

std::queue<CaptureJob>  g_jobs;
std::mutex              g_job_mutex;
std::condition_variable g_job_cv;
std::thread             g_worker;
std::atomic<bool>       g_worker_stop {false};
std::atomic<bool>       g_worker_started {false};
std::atomic<bool>       g_worker_stopped {false}; // Shutdown() ran (double-call guard)

// KYTY_DBG_FRAME_DUMP_BUF_PS=<hex ps hash>[,...]|all selects the draws whose buffers/volumes are dumped.
// Default: the NHL 27 tiled deferred lighting pixel shaders.
bool WantBuffers(uint64_t ps_hash) {
	static const std::vector<uint64_t> list = [] {
		std::vector<uint64_t> v;
		const char*           e = std::getenv("KYTY_DBG_FRAME_DUMP_BUF_PS");
		std::string           s = (e != nullptr && *e != '\0') ? e : "1da1fd2871174330,68839c4d8fc9ada5";
		if (s == "all") {
			v.push_back(0);
			return v;
		}
		size_t pos = 0;
		while (pos < s.size()) {
			size_t n = s.find(',', pos);
			if (n == std::string::npos) n = s.size();
			v.push_back(std::strtoull(s.substr(pos, n - pos).c_str(), nullptr, 16));
			pos = n + 1;
		}
		return v;
	}();
	for (const auto h : list) {
		if (h == 0 || h == ps_hash) return true;
	}
	return false;
}

std::string WordsText(const uint8_t* p, size_t bytes, const char* indent = "      ") {
	std::string s;
	char        b[512];
	for (size_t o = 0; o + 4 <= bytes; o += 16) {
		std::string hex, fl;
		for (size_t k = 0; k < 4 && o + k * 4 + 4 <= bytes; k++) {
			uint32_t w;
			std::memcpy(&w, p + o + k * 4, 4);
			float f;
			std::memcpy(&f, &w, 4);
			char h[16], g[32];
			std::snprintf(h, sizeof(h), "%08x ", w);
			if (std::isfinite(f) && (f == 0.0f || (std::fabs(f) > 1e-20f && std::fabs(f) < 1e20f))) {
				std::snprintf(g, sizeof(g), "%g ", f);
			} else {
				std::snprintf(g, sizeof(g), "- ");
			}
			hex += h;
			fl += g;
		}
		std::snprintf(b, sizeof(b), "%s+%04zx: %s | %s\n", indent, o, hex.c_str(), fl.c_str());
		s += b;
	}
	return s;
}

// Dword lines with explicit indices, 8 per line: "<tag> <seq> ps=0x<hash> dwords: [i]=0x........".
void DwordLines(std::string& out, const char* tag, uint32_t seq, uint64_t ps_hash, const uint32_t* data,
                size_t count) {
	char b[96];
	for (size_t i = 0; i < count; i++) {
		if (i % 8 == 0) {
			std::snprintf(b, sizeof(b), "%s %05u ps=0x%016" PRIx64 " dwords:", tag, seq, ps_hash);
			out += b;
		}
		std::snprintf(b, sizeof(b), " [%zu]=0x%08x", i, data[i]);
		out += b;
		if (i % 8 == 7 || i + 1 == count) out += "\n";
	}
}

void QueueBuffer(vk::Buffer buf, uint64_t offset, uint64_t range, uint64_t addr, uint64_t guest_size,
                 uint32_t seq, const std::string& label) {
	if (!buf || range == 0 || range == VK_WHOLE_SIZE) return;
	char key[96];
	std::snprintf(key, sizeof(key), "%" PRIx64 ":%" PRIx64 ":%u", addr, guest_size, seq);
	if (!g_seen_buf.insert(key).second) {
		char b[160];
		std::snprintf(b, sizeof(b), "    (%s addr=0x%" PRIx64 " size=%" PRIu64 " already dumped for an earlier draw)\n",
		              label.c_str(), addr, guest_size);
		g_buflog += b;
		return;
	}
	BufReq r;
	r.buf    = buf;
	r.offset = offset;
	r.size   = std::min<uint64_t>(range, (4ull << 20));
	r.seq    = seq;
	r.label  = label;
	r.addr   = addr;
	g_bufreq.push_back(std::move(r));
}

// Logs the shader-visible resource tables of one draw and queues GPU copies of the bound buffers.
void CaptureBindings(const PreparedBindings& b, uint32_t seq, uint64_t ps_hash,
                     bool constants_only = false) {
	char line[512];
	std::snprintf(line, sizeof(line), "== #%05u ps=0x%016" PRIx64 " ==\n", seq, ps_hash);
	g_buflog += line;
	if (b.runtime != nullptr && b.runtime->resources != nullptr) {
		const auto& r = *b.runtime->resources;
		g_buflog += "  user_data (SGPR/user-data dwords, " + std::to_string(r.user_data.size()) + "):\n";
		g_buflog += WordsText(reinterpret_cast<const uint8_t*>(r.user_data.data()), r.user_data.size() * 4);
		g_buflog += "  snapshot buffer V# descriptors: " + std::to_string(r.buffers.size()) + "\n";
		for (size_t i = 0; i < r.buffers.size(); i++) {
			std::snprintf(line, sizeof(line), "    V#[%zu]:", i);
			g_buflog += line;
			for (uint32_t k = 0; k < r.buffers[i].dword_count; k++) {
				std::snprintf(line, sizeof(line), " %08x", r.buffers[i].dwords[k]);
				g_buflog += line;
			}
			g_buflog += "\n";
		}
		g_buflog += "  flattened_srt (" + std::to_string(r.flattened_srt.size()) + " dwords, first 64):\n";
		g_buflog += WordsText(reinterpret_cast<const uint8_t*>(r.flattened_srt.data()),
		                      std::min<size_t>(r.flattened_srt.size(), 64) * 4);
		g_buflog += "  specialization_reads (scalar-read guest ranges, CPU view of guest memory now): " +
		            std::to_string(r.specialization_reads.size()) + "\n";
		for (const auto& [addr, size] : r.specialization_reads) {
			std::snprintf(line, sizeof(line), "    addr=0x%" PRIx64 " size=%" PRIu64 "\n", addr, size);
			g_buflog += line;
			if (!constants_only && addr != 0 && size > 0 && size <= 4096) {
				g_buflog += WordsText(reinterpret_cast<const uint8_t*>(addr),
				                      static_cast<size_t>(std::min<uint64_t>(size, 256)));
			}
		}
	}
	// Explicit dword dumps for pixel-shader draws (lighting investigation). Dword index = [i].
	if (b.runtime != nullptr && b.runtime->resources != nullptr && b.runtime->program &&
	    b.runtime->program->stage == ShaderType::Pixel) {
		const auto& prog = *b.runtime->program;
		const auto& r    = *b.runtime->resources;
		// USERDATA: [i] is user-data register user_data_base + i.
		std::snprintf(line, sizeof(line), "USERDATA %05u ps=0x%016" PRIx64 " first_reg=%u count=%zu\n", seq, ps_hash,
		              prog.user_data_base, static_cast<size_t>(r.user_data.size()));
		g_buflog += line;
		DwordLines(g_buflog, "USERDATA", seq, ps_hash, r.user_data.data(), r.user_data.size());
		// SRT: the flattened_srt buffer bound at binding NativeBinding(stage, FlattenedSrt) (= 109 for PS).
		const uint32_t srt_binding = ShaderRecompiler::IR::NativeBinding(
		    prog.stage, ShaderRecompiler::IR::DescriptorBindingKind::FlattenedSrt);
		const bool srt_bound = prog.bindings.descriptor_counts[static_cast<size_t>(
		                           ShaderRecompiler::IR::DescriptorBindingKind::FlattenedSrt)] != 0;
		if (!srt_bound) {
			std::snprintf(line, sizeof(line), "SRT %05u ps=0x%016" PRIx64 " binding=%u not bound\n", seq, ps_hash,
			              srt_binding);
			g_buflog += line;
		} else {
			const uint64_t bytes = static_cast<uint64_t>(r.flattened_srt.size()) * 4u;
			std::string    fname = "(write failed)";
			char           path[640];
			std::snprintf(path, sizeof(path), "%s/srt_%05u_%016" PRIx64 "_b%u.bin", g_dir.c_str(), seq, ps_hash,
			              srt_binding);
			if (FILE* f = std::fopen(path, "wb")) {
				std::fwrite(r.flattened_srt.data(), 1, static_cast<size_t>(bytes), f);
				std::fclose(f);
				fname = std::filesystem::path(path).filename().string();
			}
			std::snprintf(line, sizeof(line), "SRT %05u ps=0x%016" PRIx64 " binding=%u size=%" PRIu64 " -> %s\n", seq,
			              ps_hash, srt_binding, bytes, fname.c_str());
			g_buflog += line;
			DwordLines(g_buflog, "SRT", seq, ps_hash, r.flattened_srt.data(),
			           std::min<size_t>(r.flattened_srt.size(), 1024));
		}
		// PUSH: the 32-dword push-constant block committed for this draw (SPIR-V access chain index = [i]).
		if (b.push_dwords_valid) {
			DwordLines(g_buflog, "PUSH", seq, ps_hash, b.push_dwords.data(), b.push_dwords.size());
		} else {
			std::snprintf(line, sizeof(line), "PUSH %05u ps=0x%016" PRIx64 " none (no push-constant block for this draw)\n",
			              seq, ps_hash);
			g_buflog += line;
		}
	}
	g_buflog += "  shader_data (" + std::to_string(b.shader_data.size()) + " dwords):\n";
	g_buflog += WordsText(reinterpret_cast<const uint8_t*>(b.shader_data.data()),
	                      std::min<size_t>(b.shader_data.size(), 64) * 4);
	g_buflog += "  bound storage buffers: " + std::to_string(b.buffers.size()) + "\n";
	for (size_t i = 0; i < b.buffers.size(); i++) {
		const auto src = i < b.buffer_sources.size() ? b.buffer_sources[i] : PreparedBindings::BufferSource {};
		std::snprintf(line, sizeof(line), "    buf[%zu]: guest addr=0x%" PRIx64 " size=%" PRIu64
		              " host_offset=%" PRIu64 " host_range=%" PRIu64 "\n", i, src.address, src.size,
		              static_cast<uint64_t>(b.buffers[i].offset), static_cast<uint64_t>(b.buffers[i].range));
		g_buflog += line;
		if (src.address == 0) continue;
		// The opt-in compute survey copies only small GPU buffers, avoiding large
		// geometry/indirect allocations and protected guest-memory reads.
		if (constants_only && (src.size > 4096 || b.buffers[i].range > 4096)) continue;
		QueueBuffer(b.buffers[i].buffer, b.buffers[i].offset, b.buffers[i].range, src.address, src.size, seq,
		            "buf" + std::to_string(i));
	}
}

void RecordReadback(GraphicContext& gfx, const CommandRecorder& cmd, const Target& t, const char* label,
                    uint32_t seq);

// Records the GPU->host copies of the queued buffers and volumes. cmd must be outside a render pass.
void FlushRequests(GraphicContext& gfx, const CommandRecorder& cmd) {
	if (g_bufreq.empty() && g_volreq.empty()) return;
	using S = vk::PipelineStageFlagBits2;
	using A = vk::AccessFlagBits2;
	if (!g_bufreq.empty()) {
		vk::MemoryBarrier2 mb {};
		mb.srcStageMask  = S::eAllCommands;
		mb.srcAccessMask = A::eMemoryWrite;
		mb.dstStageMask  = S::eTransfer;
		mb.dstAccessMask = A::eTransferRead;
		vk::DependencyInfo dep {};
		dep.memoryBarrierCount = 1;
		dep.pMemoryBarriers    = &mb;
		cmd.pipelineBarrier2(dep);
	}
	for (auto& r : g_bufreq) {
		vk::BufferCreateInfo bi {};
		bi.size  = r.size;
		bi.usage = vk::BufferUsageFlagBits::eTransferDst;
		VmaAllocationCreateInfo ai {};
		ai.usage = VMA_MEMORY_USAGE_AUTO;
		ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		VkBuffer          native = VK_NULL_HANDLE;
		VmaAllocation     alloc  = nullptr;
		VmaAllocationInfo info {};
		if (vmaCreateBuffer(gfx.allocator, reinterpret_cast<const VkBufferCreateInfo*>(&bi), &ai, &native,
		                    &alloc, &info) != VK_SUCCESS) {
			g_buflog += "  buffer readback allocation failed\n";
			continue;
		}
		vk::BufferCopy region {r.offset, 0, r.size};
		cmd.copyBuffer(r.buf, vk::Buffer(native), 1, &region);
		BufPending p;
		p.buffer   = vk::Buffer(native);
		p.alloc    = alloc;
		p.mapped   = info.pMappedData;
		p.req      = r;
		p.graphics = &gfx;
		g_bufpending.push_back(std::move(p));
	}
	g_bufreq.clear();
	for (auto& v : g_volreq) {
		RecordReadback(gfx, cmd, v.target, "vol", v.seq);
	}
	g_volreq.clear();
}

std::string Hex(uint64_t v);

void ProcessBuffers(std::vector<BufPending>& bufpending, std::string& buflog,
                    const std::string& dir) {
	for (auto& p : bufpending) {
		char line[512];
		std::snprintf(line, sizeof(line), "== buffer %s of #%05u guest addr=0x%" PRIx64 " (dumped %" PRIu64 " bytes) ==\n",
		              p.req.label.c_str(), p.req.seq, p.req.addr, p.req.size);
		buflog += line;
		if (p.mapped != nullptr) {
			vmaInvalidateAllocation(p.graphics->allocator, p.alloc, 0, VK_WHOLE_SIZE);
			const auto* d = static_cast<const uint8_t*>(p.mapped);
			buflog += WordsText(d, static_cast<size_t>(std::min<uint64_t>(p.req.size, 256)));
			{
				size_t nz = 0;
				const size_t nd = static_cast<size_t>(p.req.size / 4);
				for (size_t k = 0; k < nd; k++) {
					uint32_t w;
					std::memcpy(&w, d + k * 4, 4);
					nz += w != 0;
				}
				buflog += "    nonzero dwords: " + std::to_string(nz) + " / " + std::to_string(nd) + "\n";
			}
			if (p.req.size <=(4ull << 20)) {
				std::snprintf(line, sizeof(line), "%s/buf_%05u_%s_%s.bin", dir.c_str(), p.req.seq,
				              p.req.label.c_str(), Hex(p.req.addr).c_str());
				if (FILE* f = std::fopen(line, "wb")) {
					std::fwrite(d, 1, static_cast<size_t>(p.req.size), f);
					std::fclose(f);
					buflog += std::string("    full dump -> ") + std::filesystem::path(line).filename().string() + "\n";
				}
			} else {
				buflog += "    (>=64KB: only the first 64KB copied, first 256 bytes shown)\n";
			}
		}
		vmaDestroyBuffer(p.graphics->allocator, static_cast<VkBuffer>(p.buffer), p.alloc);
	}
	bufpending.clear();
}

const char* TypeName(Prospero::ImageType t) {
	switch (t) {
		case Prospero::ImageType::kColor2D: return "2D";
		case Prospero::ImageType::kColor2DArray: return "2DArray";
		case Prospero::ImageType::kCube: return "Cube";
		case Prospero::ImageType::kColor3D: return "3D";
		case Prospero::ImageType::kColor1D: return "1D";
		case Prospero::ImageType::kColor2DMsaa: return "2DMsaa";
		default: return "other";
	}
}

bool HasStencil(vk::Format f) {
	return f == vk::Format::eD16UnormS8Uint || f == vk::Format::eD24UnormS8Uint ||
	       f == vk::Format::eD32SfloatS8Uint;
}

bool IsDepthFormat(vk::Format f) {
	return f == vk::Format::eD16Unorm || f == vk::Format::eD32Sfloat ||
	       f == vk::Format::eX8D24UnormPack32 || HasStencil(f);
}

uint32_t BytesPerPixel(vk::Format f) {
	switch (f) {
		case vk::Format::eR8Unorm: case vk::Format::eR8Uint: case vk::Format::eR8Snorm:
		case vk::Format::eR8Sint: return 1;
		case vk::Format::eR8G8Unorm: case vk::Format::eR8G8Uint: case vk::Format::eR8G8Snorm:
		case vk::Format::eR8G8Sint:
		case vk::Format::eR16Unorm: case vk::Format::eR16Sfloat: case vk::Format::eR16Uint:
		case vk::Format::eR16Snorm: case vk::Format::eR16Sint:
		case vk::Format::eR5G6B5UnormPack16: case vk::Format::eD16Unorm:
		case vk::Format::eD16UnormS8Uint: return 2;
		case vk::Format::eR8G8B8A8Unorm: case vk::Format::eR8G8B8A8Srgb:
		case vk::Format::eR8G8B8A8Snorm: case vk::Format::eR8G8B8A8Uint:
		case vk::Format::eR8G8B8A8Sint:
		case vk::Format::eB8G8R8A8Unorm: case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eA2B10G10R10UnormPack32: case vk::Format::eA2R10G10B10UnormPack32:
		case vk::Format::eA2B10G10R10UintPack32: case vk::Format::eB10G11R11UfloatPack32:
		case vk::Format::eR16G16Unorm: case vk::Format::eR16G16Sfloat: case vk::Format::eR16G16Uint:
		case vk::Format::eR32Sfloat: case vk::Format::eR32Uint: case vk::Format::eR32Sint:
		case vk::Format::eD32Sfloat: case vk::Format::eX8D24UnormPack32:
		case vk::Format::eD24UnormS8Uint: case vk::Format::eD32SfloatS8Uint: return 4;
		case vk::Format::eR16G16B16A16Sfloat: case vk::Format::eR16G16B16A16Unorm:
		case vk::Format::eR16G16B16A16Uint: case vk::Format::eR16G16B16A16Snorm:
		case vk::Format::eR16G16B16A16Sint:
		case vk::Format::eR32G32Sfloat: case vk::Format::eR32G32Uint: case vk::Format::eR32G32Sint:
		case vk::Format::eR64Uint: case vk::Format::eR64Sint: case vk::Format::eR64Sfloat: return 8;
		case vk::Format::eR32G32B32Sfloat: case vk::Format::eR32G32B32Uint:
		case vk::Format::eR32G32B32Sint: return 12;
		case vk::Format::eR32G32B32A32Sfloat: case vk::Format::eR32G32B32A32Uint:
		case vk::Format::eR32G32B32A32Sint: return 16;
		default: return 0;
	}
}

float HalfToFloat(uint16_t h) {
	const uint32_t s = (h >> 15u) & 1u, e = (h >> 10u) & 31u, m = h & 1023u;
	float v;
	if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
	else if (e == 31) v = m ? 0.0f : 65504.0f;
	else v = std::ldexp(static_cast<float>(m | 1024u), static_cast<int>(e) - 25);
	return s ? -v : v;
}

float SmallFloat(uint32_t v, uint32_t mant_bits) {
	const uint32_t e = v >> mant_bits, m = v & ((1u << mant_bits) - 1u);
	if (e == 0) return std::ldexp(static_cast<float>(m), -14 - static_cast<int>(mant_bits));
	if (e == 31) return m ? 0.0f : 65504.0f;
	return std::ldexp(1.0f + static_cast<float>(m) / static_cast<float>(1u << mant_bits),
	                  static_cast<int>(e) - 15);
}

enum class Kind { Ldr, Hdr, Int, Depth };

struct Decoded {
	uint32_t           w = 0, h = 0;
	int                channels = 4;
	bool               has_alpha = false;
	Kind               kind = Kind::Ldr;
	std::vector<float> px; // 4 floats per pixel
	bool               ok = false;
};

template <typename T> T Load(const uint8_t* p) {
	T v;
	std::memcpy(&v, p, sizeof(T));
	return v;
}

Decoded Decode(const Target& t, const uint8_t* src) {
	Decoded d;
	d.w = t.w;
	d.h = t.h;
	const size_t n   = static_cast<size_t>(t.w) * t.h;
	const uint32_t bpp = BytesPerPixel(t.format);
	d.px.assign(n * 4, 0.0f);
	d.ok = true;
	const auto f = t.format;
	if (t.depth) {
		d.kind = Kind::Depth;
		d.channels = 1;
		for (size_t i = 0; i < n; i++) {
			const uint8_t* p = src + i * bpp;
			float v = 0;
			if (f == vk::Format::eD16Unorm || f == vk::Format::eD16UnormS8Uint)
				v = static_cast<float>(Load<uint16_t>(p)) / 65535.0f;
			else if (f == vk::Format::eD32Sfloat || f == vk::Format::eD32SfloatS8Uint)
				v = Load<float>(p);
			else
				v = static_cast<float>(Load<uint32_t>(p) & 0xFFFFFFu) / 16777215.0f;
			d.px[i * 4] = std::isfinite(v) ? v : 0.0f;
		}
		return d;
	}
	const auto set = [&](size_t i, float r, float g, float b, float a) {
		d.px[i * 4 + 0] = r; d.px[i * 4 + 1] = g; d.px[i * 4 + 2] = b; d.px[i * 4 + 3] = a;
	};
	switch (f) {
		case vk::Format::eR8G8B8A8Unorm: case vk::Format::eR8G8B8A8Srgb:
		case vk::Format::eB8G8R8A8Unorm: case vk::Format::eB8G8R8A8Srgb: {
			const bool bgra = f == vk::Format::eB8G8R8A8Unorm || f == vk::Format::eB8G8R8A8Srgb;
			d.has_alpha = true;
			for (size_t i = 0; i < n; i++) {
				const uint8_t* p = src + i * 4;
				set(i, p[bgra ? 2 : 0] / 255.f, p[1] / 255.f, p[bgra ? 0 : 2] / 255.f, p[3] / 255.f);
			}
			break;
		}
		case vk::Format::eR8G8B8A8Snorm:
			d.has_alpha = true;
			for (size_t i = 0; i < n; i++) {
				const int8_t* p = reinterpret_cast<const int8_t*>(src + i * 4);
				set(i, std::max(p[0] / 127.f, 0.f), std::max(p[1] / 127.f, 0.f),
				    std::max(p[2] / 127.f, 0.f), std::max(p[3] / 127.f, 0.f));
			}
			break;
		case vk::Format::eR8G8B8A8Uint:
			d.kind = Kind::Int;
			d.has_alpha = true;
			for (size_t i = 0; i < n; i++) {
				const uint8_t* p = src + i * 4;
				set(i, p[0], p[1], p[2], p[3]);
			}
			break;
		case vk::Format::eA2B10G10R10UnormPack32: case vk::Format::eA2B10G10R10UintPack32:
		case vk::Format::eA2R10G10B10UnormPack32: {
			const bool argb = f == vk::Format::eA2R10G10B10UnormPack32;
			const bool is_int = f == vk::Format::eA2B10G10R10UintPack32;
			if (is_int) d.kind = Kind::Int;
			d.has_alpha = true;
			const float s = is_int ? 1.0f : 1.0f / 1023.0f;
			for (size_t i = 0; i < n; i++) {
				const uint32_t v = Load<uint32_t>(src + i * 4);
				float c0 = static_cast<float>(v & 1023u) * s, c1 = static_cast<float>((v >> 10u) & 1023u) * s,
				      c2 = static_cast<float>((v >> 20u) & 1023u) * s;
				const float a = is_int ? static_cast<float>(v >> 30u) : static_cast<float>(v >> 30u) / 3.0f;
				if (argb) std::swap(c0, c2);
				set(i, c0, c1, c2, a);
			}
			break;
		}
		case vk::Format::eB10G11R11UfloatPack32:
			d.kind = Kind::Hdr;
			d.channels = 3;
			for (size_t i = 0; i < n; i++) {
				const uint32_t v = Load<uint32_t>(src + i * 4);
				set(i, SmallFloat(v & 2047u, 6), SmallFloat((v >> 11u) & 2047u, 6),
				    SmallFloat(v >> 22u, 5), 1.0f);
			}
			break;
		case vk::Format::eR16G16B16A16Sfloat:
			d.kind = Kind::Hdr;
			d.has_alpha = true;
			for (size_t i = 0; i < n; i++) {
				const uint8_t* p = src + i * 8;
				set(i, HalfToFloat(Load<uint16_t>(p)), HalfToFloat(Load<uint16_t>(p + 2)),
				    HalfToFloat(Load<uint16_t>(p + 4)), HalfToFloat(Load<uint16_t>(p + 6)));
			}
			break;
		case vk::Format::eR16G16B16A16Unorm:
			d.has_alpha = true;
			for (size_t i = 0; i < n; i++) {
				const uint8_t* p = src + i * 8;
				set(i, Load<uint16_t>(p) / 65535.f, Load<uint16_t>(p + 2) / 65535.f,
				    Load<uint16_t>(p + 4) / 65535.f, Load<uint16_t>(p + 6) / 65535.f);
			}
			break;
		case vk::Format::eR16G16B16A16Uint:
			d.kind = Kind::Int;
			d.has_alpha = true;
			for (size_t i = 0; i < n; i++) {
				const uint8_t* p = src + i * 8;
				set(i, Load<uint16_t>(p), Load<uint16_t>(p + 2), Load<uint16_t>(p + 4),
				    Load<uint16_t>(p + 6));
			}
			break;
		case vk::Format::eR32G32B32A32Sfloat:
			d.kind = Kind::Hdr;
			d.has_alpha = true;
			for (size_t i = 0; i < n; i++) {
				const uint8_t* p = src + i * 16;
				set(i, Load<float>(p), Load<float>(p + 4), Load<float>(p + 8), Load<float>(p + 12));
			}
			break;
		case vk::Format::eR32G32B32A32Uint:
			d.kind = Kind::Int;
			d.has_alpha = true;
			for (size_t i = 0; i < n; i++) {
				const uint8_t* p = src + i * 16;
				set(i, static_cast<float>(Load<uint32_t>(p)), static_cast<float>(Load<uint32_t>(p + 4)),
				    static_cast<float>(Load<uint32_t>(p + 8)), static_cast<float>(Load<uint32_t>(p + 12)));
			}
			break;
		case vk::Format::eR8Unorm: case vk::Format::eR8Uint:
			d.channels = 1;
			if (f == vk::Format::eR8Uint) d.kind = Kind::Int;
			for (size_t i = 0; i < n; i++) {
				const float v = f == vk::Format::eR8Uint ? src[i] : src[i] / 255.f;
				set(i, v, v, v, 1);
			}
			break;
		case vk::Format::eR8G8Unorm: case vk::Format::eR8G8Uint:
			d.channels = 2;
			if (f == vk::Format::eR8G8Uint) d.kind = Kind::Int;
			for (size_t i = 0; i < n; i++) {
				const float s = f == vk::Format::eR8G8Uint ? 1.f : 1.f / 255.f;
				set(i, src[i * 2] * s, src[i * 2 + 1] * s, 0, 1);
			}
			break;
		case vk::Format::eR16Unorm: case vk::Format::eR16Uint: case vk::Format::eR16Sfloat:
			d.channels = 1;
			if (f == vk::Format::eR16Uint) d.kind = Kind::Int;
			if (f == vk::Format::eR16Sfloat) d.kind = Kind::Hdr;
			for (size_t i = 0; i < n; i++) {
				const uint16_t u = Load<uint16_t>(src + i * 2);
				const float v = f == vk::Format::eR16Sfloat ? HalfToFloat(u)
				                : f == vk::Format::eR16Uint ? static_cast<float>(u) : u / 65535.f;
				set(i, v, v, v, 1);
			}
			break;
		case vk::Format::eR16G16Unorm: case vk::Format::eR16G16Uint: case vk::Format::eR16G16Sfloat:
			d.channels = 2;
			if (f == vk::Format::eR16G16Uint) d.kind = Kind::Int;
			if (f == vk::Format::eR16G16Sfloat) d.kind = Kind::Hdr;
			for (size_t i = 0; i < n; i++) {
				const uint16_t u0 = Load<uint16_t>(src + i * 4), u1 = Load<uint16_t>(src + i * 4 + 2);
				if (f == vk::Format::eR16G16Sfloat) set(i, HalfToFloat(u0), HalfToFloat(u1), 0, 1);
				else if (f == vk::Format::eR16G16Uint) set(i, u0, u1, 0, 1);
				else set(i, u0 / 65535.f, u1 / 65535.f, 0, 1);
			}
			break;
		case vk::Format::eR32Sfloat: case vk::Format::eR32Uint:
			d.channels = 1;
			d.kind = f == vk::Format::eR32Sfloat ? Kind::Hdr : Kind::Int;
			for (size_t i = 0; i < n; i++) {
				const float v = f == vk::Format::eR32Sfloat ? Load<float>(src + i * 4)
				                                              : static_cast<float>(Load<uint32_t>(src + i * 4));
				set(i, v, v, v, 1);
			}
			break;
		case vk::Format::eR32G32Sfloat: case vk::Format::eR32G32Uint:
			d.channels = 2;
			d.kind = f == vk::Format::eR32G32Sfloat ? Kind::Hdr : Kind::Int;
			for (size_t i = 0; i < n; i++) {
				if (f == vk::Format::eR32G32Sfloat)
					set(i, Load<float>(src + i * 8), Load<float>(src + i * 8 + 4), 0, 1);
				else
					set(i, static_cast<float>(Load<uint32_t>(src + i * 8)),
					    static_cast<float>(Load<uint32_t>(src + i * 8 + 4)), 0, 1);
			}
			break;
		default: d.ok = false; break;
	}
	for (auto& v : d.px) {
		if (!std::isfinite(v)) v = 0.0f;
	}
	return d;
}

uint8_t ToByte(float v) {
	return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

float Gamma(float v) {
	v = std::clamp(v, 0.0f, 1.0f);
	return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

bool WritePng(const std::string& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgb) {
	return stbi_write_png(path.c_str(), static_cast<int>(w), static_cast<int>(h), 3, rgb.data(),
	                      static_cast<int>(w * 3)) != 0;
}

// Maps decoded pixels through f(px) -> rgb and writes the png.
template <typename F>
bool WriteMapped(const std::string& path, const Decoded& d, F&& f) {
	std::vector<uint8_t> rgb(static_cast<size_t>(d.w) * d.h * 3);
	for (size_t i = 0; i < static_cast<size_t>(d.w) * d.h; i++) {
		float r, g, b;
		f(&d.px[i * 4], r, g, b);
		rgb[i * 3] = ToByte(r); rgb[i * 3 + 1] = ToByte(g); rgb[i * 3 + 2] = ToByte(b);
	}
	return WritePng(path, d.w, d.h, rgb);
}

std::string Hex(uint64_t v) {
	char b[32];
	std::snprintf(b, sizeof(b), "%" PRIx64, v);
	return b;
}

// Records one sampled binding for the guest-byte dump (see GuestTexReq).
// Called with g_mutex held. No byte budget here: admission under the budget
// happens smallest-first in the worker (ProcessGuestTex), so a big texture
// bound early cannot starve the small ones bound late.
void CollectGuestTex(uint32_t seq, const TextureBinding& b) {
	const auto& info = b.desc.info;
	if (b.desc.type != TextureCache::BindingType::Texture) return;
	const uint64_t addr = info.data.address;
	const uint64_t size = info.data.size;
	if (addr == 0 || size == 0 || size > kGuestTexMaxSize) return;
	if (!info.data.Valid()) return;
	if (g_guesttex.size() >= kGuestTexMaxCollect) return;
	if (!g_seen_guesttex.insert(std::make_pair(addr, size)).second) return;
	GuestTexReq r;
	r.addr      = addr;
	r.size      = size;
	r.seq       = seq;
	r.guest_fmt = static_cast<uint32_t>(info.guest_format);
	r.vk_fmt    = vk::to_string(info.pixel_format);
	r.w         = info.extent.width;
	r.h         = info.extent.height;
	r.d         = info.extent.depth;
	r.levels    = info.resources.levels;
	r.layers    = info.resources.layers;
	r.tile_mode = static_cast<uint32_t>(info.tile_mode);
	g_guesttex.push_back(std::move(r));
	g_guesttex_bytes += size;
}

// Worker side of the guest sampled-texture dump: reads the guest bytes for
// each admitted candidate and writes them as .bin files plus the guesttex.txt
// index. Runs on the download worker thread (ProcessJob).
void ProcessGuestTex(CaptureJob& job) {
	if (job.guesttex.empty()) return;
	// Admission under the byte/count budgets, in two classes. In-frame
	// candidates (seq != 0) go first, SMALLEST FIRST: collection order is
	// draw order, so first-come admission let early multi-megabyte textures
	// starve the small UI textures bound late in the frame. Session-record
	// candidates (seq == 0) follow in UPLOAD ORDER (first upload at their
	// address): the textures the record exists for were uploaded early —
	// a hub photo during a menu load — and thousands of later small uploads
	// would bury them under any size order. Candidates that do not fit are
	// still listed in the index as SKIPPED (over guesttex budget).
	std::vector<size_t> order;
	order.reserve(job.guesttex.size());
	for (size_t i = 0; i < job.guesttex.size(); i++) {
		if (job.guesttex[i].seq != 0) order.push_back(i);
	}
	std::sort(order.begin(), order.end(), [&job](size_t a, size_t b) {
		if (job.guesttex[a].size != job.guesttex[b].size) return job.guesttex[a].size < job.guesttex[b].size;
		return a < b;
	});
	std::vector<size_t> session;
	for (size_t i = 0; i < job.guesttex.size(); i++) {
		if (job.guesttex[i].seq == 0) session.push_back(i);
	}
	std::sort(session.begin(), session.end(), [&job](size_t a, size_t b) {
		if (job.guesttex[a].upload_order != job.guesttex[b].upload_order)
			return job.guesttex[a].upload_order < job.guesttex[b].upload_order;
		return a < b;
	});
	order.insert(order.end(), session.begin(), session.end());
	std::vector<bool> admit(job.guesttex.size(), false);
	uint64_t          total_bytes = 0;
	uint64_t          admit_bytes = 0;
	size_t            admit_count = 0;
	for (const size_t i : order) {
		const auto& r = job.guesttex[i];
		total_bytes += r.size;
		if (admit_count >= kGuestTexMaxCount || admit_bytes + r.size > kGuestTexMaxBytes) continue;
		admit[i] = true;
		admit_count++;
		admit_bytes += r.size;
	}
	const size_t   skipped_budget       = job.guesttex.size() - admit_count;
	const uint64_t skipped_budget_bytes = total_bytes - admit_bytes;
	std::string          index;
	uint32_t             written       = 0;
	uint32_t             skipped_read  = 0;
	uint32_t             skipped_write = 0;
	uint64_t             bytes         = 0;
	std::vector<uint8_t> data;
	for (size_t i = 0; i < job.guesttex.size(); i++) {
		const auto& r = job.guesttex[i];
		char line[768];
		if (!admit[i]) {
			std::snprintf(line, sizeof(line),
			              "NHL27GUESTTEX seq=%u addr=0x%s size=%" PRIu64 " SKIPPED (over guesttex budget)\n",
			              r.seq, Hex(r.addr).c_str(), r.size);
			index += line;
			continue;
		}
		char fname[320];
		std::snprintf(fname, sizeof(fname), "guesttex_%05u_%s_%ux%ux%u_g%u_%s.bin", r.seq,
		              Hex(r.addr).c_str(), r.w, r.h, r.d, r.guest_fmt, r.vk_fmt.c_str());
		data.assign(static_cast<size_t>(r.size), 0);
		if (!LibKernel::Memory::TryReadBacking(r.addr, data.data(), r.size)) {
			skipped_read++;
			std::snprintf(line, sizeof(line),
			              "NHL27GUESTTEX seq=%u addr=0x%s size=%" PRIu64 " SKIPPED (guest read failed)\n",
			              r.seq, Hex(r.addr).c_str(), r.size);
			index += line;
			continue;
		}
		const std::string path = job.dir + "/" + fname;
		if (FILE* f = std::fopen(path.c_str(), "wb")) {
			std::fwrite(data.data(), 1, data.size(), f);
			std::fclose(f);
			written++;
			bytes += r.size;
			std::snprintf(line, sizeof(line),
			              "NHL27GUESTTEX seq=%u addr=0x%s size=%" PRIu64 " guest_fmt=%u vk=%s %ux%ux%u "
			              "levels=%u layers=%u tile_mode=%u -> %s\n",
			              r.seq, Hex(r.addr).c_str(), r.size, r.guest_fmt, r.vk_fmt.c_str(), r.w, r.h, r.d,
			              r.levels, r.layers, r.tile_mode, fname);
		} else {
			skipped_write++;
			std::snprintf(line, sizeof(line),
			              "NHL27GUESTTEX seq=%u addr=0x%s size=%" PRIu64 " SKIPPED (file write failed)\n",
			              r.seq, Hex(r.addr).c_str(), r.size);
		}
		index += line;
	}
	if (FILE* f = std::fopen((job.dir + "/guesttex.txt").c_str(), "wb")) {
		std::fwrite(index.data(), 1, index.size(), f);
		std::fclose(f);
	}
	std::printf("NHL27GUESTTEX: %u/%zu sampled textures dumped (%" PRIu64 " bytes) -> %s/guesttex.txt\n",
	            written, job.guesttex.size(), bytes, job.dir.c_str());
	std::printf("NHL27GUESTTEX: candidates=%zu session=%zu recorded=%u (%" PRIu64 " bytes) "
	            "skipped_budget=%zu (%" PRIu64 " bytes) skipped_read=%u skipped_write=%u\n",
	            job.guesttex.size(), job.guesttex_session, written, bytes, skipped_budget,
	            skipped_budget_bytes, skipped_read, skipped_write);
	std::fflush(stdout);
}

// 3D volume: per-channel min/max/mean/nonzero over all voxels, a few voxel samples, z-slice PNGs.
void ProcessVolume(Pending& p, const std::string& dir, std::string& log, std::string& buflog) {
	const auto&    t   = p.target;
	const uint32_t bpp = BytesPerPixel(t.format);
	const size_t   sl  = static_cast<size_t>(t.w) * t.h * bpp;
	if (t.img3d && p.mapped != nullptr) {
		// Raw bytes of a 3D image bound to a BUF_PS draw (tightly packed, x fastest, then y, then z).
		const auto*   d  = static_cast<const uint8_t*>(p.mapped);
		const uint64_t nd = p.size / 4;
		uint64_t      nz = 0;
		for (uint64_t k = 0; k < nd; k++) {
			uint32_t w;
			std::memcpy(&w, d + k * 4, 4);
			nz += w != 0;
		}
		char path[640];
		std::snprintf(path, sizeof(path), "%s/img3d_%u_%u_%s.bin", dir.c_str(), p.seq, t.slot, Hex(t.addr).c_str());
		std::string fname = "(write failed)";
		if (FILE* f = std::fopen(path, "wb")) {
			std::fwrite(d, 1, static_cast<size_t>(p.size), f);
			std::fclose(f);
			fname = std::filesystem::path(path).filename().string();
		}
		char head[256];
		std::snprintf(head, sizeof(head), "IMG3D %u %u 0x%s %ux%ux%u %s nonzero_dwords=%" PRIu64 "/%" PRIu64 " first8:",
		              p.seq, t.slot, Hex(t.addr).c_str(), t.w, t.h, t.slices, vk::to_string(t.format).c_str(), nz, nd);
		std::string line = head;
		for (uint64_t k = 0; k < 8 && k < nd; k++) {
			uint32_t w;
			std::memcpy(&w, d + k * 4, 4);
			char wb[16];
			std::snprintf(wb, sizeof(wb), " %08x", w);
			line += wb;
		}
		buflog += line + " -> " + fname + "\n";
	}
	char base[512];
	std::snprintf(base, sizeof(base), "%s/%05u_%s_%s_%ux%ux%u_%s", dir.c_str(), p.seq, p.label.c_str(),
	              Hex(t.addr).c_str(), t.w, t.h, t.slices, vk::to_string(t.format).c_str());
	char line[1024];
	double   sum[4] = {0, 0, 0, 0};
	float    mn[4] = {3.4e38f, 3.4e38f, 3.4e38f, 3.4e38f}, mx[4] = {-3.4e38f, -3.4e38f, -3.4e38f, -3.4e38f};
	uint64_t nonzero = 0, total = 0;
	std::vector<float> lum;
	std::vector<Decoded> slices(t.slices);
	for (uint32_t z = 0; z < t.slices; z++) {
		slices[z] = Decode(t, static_cast<const uint8_t*>(p.mapped) + sl * z);
		if (!slices[z].ok) {
			std::snprintf(line, sizeof(line), "VOL seq=%u addr=0x%s %ux%ux%u fmt=%s UNSUPPORTED\n", p.seq,
			              Hex(t.addr).c_str(), t.w, t.h, t.slices, vk::to_string(t.format).c_str());
			log += line;
			return;
		}
		const auto& d = slices[z];
		for (size_t i = 0; i < static_cast<size_t>(t.w) * t.h; i++) {
			bool nz = false;
			for (int c = 0; c < 4; c++) {
				const float v = d.px[i * 4 + c];
				mn[c] = std::min(mn[c], v);
				mx[c] = std::max(mx[c], v);
				sum[c] += v;
				nz |= v != 0.0f;
			}
			nonzero += nz ? 1 : 0;
			total++;
			lum.push_back(0.2126f * d.px[i * 4] + 0.7152f * d.px[i * 4 + 1] + 0.0722f * d.px[i * 4 + 2]);
		}
	}
	std::string files;
	const auto  at = [&](uint32_t x, uint32_t y, uint32_t z, int c) {
        return slices[z].px[(static_cast<size_t>(y) * t.w + x) * 4 + c];
	};
	std::snprintf(line, sizeof(line),
	              "VOL seq=%u addr=0x%s %ux%ux%u fmt=%s voxels=%llu nonzero=%llu\n"
	              "    min  = %g %g %g %g\n    max  = %g %g %g %g\n    mean = %g %g %g %g\n",
	              p.seq, Hex(t.addr).c_str(), t.w, t.h, t.slices, vk::to_string(t.format).c_str(),
	              static_cast<unsigned long long>(total), static_cast<unsigned long long>(nonzero), mn[0], mn[1],
	              mn[2], mn[3], mx[0], mx[1], mx[2], mx[3], sum[0] / static_cast<double>(total),
	              sum[1] / static_cast<double>(total), sum[2] / static_cast<double>(total),
	              sum[3] / static_cast<double>(total));
	log += line;
	const uint32_t cx = t.w / 2, cy = t.h / 2, cz = t.slices / 2;
	const uint32_t probes[5][3] = {{0, 0, 0}, {cx, cy, cz}, {cx, cy, 0}, {t.w - 1, t.h - 1, t.slices - 1}, {cx / 2, cy / 2, cz / 2}};
	for (const auto& pr : probes) {
		std::snprintf(line, sizeof(line), "    voxel(%u,%u,%u) = %g %g %g %g\n", pr[0], pr[1], pr[2],
		              at(pr[0], pr[1], pr[2], 0), at(pr[0], pr[1], pr[2], 1), at(pr[0], pr[1], pr[2], 2),
		              at(pr[0], pr[1], pr[2], 3));
		log += line;
	}
	float scale = 1.0f;
	{
		const size_t k = std::min(lum.size() - 1, static_cast<size_t>(static_cast<double>(lum.size()) * 0.99));
		std::nth_element(lum.begin(), lum.begin() + static_cast<ptrdiff_t>(k), lum.end());
		scale = lum[k] > 1e-6f ? 1.0f / lum[k] : (mx[0] > 1e-6f ? 1.0f / mx[0] : 1.0f);
	}
	const uint32_t zs[5] = {0, t.slices / 4, t.slices / 2, t.slices * 3 / 4, t.slices - 1};
	for (const uint32_t z : zs) {
		const std::string f = std::string(base) + "_z" + std::to_string(z) + "_exp.png";
		WriteMapped(f, slices[z], [&](const float* px, float& r, float& g, float& bl) {
			r = Gamma(px[0] * scale); g = Gamma(px[1] * scale); bl = Gamma(px[2] * scale);
		});
		files += " " + std::filesystem::path(f).filename().string();
	}
	log += "    slices (auto-exposed, p99 lum scale " + std::to_string(scale) + "):" + files + "\n";
}

void ProcessOne(Pending& p, const std::string& dir, std::string& log, std::string& buflog) {
	if (p.mapped == nullptr) return;
	vmaInvalidateAllocation(p.graphics->allocator, p.alloc, 0, VK_WHOLE_SIZE);
	const auto& t = p.target;
	static const bool raw_enabled = [] {
		const char* value = std::getenv("KYTY_DBG_FRAME_DUMP_RAW");
		return value != nullptr && value[0] == '1';
	}();
	if (raw_enabled) {
		char path[640];
		std::snprintf(path, sizeof(path), "%s/%05u_%s_%s_%ux%ux%u_%s.raw", dir.c_str(),
		    p.seq, p.label.c_str(), Hex(t.addr).c_str(), t.w, t.h, t.slices,
		    vk::to_string(t.format).c_str());
		if (FILE* output = std::fopen(path, "wb")) {
			const auto written = std::fwrite(p.mapped, 1, static_cast<size_t>(p.size), output);
			std::fclose(output);
			log += "RAW seq=" + std::to_string(p.seq) + " bytes=" + std::to_string(written) +
			    " expected=" + std::to_string(p.size) + " mip=" + std::to_string(t.mip) +
			    " layer=" + std::to_string(t.layer) + " -> " +
			    std::filesystem::path(path).filename().string() + "\n";
		} else {
			log += "RAW seq=" + std::to_string(p.seq) + " file_open_failed\n";
		}
	}
	if (t.slices > 1) {
		ProcessVolume(p, dir, log, buflog);
		return;
	}	Decoded d = Decode(t, static_cast<const uint8_t*>(p.mapped));
	char base[512];
	std::snprintf(base, sizeof(base), "%s/%05u_%s_%s_%ux%u_%s", dir.c_str(), p.seq, p.label.c_str(),
	              Hex(t.addr).c_str(), t.w, t.h, vk::to_string(t.format).c_str());
	const std::string b = base;
	char line[1024];
	// NHL27FRAMEDUMP label fix: when a flip-buffer target was relabelled, the
	// IMG lines below carry the registered format in fmt= and this note keeps
	// the cache creation format on record.
	const std::string relabel =
	    t.relabelled ? " relabelled: cache image created as " + vk::to_string(t.created_format) +
	                       ", decoded with the registered video-out format"
	                 : "";
	if (!d.ok) {
		// No PNG decode for this format: keep the raw readback bytes instead of
		// discarding them, so the target's content survives for offline analysis.
		const std::string f      = b + ".bin";
		std::string       suffix = " (raw write failed)";
		if (FILE* out = std::fopen(f.c_str(), "wb")) {
			if (std::fwrite(p.mapped, 1, static_cast<size_t>(p.size), out) ==
			    static_cast<size_t>(p.size)) {
				suffix = " -> " + std::filesystem::path(f).filename().string();
			}
			std::fclose(out);
		}
		std::snprintf(line, sizeof(line), "IMG seq=%u %s addr=0x%s %ux%u fmt=%s UNSUPPORTED (not decoded)%s%s\n",
		              p.seq, p.label.c_str(), Hex(t.addr).c_str(), t.w, t.h,
		              vk::to_string(t.format).c_str(), suffix.c_str(), relabel.c_str());
		log += line;
		return;
	}
	// Stats over the used channels.
	float mn = 3.4e38f, mx = -3.4e38f;
	const int stat_ch = d.kind == Kind::Depth ? 1 : std::min(d.channels, 3);
	const size_t n = static_cast<size_t>(d.w) * d.h;
	for (size_t i = 0; i < n; i++) {
		for (int c = 0; c < stat_ch; c++) {
			mn = std::min(mn, d.px[i * 4 + c]);
			mx = std::max(mx, d.px[i * 4 + c]);
		}
	}
	const float range = mx > mn ? mx - mn : 1.0f;
	std::string files;
	const auto note = [&](const std::string& f) {
		files += " ";
		files += std::filesystem::path(f).filename().string();
	};
	const int ch = d.channels;
	const auto chan = [ch](const float* px, float& r, float& g, float& bl, float sc, float off) {
		r = (px[0] - off) * sc;
		g = ch >= 2 ? (px[1] - off) * sc : r;
		bl = ch >= 3 ? (px[2] - off) * sc : (ch == 2 ? 0.0f : r);
	};
	if (d.kind == Kind::Depth) {
		const std::string f = b + ".png";
		WriteMapped(f, d, [&](const float* px, float& r, float& g, float& bl) {
			r = g = bl = (px[0] - mn) / range;
		});
		note(f);
	} else if (d.kind == Kind::Ldr) {
		const std::string f = b + ".png";
		WriteMapped(f, d, [&](const float* px, float& r, float& g, float& bl) { chan(px, r, g, bl, 1.f, 0.f); });
		note(f);
	} else if (d.kind == Kind::Hdr) {
		const std::string f = b + ".png";
		WriteMapped(f, d, [&](const float* px, float& r, float& g, float& bl) { chan(px, r, g, bl, 1.f, 0.f); });
		note(f);
		float scale = 1.0f;
		if (ch >= 3) {
			std::vector<float> lum(n);
			for (size_t i = 0; i < n; i++) {
				lum[i] = 0.2126f * d.px[i * 4] + 0.7152f * d.px[i * 4 + 1] + 0.0722f * d.px[i * 4 + 2];
			}
			const size_t k = std::min(n - 1, static_cast<size_t>(static_cast<double>(n) * 0.99));
			std::nth_element(lum.begin(), lum.begin() + static_cast<ptrdiff_t>(k), lum.end());
			scale = lum[k] > 1e-6f ? 1.0f / lum[k] : (mx > 1e-6f ? 1.0f / mx : 1.0f);
			const std::string fe = b + "_exp.png";
			WriteMapped(fe, d, [&](const float* px, float& r, float& g, float& bl) {
				r = Gamma(px[0] * scale); g = Gamma(px[1] * scale); bl = Gamma(px[2] * scale);
			});
			note(fe);
		} else {
			const std::string fe = b + "_norm.png";
			WriteMapped(fe, d, [&](const float* px, float& r, float& g, float& bl) {
				chan(px, r, g, bl, 1.0f / range, mn);
			});
			note(fe);
		}
	} else { // Int
		const std::string f = b + "_norm.png";
		WriteMapped(f, d, [&](const float* px, float& r, float& g, float& bl) {
			chan(px, r, g, bl, 1.0f / (mx > 0 ? mx : 1.0f), 0.f);
		});
		note(f);
	}
	if (d.has_alpha && d.kind != Kind::Depth) {
		bool varies = false;
		for (size_t i = 0; i < n && !varies; i++) varies = d.px[i * 4 + 3] < 0.999f;
		if (varies) {
			const std::string f = b + "_alpha.png";
			float amax = 0;
			for (size_t i = 0; i < n; i++) amax = std::max(amax, d.px[i * 4 + 3]);
			const float as = d.kind == Kind::Ldr ? 1.0f : (amax > 0 ? 1.0f / amax : 1.0f);
			WriteMapped(f, d, [&](const float* px, float& r, float& g, float& bl) {
				r = g = bl = px[3] * as;
			});
			note(f);
		}
	}
	if (d.kind == Kind::Int) {
		// Integer targets (material-ID buffers, lighting masks) lose their exact
		// values in the normalized PNG; keep the raw readback bytes alongside it
		// so individual pixel values stay readable offline.
		const std::string f = b + ".bin";
		if (FILE* out = std::fopen(f.c_str(), "wb")) {
			std::fwrite(p.mapped, 1, static_cast<size_t>(p.size), out);
			std::fclose(out);
			note(f);
		}
	}
	std::snprintf(line, sizeof(line), "IMG seq=%u %s addr=0x%s %ux%u fmt=%s min=%g max=%g ->%s%s\n", p.seq,
	              p.label.c_str(), Hex(t.addr).c_str(), t.w, t.h, vk::to_string(t.format).c_str(), mn, mx,
	              files.c_str(), relabel.c_str());
	log += line;
}

void BarrierImage(const CommandRecorder& cmd, const Target& t, vk::ImageLayout from, vk::ImageLayout to,
                  vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                  vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
	vk::ImageMemoryBarrier2 b {};
	b.srcStageMask                    = src_stage;
	b.srcAccessMask                   = src_access;
	b.dstStageMask                    = dst_stage;
	b.dstAccessMask                   = dst_access;
	b.oldLayout                       = from;
	b.newLayout                       = to;
	b.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	b.image                           = t.image;
	b.subresourceRange.aspectMask     = t.full_aspect;
	b.subresourceRange.baseMipLevel   = t.mip;
	b.subresourceRange.levelCount     = 1;
	b.subresourceRange.baseArrayLayer = t.layer;
	b.subresourceRange.layerCount     = 1;
	vk::DependencyInfo dep {};
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers    = &b;
	cmd.pipelineBarrier2(dep);
}

// Must be called with g_mutex held. Records the copy into cmd.
void RecordReadback(GraphicContext& gfx, const CommandRecorder& cmd, const Target& t, const char* label,
                    uint32_t seq) {
	char line[256];
	if (t.w < kMinSize || (t.h < kMinSize && !t.img3d)) return;
	// KYTY_DBG_FRAME_DUMP_FROM=<seq>: images of the draws and dispatches from that one on only (the
	// last passes of a frame, which the readback cap otherwise skips).
	static const uint32_t from = [] {
		const char* v = std::getenv("KYTY_DBG_FRAME_DUMP_FROM");
		return v != nullptr ? static_cast<uint32_t>(std::strtoul(v, nullptr, 0)) : 0u;
	}();
	if (seq < from) return;
	if (t.samples > 1) {
		std::snprintf(line, sizeof(line), "IMG seq=%u %s addr=0x%s %ux%u SKIPPED (multisampled)\n", seq,
		              label, Hex(t.addr).c_str(), t.w, t.h);
		g_log += line;
		return;
	}
	if (g_images >= kMaxImages) {
		if (g_images == kMaxImages) {
			g_log += "IMG cap reached (400 images), further images skipped\n";
			g_images++;
		}
		return;
	}
	const uint32_t bpp = BytesPerPixel(t.format);
	if (bpp == 0 || t.image == nullptr) {
		std::snprintf(line, sizeof(line), "IMG seq=%u %s addr=0x%s %ux%u fmt=%s SKIPPED (unsupported format)\n",
		              seq, label, Hex(t.addr).c_str(), t.w, t.h, vk::to_string(t.format).c_str());
		g_log += line;
		return;
	}
	const uint64_t size = static_cast<uint64_t>(t.w) * t.h * bpp * t.slices;
	using S = vk::PipelineStageFlagBits2;
	using A = vk::AccessFlagBits2;
	// State refresh: a target read back three or more times in one capture keeps only its FIRST
	// state and its LATEST state. The latest state's staging buffer is reused in place — the
	// copies are recorded in stream order, so the buffer ends up holding the newest state at no
	// extra pending bytes. Without this, a target written dozens of times in one frame (the
	// scene target, ~30 states) burns the whole readback budget and the frame's last passes —
	// the composition/UI stage — are all skipped.
	{
		uint32_t matches = 0;
		Pending* latest  = nullptr;
		for (auto& p : g_pending) {
			if (p.target.image == t.image && p.target.mip == t.mip && p.target.layer == t.layer &&
			    p.target.copy_aspect == t.copy_aspect && p.label == label) {
				if (++matches == 2) latest = &p;
			}
		}
		if (latest != nullptr && latest->size == size) {
			// The previous copy into this buffer must complete before the refresh copy
			// overwrites it (write-after-write within the same stream).
			vk::BufferMemoryBarrier2 bb {};
			bb.srcStageMask        = vk::PipelineStageFlagBits2::eTransfer;
			bb.srcAccessMask       = vk::AccessFlagBits2::eTransferWrite;
			bb.dstStageMask        = vk::PipelineStageFlagBits2::eTransfer;
			bb.dstAccessMask       = vk::AccessFlagBits2::eTransferWrite;
			bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			bb.buffer              = latest->buffer;
			bb.offset              = 0;
			bb.size                = VK_WHOLE_SIZE;
			vk::DependencyInfo dep {};
			dep.bufferMemoryBarrierCount = 1;
			dep.pBufferMemoryBarriers    = &bb;
			cmd.pipelineBarrier2(dep);
			BarrierImage(cmd, t, t.layout, vk::ImageLayout::eTransferSrcOptimal, S::eAllCommands,
			             A::eMemoryWrite, S::eTransfer, A::eTransferRead);
			vk::BufferImageCopy copy {};
			copy.imageSubresource = {t.copy_aspect, t.mip, t.layer, 1};
			copy.imageExtent      = vk::Extent3D {t.w, t.h, t.slices};
			cmd.copyImageToBuffer(t.image, vk::ImageLayout::eTransferSrcOptimal, latest->buffer, 1, &copy);
			BarrierImage(cmd, t, vk::ImageLayout::eTransferSrcOptimal, t.layout, S::eTransfer, A::eTransferRead,
			             S::eAllCommands, A::eMemoryRead | A::eMemoryWrite);
			std::snprintf(line, sizeof(line),
			              "IMG seq=%u %s addr=0x%s %ux%u REFRESH (latest state reuses the readback slot of seq=%u)\n",
			              seq, label, Hex(t.addr).c_str(), t.w, t.h, latest->seq);
			g_log += line;
			latest->seq    = seq;
			latest->target = t;
			return;
		}
	}
	if (g_pending_bytes + size > kMaxPendingBytes) {
		std::snprintf(line, sizeof(line), "IMG seq=%u %s addr=0x%s %ux%u SKIPPED (readback memory cap)\n", seq,
		              label, Hex(t.addr).c_str(), t.w, t.h);
		g_log += line;
		return;
	}
	vk::BufferCreateInfo bi {};
	bi.size  = size;
	bi.usage = vk::BufferUsageFlagBits::eTransferDst;
	VmaAllocationCreateInfo ai {};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VkBuffer          native = VK_NULL_HANDLE;
	VmaAllocation     alloc  = nullptr;
	VmaAllocationInfo info {};
	if (vmaCreateBuffer(gfx.allocator, reinterpret_cast<const VkBufferCreateInfo*>(&bi), &ai, &native,
	                    &alloc, &info) != VK_SUCCESS) {
		g_log += "IMG buffer allocation failed\n";
		return;
	}
	g_images++;
	g_pending_bytes += size;

	BarrierImage(cmd, t, t.layout, vk::ImageLayout::eTransferSrcOptimal, S::eAllCommands,
	             A::eMemoryWrite, S::eTransfer, A::eTransferRead);
	vk::BufferImageCopy copy {};
	copy.imageSubresource = {t.copy_aspect, t.mip, t.layer, 1};
	copy.imageExtent      = vk::Extent3D {t.w, t.h, t.slices};
	cmd.copyImageToBuffer(t.image, vk::ImageLayout::eTransferSrcOptimal, vk::Buffer(native), 1, &copy);
	BarrierImage(cmd, t, vk::ImageLayout::eTransferSrcOptimal, t.layout, S::eTransfer, A::eTransferRead,
	             S::eAllCommands, A::eMemoryRead | A::eMemoryWrite);

	Pending p;
	p.buffer   = vk::Buffer(native);
	p.alloc    = alloc;
	p.mapped   = info.pMappedData;
	p.size     = size;
	p.target   = t;
	p.seq      = seq;
	p.label    = label;
	p.graphics = &gfx;
	g_pending.push_back(std::move(p));
}

// ---- registered video-out formats (NHL27FRAMEDUMP label fix) ----
// PresentDiag::NoteFlip is fed the REGISTERED format of every flipped buffer
// (videoOut.cpp passes the registration group's ImageInfo.pixel_format), while
// a flip buffer's texture-cache image keeps the format of whichever lookup
// created it first: a guest-descriptor lookup derives A2B10G10R10 for the
// guest k10_10_10_2UNorm data, the registration derives A2R10G10B10 for this
// game. The guest bits always follow the registered layout — that is the
// layout the display path presents them with — so a readback labelled with
// the creation format decodes with red and blue exchanged. NoteFlip records
// the registered format per address here (via NoteRegisteredFlipFormat,
// defined below the anonymous namespace) and MakeTarget decodes flip-buffer
// targets with it. Labelling only: the readback bytes, the copy and every
// rendering decision are unchanged. Bounded table, the same trade-off as the
// present diagnostic's own tables; a never-flipped or evicted address simply
// keeps its creation-format label, which is the pre-fix behaviour.
struct FlipFormatSeen {
	uint64_t   addr   = 0;
	vk::Format format = vk::Format::eUndefined;
};
constexpr size_t kFlipFormatSeenCount = 16;
std::array<FlipFormatSeen, kFlipFormatSeenCount> g_flip_formats {};
std::mutex                                       g_flip_formats_mutex;

vk::Format RegisteredFlipFormat(uint64_t addr) {
	std::lock_guard lock(g_flip_formats_mutex);
	for (const auto& e : g_flip_formats) {
		if (e.addr == addr) return e.format;
	}
	return vk::Format::eUndefined;
}

// True when a and b are the same packed family in opposite channel order —
// the only way a cache creation format and a registered video-out format can
// legitimately disagree about the same guest bits (the VIDEO_OUT_FORMAT_
// POLICIES table splits exactly these families by channel order). Anything
// else (different family, different packing, float formats) is not a label
// ambiguity and is never relabelled.
bool FlipFormatChannelOrderPair(vk::Format a, vk::Format b) {
	const auto family = [](vk::Format f) {
		switch (f) {
			case vk::Format::eR8G8B8A8Unorm: case vk::Format::eR8G8B8A8Srgb:
			case vk::Format::eB8G8R8A8Unorm: case vk::Format::eB8G8R8A8Srgb: return 1;
			case vk::Format::eA2B10G10R10UnormPack32:
			case vk::Format::eA2R10G10B10UnormPack32: return 2;
			default: return 0;
		}
	};
	const auto b_first = [](vk::Format f) {
		return f == vk::Format::eB8G8R8A8Unorm || f == vk::Format::eB8G8R8A8Srgb ||
		       f == vk::Format::eA2B10G10R10UnormPack32;
	};
	const int fa = family(a), fb = family(b);
	return fa != 0 && fa == fb && b_first(a) != b_first(b);
}

bool MakeTarget(RenderContext& ctx, ImageId id, const ImageViewInfo& view, vk::ImageLayout layout,
                uint64_t addr, Target& out) {
	if (!id) return false;
	auto& image = ctx.GetTextureCache().GetImage(id);
	if (image.backing.image == nullptr) return false;
	out.image   = image.backing.image;
	out.format  = image.backing.format;
	out.depth   = IsDepthFormat(out.format);
	out.full_aspect = out.depth ? (HasStencil(out.format) ? vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil
	                                                      : vk::ImageAspectFlagBits::eDepth)
	                            : vk::ImageAspectFlagBits::eColor;
	out.copy_aspect = out.depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor;
	out.mip     = view.base_level;
	out.layer   = image.info.IsVolume() ? 0 : view.base_layer;
	out.w       = std::max(image.backing.extent.width >> out.mip, 1u);
	out.h       = std::max(image.backing.extent.height >> out.mip, 1u);
	out.samples = image.backing.samples;
	out.slices  = image.info.IsVolume() ? std::max(image.backing.extent.depth >> out.mip, 1u) : 1u;
	out.layout  = layout;
	out.addr    = addr;
	// NHL27FRAMEDUMP label fix: for a video-out flip buffer, decode the
	// readback with the REGISTERED format, not the cache image's creation
	// format (see the table above). Every format-derived field above was
	// computed from the creation format; the channel-order pairs this fires
	// on are all non-depth colour formats with identical byte size and
	// aspects, so those fields are the same under either format — only the
	// decode label (Target.format) changes.
	if (addr != 0) {
		const vk::Format registered = RegisteredFlipFormat(addr);
		if (registered != vk::Format::eUndefined && registered != out.format &&
		    FlipFormatChannelOrderPair(out.format, registered)) {
			out.created_format = out.format;
			out.format         = registered;
			out.relabelled     = true;
		}
	}
	return true;
}

std::string DescribeImage(const TextureCache::ImageDesc& d) {
	char b[256];
	std::snprintf(b, sizeof(b), "addr=0x%" PRIx64 " %ux%ux%u guest_fmt=%u vk=%s type=%s mip0=%u layers=%u",
	              d.info.data.address, d.info.extent.width, d.info.extent.height, d.info.extent.depth,
	              static_cast<unsigned>(d.info.guest_format), vk::to_string(d.info.pixel_format).c_str(),
	              TypeName(d.info.type), d.view_info.base_level, d.info.resources.layers);
	return b;
}

void AppendImages(std::string& s, const char* tag, const std::vector<TextureBinding>* images) {
	if (images == nullptr) return;
	for (size_t i = 0; i < images->size(); i++) {
		const auto& im = (*images)[i];
		s += "    ";
		s += tag;
		s += "[" + std::to_string(i) + "]: " + DescribeImage(im.desc) +
		     (im.desc.type == TextureCache::BindingType::Storage ? " STORAGE" : "") + "\n";
	}
}

void BeginCapture(CommandBuffer& buffer) {
	buffer.EndRendering();
	std::error_code ec;
	const char* root_env = std::getenv("KYTY_DBG_FRAME_DUMP_DIR");
	const std::string root = (root_env != nullptr && *root_env != '\0') ? root_env : "F:/emuf/captures";
	for (;; g_frame++) {
		const auto p = std::filesystem::path(root) / ("frame_" + std::to_string(g_frame));
		if (!std::filesystem::exists(p, ec)) {
			g_dir = p.generic_string();
			break;
		}
	}
	std::filesystem::create_directories(g_dir, ec);
	if (ec || !std::filesystem::is_directory(g_dir)) {
		std::printf("NHL27FRAMEDUMP: cannot create %s: %s (%d)\n", g_dir.c_str(), ec.message().c_str(),
		            ec.value());
	}
	g_targets.clear();
	g_pending.clear();
	g_log.clear();
	g_seq = 0;
	g_images = 0;
	g_pending_bytes = 0;
	g_bufreq.clear();
	g_volreq.clear();
	g_bufpending.clear();
	g_buflog.clear();
	g_seen_vol.clear();
	g_seen_buf.clear();
	g_guesttex.clear();
	g_seen_guesttex.clear();
	g_guesttex_bytes = 0;
	g_capturing = true;
	g_active.store(true, std::memory_order_relaxed);
	std::printf("NHL27FRAMEDUMP: capturing frame -> %s\n", g_dir.c_str());
	std::fflush(stdout);
}

void ProcessJob(CaptureJob& job) {
	std::string log; // IMG lines, built here (was g_log in FinishCapture)
	for (size_t i = 0; i < job.pending.size(); i++) {
		if (g_worker_stop.load()) {
			// Game is exiting (Shutdown() was called): abandon this capture instead of
			// draining the queue, so the join in Shutdown() waits at most one image,
			// not the whole download. Destroy the staging buffers that were never
			// downloaded, and write no frame.txt for this job.
			for (size_t k = i; k < job.pending.size(); k++) {
				auto& p = job.pending[k];
				vmaDestroyBuffer(p.graphics->allocator, static_cast<VkBuffer>(p.buffer), p.alloc);
			}
			for (auto& b : job.bufpending) {
				vmaDestroyBuffer(b.graphics->allocator, static_cast<VkBuffer>(b.buffer), b.alloc);
			}
			job.pending.clear();
			job.bufpending.clear();
			std::printf("NHL27FRAMEDUMP: download abandoned (game exiting) -> %s\n", job.dir.c_str());
			std::fflush(stdout);
			return;
		}
		auto& p = job.pending[i];
		ProcessOne(p, job.dir, log, job.buflog);
		vmaDestroyBuffer(p.graphics->allocator, static_cast<VkBuffer>(p.buffer), p.alloc);
	}
	job.pending.clear();
	ProcessBuffers(job.bufpending, job.buflog, job.dir);
	ProcessGuestTex(job);
	if (job.bufreq_leftover != 0) {
		job.buflog += "NOTE: " + std::to_string(job.bufreq_leftover) +
		              " buffer requests were never flushed\n";
	}
	if (!job.buflog.empty()) {
		if (FILE* f = std::fopen((job.dir + "/buffers.txt").c_str(), "wb")) {
			std::fwrite(job.buflog.data(), 1, job.buflog.size(), f);
			std::fclose(f);
		}
	}
	if (!job.op_ring.empty()) {
		if (FILE* f = std::fopen((job.dir + "/op_ring.txt").c_str(), "wb")) {
			std::fwrite(job.op_ring.data(), 1, job.op_ring.size(), f);
			std::fclose(f);
		}
	}
	std::string out = "# KytyPS5 frame dump (KYTY_DBG_FRAME_DUMP). Sequence = draw/dispatch order in the frame.\n"
	                  "# Image files are named <seq>_<rt<slot>|depth|st<slot>>_<guest addr>_<WxH>_<vk format>.png\n"
	                  "# HDR formats: <name>.png = clamped 0..1 (linear), <name>_exp.png = auto-exposed (p99 luminance) + sRGB.\n"
	                  "# Flip-buffer targets are decoded and named with the registered video-out format; when that differs\n"
	                  "# from the cache image's creation format, the IMG line notes the creation format (\"relabelled\").\n\n";
	out += job.draws;
	out += "\n# ---- images ----\n";
	out += log;
	if (FILE* f = std::fopen((job.dir + "/frame.txt").c_str(), "wb")) {
		std::fwrite(out.data(), 1, out.size(), f);
		std::fclose(f);
	} else {
		std::printf("NHL27FRAMEDUMP: cannot write %s/frame.txt: errno %d\n", job.dir.c_str(), errno);
	}
	std::printf("NHL27FRAMEDUMP: done, %u draws/dispatches, %u images -> %s\n", job.seq, job.images,
	            job.dir.c_str());
	std::fflush(stdout);
}

void WorkerLoop() {
	for (;;) {
		CaptureJob job;
		{
			std::unique_lock lock(g_job_mutex);
			g_job_cv.wait(lock, [] { return !g_jobs.empty() || g_worker_stop.load(); });
			if (g_jobs.empty()) return; // stop requested and queue drained
			// On stop, ProcessJob aborts immediately (destroys the staging buffers,
			// writes nothing), so the remaining queued jobs drain in milliseconds.
			job = std::move(g_jobs.front());
			g_jobs.pop();
		}
		ProcessJob(job);
	}
}

void EnsureWorker() {
	// Called with g_mutex held (from FinishCapture via OnGuestFlip), so two
	// simultaneous handoffs cannot start two workers.
	if (g_worker_started.exchange(true)) return;
	g_worker = std::thread(WorkerLoop);
}

void FinishCapture(CommandBuffer& buffer) {
	buffer.GetContext().GetCommandScheduler().FlushAndWait();
	g_active.store(false, std::memory_order_relaxed);
	g_capturing = false;
	CaptureJob job;
	job.pending = std::move(g_pending);
	job.bufpending = std::move(g_bufpending);
	job.guesttex = std::move(g_guesttex);
	// Merge the session record of guest texture uploads (NoteTextureUpload):
	// textures uploaded before this frame (e.g. during a menu load) that no
	// draw of this frame bound. In-frame entries win; merged entries keep
	// seq 0. The record itself is NOT cleared: it covers the whole session.
	{
		std::set<uint64_t> have;
		for (const auto& r : job.guesttex) have.insert(r.addr);
		std::lock_guard session_lock(g_guesttex_session_mutex);
		for (const auto& [addr, r] : g_guesttex_session) {
			if (job.guesttex.size() >= kGuestTexMaxCollect) break;
			if (have.insert(addr).second) {
				job.guesttex.push_back(r);
				job.guesttex_session++;
			}
		}
	}
	job.draws = std::move(g_log);
	job.buflog = std::move(g_buflog);
	job.op_ring = buffer.GetContext().GetTextureCache().DumpOpRingText();
	job.dir = g_dir;
	job.seq = g_seq;
	job.images = static_cast<uint32_t>(job.pending.size());
	job.bufreq_leftover = g_bufreq.size();
	g_log.clear();
	g_pending.clear();
	g_bufpending.clear();
	g_guesttex.clear();
	g_seen_guesttex.clear();
	g_guesttex_bytes = 0;
	g_buflog.clear();
	g_bufreq.clear();
	g_targets.clear();
	g_pending_bytes = 0;
	g_images = 0;
	EnsureWorker();
	// Save the values for the log line BEFORE the move: job.dir is a std::string
	// and is moved-from (empty) after the push below.
	const uint32_t    n_images = job.images;
	const std::string dir      = job.dir;
	{
		std::lock_guard lock(g_job_mutex);
		g_jobs.push(std::move(job));
		if (g_jobs.size() > 2) {
			std::printf("NHL27FRAMEDUMP: warning: %zu downloads queued (oldest still running)\n",
			            g_jobs.size());
		}
	}
	g_job_cv.notify_one();
	std::printf("NHL27FRAMEDUMP: frame captured, %u images queued for download -> %s\n", n_images,
	            dir.c_str());
	std::fflush(stdout);
}

} // namespace

// NHL27FRAMEDUMP label fix: PresentDiag::NoteFlip records the registered
// video-out format of each flipped buffer here so MakeTarget can label
// flip-buffer readbacks with it (see the table above MakeTarget). Called on
// every flip; updates the address's entry in place. Labelling only — nothing
// here touches a view, a mapping or any rendering state.
void NoteRegisteredFlipFormat(uint64_t addr, vk::Format format) {
	if (addr == 0 || format == vk::Format::eUndefined) return;
	std::lock_guard lock(g_flip_formats_mutex);
	FlipFormatSeen* free_slot = nullptr;
	for (auto& e : g_flip_formats) {
		if (e.addr == addr) {
			e.format = format;
			return;
		}
		if (e.addr == 0 && free_slot == nullptr) free_slot = &e;
	}
	if (free_slot != nullptr) {
		free_slot->addr   = addr;
		free_slot->format = format;
	}
}

bool Enabled() noexcept {
	static const bool enabled = [] {
		const char* v = std::getenv("KYTY_DBG_FRAME_DUMP");
		return v != nullptr && *v != '\0' && *v != '0';
	}();
	return enabled;
}

bool WriteRgbPng(const std::string& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgb) {
	return WritePng(path, w, h, rgb);
}

void Request() {
	if (!Enabled()) return;
	g_request.store(true);
	std::printf("NHL27FRAMEDUMP: F2 - will capture the next complete frame\n");
	std::fflush(stdout);
}

// Session record for the guesttex dump (see the NHL27GUESTTEX comment block):
// the texture cache calls this on every guest texture upload, so textures
// uploaded before a captured frame are dump candidates too. Descriptors only;
// the worker reads the guest bytes at dump time. Takes only the record's own
// mutex — callers may hold the texture cache lock, so this must never wait on
// g_mutex or do any I/O beyond the one-time "record full" line.
void NoteTextureUpload(const ImageInfo& info) {
	const uint64_t addr = info.data.address;
	const uint64_t size = info.data.size;
	if (addr == 0 || size == 0 || size > kGuestTexMaxSize) return;
	if (!info.data.Valid()) return;
	GuestTexReq r;
	r.addr      = addr;
	r.size      = size;
	r.seq       = 0; // not bound by a draw of the captured frame (FinishCapture merge)
	r.guest_fmt = static_cast<uint32_t>(info.guest_format);
	r.vk_fmt    = vk::to_string(info.pixel_format);
	r.w         = info.extent.width;
	r.h         = info.extent.height;
	r.d         = info.extent.depth;
	r.levels    = info.resources.levels;
	r.layers    = info.resources.layers;
	r.tile_mode = static_cast<uint32_t>(info.tile_mode);
	std::lock_guard lock(g_guesttex_session_mutex);
	const auto      it = g_guesttex_session.find(addr);
	if (it != g_guesttex_session.end()) {
		r.upload_order = it->second.upload_order; // first-upload order survives replacement
		it->second     = std::move(r); // a re-upload at the same address replaces the entry
		return;
	}
	if (g_guesttex_session.size() >= kGuestTexSessionMax) {
		if (!g_guesttex_session_full) {
			g_guesttex_session_full = true;
			std::printf("NHL27GUESTTEX: session record full (%zu entries); new uploads are no "
			            "longer recorded\n",
			            kGuestTexSessionMax);
			std::fflush(stdout);
		}
		return;
	}
	r.upload_order = g_guesttex_session_uploads++;
	g_guesttex_session.emplace(addr, std::move(r));
}

void Shutdown() {
	if (!g_worker_started.load()) return;
	if (g_worker_stopped.exchange(true)) return; // second call is a no-op
	{
		std::lock_guard lock(g_job_mutex);
		g_worker_stop.store(true);
	}
	g_job_cv.notify_one();
	if (g_worker.joinable()) g_worker.join();
}

void OnGuestFlip(CommandBuffer& buffer) {
	g_trace_frame.fetch_add(1, std::memory_order_relaxed);
	if (!Enabled()) return;
	// Scripts request a capture too: a file named framedump.trigger in the working directory
	// (looked for every 30 flips, removed when found).
	static uint32_t flips = 0;
	if (++flips % 30 == 0) {
		std::error_code ec;
		if (std::filesystem::remove("framedump.trigger", ec)) {
			g_request.store(true);
		}
	}
	std::lock_guard lock(g_mutex);
	if (g_capturing) {
		FinishCapture(buffer);
	} else if (g_request.exchange(false)) {
		BeginCapture(buffer);
	}
}

void OnDraw(RenderContext& context, const DrawLog& draw) {
	std::lock_guard lock(g_mutex);
	if (!g_capturing) return;
	const uint32_t seq = g_seq++;
	char b[512];
	std::snprintf(b, sizeof(b),
	              "#%05u draw vs=0x%016" PRIx64 " ps=0x%016" PRIx64 " %s%s count=%u instances=%u ps_active=%d "
	              "ztest=%d zwrite=%d zfunc=%s\n",
	              seq, draw.vs_hash, draw.ps_hash, draw.indexed ? "indexed" : "auto",
	              draw.indirect ? " INDIRECT" : (draw.mesh ? " MESH" : ""), draw.count, draw.instances,
	              draw.ps_active ? 1 : 0, draw.depth_test ? 1 : 0, draw.depth_write ? 1 : 0,
	              vk::to_string(draw.depth_compare).c_str());
	std::string s = b;
	s += draw.state_text;
	s += std::string("    emitted=") + (draw.skipped ? "0 (shader skip)\n" : "1\n");
	if (draw.indirect_snapshot != nullptr && WantBuffers(draw.ps_hash)) {
		// Copy the frozen bytes actually consumed by vkCmdDraw*Indirect, not a CPU view
		// of the guest argument page. Every draw is captured even when guest addresses repeat.
		BufReq req;
		req.buf = draw.indirect_snapshot->Handle();
		req.size = draw.indirect_args_size + sizeof(uint32_t);
		req.seq = seq;
		req.addr = draw.indirect_guest_addr;
		req.label = "indirect";
		req.retained = draw.indirect_snapshot;
		if (req.size <= (4ull << 20)) g_bufreq.push_back(std::move(req));
		std::snprintf(b, sizeof(b), "    indirect_gpu: args_addr=0x%" PRIx64 " args_bytes=%" PRIu64
		    " stride=%u max_count=%u count_offset=%" PRIu64 " indexed=%u capture=%s\n",
		    draw.indirect_guest_addr, draw.indirect_args_size, draw.indirect_stride,
		    draw.indirect_max_count, draw.indirect_args_size, unsigned(draw.indexed),
		    draw.indirect_args_size + sizeof(uint32_t) <= (4ull << 20) ? "queued" : "over_limit");
		s += b;
	}
	for (const auto& c : draw.colors) {
		if (c.desc == nullptr) continue;
		s += "    rt" + std::to_string(c.slot) + ": " + DescribeImage(*c.desc) + " target_mip=" +
		     std::to_string(c.mip) + " target_layer=" + std::to_string(c.layer) + " extent=" +
		     std::to_string(c.extent.width) + "x" + std::to_string(c.extent.height) + "\n";
		Target t;
		if (c.view && MakeTarget(context, c.image_id, c.desc->view_info, c.layout, c.desc->info.data.address, t)) {
			t.slot = c.slot;
			t.seq  = seq;
			t.dirty = true;
			g_targets[reinterpret_cast<uint64_t>(static_cast<VkImageView>(c.view))] = t;
		}
	}
	if (draw.depth != nullptr && draw.depth->desc != nullptr) {
		const auto& c = *draw.depth;
		s += "    depth: " + DescribeImage(*c.desc) + "\n";
		Target t;
		if (c.view && MakeTarget(context, c.image_id, c.desc->view_info, c.layout, c.desc->info.data.address, t)) {
			t.seq   = seq;
			t.dirty = true;
			g_targets[reinterpret_cast<uint64_t>(static_cast<VkImageView>(c.view))] = t;
		}
	}
	for (int i = 0; i < 3; i++) AppendImages(s, ("vs_img" + std::to_string(i)).c_str(), draw.vs_images[i]);
	AppendImages(s, "ps_img", draw.ps_images);
	for (int i = 0; i < 3; i++) {
		if (draw.vs_images[i] != nullptr) {
			for (const auto& im : *draw.vs_images[i]) CollectGuestTex(seq, im);
		}
	}
	if (draw.ps_images != nullptr) {
		for (const auto& im : *draw.ps_images) CollectGuestTex(seq, im);
	}
	g_log += s;
	if (draw.ps_bindings != nullptr && draw.ps_active && WantBuffers(draw.ps_hash)) {
		CaptureBindings(*draw.ps_bindings, seq, draw.ps_hash);
		if (draw.ps_images != nullptr) {
			for (size_t si = 0; si < draw.ps_images->size(); si++) {
				const auto& im = (*draw.ps_images)[si];
				if (im.desc.info.type != Prospero::ImageType::kColor3D) continue;
				if (!g_seen_vol.insert(std::make_pair(im.desc.info.data.address, seq)).second) continue;
				VolReq v;
				v.seq = seq;
				if (MakeTarget(context, im.image_id, im.desc.view_info, im.layout, im.desc.info.data.address,
				               v.target)) {
					v.target.slot  = static_cast<uint32_t>(si);
					v.target.img3d = v.target.slices > 1;
					const uint64_t bytes = static_cast<uint64_t>(v.target.w) * v.target.h * v.target.slices *
					                       BytesPerPixel(v.target.format);
					if (v.target.img3d && bytes > (64ull << 20)) {
						char b3[256];
						std::snprintf(b3, sizeof(b3), "IMG3D %u %zu 0x%s SKIPPED (%" PRIu64 " bytes > 64MB cap)\n",
						              seq, si, Hex(im.desc.info.data.address).c_str(), bytes);
						g_buflog += b3;
					} else {
						g_volreq.push_back(std::move(v));
					}
				}
			}
		}
	}
}

void OnEndRendering(const CommandBuffer& buffer, const RenderState& pass) {
	std::lock_guard lock(g_mutex);
	if (!g_capturing) return;
	auto&      gfx = buffer.GetGraphics();
	const auto cmd = buffer.Handle();
	FlushRequests(gfx, cmd);
	const auto dump = [&](vk::ImageView view, const char* label_prefix, bool is_depth) {
		if (!view) return;
		const auto it = g_targets.find(reinterpret_cast<uint64_t>(static_cast<VkImageView>(view)));
		if (it == g_targets.end() || !it->second.dirty) return;
		it->second.dirty = false;
		std::string label = label_prefix;
		if (!is_depth) label += std::to_string(it->second.slot);
		RecordReadback(gfx, cmd, it->second, label.c_str(), it->second.seq);
	};
	for (uint32_t i = 0; i < pass.num_color_attachments; i++) {
		dump(pass.color_attachments[i].image_view, "rt", false);
	}
	if (pass.depth_stencil_attachment.has_depth || pass.depth_stencil_attachment.has_stencil) {
		dump(pass.depth_stencil_attachment.image_view, "depth", true);
	}
}

void OnDispatch(RenderContext& context, CommandBuffer& buffer, uint64_t cs_hash, uint32_t gx,
                uint32_t gy, uint32_t gz, bool indirect, std::span<const StorageUse> images,
                const PreparedBindings* bindings) {
	std::lock_guard lock(g_mutex);
	if (!g_capturing) return;
	const uint32_t seq = g_seq++;
	char b[256];
	std::snprintf(b, sizeof(b), "#%05u dispatch cs=0x%016" PRIx64 " groups=%ux%ux%u%s\n", seq, cs_hash, gx,
	              gy, gz, indirect ? " INDIRECT" : "");
	static const bool capture_constants = [] {
		const char* value = std::getenv("KYTY_DBG_FRAME_DUMP_CS_CONSTANTS");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	const bool trace_cs = TraceCsEnabled(cs_hash);
	if (bindings != nullptr && (trace_cs || capture_constants)) {
		g_buflog += "# compute bindings captured AFTER dispatch\n";
		CaptureBindings(*bindings, seq, cs_hash, capture_constants && !trace_cs);
	}
	FlushRequests(buffer.GetGraphics(), buffer.Handle());
	std::string s = b;
	for (size_t i = 0; i < images.size(); i++) {
		const auto& u = images[i];
		if (u.binding == nullptr) continue;
		s += "    img[" + std::to_string(i) + "]: " + DescribeImage(u.binding->desc) +
		     (u.storage ? " STORAGE" : " sampled") + (u.written ? " WRITTEN" : "") + "\n";
	}
	g_log += s;
	for (size_t i = 0; i < images.size(); i++) {
		const auto& u = images[i];
		if (u.binding == nullptr || u.storage) continue;
		CollectGuestTex(seq, *u.binding);
	}
	for (size_t i = 0; i < images.size(); i++) {
		const auto& u = images[i];
		if (u.binding == nullptr || !u.written || !u.storage) continue;
		Target t;
		if (!MakeTarget(context, u.binding->image_id, u.binding->desc.view_info, u.binding->layout,
		                u.binding->desc.info.data.address, t)) {
			continue;
		}
		t.slot = static_cast<uint32_t>(i);
		const std::string label = "st" + std::to_string(i);
		RecordReadback(buffer.GetGraphics(), buffer.Handle(), t, label.c_str(), seq);
	}
}

namespace {
std::vector<uint64_t>& TraceList() {
	static std::vector<uint64_t> list = [] {
		std::vector<uint64_t> v;
		if (const char* e = std::getenv("KYTY_DBG_TRACE_WRITES"); e != nullptr && *e != '\0') {
			const std::string s = e;
			size_t            pos = 0;
			while (pos < s.size()) {
				size_t n = s.find(',', pos);
				if (n == std::string::npos) n = s.size();
				const auto a = std::strtoull(s.substr(pos, n - pos).c_str(), nullptr, 16);
				if (a != 0) v.push_back(a);
				pos = n + 1;
			}
		}
		return v;
	}();
	return list;
}
} // namespace

namespace {
std::vector<uint64_t>& TraceCsList() {
	static std::vector<uint64_t> list = [] {
		std::vector<uint64_t> v;
		if (const char* e = std::getenv("KYTY_DBG_TRACE_CS"); e != nullptr && *e != '\0') {
			const std::string s = e;
			size_t            pos = 0;
			while (pos < s.size()) {
				size_t n = s.find(',', pos);
				if (n == std::string::npos) n = s.size();
				const auto a = std::strtoull(s.substr(pos, n - pos).c_str(), nullptr, 16);
				if (a != 0) v.push_back(a);
				pos = n + 1;
			}
		}
		return v;
	}();
	return list;
}
} // namespace

bool TraceCsEnabled(uint64_t cs_hash) noexcept {
	static const bool on = !TraceCsList().empty();
	if (!on) return false;
	for (const auto h : TraceCsList()) {
		if (h == cs_hash) return true;
	}
	return false;
}

void TraceDispatch(uint64_t cs_hash, uint32_t gx, uint32_t gy, uint32_t gz, const PreparedBindings& b,
                   std::span<const uint8_t> image_written) {
	static std::atomic<uint64_t> count {0};
	const uint64_t               n = count.fetch_add(1);
	if (n >= 24 && (n % 64) != 0) return;
	std::printf("NHL27CS: #%" PRIu64 " cs=0x%016" PRIx64 " groups=%ux%ux%u guest_flip=%" PRIu64 "\n", n, cs_hash,
	            gx, gy, gz, g_trace_frame.load());
	if (b.runtime != nullptr && b.runtime->resources != nullptr) {
		const auto& ud = b.runtime->resources->user_data;
		std::printf("NHL27CS:   user_data(%zu):", ud.size());
		for (size_t i = 0; i < ud.size() && i < 32; i++) std::printf(" %08x", ud[i]);
		std::printf("\n");
	}
	std::printf("NHL27CS:   shader_data(%zu):", b.shader_data.size());
	for (size_t i = 0; i < b.shader_data.size() && i < 16; i++) std::printf(" %08x", b.shader_data[i]);
	std::printf("\n");
	for (size_t i = 0; i < b.images.size(); i++) {
		const auto& im = b.images[i];
		std::printf("NHL27CS:   img[%zu]: %s view_base_level=%u view_base_layer=%u%s\n", i,
		            DescribeImage(im.desc).c_str(), im.desc.view_info.base_level, im.desc.view_info.base_layer,
		            (i < image_written.size() && image_written[i]) ? " WRITTEN" : "");
	}
	for (size_t i = 0; i < b.buffer_sources.size(); i++) {
		const auto& s = b.buffer_sources[i];
		if (s.address == 0 || s.size == 0) {
			std::printf("NHL27CS:   buf[%zu]: null\n", i);
			continue;
		}
		const size_t dwords = static_cast<size_t>(std::min<uint64_t>(s.size, 65536) / 4);
		const auto*  p      = reinterpret_cast<const uint32_t*>(s.address);
		size_t       nz     = 0;
		for (size_t k = 0; k < dwords; k++) nz += p[k] != 0;
		std::printf("NHL27CS:   buf[%zu]: guest 0x%" PRIx64 " size=%" PRIu64 " nonzero_dwords(first 64KB, CPU view)=%zu/%zu first8:",
		            i, s.address, s.size, nz, dwords);
		for (size_t k = 0; k < 8 && k < dwords; k++) std::printf(" %08x", p[k]);
		std::printf("\n");
	}
	std::fflush(stdout);
}

bool TraceEnabled() noexcept {
	static const bool on = !TraceList().empty();
	return on;
}

void TraceWrite(const char* what, uint64_t begin, uint64_t end, const char* kind, uint64_t hash) {
	for (const auto a : TraceList()) {
		if (a >= begin && a < end) {
			std::printf("NHL27TRACE: %s [0x%" PRIx64 ",0x%" PRIx64 ") covers 0x%" PRIx64 " writer=%s hash=0x%016" PRIx64
			            " guest_flip=%" PRIu64 "\n",
			            what, begin, end, a, kind != nullptr ? kind : "?", hash, g_trace_frame.load());
			std::fflush(stdout);
		}
	}
}

} // namespace Libs::Graphics::FrameDump

namespace Libs::Graphics {
// NHL27SAMPLE sample-path diagnostic summary (defined in imageView.cpp; logging only).
void SampleDiagPrintSummary(uint64_t frame);
} // namespace Libs::Graphics

// NHL27PRESENT present-stage diagnostic (see frameDump.h). Logging only: nothing in
// this namespace reads back pixels, touches descriptors or alters any rendering state.
namespace Libs::Graphics::PresentDiag {

namespace {

constexpr size_t kMaxFlipBuffers = 8;
constexpr size_t kMaxSampled     = 16;
constexpr size_t kMaxSwizzleSeen = 32;
constexpr uint64_t kSummaryEvery = 600;

std::mutex g_pd_mutex;

// Flip buffer addresses for the NoteDraw fast path (mirrors g_pd_buffers under the mutex).
std::atomic<uint64_t> g_pd_flip_addrs[kMaxFlipBuffers] {};
std::atomic<uint32_t> g_pd_flip_addr_count {0};
// Ordinal of every recorded draw; a present draw's index within its frame is derived
// against the ordinal snapshot taken at the previous flip.
std::atomic<uint64_t> g_pd_draw_ordinal {0};

struct SampledView {
	uint64_t             addr        = 0;
	vk::Format           view_format = vk::Format::eUndefined;
	vk::ComponentMapping mapping {};
};

struct LastDraw {
	bool     valid         = false;
	uint64_t flips_at_draw = 0; // Flip counter value when the draw was recorded.
	uint64_t draw_index    = 0; // Ordinal among the draws of its guest frame (1-based).
	bool     ps_active     = false;
	bool     all_null      = false;
	uint32_t slots         = 0;
	std::array<SampledView, kMaxSampled> sampled {};
	uint32_t sampled_count = 0;
};

struct FlipBuffer {
	uint64_t addr = 0;
	LastDraw last {};
};

std::array<FlipBuffer, kMaxFlipBuffers> g_pd_buffers {};
uint64_t                                g_pd_flips           = 0;
uint64_t                                g_pd_ordinal_at_flip = 0;
vk::Format                              g_pd_last_format     = vk::Format::eUndefined;
bool                                    g_pd_have_format     = false;
uint64_t                                g_pd_stat_format_flips = 0;
uint64_t                                g_pd_stat_swizzle      = 0;
uint64_t                                g_pd_stat_null_frames  = 0;
uint64_t                                g_pd_summary_from      = 1;

struct SwizzleSeen {
	uint64_t             addr = 0;
	vk::ComponentMapping mapping {};
	bool                 used = false;
};
std::array<SwizzleSeen, kMaxSwizzleSeen> g_pd_swizzle_seen {};
size_t                                   g_pd_swizzle_next = 0;

std::string FormatText(vk::Format format) {
	auto name = vk::to_string(format);
	if (format == vk::Format::eA2R10G10B10UnormPack32) {
		name += " (R-first)";
	} else if (format == vk::Format::eA2B10G10R10UnormPack32) {
		name += " (B-first)";
	}
	return name;
}

char SwizzleChar(vk::ComponentSwizzle swizzle, char own) {
	switch (swizzle) {
		case vk::ComponentSwizzle::eIdentity: return own;
		case vk::ComponentSwizzle::eZero: return '0';
		case vk::ComponentSwizzle::eOne: return '1';
		case vk::ComponentSwizzle::eR: return 'r';
		case vk::ComponentSwizzle::eG: return 'g';
		case vk::ComponentSwizzle::eB: return 'b';
		case vk::ComponentSwizzle::eA: return 'a';
		default: return '?';
	}
}

// The mapping as four source letters (identity reads "rgba", an R/B swap "bgra").
std::string MappingText(const vk::ComponentMapping& mapping) {
	std::string text;
	text += SwizzleChar(mapping.r, 'r');
	text += SwizzleChar(mapping.g, 'g');
	text += SwizzleChar(mapping.b, 'b');
	text += SwizzleChar(mapping.a, 'a');
	return text;
}

bool IsIdentityMapping(const vk::ComponentMapping& mapping) {
	const auto own = [](vk::ComponentSwizzle swizzle, vk::ComponentSwizzle channel) {
		return swizzle == vk::ComponentSwizzle::eIdentity || swizzle == channel;
	};
	return own(mapping.r, vk::ComponentSwizzle::eR) && own(mapping.g, vk::ComponentSwizzle::eG) &&
	       own(mapping.b, vk::ComponentSwizzle::eB) && own(mapping.a, vk::ComponentSwizzle::eA);
}

} // namespace

void NoteDraw(const uint64_t* color_targets, uint32_t color_target_count, bool ps_active,
              const std::vector<TextureBinding>* ps_images) {
	const uint64_t ordinal = g_pd_draw_ordinal.fetch_add(1, std::memory_order_relaxed);
	const uint32_t known   = g_pd_flip_addr_count.load(std::memory_order_relaxed);
	if (known == 0 || color_targets == nullptr) {
		return;
	}
	size_t hit = kMaxFlipBuffers;
	for (uint32_t t = 0; t < color_target_count && hit == kMaxFlipBuffers; t++) {
		if (color_targets[t] == 0) {
			continue;
		}
		for (uint32_t i = 0; i < known; i++) {
			if (g_pd_flip_addrs[i].load(std::memory_order_relaxed) == color_targets[t]) {
				hit = i;
				break;
			}
		}
	}
	if (hit == kMaxFlipBuffers) {
		return;
	}

	LastDraw rec {};
	rec.valid     = true;
	rec.ps_active = ps_active;
	rec.slots     = ps_images != nullptr ? static_cast<uint32_t>(ps_images->size()) : 0;
	bool any_bound = false;
	if (ps_images != nullptr) {
		for (const auto& im : *ps_images) {
			const uint64_t addr = im.desc.info.data.address;
			if (addr == 0) {
				continue;
			}
			any_bound = true;
			if (rec.sampled_count < kMaxSampled) {
				auto& s        = rec.sampled[rec.sampled_count++];
				s.addr         = addr;
				s.view_format  = im.desc.view_info.format;
				s.mapping      = im.desc.view_info.mapping;
			}
		}
	}
	// The dark-transition signature: a pixel-shader draw whose every texture slot
	// resolved to the null descriptor (guest address 0).
	rec.all_null = ps_active && !any_bound;

	std::lock_guard lock(g_pd_mutex);
	rec.flips_at_draw = g_pd_flips;
	rec.draw_index    = ordinal - g_pd_ordinal_at_flip + 1;
	g_pd_buffers[hit].last = rec;
}

void NoteFlip(uint64_t buffer_address, vk::Format format) {
	std::lock_guard lock(g_pd_mutex);

	// Register the presented buffer (fast-path table + per-buffer draw record).
	size_t slot = kMaxFlipBuffers;
	for (size_t i = 0; i < kMaxFlipBuffers; i++) {
		if (g_pd_buffers[i].addr == buffer_address) {
			slot = i;
			break;
		}
		if (g_pd_buffers[i].addr == 0 && slot == kMaxFlipBuffers) {
			slot = i;
		}
	}
	if (slot < kMaxFlipBuffers && g_pd_buffers[slot].addr == 0) {
		g_pd_buffers[slot].addr = buffer_address;
		g_pd_flip_addrs[slot].store(buffer_address, std::memory_order_relaxed);
		uint32_t count = 0;
		for (const auto& b : g_pd_buffers) {
			count += b.addr != 0 ? 1u : 0u;
		}
		g_pd_flip_addr_count.store(count, std::memory_order_relaxed);
	}

	// NHL27FRAMEDUMP label fix: hand the registered format to the frame dump,
	// which decodes flip-buffer readbacks with it instead of the cache image's
	// creation format (see FrameDump::NoteRegisteredFlipFormat).
	FrameDump::NoteRegisteredFlipFormat(buffer_address, format);

	g_pd_flips++;
	const uint64_t frame = g_pd_flips;

	if (!g_pd_have_format) {
		g_pd_have_format = true;
		std::printf("NHL27PRESENT format frame=%" PRIu64 " addr=0x%" PRIx64 " initial=%s\n", frame,
		            buffer_address, FormatText(format).c_str());
		std::fflush(stdout);
	} else if (format != g_pd_last_format) {
		g_pd_stat_format_flips++;
		std::printf("NHL27PRESENT format frame=%" PRIu64 " addr=0x%" PRIx64 " %s -> %s\n", frame,
		            buffer_address, FormatText(g_pd_last_format).c_str(), FormatText(format).c_str());
		std::fflush(stdout);
	}
	g_pd_last_format = format;

	// Judge the frame that just ended by the last draw into the buffer being flipped.
	if (slot < kMaxFlipBuffers) {
		const auto& last = g_pd_buffers[slot].last;
		if (last.valid && last.flips_at_draw == frame - 1) {
			if (last.ps_active && last.all_null) {
				g_pd_stat_null_frames++;
				std::printf("NHL27PRESENT nullbind frame=%" PRIu64 " draw=%" PRIu64 " slots=%u addr=0x%" PRIx64
				            "\n",
				            frame, last.draw_index, last.slots, buffer_address);
				std::fflush(stdout);
			}
			for (uint32_t i = 0; i < last.sampled_count; i++) {
				const auto& s    = last.sampled[i];
				SwizzleSeen* seen = nullptr;
				for (auto& e : g_pd_swizzle_seen) {
					if (e.used && e.addr == s.addr) {
						seen = &e;
						break;
					}
				}
				if (seen == nullptr) {
					if (!IsIdentityMapping(s.mapping)) {
						g_pd_stat_swizzle++;
						std::printf("NHL27PRESENT swizzle frame=%" PRIu64 " slot=%u addr=0x%" PRIx64
						            " view=%s map=%s (first seen)\n",
						            frame, i, s.addr, vk::to_string(s.view_format).c_str(),
						            MappingText(s.mapping).c_str());
						std::fflush(stdout);
					}
					auto& e   = g_pd_swizzle_seen[g_pd_swizzle_next];
					e.addr    = s.addr;
					e.mapping = s.mapping;
					e.used    = true;
					g_pd_swizzle_next = (g_pd_swizzle_next + 1) % kMaxSwizzleSeen;
				} else if (!(seen->mapping == s.mapping)) {
					g_pd_stat_swizzle++;
					std::printf("NHL27PRESENT swizzle frame=%" PRIu64 " slot=%u addr=0x%" PRIx64
					            " view=%s map=%s -> %s\n",
					            frame, i, s.addr, vk::to_string(s.view_format).c_str(),
					            MappingText(seen->mapping).c_str(), MappingText(s.mapping).c_str());
					std::fflush(stdout);
					seen->mapping = s.mapping;
				}
			}
		}
	}

	if (frame % kSummaryEvery == 0) {
		std::printf("NHL27PRESENT summary frame=%" PRIu64 " format_flips=%" PRIu64 " swizzle_events=%" PRIu64
		            " null_frames=%" PRIu64 " since_frame=%" PRIu64 "\n",
		            frame, g_pd_stat_format_flips, g_pd_stat_swizzle, g_pd_stat_null_frames,
		            g_pd_summary_from);
		std::fflush(stdout);
		g_pd_stat_format_flips = 0;
		g_pd_stat_swizzle      = 0;
		g_pd_stat_null_frames  = 0;
		g_pd_summary_from      = frame + 1;
		SampleDiagPrintSummary(frame);
	}

	g_pd_ordinal_at_flip = g_pd_draw_ordinal.load(std::memory_order_relaxed);
}

} // namespace Libs::Graphics::PresentDiag
