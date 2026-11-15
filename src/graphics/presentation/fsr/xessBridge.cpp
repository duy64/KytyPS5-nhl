#include "graphics/presentation/fsr/xessBridge.h"

#include "graphics/host_gpu/graphicContext.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#if KYTY_ENABLE_XESS && defined(_WIN32)
#define KYTY_XESS_BRIDGE 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <vulkan/vulkan_win32.h>
// XeSS SDK headers, used for the types only: libxess.dll is loaded at runtime and its
// functions are resolved with GetProcAddress (see the PFN_ typedefs below), so the
// emulator has no link-time or load-time dependency on the DLL.
#include "xess/xess.h"
#include "xess/xess_d3d12.h"
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#else
#define KYTY_XESS_BRIDGE 0
#endif

namespace Libs::Graphics {

#if KYTY_XESS_BRIDGE
namespace {
using Microsoft::WRL::ComPtr;

using PFN_xessD3D12CreateContext =
    xess_result_t (*)(ID3D12Device* pDevice, xess_context_handle_t* phContext);
using PFN_xessD3D12BuildPipelines = xess_result_t (*)(xess_context_handle_t hContext,
                                                      ID3D12PipelineLibrary* pPipelineLibrary, bool blocking,
                                                      uint32_t initFlags);
using PFN_xessD3D12Init =
    xess_result_t (*)(xess_context_handle_t hContext, const xess_d3d12_init_params_t* pInitParams);
using PFN_xessD3D12Execute = xess_result_t (*)(xess_context_handle_t hContext,
                                               ID3D12GraphicsCommandList* pCommandList,
                                               const xess_d3d12_execute_params_t* pExecParams);
using PFN_xessDestroyContext = xess_result_t (*)(xess_context_handle_t hContext);
using PFN_xessGetVersion     = xess_result_t (*)(xess_version_t* pVersion);
using PFN_xessGetOptimalInputResolution =
    xess_result_t (*)(xess_context_handle_t hContext, const xess_2d_t* pOutputResolution,
                      xess_quality_settings_t qualitySettings, xess_2d_t* pInputResolutionOptimal,
                      xess_2d_t* pInputResolutionMin, xess_2d_t* pInputResolutionMax);
using PFN_xessSetVelocityScale = xess_result_t (*)(xess_context_handle_t hContext, float x, float y);

template <typename T>
bool Resolve(HMODULE dll, T& fn, const char* name) {
	fn = reinterpret_cast<T>(GetProcAddress(dll, name));
	return fn != nullptr;
}

// Test switches (read once), mirroring the KYTY_FSR4_* switches:
//   KYTY_XESS_QUALITY=<name>    ultraperformance, performance, balanced, quality,
//                               ultraquality, ultraqualityplus or aa; default: the quality
//                               whose optimal input resolution is closest to the game's
//   KYTY_XESS_JITTER=<x>,<y>    multiply the jitter (default 1,1; 1,-1 matches the XeSS
//                               sample's Y convention if a session shows vertical ghosting)
//   KYTY_XESS_PREEXPOSURE=1     pass exposure scale 1 instead of the game's pre-exposure
//   KYTY_XESS_AUTOEXPOSURE=1    let XeSS compute its own exposure (init flag; has a
//                               measurable cost per the XeSS guide, so off by default)
struct Tuning {
	std::string quality;
	float       jitter_x = 1, jitter_y = 1;
	bool        unit_pre_exposure = false;
	bool        auto_exposure     = false;
};
const Tuning& GetTuning() {
	static const Tuning tuning = [] {
		Tuning t;
		if (const char* v = std::getenv("KYTY_XESS_QUALITY")) t.quality = v;
		if (const char* v = std::getenv("KYTY_XESS_JITTER")) {
			if (std::sscanf(v, "%f,%f", &t.jitter_x, &t.jitter_y) != 2) t.jitter_x = t.jitter_y = 1;
		}
		if (const char* v = std::getenv("KYTY_XESS_PREEXPOSURE")) t.unit_pre_exposure = v[0] == '1';
		if (const char* v = std::getenv("KYTY_XESS_AUTOEXPOSURE")) t.auto_exposure = v[0] == '1';
		std::printf("NHL27XESS tuning quality=%s jitter=%g,%g pre_exposure=%s auto_exposure=%d\n",
		            t.quality.empty() ? "auto" : t.quality.c_str(), double(t.jitter_x), double(t.jitter_y),
		            t.unit_pre_exposure ? "1" : "game", t.auto_exposure ? 1 : 0);
		std::fflush(stdout);
		return t;
	}();
	return tuning;
}

const char* QualityName(xess_quality_settings_t quality) {
	switch (quality) {
		case XESS_QUALITY_SETTING_ULTRA_PERFORMANCE: return "UltraPerformance";
		case XESS_QUALITY_SETTING_PERFORMANCE: return "Performance";
		case XESS_QUALITY_SETTING_BALANCED: return "Balanced";
		case XESS_QUALITY_SETTING_QUALITY: return "Quality";
		case XESS_QUALITY_SETTING_ULTRA_QUALITY: return "UltraQuality";
		case XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS: return "UltraQualityPlus";
		case XESS_QUALITY_SETTING_AA: return "AA";
	}
	return "?";
}

struct Format {
	DXGI_FORMAT dxgi;
	VkFormat    vk;
};
// Colour is R16G16B16A16, not the game's B10G11R11: XeSS requires the output format to
// equal the input colour format, and the output must be R16G16B16A16 for the present
// copy. The interceptor converts the snapshot into this image with a blit.
constexpr std::array<Format, XessBridge::Count> kFormats {{
    {DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT},
    {DXGI_FORMAT_R32_FLOAT, VK_FORMAT_R32_SFLOAT},
    {DXGI_FORMAT_R16G16_FLOAT, VK_FORMAT_R16G16_SFLOAT},
    {DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT},
    {DXGI_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM},
}};
} // namespace
#endif

struct XessBridge::Impl {
	GraphicContext& graphics;
	std::string     status = "XeSS not initialized";
	bool            failed = false;
	bool            fresh  = false;
	vk::Extent2D    render {}, display {};
	uint64_t        value = 0;

