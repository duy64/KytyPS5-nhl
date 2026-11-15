#include "graphics/presentation/fsr/dlssBridge.h"

#include "graphics/host_gpu/graphicContext.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#if KYTY_ENABLE_DLSS && defined(_WIN32)
#define KYTY_DLSS_BRIDGE 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <vulkan/vulkan_win32.h>
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_params.h"
#include "nvsdk_ngx_helpers_d3d.h"
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#else
#define KYTY_DLSS_BRIDGE 0
#endif

namespace Libs::Graphics {

#if KYTY_DLSS_BRIDGE
namespace {
using Microsoft::WRL::ComPtr;

// Self-generated NGX project ID for KyTy (the driver only checks that it is GUID-like;
// NVSDK_NGX_ENGINE_TYPE_CUSTOM is the documented path without an NVIDIA-issued app ID).
constexpr const char* kProjectId = "bca9f413-41aa-46c2-b12b-f3bfc9e2130e";

// Test switches (read once), mirroring the KYTY_FSR4_* switches:
//   KYTY_DLSS_QUALITY=<name>    maxperf, balanced, maxquality, ultraperformance,
//                               ultraquality or dlaa; default: picked from the upscale ratio
//   KYTY_DLSS_JITTER=<x>,<y>    multiply the jitter (default 1,1; -1,1 flips x; 0,0 turns it off)
//   KYTY_DLSS_MV=<x>,<y>        multiply the motion-vector scale (default 1,1)
//   KYTY_DLSS_PREEXPOSURE=1     pass pre-exposure 1 instead of the game's value
//   KYTY_DLSS_AUTOEXPOSURE=0    do not let DLSS compute its own exposure (default on: no
//                               exposure texture is passed, so DLSS needs the flag)
struct Tuning {
	std::string quality;
	float       jitter_x = 1, jitter_y = 1;
	float       mv_x = 1, mv_y = 1;
	bool        unit_pre_exposure = false;
	bool        auto_exposure     = true;
};
const Tuning& GetTuning() {
	static const Tuning tuning = [] {
		Tuning t;
		if (const char* v = std::getenv("KYTY_DLSS_QUALITY")) t.quality = v;
		if (const char* v = std::getenv("KYTY_DLSS_JITTER")) {
			if (std::sscanf(v, "%f,%f", &t.jitter_x, &t.jitter_y) != 2) t.jitter_x = t.jitter_y = 1;
		}
		if (const char* v = std::getenv("KYTY_DLSS_MV")) {
			if (std::sscanf(v, "%f,%f", &t.mv_x, &t.mv_y) != 2) t.mv_x = t.mv_y = 1;
		}
		if (const char* v = std::getenv("KYTY_DLSS_PREEXPOSURE")) t.unit_pre_exposure = v[0] == '1';
		if (const char* v = std::getenv("KYTY_DLSS_AUTOEXPOSURE")) t.auto_exposure = v[0] != '0';
		std::printf("NHL27DLSS tuning quality=%s jitter=%g,%g mv=%g,%g pre_exposure=%s auto_exposure=%d\n",
		            t.quality.empty() ? "auto" : t.quality.c_str(), double(t.jitter_x), double(t.jitter_y),
		            double(t.mv_x), double(t.mv_y), t.unit_pre_exposure ? "1" : "game",
		            t.auto_exposure ? 1 : 0);
		std::fflush(stdout);
		return t;
	}();
	return tuning;
}

const char* QualityName(NVSDK_NGX_PerfQuality_Value quality) {
	switch (quality) {
		case NVSDK_NGX_PerfQuality_Value_MaxPerf: return "MaxPerf";
		case NVSDK_NGX_PerfQuality_Value_Balanced: return "Balanced";
		case NVSDK_NGX_PerfQuality_Value_MaxQuality: return "MaxQuality";
		case NVSDK_NGX_PerfQuality_Value_UltraPerformance: return "UltraPerformance";
		case NVSDK_NGX_PerfQuality_Value_UltraQuality: return "UltraQuality";
		case NVSDK_NGX_PerfQuality_Value_DLAA: return "DLAA";
	}
	return "?";
}

// The game fixes the render size (its own FSR quality choice), so pick the DLSS mode
// whose scale best matches the actual display/render ratio.
NVSDK_NGX_PerfQuality_Value PickQuality(float ratio) {
	const auto& wanted = GetTuning().quality;
	if (!wanted.empty()) {
		if (wanted == "maxperf") return NVSDK_NGX_PerfQuality_Value_MaxPerf;
		if (wanted == "balanced") return NVSDK_NGX_PerfQuality_Value_Balanced;
		if (wanted == "maxquality") return NVSDK_NGX_PerfQuality_Value_MaxQuality;
		if (wanted == "ultraperformance") return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
		if (wanted == "ultraquality") return NVSDK_NGX_PerfQuality_Value_UltraQuality;
		if (wanted == "dlaa") return NVSDK_NGX_PerfQuality_Value_DLAA;
	}
	if (ratio >= 2.9f) return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
	if (ratio >= 1.95f) return NVSDK_NGX_PerfQuality_Value_MaxPerf;
	if (ratio >= 1.65f) return NVSDK_NGX_PerfQuality_Value_Balanced;
	if (ratio >= 1.45f) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
	if (ratio >= 1.25f) return NVSDK_NGX_PerfQuality_Value_UltraQuality;
	return NVSDK_NGX_PerfQuality_Value_DLAA;
}

std::wstring AppDirectory() {
	wchar_t path[MAX_PATH] {};
	const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
	std::wstring dir(path, n);
	const auto slash = dir.find_last_of(L"\\/");
	if (slash != std::wstring::npos) dir.resize(slash + 1);
	return dir;
}

struct Format {
	DXGI_FORMAT dxgi;
	VkFormat    vk;
};
constexpr std::array<Format, DlssBridge::Count> kFormats {{
    {DXGI_FORMAT_R11G11B10_FLOAT, VK_FORMAT_B10G11R11_UFLOAT_PACK32},
    {DXGI_FORMAT_R32_FLOAT, VK_FORMAT_R32_SFLOAT},
    {DXGI_FORMAT_R16G16_FLOAT, VK_FORMAT_R16G16_SFLOAT},
    {DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT},
    {DXGI_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM},
}};
} // namespace
#endif

struct DlssBridge::Impl {
	GraphicContext& graphics;
	std::string     status = "DLSS not initialized";
	bool            failed = false;
	bool            fresh  = false;
	vk::Extent2D    render {}, display {};
	uint64_t        value = 0;

