#pragma once

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <memory>
#include <string>

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
// NGX ships DLSS for DirectX 12, so this runs it on a small D3D12 device on the same
// graphics card, exactly like the FSR 4 bridge: five images are shared between the two
// APIs (D3D12 resources imported into Vulkan): colour, depth, motion and an unused
// reactive slot at render size, output at display size. A D3D12 fence imported as a
// Vulkan timeline semaphore orders the work on the GPU:
//   Vulkan copies the inputs in and signals `wait_value` -> D3D12 waits for it, runs DLSS
//   and signals `signal_value` -> the next Vulkan submission waits for that before copying
//   the output.
// Shared images stay in VK_IMAGE_LAYOUT_GENERAL on the Vulkan side and are released to /
// acquired from VK_QUEUE_FAMILY_EXTERNAL around the D3D12 work.
class DlssBridge final {
public:
	enum ImageIndex : int { Color = 0, Depth = 1, Motion = 2, Output = 3, Reactive = 4, Count = 5 };

	explicit DlssBridge(GraphicContext& graphics);
	~DlssBridge(); // The Vulkan owner must have finished all work that uses the shared images.
	DlssBridge(const DlssBridge&)            = delete;
	DlssBridge& operator=(const DlssBridge&) = delete;

	// False once anything failed; Status() says what. The caller then falls back.
	[[nodiscard]] bool Usable() const;
	[[nodiscard]] const std::string& Status() const;
	// True when Ensure() would recreate the shared images (the caller waits for Vulkan first).
	[[nodiscard]] bool NeedsRecreate(vk::Extent2D render, vk::Extent2D display) const;
	// Creates (or recreates) the shared images and the DLSS feature for these sizes.
	[[nodiscard]] bool Ensure(vk::Extent2D render, vk::Extent2D display);
	// True for the first frame after Ensure() created the images (their Vulkan layout is undefined).
	[[nodiscard]] bool Fresh() const;
	void ClearFresh();

	[[nodiscard]] vk::Image Image(int index) const;
	[[nodiscard]] vk::Semaphore Semaphore() const;
	// The two timeline values for the next frame: first = Vulkan signals, second = D3D12 signals.
	[[nodiscard]] std::pair<uint64_t, uint64_t> NextValues();

	// Queues the D3D12 work: wait for wait_value, DLSS, signal signal_value.
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

} // namespace Libs::Graphics