	explicit Impl(GraphicContext& gfx): graphics(gfx) {}

	bool Fail(const std::string& why) {
		failed = true;
		status = "XeSS unavailable: " + why;
		std::printf("NHL27XESS status=failed reason=%s\n", why.c_str());
		std::fflush(stdout);
		return false;
	}

#if KYTY_XESS_BRIDGE
	static constexpr int Ring = 3;
	struct Shared {
		ComPtr<ID3D12Resource> resource;
		VkImage                image  = VK_NULL_HANDLE;
		VkDeviceMemory         memory = VK_NULL_HANDLE;
	};
	struct Functions {
		PFN_xessD3D12CreateContext          create_context {};
		PFN_xessD3D12BuildPipelines         build_pipelines {};
		PFN_xessD3D12Init                   init {};
		PFN_xessD3D12Execute                execute {};
		PFN_xessDestroyContext              destroy_context {};
		PFN_xessGetVersion                  get_version {};
		PFN_xessGetOptimalInputResolution   get_optimal_input_resolution {};
		PFN_xessSetVelocityScale            set_velocity_scale {};
	};

	HMODULE                                          dll = nullptr;
	Functions                                        fns {};
	ComPtr<ID3D12Device>                             device;
	ComPtr<ID3D12CommandQueue>                       queue;
	std::array<ComPtr<ID3D12CommandAllocator>, Ring> allocators;
	std::array<uint64_t, Ring>                       allocator_value {};
	ComPtr<ID3D12GraphicsCommandList>                list;
	int                                              ring = 0;
	ComPtr<ID3D12Fence>                              fence;
	HANDLE                                           event = nullptr;
	VkSemaphore                                      semaphore = VK_NULL_HANDLE;
	std::array<Shared, Count>                        shared {};
	xess_context_handle_t                            context = nullptr;
	uint64_t                                         last_signal = 0;
	uint64_t                                         dispatched  = 0;
	std::string                                      version = "?";
	PFN_vkGetMemoryWin32HandlePropertiesKHR          get_handle_properties = nullptr;
	PFN_vkImportSemaphoreWin32HandleKHR              import_semaphore      = nullptr;

