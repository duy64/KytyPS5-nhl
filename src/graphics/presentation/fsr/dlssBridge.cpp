#include "graphics/presentation/fsr/dlssBridge.h"

#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
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

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_params.h"
#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers_vk.h"
#else
#define KYTY_DLSS_BRIDGE 0
#endif

namespace Libs::Graphics {

#if KYTY_DLSS_BRIDGE
namespace {

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
	wchar_t      path[MAX_PATH] {};
	const DWORD  n = GetModuleFileNameW(nullptr, path, MAX_PATH);
	std::wstring dir(path, n);
	const auto   slash = dir.find_last_of(L"\\/");
	if (slash != std::wstring::npos) dir.resize(slash + 1);
	return dir;
}

// NGX log sink (the LoggingInfo half of the FeatureCommonInfo passed at init):
// forwards NGX's own messages to the emulator log so they land in last-run.log next
// to the NHL27DLSS lines. NGX may call this from any thread; printf is safe for that.
void NVSDK_CONV NgxLogCallback(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
	std::printf("NHL27DLSS ngx-log %s\n", message != nullptr ? message : "");
	std::fflush(stdout);
}

// Name for an NVSDK_NGX_Result value, so the diag caps line is self-explanatory in a
// tester log (FeatureInitResult is the discriminator between the remaining theories).
const char* NgxResultName(NVSDK_NGX_Result result) {
	switch (result) {
		case NVSDK_NGX_Result_Success: return "Success";
		case NVSDK_NGX_Result_FAIL_FeatureNotSupported: return "FeatureNotSupported";
		case NVSDK_NGX_Result_FAIL_PlatformError: return "PlatformError";
		case NVSDK_NGX_Result_FAIL_FeatureNotFound: return "FeatureNotFound";
		case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "UnableToInitializeFeature";
		case NVSDK_NGX_Result_FAIL_OutOfDate: return "OutOfDate";
		case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath: return "UnableToWriteToAppDataPath";
		case NVSDK_NGX_Result_FAIL_Denied: return "Denied";
		default: return "Unknown";
	}
}

// Bridge image formats, unchanged from the D3D12 bridge: guest colour is B10G11R11,
// depth float, motion RG16F, output RGBA16F, reactive R8.
constexpr std::array<VkFormat, DlssBridge::Count> kFormats = {VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R32_SFLOAT,
                                                              VK_FORMAT_R16G16_SFLOAT,
                                                              VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8_UNORM};

// The same KYTY_DLSS=1 gate as the guest FSR 3.1 interception (nhl27Fsr3.cpp), read
// directly: window creation happens before any bridge exists.
bool DlssWanted() {
	const char* value = std::getenv("KYTY_DLSS");
	return value != nullptr && std::strcmp(value, "1") == 0;
}

// The one NGX feature description shared by the window-creation extension queries
// (which must run before NGX is initialized) and by Init(): same project ID, data
// path and FeatureCommonInfo, so the extensions enabled at device creation always
// match the feature NGX is later asked to create.
struct NgxDiscovery {
	std::wstring                   data_path    = AppDirectory();
	const wchar_t*                 feature_path = data_path.c_str();
	NVSDK_NGX_FeatureCommonInfo    common {};
	NVSDK_NGX_FeatureDiscoveryInfo info {};

	NgxDiscovery() {
		common.PathListInfo            = {&feature_path, 1};
		common.LoggingInfo             = {NgxLogCallback, NVSDK_NGX_LOGGING_LEVEL_VERBOSE, false};
		info.SDKVersion                = NVSDK_NGX_Version_API;
		info.FeatureID                 = NVSDK_NGX_Feature_SuperSampling;
		info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
		info.Identifier.v.ProjectDesc  = {kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0"};
		info.ApplicationDataPath       = data_path.c_str();
		info.FeatureInfo               = &common;
	}
};

const NgxDiscovery& Discovery() {
	static const NgxDiscovery discovery;
	return discovery;
}

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
		VkImage               image  = VK_NULL_HANDLE;
		VkDeviceMemory        memory = VK_NULL_HANDLE;
		VkImageView           view   = VK_NULL_HANDLE;
		NVSDK_NGX_Resource_VK resource {};
	};

