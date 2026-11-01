// Tier 1 decision fixture for the texture-cache partial-overlap tail
// (tailfix Stage A). No GPU, no TextureCache instance: it tests the pure
// PlanOverlapCopy tail decision extracted from ResolveOverlap.
//
// Patterns A/B come from the real depth log (guest address/size/format).
// Extents are fixture-chosen (the log has no extents): every image below
// is 1024x1024, pitch 1024, 1 level, 1 layer, linear tile, unless a case
// says otherwise. PlanOverlapCopy does not read extent/size, so this
// choice cannot change a branch outcome; extent enters only through the
// test-side level-0 block count (1024 * 1024 = 1048576 blocks).
//
// Precondition (see the header comment): each case is a shape that has
// REACHED the tail block. PlanOverlapCopy does not reproduce the earlier
// ResolveOverlap branches (equal-address, mip/view); a full-ResolveOverlap
// fixture is Tier 2 work, not this file.
//
// Stage A passes on current code. The Stage B flip is marked below in
// TestCoverageAccounting: when CopyImageRegion lands, regions must cover
// 100% of the requested level-0 blocks with 0 misplaced regions, and the
// Tail cases' uncovered checks flip from "full range" to 0.

#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using namespace Libs::Graphics;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "TextureCacheOverlapTests: failed: %s\n", text);
		std::abort();
	}
}

ImageInfo MakeInfo(uint64_t address, uint64_t size, vk::Format format, uint32_t bytes_per_block,
                   Prospero::ImageType type = Prospero::ImageType::kColor2D,
                   uint32_t samples = 1, bool bgra16 = false,
                   Prospero::BufferFormat guest_format = Prospero::BufferFormat::kInvalid) {
	ImageInfo info;
	info.data.address = address;
	info.data.size = size;
	info.pixel_format = format;
	info.bytes_per_block = bytes_per_block;
	info.extent = {1024, 1024, 1};
	info.resources = {1, 1};
	info.pitch = 1024;
	info.type = type;
	info.samples = samples;
	info.tile_mode = Prospero::TileMode::kLinear;
	info.bgra16 = bgra16;
	info.guest_format = guest_format;
	return info;
}

uint64_t Level0Blocks(const ImageInfo& info) {
	const auto extent = info.BlockExtent();
	return static_cast<uint64_t>(extent.width) * extent.height;
}

uint64_t CoveredBlocks(const OverlapPlan& plan) {
	uint64_t covered = 0;
	for (const auto& region : plan.regions) {
		covered += static_cast<uint64_t>(region.width_blocks) * region.height_blocks;
	}
	return covered;
}

// Pattern A (real log shape): requested depth reinterpreted against a
// color cache at an offset base. Tail on current code; NOT fix-eligible
// (depth involvement is excluded by the Stage B guard set, spec section 3.1).
void TestPatternAOffsetDepth() {
	const auto requested = MakeInfo(0x11c00c0000ull, 0xf00000ull, vk::Format::eD32SfloatS8Uint, 8);
	const auto cached = MakeInfo(0x11c0f00000ull, 0x1fe0000ull, vk::Format::eR8G8B8A8Unorm, 4);
	const auto plan = PlanOverlapCopy(requested, cached, true, false, true);
	Check(plan.branch == OverlapPlanBranch::TailFallThrough, "A: tail branch");
	Check(!plan.fix_eligible, "A: depth shape is not fix-eligible");
	Check(plan.regions.empty(), "A: tail copies nothing");
	Check(CoveredBlocks(plan) == 0, "A: zero blocks covered");
	Check(Level0Blocks(requested) == 1048576, "A: requested level-0 blocks");
}

// Pattern B, literally as specified (same base, different format/size).
// CORRECTION to the spec: current code takes the v2 MergedCopy branch
// here, not the tail — same base, samples/color/type equal, formats differ
// but both non-depth with blockSize 4 == 4. Asserting Tail for B would be
// a wrong regression anchor; MergedCopy is the anchor.
void TestPatternBSameBase() {
	const auto requested = MakeInfo(0x11c00c0000ull, 0xf00000ull, vk::Format::eR32Sfloat, 4);
	const auto cached = MakeInfo(0x11c00c0000ull, 0x1c0000ull, vk::Format::eA2B10G10R10UnormPack32, 4);
	const auto plan = PlanOverlapCopy(requested, cached, true, false, true);
	Check(plan.branch == OverlapPlanBranch::MergedCopy, "B: v2 merged-copy branch");
	Check(!plan.fix_eligible, "B: same base needs no offset fix");
}