	[[nodiscard]] VkDevice Vk() const { return static_cast<VkDevice>(graphics.device); }

	bool Init() {
		const VkDevice vk_device = Vk();
		get_handle_properties = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
		    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(vk_device, "vkGetMemoryWin32HandlePropertiesKHR"));
		import_semaphore = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
		    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(vk_device, "vkImportSemaphoreWin32HandleKHR"));
		if (get_handle_properties == nullptr || import_semaphore == nullptr) {
			return Fail("VK_KHR_external_memory_win32 / VK_KHR_external_semaphore_win32 not enabled");
		}

		dll = LoadLibraryW(L"libxess.dll");
		if (dll == nullptr) {
			return Fail("libxess.dll not found next to the emulator");
		}
		if (!Resolve(dll, fns.create_context, "xessD3D12CreateContext") ||
		    !Resolve(dll, fns.build_pipelines, "xessD3D12BuildPipelines") ||
		    !Resolve(dll, fns.init, "xessD3D12Init") || !Resolve(dll, fns.execute, "xessD3D12Execute") ||
		    !Resolve(dll, fns.destroy_context, "xessDestroyContext") ||
		    !Resolve(dll, fns.get_version, "xessGetVersion") ||
		    !Resolve(dll, fns.get_optimal_input_resolution, "xessGetOptimalInputResolution") ||
		    !Resolve(dll, fns.set_velocity_scale, "xessSetVelocityScale")) {
			return Fail("libxess.dll is missing XeSS D3D12 exports");
		}
		xess_version_t sdk_version {};
		if (fns.get_version(&sdk_version) == XESS_RESULT_SUCCESS) {
			version = std::to_string(sdk_version.major) + "." + std::to_string(sdk_version.minor) + "." +
			          std::to_string(sdk_version.patch);
		}