	VkCommandPool                     pool          = VK_NULL_HANDLE;
	std::array<VkCommandBuffer, Ring> buffers {};
	VkCommandBuffer                   create_buffer = VK_NULL_HANDLE;
	std::array<uint64_t, Ring>        buffer_value {};
	int                               ring = 0;
	VkSemaphore                       semaphore = VK_NULL_HANDLE;
	std::array<Shared, Count>         shared {};
	uint64_t                          last_signal = 0;
	uint64_t                          dispatched  = 0;

	// NGX state (one instance for the rendering device).
	bool                        ngx_ready         = false;
	NVSDK_NGX_Parameter*        capability_params = nullptr;
	NVSDK_NGX_Parameter*        params            = nullptr;
	NVSDK_NGX_Handle*           feature           = nullptr;
	NVSDK_NGX_PerfQuality_Value quality           = NVSDK_NGX_PerfQuality_Value_MaxQuality;

	[[nodiscard]] VkDevice Vk() const {
		return static_cast<VkDevice>(graphics.device);
	}

	bool Init() {
		const VkDevice vk_device = Vk();

		// The adapter NGX will initialize against is the Vulkan physical device
		// itself now (no second device to match by LUID); the LUID is still printed
		// so the line ties to the Windows adapter list in the same log.
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
		const auto& device_properties = graphics.physical_device_properties;
		std::printf("NHL27DLSS diag adapter=\"%s\" luid=%08x:%08x vendor=0x%04x device=0x%04x\n",
		            device_properties.deviceName.data(), static_cast<unsigned int>(luid.HighPart),
		            static_cast<unsigned int>(luid.LowPart), static_cast<unsigned int>(device_properties.vendorID),
		            static_cast<unsigned int>(device_properties.deviceID));
		std::fflush(stdout);

		// NGX itself: the driver-side NGX Core loads the signed nvngx_dlss.dll from the
		// emulator's folder (installed there by CMake). The data path holds NGX logs and
		// temporary files and must be writable. The FeatureCommonInfo names that same
		// folder explicitly and registers the log callback: a nullptr info leaves the
		// feature-DLL discovery to the default search only and gives NGX no log voice,
		// while both working NGX integrations (hilals' bridge, upstream KytyPS5) pass it.
		// The discovery description is shared with the window-creation extension
		// queries, so the device was created with the extensions this feature needs.
		const auto& discovery = Discovery();
		const auto  inited    = NVSDK_NGX_VULKAN_Init_with_ProjectID(
            kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", discovery.data_path.c_str(),
            static_cast<VkInstance>(graphics.instance), static_cast<VkPhysicalDevice>(graphics.physical_device),
            vk_device, VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
            VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr, &discovery.common);
		std::printf("NHL27DLSS diag ngx_init result=%d (0x%08x) project_id=%s engine_type=%d\n",
		            static_cast<int>(inited), static_cast<unsigned int>(inited), kProjectId,
		            static_cast<int>(NVSDK_NGX_ENGINE_TYPE_CUSTOM));
		std::fflush(stdout);
		if (inited != NVSDK_NGX_Result_Success) {
			return Fail("NVSDK_NGX_VULKAN_Init_with_ProjectID returned " + std::to_string(inited));
		}
		ngx_ready = true;
		if (NVSDK_NGX_VULKAN_GetCapabilityParameters(&capability_params) != NVSDK_NGX_Result_Success ||
		    capability_params == nullptr) {
			return Fail("NVSDK_NGX_VULKAN_GetCapabilityParameters");
		}
		// Diagnostics (#174): dump every SuperSampling capability value NGX reports, so one
		// tester log shows whether the gate is the adapter, the driver, or per-app
		// availability. Values left at the sentinel were not populated by NGX.
		unsigned int diag_available = 0xFFFFFFFFu, diag_needs_driver = 0xFFFFFFFFu;
		unsigned int diag_min_major = 0xFFFFFFFFu, diag_min_minor = 0xFFFFFFFFu;
		int          diag_feature_init = 0x7FFFFFFF;
		capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &diag_available);
		capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &diag_needs_driver);
		capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &diag_min_major);
		capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &diag_min_minor);
		capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &diag_feature_init);
		std::printf("NHL27DLSS diag caps supersampling_available=%u needs_updated_driver=%u "
		            "min_driver_version=%u.%u feature_init_result=%d (0x%08x %s)\n",
		            diag_available, diag_needs_driver, diag_min_major, diag_min_minor, diag_feature_init,
		            static_cast<unsigned int>(diag_feature_init),
		            NgxResultName(static_cast<NVSDK_NGX_Result>(diag_feature_init)));
		std::fflush(stdout);
		unsigned int available = 0, needs_driver = 0;
		capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
		capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
		if (available == 0) {
			return Fail(needs_driver != 0 ? "DLSS needs a newer NVIDIA driver"
		                              : "DLSS Super Sampling is not available on this GPU");
		}
		if (NVSDK_NGX_VULKAN_AllocateParameters(&params) != NVSDK_NGX_Result_Success || params == nullptr) {
			return Fail("NVSDK_NGX_VULKAN_AllocateParameters");
		}
		(void)GetTuning();

		VkCommandPoolCreateInfo pool_info {};
		pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pool_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pool_info.queueFamilyIndex = graphics.queue_family;
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateCommandPool(vk_device, &pool_info, nullptr, &pool) != VK_SUCCESS) {
			return Fail("vkCreateCommandPool");
		}
		std::array<VkCommandBuffer, Ring + 1> allocated {};
		VkCommandBufferAllocateInfo           alloc_info {};
		alloc_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		alloc_info.commandPool        = pool;
		alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		alloc_info.commandBufferCount = static_cast<uint32_t>(allocated.size());
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkAllocateCommandBuffers(vk_device, &alloc_info, allocated.data()) !=
		    VK_SUCCESS) {
			return Fail("vkAllocateCommandBuffers");
		}
		std::copy(allocated.begin(), allocated.begin() + Ring, buffers.begin());
		create_buffer = allocated[Ring];

		// A plain timeline semaphore orders the bridge with the caller (the D3D12
		// bridge imported its fence into one; on one device no import is needed).
		VkSemaphoreTypeCreateInfo timeline {};
		timeline.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
		timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		timeline.initialValue  = 0;
		VkSemaphoreCreateInfo semaphore_info {};
		semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		semaphore_info.pNext = &timeline;
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateSemaphore(vk_device, &semaphore_info, nullptr, &semaphore) !=
		    VK_SUCCESS) {
			return Fail("vkCreateSemaphore");
		}
		status = "DLSS ready";
		return true;
	}

	void WaitIdle() {
		if (semaphore != VK_NULL_HANDLE && last_signal != 0) {
			VkSemaphoreWaitInfo wait {};
			wait.sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
			wait.semaphoreCount = 1;
			wait.pSemaphores    = &semaphore;
			wait.pValues        = &last_signal;
			VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitSemaphores(Vk(), &wait, 5000000000ull);
		}
	}

	void Release() {
		WaitIdle();
		if (feature != nullptr) {
			NVSDK_NGX_VULKAN_ReleaseFeature(feature);
			feature = nullptr;
		}
		const VkDevice vk_device = Vk();
		for (auto& s: shared) {
			if (s.view != VK_NULL_HANDLE) VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyImageView(vk_device, s.view, nullptr);
			if (s.image != VK_NULL_HANDLE) VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyImage(vk_device, s.image, nullptr);
			if (s.memory != VK_NULL_HANDLE) VULKAN_HPP_DEFAULT_DISPATCHER.vkFreeMemory(vk_device, s.memory, nullptr);
			s = {};
		}
		render  = vk::Extent2D {};
		display = vk::Extent2D {};
	}

	bool CreateImage(int index, vk::Extent2D extent) {
		auto&          s         = shared[index];
		const VkDevice vk_device = Vk();
		VkImageCreateInfo info {};
		info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		info.imageType     = VK_IMAGE_TYPE_2D;
		info.format        = kFormats[index];
		info.extent        = {extent.width, extent.height, 1};
		info.mipLevels     = 1;
		info.arrayLayers   = 1;
		info.samples       = VK_SAMPLE_COUNT_1_BIT;
		info.tiling        = VK_IMAGE_TILING_OPTIMAL;
		info.usage         = index == Output ? (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT)
		                                     : (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
		info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateImage(vk_device, &info, nullptr, &s.image) != VK_SUCCESS) {
			return Fail("vkCreateImage");
		}
		VkMemoryRequirements requirements {};
		VULKAN_HPP_DEFAULT_DISPATCHER.vkGetImageMemoryRequirements(vk_device, s.image, &requirements);
		VkPhysicalDeviceMemoryProperties memory_properties {};
		VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceMemoryProperties(
		    static_cast<VkPhysicalDevice>(graphics.physical_device), &memory_properties);
		uint32_t type = UINT32_MAX;
		for (uint32_t i = 0; i < memory_properties.memoryTypeCount && type == UINT32_MAX; ++i) {
			if ((requirements.memoryTypeBits & (1u << i)) != 0 &&
			    (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
				type = i;
			}
		}
		for (uint32_t i = 0; i < memory_properties.memoryTypeCount && type == UINT32_MAX; ++i) {
			if ((requirements.memoryTypeBits & (1u << i)) != 0) type = i;
		}
		if (type == UINT32_MAX) {
			return Fail("no memory type for the bridge image");
		}
		VkMemoryAllocateInfo allocate {};
		allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocate.allocationSize  = requirements.size;
		allocate.memoryTypeIndex = type;
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkAllocateMemory(vk_device, &allocate, nullptr, &s.memory) != VK_SUCCESS) {
			return Fail("vkAllocateMemory");
		}
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkBindImageMemory(vk_device, s.image, s.memory, 0) != VK_SUCCESS) {
			return Fail("vkBindImageMemory");
		}
		VkImageSubresourceRange range {};
		range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		range.levelCount = 1;
		range.layerCount = 1;
		VkImageViewCreateInfo view_info {};
		view_info.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view_info.image            = s.image;
		view_info.viewType         = VK_IMAGE_VIEW_TYPE_2D;
		view_info.format           = kFormats[index];
		view_info.subresourceRange = range;
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateImageView(vk_device, &view_info, nullptr, &s.view) != VK_SUCCESS) {
			return Fail("vkCreateImageView");
		}
		s.resource = NVSDK_NGX_Create_ImageView_Resource_VK(s.view, s.image, range, kFormats[index], extent.width,
		                                                    extent.height, index == Output);
		return true;
	}

	bool Ensure(vk::Extent2D new_render, vk::Extent2D new_display) {
		if (failed) return false;
		if (render == new_render && display == new_display && feature != nullptr) return true;
		Release();
		for (int i = 0; i < Count; ++i) {
			if (!CreateImage(i, i == Output ? new_display : new_render)) return false;
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
		// CreateFeature records into a command buffer that must run on the queue; Ensure is
		// only called with the Vulkan scheduler drained, so a CPU fence wait is fine here.
		const VkDevice vk_device = Vk();
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkResetCommandBuffer(create_buffer, 0) != VK_SUCCESS) {
			return Fail("vkResetCommandBuffer (create)");
		}
		VkCommandBufferBeginInfo begin {};
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkBeginCommandBuffer(create_buffer, &begin) != VK_SUCCESS) {
			return Fail("vkBeginCommandBuffer (create)");
		}
		const auto created = NGX_VULKAN_CREATE_DLSS_EXT1(vk_device, create_buffer, 1, 1, &feature, params, &create);
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkEndCommandBuffer(create_buffer) != VK_SUCCESS) {
			return Fail("vkEndCommandBuffer (create)");
		}
		VkFence           fence = VK_NULL_HANDLE;
		VkFenceCreateInfo fence_info {};
		fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateFence(vk_device, &fence_info, nullptr, &fence) != VK_SUCCESS) {
			return Fail("vkCreateFence (create)");
		}
		VkSubmitInfo submit {};
		submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers    = &create_buffer;
		VkResult submitted        = VK_ERROR_INITIALIZATION_FAILED;
		{
			Common::LockGuard lock(graphics.queue_mutex);
			submitted = VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueSubmit(static_cast<VkQueue>(graphics.queue), 1, &submit,
			                                                        fence);
		}
		VkResult waited = VK_ERROR_INITIALIZATION_FAILED;
		if (submitted == VK_SUCCESS) {
			waited = VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitForFences(vk_device, 1, &fence, VK_TRUE, 30000000000ull);
		}
		VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyFence(vk_device, fence, nullptr);
		if (submitted != VK_SUCCESS || waited != VK_SUCCESS) {
			return Fail("DLSS feature creation submission failed");
		}
		if (created != NVSDK_NGX_Result_Success || feature == nullptr) {
			feature = nullptr;
			return Fail("NVSDK_NGX_VULKAN_CreateFeature(SuperSampling) returned " + std::to_string(created));
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
		const VkDevice vk_device = Vk();
		if (buffer_value[ring] != 0) {
			VkSemaphoreWaitInfo wait {};
			wait.sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
			wait.semaphoreCount = 1;
			wait.pSemaphores    = &semaphore;
			wait.pValues        = &buffer_value[ring];
			if (VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitSemaphores(vk_device, &wait, 5000000000ull) != VK_SUCCESS) {
				Fail("vkWaitSemaphores (command buffer ring)");
				return Result::NotQueued;
			}
		}
		const VkCommandBuffer cmd = buffers[ring];
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkResetCommandBuffer(cmd, 0) != VK_SUCCESS) {
			Fail("vkResetCommandBuffer");
			return Result::NotQueued;
		}
		VkCommandBufferBeginInfo begin {};
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
			Fail("vkBeginCommandBuffer");
			return Result::NotQueued;
		}
		// The caller released the images to VK_QUEUE_FAMILY_EXTERNAL after copying the
		// inputs in; acquire the ones NGX reads and writes, evaluate, and release them
		// back so the caller can acquire the output. The reactive slot is never passed
		// to NGX (no reactive mask in this version), so it needs no transfer. Layouts
		// stay VK_IMAGE_LAYOUT_GENERAL throughout, as on the D3D12 bridge.
		constexpr std::array<int, 4>                  used = {Color, Depth, Motion, Output};
		std::array<VkImageMemoryBarrier, used.size()> barriers {};
		for (size_t i = 0; i < used.size(); ++i) {
			auto& barrier               = barriers[i];
			barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
			barrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
			barrier.dstQueueFamilyIndex = graphics.queue_family;
			barrier.image               = shared[used[i]].image;
			barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			barrier.dstAccessMask       = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		}
		VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		                                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
		                                                   nullptr, static_cast<uint32_t>(barriers.size()),
		                                                   barriers.data());
		const auto&                   tuning = GetTuning();
		NVSDK_NGX_VK_DLSS_Eval_Params eval {};
		eval.Feature.pInColor          = &shared[Color].resource;
		eval.Feature.pInOutput         = &shared[Output].resource;
		eval.pInDepth                  = &shared[Depth].resource;
		eval.pInMotionVectors          = &shared[Motion].resource;
		eval.InJitterOffsetX           = p.jitter_x * tuning.jitter_x;
		eval.InJitterOffsetY           = p.jitter_y * tuning.jitter_y;
		eval.InRenderSubrectDimensions = {render.width, render.height};
		eval.InReset                   = p.reset ? 1 : 0;
		eval.InMVScaleX                = p.motion_scale_x * tuning.mv_x;
		eval.InMVScaleY                = p.motion_scale_y * tuning.mv_y;
		eval.InPreExposure             = tuning.unit_pre_exposure ? 1.0f : p.pre_exposure;
		eval.InFrameTimeDeltaInMsec    = p.delta_ms;
		const auto result              = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, feature, params, &eval);
		for (auto& barrier: barriers) {
			barrier.srcAccessMask       = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			barrier.dstAccessMask       = 0;
			barrier.srcQueueFamilyIndex = graphics.queue_family;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
		}
		VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		                                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
		                                                   nullptr, static_cast<uint32_t>(barriers.size()),
		                                                   barriers.data());
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkEndCommandBuffer(cmd) != VK_SUCCESS) {
			Fail("vkEndCommandBuffer");
			return Result::NotQueued;
		}
		// Even when DLSS refused the frame, the Vulkan side waits for signal_value:
		// always signal it (with no command buffer when the evaluation failed, like
		// the D3D12 bridge skipping ExecuteCommandLists).
		VkTimelineSemaphoreSubmitInfo timeline {};
		timeline.sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
		timeline.waitSemaphoreValueCount   = 1;
		timeline.pWaitSemaphoreValues      = &wait_value;
		timeline.signalSemaphoreValueCount = 1;
		timeline.pSignalSemaphoreValues    = &signal_value;
		const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		VkSubmitInfo               submit {};
		submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.pNext                = &timeline;
		submit.waitSemaphoreCount   = 1;
		submit.pWaitSemaphores      = &semaphore;
		submit.pWaitDstStageMask    = &wait_stage;
		submit.commandBufferCount   = result == NVSDK_NGX_Result_Success ? 1u : 0u;
		submit.pCommandBuffers      = &cmd;
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores    = &semaphore;
		VkResult submitted          = VK_ERROR_INITIALIZATION_FAILED;
		{
			Common::LockGuard lock(graphics.queue_mutex);
			submitted = VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueSubmit(static_cast<VkQueue>(graphics.queue), 1, &submit,
			                                                        VK_NULL_HANDLE);
		}
		if (submitted != VK_SUCCESS) {
			Fail("vkQueueSubmit");
			return Result::NotQueued;
		}
		buffer_value[ring] = signal_value;
		ring               = (ring + 1) % Ring;
		last_signal        = signal_value;
		if (result != NVSDK_NGX_Result_Success) {
			Fail("NVSDK_NGX_VULKAN_EvaluateFeature returned " + std::to_string(result));
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
			NVSDK_NGX_VULKAN_DestroyParameters(params);
			params = nullptr;
		}
		if (capability_params != nullptr) {
			NVSDK_NGX_VULKAN_DestroyParameters(capability_params);
			capability_params = nullptr;
		}
		if (ngx_ready) {
			NVSDK_NGX_VULKAN_Shutdown1(Vk());
			ngx_ready = false;
		}
		if (semaphore != VK_NULL_HANDLE) {
			VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroySemaphore(Vk(), semaphore, nullptr);
			semaphore = VK_NULL_HANDLE;
		}
		if (pool != VK_NULL_HANDLE) {
			VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyCommandPool(Vk(), pool, nullptr);
			pool = VK_NULL_HANDLE;
		}
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

