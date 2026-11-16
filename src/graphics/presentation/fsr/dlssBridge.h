#pragma once

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;

// One frame of DLSS inputs that are not images (same meaning as Fsr4Params;
// DLSS takes no camera parameters and no reactive mask in this version).
struct DlssParams {
	float jitter_x = 0, jitter_y = 0;
	float motion_scale_x = 0, motion_scale_y = 0;
	float near_plane = 0, far_plane = 0, vertical_fov = 0;
	float delta_ms = 0, pre_exposure = 1;
	bool  reset = false;
};

// DLSS Super Resolution for the Vulkan renderer (KYTY_DLSS=1, Windows, builds with
// KYTY_ENABLE_DLSS).
//
// Since build #179 NGX runs through its Vulkan API on the emulator's own instance,
// physical device and device -- the ones that render and present the game. (The D3D12
// bridge used before initialized NGX on a private side device that never rendered;
// NGX answered SuperSampling_Available=1 there but feature creation died with
// PlatformError when the DLSS feature tried to create its kernels on that device --
// Falcon's #176 log. Both working DLSS integrations, hilals' bbport bridge for
// nhl24pc and upstream KytyPS5's DLSS, use Vulkan NGX on the rendering device.)
//
// The bridge owns five plain Vulkan images (colour, depth, motion and an unused
// reactive slot at render size, output at display size), each with a view for NGX.
// The caller copies the inputs in, releases the images to VK_QUEUE_FAMILY_EXTERNAL
// and signals the bridge timeline semaphore; Dispatch() then records the NGX
// evaluation into a bridge command buffer -- acquiring the images back from
// VK_QUEUE_FAMILY_EXTERNAL -- and submits it on the graphics queue between the
// caller's two submissions, waiting for the first timeline value and signalling the
// second, after releasing the images to VK_QUEUE_FAMILY_EXTERNAL again so the caller
// can acquire the output. The images stay in VK_IMAGE_LAYOUT_GENERAL throughout.
class DlssBridge final {
public:
	enum ImageIndex : int { Color = 0, Depth = 1, Motion = 2, Output = 3, Reactive = 4, Count = 5 };

	explicit DlssBridge(GraphicContext& graphics);
	~DlssBridge(); // The Vulkan owner must have finished all work that uses the bridge images.
	DlssBridge(const DlssBridge&)            = delete;
	DlssBridge& operator=(const DlssBridge&) = delete;

	// False once anything failed; Status() says what. The caller then falls back.
	[[nodiscard]] bool Usable() const;
	[[nodiscard]] const std::string& Status() const;
	// True when Ensure() would recreate the bridge images (the caller waits for Vulkan first).
	[[nodiscard]] bool NeedsRecreate(vk::Extent2D render, vk::Extent2D display) const;
	// Creates (or recreates) the bridge images and the DLSS feature for these sizes.
	[[nodiscard]] bool Ensure(vk::Extent2D render, vk::Extent2D display);
	// True for the first frame after Ensure() created the images (their Vulkan layout is undefined).
	[[nodiscard]] bool Fresh() const;
	void ClearFresh();

	[[nodiscard]] vk::Image Image(int index) const;
	[[nodiscard]] vk::Semaphore Semaphore() const;
	// The two timeline values for the next frame: first = Vulkan signals, second = the bridge signals.
	[[nodiscard]] std::pair<uint64_t, uint64_t> NextValues();

	// Queues the NGX evaluation: wait for wait_value, DLSS, signal signal_value.
	enum class Result {
		NotQueued, // nothing will signal signal_value: the caller must not wait for it
		Failed,    // signal_value will be signalled, but the output holds no new frame
		Upscaled,  // signal_value will be signalled after DLSS wrote the output
	};
	[[nodiscard]] Result Dispatch(const DlssParams& params, uint64_t wait_value, uint64_t signal_value);

private:
	struct Impl;
	std::unique_ptr<Impl> m;
};

// NGX needs its own Vulkan extensions on the instance and device it initializes
// against, so window creation calls these while building the extension lists. Both
// honour the same KYTY_DLSS=1 gate as the interception and are no-ops in builds
// without DLSS (or when NGX cannot be queried, e.g. no NVIDIA driver): the bridge
// then simply fails its capability check at run time and the caller falls back.
void DlssAppendInstanceExtensions(std::vector<const char*>& extensions,
                                  const std::vector<vk::ExtensionProperties>& available);
void DlssAppendDeviceExtensions(vk::Instance instance, vk::PhysicalDevice physical,
                                const std::vector<vk::ExtensionProperties>& available,
                                std::vector<const char*>& extensions);

} // namespace Libs::Graphics
