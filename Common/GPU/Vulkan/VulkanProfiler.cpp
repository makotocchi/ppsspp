#include <stdarg.h>

#include "VulkanProfiler.h"
#include "VulkanContext.h"

using namespace PPSSPP_VK;

void VulkanProfiler::Init(VulkanContext *vulkan) {
	vulkan_ = vulkan;

	VkQueryPoolCreateInfo ci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
	ci.queryCount = MAX_QUERY_COUNT;
	ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
	vkCreateQueryPool(vulkan->GetDevice(), &ci, nullptr, &queryPool_);
}

void VulkanProfiler::Shutdown() {
	vkDestroyQueryPool(vulkan_->GetDevice(), queryPool_, nullptr);
}

void VulkanProfiler::BeginFrame(VulkanContext *vulkan, VkCommandBuffer firstCommandBuf) {
	vulkan_ = vulkan;

	// Check for old queries belonging to this frame context that we can log out - these are now
	// guaranteed to be done.
	if (numQueries_ > 0) {
		std::vector<uint64_t> results(numQueries_);
		vkGetQueryPoolResults(vulkan->GetDevice(), queryPool_, 0, numQueries_, sizeof(uint64_t) * numQueries_, results.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);

		double timestampConversionFactor = (double)vulkan_->GetPhysicalDeviceProperties().properties.limits.timestampPeriod * (1.0 / 1000000.0);
		int validBits = vulkan_->GetQueueFamilyProperties(vulkan_->GetGraphicsQueueFamilyIndex()).timestampValidBits;
		uint64_t timestampDiffMask = validBits == 64 ? 0xFFFFFFFFFFFFFFFFULL : ((1ULL << validBits) - 1);

		static const char * const indent[4] = { "", "  ", "    ", "      " };

		if (!scopes_.empty()) {
			NOTICE_LOG(G3D, "Profiling events this frame:");
		}

		// Log it all out.
		for (auto &scope : scopes_) {
			if (scope.endQueryId == -1) {
				NOTICE_LOG(G3D, "Unclosed scope: %s", scope.name.c_str());
				continue;
			}
			uint64_t startTime = results[scope.startQueryId];
			uint64_t endTime = results[scope.endQueryId];

			uint64_t delta = (endTime - startTime) & timestampDiffMask;

			double milliseconds = (double)delta * timestampConversionFactor;

			NOTICE_LOG(G3D, "%s%s (%0.3f ms)", indent[scope.level & 3], scope.name.c_str(), milliseconds);
		}

		scopes_.clear();
		scopeStack_.clear();
	}

	// Only need to reset all on the first frame.
	if (firstFrame_) {
		numQueries_ = MAX_QUERY_COUNT;
		firstFrame_ = false;
	}
	if (numQueries_ > 0) {
		vkCmdResetQueryPool(firstCommandBuf, queryPool_, 0, numQueries_);
	}
	numQueries_ = 0;
}

void VulkanProfiler::Begin(VkCommandBuffer cmdBuf, VkPipelineStageFlagBits stageFlags, const char *fmt, ...) {
	if ((enabledPtr_ && !*enabledPtr_) || numQueries_ >= MAX_QUERY_COUNT - 1) {
		return;
	}

	va_list args;
	va_start(args, fmt);
	char temp[512];
	vsnprintf(temp, sizeof(temp), fmt, args);
	va_end(args);

	ProfilerScope scope;
	scope.name = temp;
	scope.startQueryId = numQueries_;
	scope.endQueryId = -1;
	scope.level = (int)scopeStack_.size();

	scopeStack_.push_back(scopes_.size());
	scopes_.push_back(scope);

	vkCmdWriteTimestamp(cmdBuf, stageFlags, queryPool_, numQueries_);
	numQueries_++;
}

void VulkanProfiler::End(VkCommandBuffer cmdBuf, VkPipelineStageFlagBits stageFlags) {
	if ((enabledPtr_ && !*enabledPtr_) || numQueries_ >= MAX_QUERY_COUNT - 1) {
		return;
	}

	size_t scopeId = scopeStack_.back();
	scopeStack_.pop_back();

	ProfilerScope &scope = scopes_[scopeId];
	scope.endQueryId = numQueries_;

	vkCmdWriteTimestamp(cmdBuf, stageFlags, queryPool_, numQueries_);
	numQueries_++;
}