#if KYTY_DLSS_BRIDGE
namespace {

// Appends the NGX-required extension names the driver actually exposes and that are
// not already enabled (the filter upstream KytyPS5's DLSS applies to the same
// lists). The strings point into NGX's own storage and stay valid for the process.
void AppendNgxExtensions(const VkExtensionProperties* required, uint32_t count,
                         const std::vector<vk::ExtensionProperties>& available,
                         std::vector<const char*>& extensions) {
	for (uint32_t i = 0; i < count; ++i) {
		const char* name = required[i].extensionName;
		const bool  exposed =
		    std::any_of(available.begin(), available.end(), [name](const vk::ExtensionProperties& e) {
			    return std::strcmp(e.extensionName.data(), name) == 0;
		    });
		const bool present = std::any_of(extensions.begin(), extensions.end(), [name](const char* e) {
			return std::strcmp(e, name) == 0;
		});
		if (exposed && !present) {
			extensions.push_back(name);
		}
	}
}

} // namespace
#endif

void DlssAppendInstanceExtensions(std::vector<const char*>& extensions,
                                  const std::vector<vk::ExtensionProperties>& available) {
#if KYTY_DLSS_BRIDGE
	if (!DlssWanted()) {
		return;
	}
	uint32_t               count    = 0;
	VkExtensionProperties* required = nullptr;
	if (NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&Discovery().info, &count, &required) !=
	    NVSDK_NGX_Result_Success) {
		return;
	}
	AppendNgxExtensions(required, count, available, extensions);