	explicit Impl(GraphicContext& gfx): graphics(gfx) {}

	bool Fail(const std::string& why) {
		failed = true;
		status = "DLSS unavailable: " + why;
		std::printf("NHL27DLSS status=failed reason=%s\n", why.c_str());
		std::fflush(stdout);
		return false;
	}

#if KYTY_DLSS_BRIDGE
	static constexpr int Ring = 3;
	struct Shared {
		ComPtr<ID3D12Resource> resource;
		VkImage                image  = VK_NULL_HANDLE;
		VkDeviceMemory         memory = VK_NULL_HANDLE;
	};

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
	uint64_t                                         last_signal = 0;
	uint64_t                                         dispatched  = 0;
	PFN_vkGetMemoryWin32HandlePropertiesKHR          get_handle_properties = nullptr;
	PFN_vkImportSemaphoreWin32HandleKHR              import_semaphore      = nullptr;

	// NGX state (one instance for this D3D12 device).
	bool                  ngx_ready = false;
	NVSDK_NGX_Parameter*  capability_params = nullptr;
	NVSDK_NGX_Parameter*  params            = nullptr;
	NVSDK_NGX_Handle*     feature           = nullptr;
	NVSDK_NGX_PerfQuality_Value quality     = NVSDK_NGX_PerfQuality_Value_MaxQuality;

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

