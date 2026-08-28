// Copyright (c) 2024
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <array>

#include "Common.h"
#include "Buffer.h"
#include "MemoryAllocator.h"

namespace vkpt
{

// Number of ray categories the RT shaders atomically accumulate per frame.
// Must match RAY_STATS_CATEGORY_COUNT in GenerateShaderCommon.py.
constexpr uint32_t RAY_STATS_CATEGORY_COUNT = 4;

// Per-frame host-visible ray counters. The RT shaders atomicAdd into a
// storage buffer (desc set 11 of the ray tracing pipeline) when the stats
// overlay is enabled; the host reads the value back on the next frame that
// reuses that frame index (safe because BeginFrame waits on the frame fence).
class RayStats
{
public:
    RayStats(VkDevice device, std::shared_ptr<MemoryAllocator> &allocator);
    ~RayStats();

    RayStats(const RayStats &other) = delete;
    RayStats(RayStats &&other) noexcept = delete;
    RayStats &operator=(const RayStats &other) = delete;
    RayStats &operator=(RayStats &&other) noexcept = delete;

    void Reset(uint32_t frameIndex);
    // Total number of rays accumulated for the given frame index.
    uint32_t GetRays(uint32_t frameIndex) const;

    VkDescriptorSetLayout GetDescSetLayout() const;
    VkDescriptorSet GetDescSet(uint32_t frameIndex) const;

private:
    void CreateBuffers(std::shared_ptr<MemoryAllocator> &allocator);
    void CreateDescSetLayout();
    void CreateDescSet();

private:
    VkDevice device;
    std::shared_ptr<MemoryAllocator> allocator;

    std::array<Buffer, MAX_FRAMES_IN_FLIGHT> buffers;
    std::array<void *, MAX_FRAMES_IN_FLIGHT> mapped;

    VkDescriptorSetLayout descSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet descSets[MAX_FRAMES_IN_FLIGHT] = {};
};

}