#else
	(void)extensions;
	(void)available;
#endif
}

void DlssAppendDeviceExtensions(vk::Instance instance, vk::PhysicalDevice physical,
                                const std::vector<vk::ExtensionProperties>& available,
                                std::vector<const char*>& extensions) {
#if KYTY_DLSS_BRIDGE
	if (!DlssWanted()) {
		return;
	}
	const auto vk_instance = static_cast<VkInstance>(instance);
	const auto vk_physical = static_cast<VkPhysicalDevice>(physical);
	NVSDK_NGX_FeatureRequirement requirement {};
	const auto                   req_result =
	    NVSDK_NGX_VULKAN_GetFeatureRequirements(vk_instance, vk_physical, &Discovery().info, &requirement);
	uint32_t               count    = 0;
	VkExtensionProperties* required = nullptr;
	const auto             ext_result = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
        vk_instance, vk_physical, &Discovery().info, &count, &required);
	// The "vk" line doubles as the build marker: its presence in a log (and in the
	// binary) proves the Vulkan NGX path is the compiled one.
	std::printf("NHL27DLSS vk requirements result=%d feature_supported=%u min_hw_arch=%u extensions result=%d "
	            "count=%u\n",
	            static_cast<int>(req_result), static_cast<unsigned>(requirement.FeatureSupported),
	            requirement.MinHWArchitecture, static_cast<int>(ext_result), count);
	std::fflush(stdout);
	if (ext_result == NVSDK_NGX_Result_Success) {
		AppendNgxExtensions(required, count, available, extensions);
	}
#else
	(void)instance;
	(void)physical;
	(void)available;
	(void)extensions;
#endif
}

} // namespace Libs::Graphics
