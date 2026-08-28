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

#include "RayStats.h"

#include <cstring>

#include "Utils.h"

namespace vkpt
{

RayStats::RayStats(VkDevice _device, std::shared_ptr<MemoryAllocator> &_allocator)
    : device(_device)
    , allocator(_allocator)
{
    CreateBuffers(allocator);
    CreateDescSetLayout();
    CreateDescSet();
}

RayStats::~RayStats()
{
    if (descPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(device, descPool, nullptr);
    }
    if (descSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(device, descSetLayout, nullptr);
    }
    for (auto &b : buffers)
    {
        b.Destroy();
    }
}

void RayStats::CreateBuffers(std::shared_ptr<MemoryAllocator> &allocator)
{
    const VkDeviceSize size = RAY_STATS_CATEGORY_COUNT * sizeof(uint32_t);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        buffers[i].Init(allocator, size,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        "RayStats buffer");
        mapped[i] = buffers[i].Map();
    }
}

void RayStats::CreateDescSetLayout()
{
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                         VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                         VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;

    VkResult r = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descSetLayout);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, descSetLayout, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "RayStats desc set layout");
}

void RayStats::CreateDescSet()
{
    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;

    VkResult r = vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool);
    VK_CHECKERROR(r);

    VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT] = {};
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        layouts[i] = descSetLayout;
    }

    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descPool;
    allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    allocInfo.pSetLayouts = layouts;

    r = vkAllocateDescriptorSets(device, &allocInfo, descSets);
    VK_CHECKERROR(r);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        VkDescriptorBufferInfo bufferInfo = {};
        bufferInfo.buffer = buffers[i].GetBuffer();
        bufferInfo.offset = 0;
        bufferInfo.range = RAY_STATS_CATEGORY_COUNT * sizeof(uint32_t);

        VkWriteDescriptorSet write = {};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descSets[i];
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
}

void RayStats::Reset(uint32_t frameIndex)
{
    assert(frameIndex < MAX_FRAMES_IN_FLIGHT);
    if (mapped[frameIndex] != nullptr)
    {
        memset(mapped[frameIndex], 0, RAY_STATS_CATEGORY_COUNT * sizeof(uint32_t));
    }
}

uint32_t RayStats::GetRays(uint32_t frameIndex) const
{
    assert(frameIndex < MAX_FRAMES_IN_FLIGHT);

    const uint32_t *p = static_cast<const uint32_t *>(mapped[frameIndex]);
    uint32_t total = 0;
    for (uint32_t i = 0; i < RAY_STATS_CATEGORY_COUNT; i++)
    {
        total += p[i];
    }
    return total;
}

VkDescriptorSetLayout RayStats::GetDescSetLayout() const
{
    return descSetLayout;
}

VkDescriptorSet RayStats::GetDescSet(uint32_t frameIndex) const
{
    return descSets[frameIndex];
}

}
