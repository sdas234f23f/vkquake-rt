// Copyright (c) 2021 Sultim Tsyrendashiev
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

#include "GodRays.h"

#include <cstring>

#include "BlueNoise.h"
#include "CmdLabel.h"
#include "Framebuffers.h"
#include "GlobalUniform.h"
#include "Generated/ShaderCommonC.h"
#include "ShadowMap.h"
#include "Utils.h"

RTGL1::GodRays::GodRays(VkDevice _device, std::shared_ptr<MemoryAllocator> &_allocator,
                        const std::shared_ptr<Framebuffers> &_framebuffers,
                        const std::shared_ptr<const ShaderManager> &_shaderManager,
                        const std::shared_ptr<const GlobalUniform> &_uniform,
                        const std::shared_ptr<const BlueNoise> &_blueNoise,
                        const std::shared_ptr<const ShadowMap> &_shadowMap)
: device(_device), allocator(_allocator),
  framebuffers(_framebuffers), uniform(_uniform), blueNoise(_blueNoise), shadowMap(_shadowMap)
{
    CreateParamsBuffer();
    CreateDescriptors();
    CreatePipelineLayout();
    CreatePipelines(_shaderManager.get());
}

RTGL1::GodRays::~GodRays()
{
    if (mappedParams)
    {
        paramsBuffer.TryUnmap();
    }

    paramsBuffer.Destroy();

    vkDestroyDescriptorPool(device, paramsDescPool, nullptr);
    vkDestroyDescriptorSetLayout(device, paramsDescSetLayout, nullptr);

    DestroyPipelines();
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
}

void RTGL1::GodRays::CreateParamsBuffer()
{
    paramsBuffer.Init(
        allocator,
        sizeof(Params),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        "God rays params buffer");

    mappedParams = paramsBuffer.Map();
    if (mappedParams)
    {
        memset(mappedParams, 0, sizeof(Params));
    }
}

void RTGL1::GodRays::CreateDescriptors()
{
    VkResult r;

    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;

    r = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &paramsDescSetLayout);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, paramsDescSetLayout, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "God rays params desc set layout");

    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;

    r = vkCreateDescriptorPool(device, &poolInfo, nullptr, &paramsDescPool);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, paramsDescPool, VK_OBJECT_TYPE_DESCRIPTOR_POOL, "God rays params desc pool");

    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = paramsDescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &paramsDescSetLayout;

    r = vkAllocateDescriptorSets(device, &allocInfo, &paramsDescSet);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, paramsDescSet, VK_OBJECT_TYPE_DESCRIPTOR_SET, "God rays params desc set");

    VkDescriptorBufferInfo bfInfo = {};
    bfInfo.buffer = paramsBuffer.GetBuffer();
    bfInfo.offset = 0;
    bfInfo.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = paramsDescSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &bfInfo;

    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void RTGL1::GodRays::CreatePipelineLayout()
{
    VkResult r;

    VkDescriptorSetLayout setLayouts[] = {
        shadowMap->GetDescSetLayout(),  // 0: shadow map
        paramsDescSetLayout,            // 1: params
        framebuffers->GetDescSetLayout(), // 2: framebuffers
        uniform->GetDescSetLayout(),    // 3: global uniform
        blueNoise->GetDescSetLayout(),  // 4: blue noise
    };

    // passIndex (CmGodRays.comp): 0 = primary, 1 = reflections
    VkPushConstantRange pushConstant = {};
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(uint32_t);

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(std::size(setLayouts));
    layoutInfo.pSetLayouts = setLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;

    r = vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, pipelineLayout, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "God rays pipeline layout");
}

void RTGL1::GodRays::CreatePipelines(const ShaderManager *shaderManager)
{
    VkResult r;

    // half-res ray march (CmGodRays.comp)
    VkComputePipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.stage = shaderManager->GetStageInfo("CGodRays");

    r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &tracePipeline);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, tracePipeline, VK_OBJECT_TYPE_PIPELINE, "God rays trace pipeline");

    // full-res bilateral upscale (CmGodRaysFilter.comp)
    pipelineInfo.stage = shaderManager->GetStageInfo("CGodRaysFilter");

    r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &filterPipeline);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, filterPipeline, VK_OBJECT_TYPE_PIPELINE, "God rays filter pipeline");
}