// Pattern B-prime: B's formats with an OFFSET base — the true Stage B
// shape. Cached base 0x11c0f00000 lies inside the requested range
// [0x11c00c0000, 0x11c0fc0000), so this is a genuine partial overlap.
// Tail on current code; fix-eligible (every v2 guard holds except base).
void TestPatternBOffsetAlias() {
	const auto requested = MakeInfo(0x11c00c0000ull, 0xf00000ull, vk::Format::eR32Sfloat, 4);
	const auto cached = MakeInfo(0x11c0f00000ull, 0x1c0000ull, vk::Format::eA2B10G10R10UnormPack32, 4);
	const auto plan = PlanOverlapCopy(requested, cached, true, false, true);
	Check(plan.branch == OverlapPlanBranch::TailFallThrough, "B-prime: tail branch");
	Check(plan.fix_eligible, "B-prime: offset alias is fix-eligible");
	Check(plan.regions.empty(), "B-prime: tail copies nothing on Stage A");
}

// Format-equal offset alias: also fix-eligible (the format-equal arm of
// the direct-copy condition), same tail behavior on Stage A.
void TestFormatEqualOffsetAlias() {
	const auto requested = MakeInfo(0x11c00c0000ull, 0xf00000ull, vk::Format::eR8G8B8A8Unorm, 4);
	const auto cached = MakeInfo(0x11c0f00000ull, 0x1c0000ull, vk::Format::eR8G8B8A8Unorm, 4);
	const auto plan = PlanOverlapCopy(requested, cached, true, false, true);
	Check(plan.branch == OverlapPlanBranch::TailFallThrough, "format-equal: tail branch");
	Check(plan.fix_eligible, "format-equal: offset alias is fix-eligible");
}

// Guard rejections: each case flips exactly one v2 condition away from
// the fix-eligible B-prime shape. Every one must stay Tail +
// not-fix-eligible on Stage A (and must stay uncopied by Stage B).
void TestGuardRejections() {
	const auto base_requested = MakeInfo(0x11c00c0000ull, 0xf00000ull, vk::Format::eR32Sfloat, 4);
	const auto base_cached = MakeInfo(0x11c0f00000ull, 0x1c0000ull, vk::Format::eA2B10G10R10UnormPack32, 4);

	// Depth involvement (requested depth, cached color).
	{
		const auto requested = MakeInfo(0x11c00c0000ull, 0xf00000ull, vk::Format::eD32Sfloat, 4);
		const auto plan = PlanOverlapCopy(requested, base_cached, true, false, true);
		Check(plan.branch == OverlapPlanBranch::TailFallThrough, "guard depth: tail");
		Check(!plan.fix_eligible, "guard depth: not fix-eligible");
	}
	// Sample-count mismatch.
	{
		auto cached = base_cached;
		cached.samples = 2;
		const auto plan = PlanOverlapCopy(base_requested, cached, true, false, true);
		Check(plan.branch == OverlapPlanBranch::TailFallThrough, "guard samples: tail");
		Check(!plan.fix_eligible, "guard samples: not fix-eligible");
	}
	// Color-transform mismatch (requested SwapBgra16 via bgra16, cached None).
	{
		auto requested = base_requested;
		requested.bgra16 = true;
		const auto plan = PlanOverlapCopy(requested, base_cached, true, false, true);
		Check(plan.branch == OverlapPlanBranch::TailFallThrough, "guard color: tail");
		Check(!plan.fix_eligible, "guard color: not fix-eligible");
	}
	// Type mismatch (cached volume).
	{
		auto cached = base_cached;
		cached.type = Prospero::ImageType::kColor3D;
		const auto plan = PlanOverlapCopy(base_requested, cached, true, false, true);
		Check(plan.branch == OverlapPlanBranch::TailFallThrough, "guard type: tail");
		Check(!plan.fix_eligible, "guard type: not fix-eligible");
	}
	// Block-size mismatch (4 vs 16, formats differ, both non-depth).
	{
		const auto requested = MakeInfo(0x11c00c0000ull, 0xf00000ull, vk::Format::eR8G8B8A8Unorm, 4);
		const auto cached = MakeInfo(0x11c0f00000ull, 0x1c0000ull, vk::Format::eR32G32B32A32Sfloat, 16);
		const auto plan = PlanOverlapCopy(requested, cached, true, false, true);
		Check(plan.branch == OverlapPlanBranch::TailFallThrough, "guard block size: tail");
		Check(!plan.fix_eligible, "guard block size: not fix-eligible");
	}
	// Speculative lookup (results discarded anyway).
	{
		const auto plan = PlanOverlapCopy(base_requested, base_cached, true, true, true);
		Check(plan.branch == OverlapPlanBranch::TailFallThrough, "guard speculative: tail");
		Check(!plan.fix_eligible, "guard speculative: not fix-eligible");
	}
	// No prior resolution (merged absent): the tail returns empty, not a hole.
	{
		const auto plan = PlanOverlapCopy(base_requested, base_cached, false, false, true);
		Check(plan.branch == OverlapPlanBranch::TailFallThrough, "guard no-merged: tail");
		Check(!plan.fix_eligible, "guard no-merged: not fix-eligible");
	}
	// Not safe to delete (heap refs / recent access, via safe_to_delete=false).
	{
		const auto plan = PlanOverlapCopy(base_requested, base_cached, true, false, false);
		Check(plan.branch == OverlapPlanBranch::TailFallThrough, "guard unsafe: tail");
		Check(!plan.fix_eligible, "guard unsafe: not fix-eligible");
	}
}