		// The D3D12 device must be on the same graphics card as the Vulkan device.
		VkPhysicalDeviceIDProperties id {};
		id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
		VkPhysicalDeviceProperties2 properties {};
		properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		properties.pNext = &id;
		VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceProperties2(
		    static_cast<VkPhysicalDevice>(graphics.physical_device), &properties);
		if (id.deviceLUIDValid != VK_TRUE) {
			return Fail("Vulkan device has no LUID");
		}
		LUID luid {};
		std::memcpy(&luid, id.deviceLUID, sizeof(luid));
		ComPtr<IDXGIFactory4> factory;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
			return Fail("CreateDXGIFactory1");
		}
		ComPtr<IDXGIAdapter1> adapter;
		if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) {
			return Fail("no DXGI adapter with the Vulkan device's LUID");
		}
		if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
			return Fail("D3D12CreateDevice");
		}
		D3D12_COMMAND_QUEUE_DESC queue_desc {};
		queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)))) {
			return Fail("CreateCommandQueue");
		}
		for (auto& allocator: allocators) {
			if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) {
				return Fail("CreateCommandAllocator");
			}
		}
		if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr,
		                                     IID_PPV_ARGS(&list))) ||
		    FAILED(list->Close())) {
			return Fail("CreateCommandList");
		}
		event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

		// Shared fence -> Vulkan timeline semaphore.
		if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)))) {
			return Fail("CreateFence");
		}
		HANDLE fence_handle = nullptr;
		if (FAILED(device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &fence_handle))) {
			return Fail("CreateSharedHandle(fence)");
		}
		VkSemaphoreTypeCreateInfo timeline {};
		timeline.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
		timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		timeline.initialValue  = 0;
		VkSemaphoreCreateInfo semaphore_info {};
		semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		semaphore_info.pNext = &timeline;
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateSemaphore(vk_device, &semaphore_info, nullptr, &semaphore) !=
		    VK_SUCCESS) {
			CloseHandle(fence_handle);
			return Fail("vkCreateSemaphore");
		}
		VkImportSemaphoreWin32HandleInfoKHR import {};
		import.sType      = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
		import.semaphore  = semaphore;
		import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
		import.handle     = fence_handle;
		const VkResult imported = import_semaphore(vk_device, &import);
		CloseHandle(fence_handle);
		if (imported != VK_SUCCESS) {
			return Fail("vkImportSemaphoreWin32HandleKHR");
		}
		(void)GetTuning();
		status = "XeSS ready";
		return true;
	}

	void WaitIdle() {
		if (fence && last_signal != 0 && fence->GetCompletedValue() < last_signal) {
			fence->SetEventOnCompletion(last_signal, event);
			WaitForSingleObject(event, 5000);
		}
	}

	void Release() {
		WaitIdle();
		if (context != nullptr) {
			fns.destroy_context(context);
			context = nullptr;
		}
		const VkDevice vk_device = Vk();
		for (auto& s: shared) {
			if (s.image != VK_NULL_HANDLE) VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyImage(vk_device, s.image, nullptr);
			if (s.memory != VK_NULL_HANDLE) VULKAN_HPP_DEFAULT_DISPATCHER.vkFreeMemory(vk_device, s.memory, nullptr);
			s = {};
		}
		render  = {};
		display = {};
	}

	bool CreateShared(int index, vk::Extent2D extent) {
		auto& s = shared[index];
		D3D12_RESOURCE_DESC desc {};
		desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width            = extent.width;
		desc.Height           = extent.height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels        = 1;
		desc.Format           = kFormats[index].dxgi;
		desc.SampleDesc.Count = 1;
		desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags            = index == Output ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
		D3D12_HEAP_PROPERTIES heap {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON,
		                                           nullptr, IID_PPV_ARGS(&s.resource)))) {
			return Fail("CreateCommittedResource(shared)");
		}
		HANDLE handle = nullptr;
		if (FAILED(device->CreateSharedHandle(s.resource.Get(), nullptr, GENERIC_ALL, nullptr, &handle))) {
			return Fail("CreateSharedHandle(image)");
		}
		const VkDevice vk_device = Vk();
		VkExternalMemoryImageCreateInfo external {};
		external.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
		external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
		VkImageCreateInfo info {};
		info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		info.pNext         = &external;
		info.imageType     = VK_IMAGE_TYPE_2D;
		info.format        = kFormats[index].vk;
		info.extent        = {extent.width, extent.height, 1};
		info.mipLevels     = 1;
		info.arrayLayers   = 1;
		info.samples       = VK_SAMPLE_COUNT_1_BIT;
		info.tiling        = VK_IMAGE_TILING_OPTIMAL;
		info.usage         = index == Output ? (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT)
		                                     : VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
		info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateImage(vk_device, &info, nullptr, &s.image) != VK_SUCCESS) {
			CloseHandle(handle);
			return Fail("vkCreateImage(external)");
		}
		VkMemoryRequirements requirements {};
		VULKAN_HPP_DEFAULT_DISPATCHER.vkGetImageMemoryRequirements(vk_device, s.image, &requirements);
		VkMemoryWin32HandlePropertiesKHR handle_properties {};
		handle_properties.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR;
		uint32_t type_bits      = requirements.memoryTypeBits;
		if (get_handle_properties(vk_device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, handle,
		                          &handle_properties) == VK_SUCCESS &&
		    (handle_properties.memoryTypeBits & type_bits) != 0) {
			type_bits &= handle_properties.memoryTypeBits;
		}
		VkPhysicalDeviceMemoryProperties memory_properties {};
		VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceMemoryProperties(
		    static_cast<VkPhysicalDevice>(graphics.physical_device), &memory_properties);
		uint32_t type = UINT32_MAX;
		for (uint32_t i = 0; i < memory_properties.memoryTypeCount && type == UINT32_MAX; ++i) {
			if ((type_bits & (1u << i)) != 0 &&
			    (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
				type = i;
			}
		}
		for (uint32_t i = 0; i < memory_properties.memoryTypeCount && type == UINT32_MAX; ++i) {
			if ((type_bits & (1u << i)) != 0) type = i;
		}
		if (type == UINT32_MAX) {
			CloseHandle(handle);
			return Fail("no memory type for the shared image");
		}
		VkMemoryDedicatedAllocateInfo dedicated {};
		dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
		dedicated.image = s.image;
		VkImportMemoryWin32HandleInfoKHR import {};
		import.sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
		import.pNext      = &dedicated;
		import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
		import.handle     = handle;
		VkMemoryAllocateInfo allocate {};
		allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocate.pNext           = &import;
		allocate.allocationSize  = requirements.size;
		allocate.memoryTypeIndex = type;
		const VkResult allocated =
		    VULKAN_HPP_DEFAULT_DISPATCHER.vkAllocateMemory(vk_device, &allocate, nullptr, &s.memory);
		CloseHandle(handle);
		if (allocated != VK_SUCCESS) {
			return Fail("vkAllocateMemory(import D3D12 resource)");
		}
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkBindImageMemory(vk_device, s.image, s.memory, 0) != VK_SUCCESS) {
			return Fail("vkBindImageMemory(shared)");
		}
		return true;
	}

	// The game fixes the render size (its own FSR quality choice), so pick the XeSS
	// quality whose optimal input width is closest to the actual render width.
	xess_quality_settings_t PickQuality(vk::Extent2D new_render, vk::Extent2D new_display) {
		const auto& wanted = GetTuning().quality;
		if (!wanted.empty()) {
			if (wanted == "ultraperformance") return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
			if (wanted == "performance") return XESS_QUALITY_SETTING_PERFORMANCE;
			if (wanted == "balanced") return XESS_QUALITY_SETTING_BALANCED;
			if (wanted == "quality") return XESS_QUALITY_SETTING_QUALITY;
			if (wanted == "ultraquality") return XESS_QUALITY_SETTING_ULTRA_QUALITY;
			if (wanted == "ultraqualityplus") return XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS;
			if (wanted == "aa") return XESS_QUALITY_SETTING_AA;
		}
		const xess_2d_t output {new_display.width, new_display.height};
		xess_quality_settings_t best = XESS_QUALITY_SETTING_QUALITY;
		uint32_t best_diff           = UINT32_MAX;
		for (const auto candidate : {XESS_QUALITY_SETTING_ULTRA_PERFORMANCE, XESS_QUALITY_SETTING_PERFORMANCE,
		                             XESS_QUALITY_SETTING_BALANCED, XESS_QUALITY_SETTING_QUALITY,
		                             XESS_QUALITY_SETTING_ULTRA_QUALITY, XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS,
		                             XESS_QUALITY_SETTING_AA}) {
			xess_2d_t optimal {}, minimum {}, maximum {};
			if (fns.get_optimal_input_resolution(context, &output, candidate, &optimal, &minimum, &maximum) !=
			    XESS_RESULT_SUCCESS) {
				continue;
			}
			const uint32_t diff = optimal.x > new_render.width ? optimal.x - new_render.width
			                                                   : new_render.width - optimal.x;
			if (diff < best_diff) {
				best_diff = diff;
				best      = candidate;
			}
		}
		return best;
	}

	bool Ensure(vk::Extent2D new_render, vk::Extent2D new_display) {
		if (failed) return false;
		if (render == new_render && display == new_display && context != nullptr) return true;
		Release();
		for (int i = 0; i < Count; ++i) {
			if (!CreateShared(i, i == Output ? new_display : new_render)) return false;
		}
		if (fns.create_context(device.Get(), &context) != XESS_RESULT_SUCCESS || context == nullptr) {
			context = nullptr;
			return Fail("xessD3D12CreateContext");
		}
		const auto quality = PickQuality(new_render, new_display);
		const uint32_t flags = XESS_INIT_FLAG_INVERTED_DEPTH |
		    (GetTuning().auto_exposure ? uint32_t(XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE) : 0u);
		// Optional pre-build (blocking): moves kernel JIT off the Init call. Init builds
		// whatever is missing either way, so a failure here is not fatal.
		(void)fns.build_pipelines(context, nullptr, true, flags);
		xess_d3d12_init_params_t init {};
		init.outputResolution  = {new_display.width, new_display.height};
		init.qualitySetting    = quality;
		init.initFlags         = flags;
		init.creationNodeMask  = 1;
		init.visibleNodeMask   = 1;
		const auto inited      = fns.init(context, &init);
		if (inited != XESS_RESULT_SUCCESS) {
			return Fail("xessD3D12Init returned " + std::to_string(inited));
		}
		status = "XeSS bridge: " + version + " quality " + QualityName(quality);
		std::printf("NHL27XESS status=created provider=%s render=%ux%u display=%ux%u quality=%s\n",
		            version.c_str(), new_render.width, new_render.height, new_display.width, new_display.height,
		            QualityName(quality));
		std::fflush(stdout);
		render  = new_render;
		display = new_display;
		fresh   = true;
		return true;
	}

	Result Dispatch(const XessParams& p, uint64_t wait_value, uint64_t signal_value) {
		if (failed || context == nullptr) return Result::NotQueued;
		auto& allocator = allocators[ring];
		if (allocator_value[ring] != 0 && fence->GetCompletedValue() < allocator_value[ring]) {
			fence->SetEventOnCompletion(allocator_value[ring], event);
			WaitForSingleObject(event, 5000);
		}
		if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr))) {
			Fail("command list reset");
			return Result::NotQueued;
		}
		// XeSS expects its inputs in NON_PIXEL_SHADER_RESOURCE and the output as a UAV and
		// transitions nothing itself; between frames the shared images stay in COMMON,
		// which Vulkan sees as VK_IMAGE_LAYOUT_GENERAL.
		const auto transition = [this](bool to_xess) {
			std::array<D3D12_RESOURCE_BARRIER, Count> barriers {};
			for (int i = 0; i < Count; ++i) {
				const auto used = i == Output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
				                              : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
				auto& b                  = barriers[i];
				b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				b.Transition.pResource   = shared[i].resource.Get();
				b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				b.Transition.StateBefore = to_xess ? D3D12_RESOURCE_STATE_COMMON : used;
				b.Transition.StateAfter  = to_xess ? used : D3D12_RESOURCE_STATE_COMMON;
			}
			list->ResourceBarrier(Count, barriers.data());
		};
		transition(true);
		const auto& tuning = GetTuning();
		// XeSS has no per-execute motion-vector scale: it is a context setter.
		(void)fns.set_velocity_scale(context, p.motion_scale_x, p.motion_scale_y);
		xess_d3d12_execute_params_t exec {};
		exec.pColorTexture   = shared[Color].resource.Get();
		exec.pVelocityTexture = shared[Motion].resource.Get();
		exec.pDepthTexture   = shared[Depth].resource.Get();
		exec.pOutputTexture  = shared[Output].resource.Get();
		exec.jitterOffsetX   = p.jitter_x * tuning.jitter_x;
		exec.jitterOffsetY   = p.jitter_y * tuning.jitter_y;
		exec.exposureScale   = tuning.auto_exposure || tuning.unit_pre_exposure ? 1.0f : p.pre_exposure;
		exec.resetHistory    = p.reset ? 1u : 0u;
		exec.inputWidth      = render.width;
		exec.inputHeight     = render.height;
		const auto result    = fns.execute(context, list.Get(), &exec);
		transition(false);
		if (FAILED(list->Close())) {
			Fail("command list close");
			return Result::NotQueued;
		}
		// Even when XeSS refused the frame, the Vulkan side waits for signal_value: always signal it.
		queue->Wait(fence.Get(), wait_value);
		if (result == XESS_RESULT_SUCCESS) {
			ID3D12CommandList* lists[] = {list.Get()};
			queue->ExecuteCommandLists(1, lists);
		}
		queue->Signal(fence.Get(), signal_value);
		allocator_value[ring] = signal_value;
		ring                  = (ring + 1) % Ring;
		last_signal           = signal_value;
		if (result != XESS_RESULT_SUCCESS) {
			Fail("xessD3D12Execute returned " + std::to_string(result));
			return Result::Failed;
		}
		if (++dispatched <= 24 || dispatched % 600 == 0) {
			std::printf("NHL27XESS status=dispatched count=%llu wait=%llu signal=%llu reset=%d jitter=%.4f,%.4f "
			            "mv_scale=%.2f,%.2f exposure=%g\n",
			            static_cast<unsigned long long>(dispatched), static_cast<unsigned long long>(wait_value),
			            static_cast<unsigned long long>(signal_value), p.reset ? 1 : 0, double(exec.jitterOffsetX),
			            double(exec.jitterOffsetY), double(p.motion_scale_x), double(p.motion_scale_y),
			            double(exec.exposureScale));
			std::fflush(stdout);
		}
		return Result::Upscaled;
	}

	void Shutdown() {
		Release();
		if (semaphore != VK_NULL_HANDLE) {
			VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroySemaphore(Vk(), semaphore, nullptr);
			semaphore = VK_NULL_HANDLE;
		}
		if (event != nullptr) CloseHandle(event);
		event = nullptr;
		list.Reset();
		for (auto& allocator: allocators) allocator.Reset();
		queue.Reset();
		fence.Reset();
		device.Reset();
		// libxess.dll stays loaded: it may keep threads or callbacks around.
	}