void RTGL1::GodRays::DestroyPipelines()
{
    if (tracePipeline)
    {
        vkDestroyPipeline(device, tracePipeline, nullptr);
        tracePipeline = VK_NULL_HANDLE;
    }
    if (filterPipeline)
    {
        vkDestroyPipeline(device, filterPipeline, nullptr);
        filterPipeline = VK_NULL_HANDLE;
    }
}

void RTGL1::GodRays::OnShaderReload(const ShaderManager *shaderManager)
{
    DestroyPipelines();
    CreatePipelines(shaderManager);
}

void RTGL1::GodRays::Trace(VkCommandBuffer cmd, uint32_t frameIndex, const Params &params, uint32_t passIndex)
{
    CmdLabel label(cmd, passIndex == 0 ? "God rays" : "God rays (reflections)");

    if (mappedParams)
    {
        memcpy(mappedParams, &params, sizeof(Params));
    }

    using FI = FramebufferImageIndex;
    FI inputs[] = {
        FI::FB_IMAGE_INDEX_SURFACE_POSITION,
        FI::FB_IMAGE_INDEX_VIEW_DIRECTION,
        FI::FB_IMAGE_INDEX_THROUGHPUT,
        FI::FB_IMAGE_INDEX_DEPTH_WORLD,
        FI::FB_IMAGE_INDEX_Q2_VIEW_DEPTH,
        FI::FB_IMAGE_INDEX_Q2_GOD_RAYS_THROUGHPUT_DIST,
        FI::FB_IMAGE_INDEX_GOD_RAYS,
    };
    framebuffers->BarrierMultiple(cmd, frameIndex, inputs);

    VkDescriptorSet sets[] = {
        shadowMap->GetDescSet(),              // 0
        paramsDescSet,                        // 1
        framebuffers->GetDescSet(frameIndex), // 2
        uniform->GetDescSet(frameIndex),      // 3
        blueNoise->GetDescSet(),              // 4
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tracePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout,
                            0, static_cast<uint32_t>(std::size(sets)), sets, 0, nullptr);

    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t), &passIndex);

    // half-resolution pass (Q2RTX god_rays.comp runs at half res and is
    // upscaled by the filter pass)
    const uint32_t halfW = (uniform->GetData()->renderWidth + 1) / 2;
    const uint32_t halfH = (uniform->GetData()->renderHeight + 1) / 2;
    const uint32_t wgCountX = Utils::GetWorkGroupCount(halfW, 16);
    const uint32_t wgCountY = Utils::GetWorkGroupCount(halfH, 16);

    vkCmdDispatch(cmd, wgCountX, wgCountY, 1);
}

void RTGL1::GodRays::Filter(VkCommandBuffer cmd, uint32_t frameIndex)
{
    CmdLabel label(cmd, "God rays filter");

    using FI = FramebufferImageIndex;
    FI inputs[] = {
        FI::FB_IMAGE_INDEX_GOD_RAYS,            // half-res intermediate, read
        FI::FB_IMAGE_INDEX_Q2_VIEW_DEPTH,       // full-res depth for the bilateral weight
        FI::FB_IMAGE_INDEX_GOD_RAYS_FILTERED,   // full-res result, written
    };
    framebuffers->BarrierMultiple(cmd, frameIndex, inputs);

    VkDescriptorSet sets[] = {
        shadowMap->GetDescSet(),              // 0 (unused by the filter)
        paramsDescSet,                        // 1 (unused by the filter)
        framebuffers->GetDescSet(frameIndex), // 2
        uniform->GetDescSet(frameIndex),      // 3
        blueNoise->GetDescSet(),              // 4 (unused by the filter)
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, filterPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout,
                            0, static_cast<uint32_t>(std::size(sets)), sets, 0, nullptr);

    const uint32_t passIndex = 0;
    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t), &passIndex);

    const uint32_t wgCountX = Utils::GetWorkGroupCount(uniform->GetData()->renderWidth, 16);
    const uint32_t wgCountY = Utils::GetWorkGroupCount(uniform->GetData()->renderHeight, 16);

    vkCmdDispatch(cmd, wgCountX, wgCountY, 1);
}