		// NGX itself: the driver-side NGX Core loads the signed nvngx_dlss.dll from the
		// emulator's folder (installed there by CMake). The data path holds NGX logs and
		// temporary files and must be writable.
		const std::wstring data_path = AppDirectory();
		const auto inited = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
		                                                        data_path.c_str(), device.Get(), nullptr);
		if (inited != NVSDK_NGX_Result_Success) {
			return Fail("NVSDK_NGX_D3D12_Init_with_ProjectID returned " + std::to_string(inited));
		}
		ngx_ready = true;
		if (NVSDK_NGX_D3D12_GetCapabilityParameters(&capability_params) != NVSDK_NGX_Result_Success ||
		    capability_params == nullptr) {
			return Fail("NVSDK_NGX_D3D12_GetCapabilityParameters");
		}
		unsigned int available = 0, needs_driver = 0;
		capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
		capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
		if (available == 0) {
			return Fail(needs_driver != 0 ? "DLSS needs a newer NVIDIA driver"
			                              : "DLSS Super Sampling is not available on this GPU");
		}
		if (NVSDK_NGX_D3D12_AllocateParameters(&params) != NVSDK_NGX_Result_Success || params == nullptr) {
			return Fail("NVSDK_NGX_D3D12_AllocateParameters");
		}
		(void)GetTuning();
		status = "DLSS ready";
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
		if (feature != nullptr) {
			NVSDK_NGX_D3D12_ReleaseFeature(feature);
			feature = nullptr;
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

	bool Ensure(vk::Extent2D new_render, vk::Extent2D new_display) {
		if (failed) return false;
		if (render == new_render && display == new_display && feature != nullptr) return true;
		Release();
		for (int i = 0; i < Count; ++i) {
			if (!CreateShared(i, i == Output ? new_display : new_render)) return false;
		}
		const float ratio = float(new_display.width) / float(new_render.width);
		quality           = PickQuality(ratio);
		NVSDK_NGX_DLSS_Create_Params create {};
		create.Feature.InWidth            = new_render.width;
		create.Feature.InHeight           = new_render.height;
		create.Feature.InTargetWidth      = new_display.width;
		create.Feature.InTargetHeight     = new_display.height;
		create.Feature.InPerfQualityValue = quality;
		create.InFeatureCreateFlags       = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
		                              NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
		                              NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
		                              (GetTuning().auto_exposure ? NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0);
		create.InEnableOutputSubrects = false;
		// CreateFeature records into a command list that must run on the queue; Ensure is
		// only called with the Vulkan scheduler drained, so a CPU fence wait is fine here.
		auto& allocator = allocators[0];
		if (allocator_value[0] != 0 && fence->GetCompletedValue() < allocator_value[0]) {
			fence->SetEventOnCompletion(allocator_value[0], event);
			WaitForSingleObject(event, 5000);
		}
		if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr))) {
			return Fail("command list reset (create)");
		}
		const auto created = NGX_D3D12_CREATE_DLSS_EXT(list.Get(), 1, 1, &feature, params, &create);
		if (FAILED(list->Close())) {
			return Fail("command list close (create)");
		}
		ID3D12CommandList* lists[] = {list.Get()};
		queue->ExecuteCommandLists(1, lists);
		const uint64_t create_value = ++value;
		queue->Signal(fence.Get(), create_value);
		allocator_value[0] = create_value;
		last_signal        = create_value;
		fence->SetEventOnCompletion(create_value, event);
		WaitForSingleObject(event, 30000);
		if (created != NVSDK_NGX_Result_Success || feature == nullptr) {
			feature = nullptr;
			return Fail("NVSDK_NGX_D3D12_CreateFeature(SuperSampling) returned " + std::to_string(created));
		}
		status = std::string("DLSS bridge: quality ") + QualityName(quality);
		std::printf("NHL27DLSS status=created provider=DLSS-NGX render=%ux%u display=%ux%u quality=%s\n",
		            new_render.width, new_render.height, new_display.width, new_display.height,
		            QualityName(quality));
		std::fflush(stdout);
		render  = new_render;
		display = new_display;
		fresh   = true;
		return true;
	}

	Result Dispatch(const DlssParams& p, uint64_t wait_value, uint64_t signal_value) {
		if (failed || feature == nullptr) return Result::NotQueued;
		auto& allocator = allocators[ring];
		if (allocator_value[ring] != 0 && fence->GetCompletedValue() < allocator_value[ring]) {
			fence->SetEventOnCompletion(allocator_value[ring], event);
			WaitForSingleObject(event, 5000);
		}
		if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr))) {
			Fail("command list reset");
			return Result::NotQueued;
		}
		// DLSS expects its inputs in NON_PIXEL_SHADER_RESOURCE and the output as a UAV;
		// between frames the shared images stay in COMMON, which Vulkan sees as
		// VK_IMAGE_LAYOUT_GENERAL. NGX restores the states it was given after evaluating.
		const auto transition = [this](bool to_dlss) {
			std::array<D3D12_RESOURCE_BARRIER, Count> barriers {};
			for (int i = 0; i < Count; ++i) {
				const auto used = i == Output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
				                              : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
				auto& b                  = barriers[i];
				b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				b.Transition.pResource   = shared[i].resource.Get();
				b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				b.Transition.StateBefore = to_dlss ? D3D12_RESOURCE_STATE_COMMON : used;
				b.Transition.StateAfter  = to_dlss ? used : D3D12_RESOURCE_STATE_COMMON;
			}
			list->ResourceBarrier(Count, barriers.data());
		};
		transition(true);
		const auto& tuning             = GetTuning();
		NVSDK_NGX_D3D12_DLSS_Eval_Params eval {};
		eval.Feature.pInColor          = shared[Color].resource.Get();
		eval.Feature.pInOutput         = shared[Output].resource.Get();
		eval.pInDepth                  = shared[Depth].resource.Get();
		eval.pInMotionVectors          = shared[Motion].resource.Get();
		eval.InJitterOffsetX           = p.jitter_x * tuning.jitter_x;
		eval.InJitterOffsetY           = p.jitter_y * tuning.jitter_y;
		eval.InRenderSubrectDimensions = {render.width, render.height};
		eval.InReset                   = p.reset ? 1 : 0;
		eval.InMVScaleX                = p.motion_scale_x * tuning.mv_x;
		eval.InMVScaleY                = p.motion_scale_y * tuning.mv_y;
		eval.InPreExposure             = tuning.unit_pre_exposure ? 1.0f : p.pre_exposure;
		eval.InFrameTimeDeltaInMsec    = p.delta_ms;
		const auto result              = NGX_D3D12_EVALUATE_DLSS_EXT(list.Get(), feature, params, &eval);
		transition(false);
		if (FAILED(list->Close())) {
			Fail("command list close");
			return Result::NotQueued;
		}
		// Even when DLSS refused the frame, the Vulkan side waits for signal_value: always signal it.
		queue->Wait(fence.Get(), wait_value);
		if (result == NVSDK_NGX_Result_Success) {
			ID3D12CommandList* lists[] = {list.Get()};
			queue->ExecuteCommandLists(1, lists);
		}
		queue->Signal(fence.Get(), signal_value);
		allocator_value[ring] = signal_value;
		ring                  = (ring + 1) % Ring;
		last_signal           = signal_value;
		if (result != NVSDK_NGX_Result_Success) {
			Fail("NVSDK_NGX_D3D12_EvaluateFeature returned " + std::to_string(result));
			return Result::Failed;
		}
		if (++dispatched <= 24 || dispatched % 600 == 0) {
			std::printf("NHL27DLSS status=dispatched count=%llu wait=%llu signal=%llu reset=%d jitter=%.4f,%.4f "
			            "mv_scale=%.2f,%.2f dt_ms=%.3f pre_exposure=%g\n",
			            static_cast<unsigned long long>(dispatched), static_cast<unsigned long long>(wait_value),
			            static_cast<unsigned long long>(signal_value), p.reset ? 1 : 0, double(eval.InJitterOffsetX),
			            double(eval.InJitterOffsetY), double(eval.InMVScaleX), double(eval.InMVScaleY),
			            double(eval.InFrameTimeDeltaInMsec), double(eval.InPreExposure));
			std::fflush(stdout);
		}
		return Result::Upscaled;
	}

	void Shutdown() {
		Release();
		if (params != nullptr) {
			NVSDK_NGX_D3D12_DestroyParameters(params);
			params = nullptr;
		}
		if (capability_params != nullptr) {
			NVSDK_NGX_D3D12_DestroyParameters(capability_params);
			capability_params = nullptr;
		}
		if (ngx_ready) {
			NVSDK_NGX_D3D12_Shutdown1(device.Get());
			ngx_ready = false;
		}
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
	}