#endif
};

XessBridge::XessBridge(GraphicContext& graphics): m(std::make_unique<Impl>(graphics)) {
#if KYTY_XESS_BRIDGE
	m->Init();
#else
	m->Fail("build without KYTY_ENABLE_XESS (Windows only)");
#endif
}

XessBridge::~XessBridge() {
#if KYTY_XESS_BRIDGE
	m->Shutdown();
#endif
}

bool XessBridge::Usable() const {
	return !m->failed;
}

const std::string& XessBridge::Status() const {
	return m->status;
}

bool XessBridge::NeedsRecreate(vk::Extent2D render, vk::Extent2D display) const {
#if KYTY_XESS_BRIDGE
	return m->context == nullptr || m->render != render || m->display != display;
#else
	(void)render;
	(void)display;
	return false;
#endif
}

bool XessBridge::Ensure(vk::Extent2D render, vk::Extent2D display) {
#if KYTY_XESS_BRIDGE
	return m->Ensure(render, display);
#else
	(void)render;
	(void)display;
	return false;
#endif
}

bool XessBridge::Fresh() const {
	return m->fresh;
}

void XessBridge::ClearFresh() {
	m->fresh = false;
}

vk::Image XessBridge::Image(int index) const {
#if KYTY_XESS_BRIDGE
	return index >= 0 && index < Count ? vk::Image(m->shared[index].image) : vk::Image {};
#else
	(void)index;
	return {};
#endif
}

vk::Semaphore XessBridge::Semaphore() const {
#if KYTY_XESS_BRIDGE
	return vk::Semaphore(m->semaphore);
#else
	return {};
#endif
}

std::pair<uint64_t, uint64_t> XessBridge::NextValues() {
	m->value += 2;
	return {m->value - 1, m->value};
}

XessBridge::Result XessBridge::Dispatch(const XessParams& params, uint64_t wait_value, uint64_t signal_value) {
#if KYTY_XESS_BRIDGE
	return m->Dispatch(params, wait_value, signal_value);
#else
	(void)params;
	(void)wait_value;
	(void)signal_value;
	return Result::NotQueued;
#endif
}

} // namespace Libs::Graphics