// The caller-side safe_to_delete computation, verbatim from
// textureCache.cpp:1180-1182 (NumFramesBeforeRemoval = 32): tick age must
// be STRICTLY greater than 32, and heap refs must be zero.
void TestSafeToDelete() {
	Check(OverlapSafeToDelete(1000, 967, 0), "safe: age 33 passes");
	Check(!OverlapSafeToDelete(1000, 968, 0), "safe: age 32 fails (strictly greater)");
	Check(!OverlapSafeToDelete(1000, 100, 1), "safe: heap refs fail");
	Check(OverlapSafeToDelete(100, 100, 0) == false, "safe: age 0 fails");
	// Clock running backwards is clamped by the min(): age 0, not negative.
	Check(!OverlapSafeToDelete(100, 200, 0), "safe: backwards clock fails");
}

// Coverage accounting on Stage A: regions are empty, so covered = 0 and
// the whole requested level-0 range is uncovered for every Tail case.
// STAGE B FLIP (do not enable until CopyImageRegion lands): for every
// fix-eligible case, regions must cover 100% of the requested level-0
// blocks (uncovered == 0), every region's dst offset must equal
// (guest_addr - merged_base) mapped through the MERGED layout (0 misplaced),
// and the guard-rejection cases must still have empty regions.
void TestCoverageAccounting() {
	const auto requested = MakeInfo(0x11c00c0000ull, 0xf00000ull, vk::Format::eR32Sfloat, 4);
	const auto cached = MakeInfo(0x11c0f00000ull, 0x1c0000ull, vk::Format::eA2B10G10R10UnormPack32, 4);
	const auto plan = PlanOverlapCopy(requested, cached, true, false, true);
	Check(CoveredBlocks(plan) == 0, "coverage: Stage A covers nothing");
	Check(Level0Blocks(requested) - CoveredBlocks(plan) == 1048576,
	      "coverage: Stage A leaves the full requested range uncovered");
}

} // namespace

int main() {
	TestPatternAOffsetDepth();
	TestPatternBSameBase();
	TestPatternBOffsetAlias();
	TestFormatEqualOffsetAlias();
	TestGuardRejections();
	TestSafeToDelete();
	TestCoverageAccounting();
	std::puts("TextureCacheOverlapTests: all cases passed");
	return 0;
}