#endif
};

DlssBridge::DlssBridge(GraphicContext& graphics): m(std::make_unique<Impl>(graphics)) {
#if KYTY_DLSS_BRIDGE
	m->Init();
#else
	m->Fail("build without KYTY_ENABLE_DLSS (Windows only)");
#endif
}

DlssBridge::~DlssBridge() {
#if KYTY_DLSS_BRIDGE
	m->Shutdown();
#endif
}

bool DlssBridge::Usable() const {
	return !m->failed;
}

const std::string& DlssBridge::Status() const {
	return m->status;
}

bool DlssBridge::NeedsRecreate(vk::Extent2D render, vk::Extent2D display) const {
#if KYTY_DLSS_BRIDGE
	return m->feature == nullptr || m->render != render || m->display != display;
#else
	(void)render;
	(void)display;
	return false;
#endif
}

bool DlssBridge::Ensure(vk::Extent2D render, vk::Extent2D display) {
#if KYTY_DLSS_BRIDGE
	return m->Ensure(render, display);
#else
	(void)render;
	(void)display;
	return false;
#endif
}

bool DlssBridge::Fresh() const {
	return m->fresh;
}

void DlssBridge::ClearFresh() {
	m->fresh = false;
}

vk::Image DlssBridge::Image(int index) const {
#if KYTY_DLSS_BRIDGE
	return index >= 0 && index < Count ? vk::Image(m->shared[index].image) : vk::Image {};
#else
	(void)index;
	return {};
#endif
}

vk::Semaphore DlssBridge::Semaphore() const {
#if KYTY_DLSS_BRIDGE
	return vk::Semaphore(m->semaphore);
#else
	return {};
#endif
}

std::pair<uint64_t, uint64_t> DlssBridge::NextValues() {
	m->value += 2;
	return {m->value - 1, m->value};
}

DlssBridge::Result DlssBridge::Dispatch(const DlssParams& params, uint64_t wait_value, uint64_t signal_value) {
#if KYTY_DLSS_BRIDGE
	return m->Dispatch(params, wait_value, signal_value);
#else
	(void)params;
	(void)wait_value;
	(void)signal_value;
	return Result::NotQueued;
#endif
}

} // namespace Libs::Graphics
