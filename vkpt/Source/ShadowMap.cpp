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

#include "ShadowMap.h"

#include <algorithm>
#include <cmath>

#include "CmdLabel.h"
#include "Generated/ShaderCommonC.h"
#include "Matrix.h"
#include "Utils.h"
#include "VertexCollector.h"

namespace
{
void ComputeViewProjection(const float sunDirection[3],
                           const float aabbMin[3], const float aabbMax[3],
                           float outViewProjection[16], float *outDepthScale)
{
    float upDir[3] = {0, 0, 1};
    // handle both sun straight up (z=+1) and straight down / zenith (z=-1);
    // in both cases lookDir is parallel to world up and the cross product would be zero
    if (std::fabs(sunDirection[2]) >= 0.99f)
    {
        upDir[0] = 1.0f; upDir[1] = 0.0f; upDir[2] = 0.0f;
    }

    // The caller passes the FROM-sun light direction (sunDir). Q2RTX's
    // vkpt_shadow_map_setup takes light->direction (TOWARD the sun) and
    // negates it, so its shadow camera looks FROM the sun down at the scene.
    // We must therefore use sunDir AS-IS (no negation) to get the same camera
    // orientation. Negating it pointed the camera TOWARD the sky, which
    // flipped near/far depth and produced inverted god rays (light shafts
    // everywhere except where the sun actually shines).
    float lookDir[3] = {sunDirection[0], sunDirection[1], sunDirection[2]};
    vkpt::Utils::Normalize(lookDir);

    float leftDir[3];
    vkpt::Utils::Cross(upDir, lookDir, leftDir);
    vkpt::Utils::Normalize(leftDir);

    vkpt::Utils::Cross(lookDir, leftDir, upDir);
    vkpt::Utils::Normalize(upDir);

    // view matrix (column-major); rows are left/up/look
    float viewMatrix[16] = {
        leftDir[0], upDir[0], lookDir[0], 0.0f,
        leftDir[1], upDir[1], lookDir[1], 0.0f,
        leftDir[2], upDir[2], lookDir[2], 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };

    // fit world AABB into view space
    float viewMin[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    float viewMax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

    for (int i = 0; i < 8; i++)
    {
        const float corner[4] = {
            (i & 1) ? aabbMax[0] : aabbMin[0],
            (i & 2) ? aabbMax[1] : aabbMin[1],
            (i & 4) ? aabbMax[2] : aabbMin[2],
            1.0f,
        };

        for (int k = 0; k < 4; k++)
        {
            const float v =
                viewMatrix[0 * 4 + k] * corner[0] +
                viewMatrix[1 * 4 + k] * corner[1] +
                viewMatrix[2 * 4 + k] * corner[2] +
                viewMatrix[3 * 4 + k] * corner[3];

            if (k < 3)
            {
                viewMin[k] = std::min(viewMin[k], v);
                viewMax[k] = std::max(viewMax[k], v);
            }
        }
    }

    float diagonal[3];
    for (int k = 0; k < 3; k++)
    {
        diagonal[k] = viewMax[k] - viewMin[k];
    }

    const float maxXY = std::max(diagonal[0], diagonal[1]);

    // make the XY footprint square
    viewMin[0] -= (maxXY - diagonal[0]) * 0.5f;
    viewMin[1] -= (maxXY - diagonal[1]) * 0.5f;
    viewMax[0] += (maxXY - diagonal[0]) * 0.5f;
    viewMax[1] += (maxXY - diagonal[1]) * 0.5f;

    // orthographic projection (column-major)
    const float width  = viewMax[0] - viewMin[0];
    const float height = viewMax[1] - viewMin[1];
    const float depth  = viewMax[2] - viewMin[2];

    float projectionMatrix[16] = {
        2.0f / width, 0.0f, 0.0f, 0.0f,
        0.0f, 2.0f / height, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f / depth, 0.0f,
        -(viewMax[0] + viewMin[0]) / width,
        -(viewMax[1] + viewMin[1]) / height,
        -viewMin[2] / depth,
        1.0f,
    };

    // Matrix::Multiply(result, a, b) computes b*a (applies a first, then b),
    // so passing (view, proj) yields Proj*View = world -> view -> clip, which
    // is the convention used everywhere else (see Matrix::GetViewProjection).
    // Passing (proj, view) here produced a transposed VP and a broken shadow
    // map (depth encoded along a garbage direction -> shadows always lit).
    vkpt::Matrix::Multiply(outViewProjection, viewMatrix, projectionMatrix);

    *outDepthScale = depth;
}
}

namespace vkpt
{

ShadowMap::ShadowMap(VkDevice _device, std::shared_ptr<MemoryAllocator> &_allocator,
                     const std::shared_ptr<const ShaderManager> &shaderManager)
: device(_device), allocator(_allocator)
{
    CreateImage();
    CreateRenderPass();
    CreatePipelineLayout();
    CreatePipelines(shaderManager.get());
    CreateDescriptors();
}

ShadowMap::~ShadowMap()
{
    DestroyAll();
}

void ShadowMap::CreateImage()
{
    VkResult r;

    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.format = VK_FORMAT_D32_SFLOAT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    r = vkCreateImage(device, &imageInfo, nullptr, &depthImage);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, depthImage, VK_OBJECT_TYPE_IMAGE, "Shadow map image");

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, depthImage, &memReq);

    depthMemory = allocator->AllocDedicated(memReq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                            MemoryAllocator::AllocType::DEFAULT, "Shadow map memory");

    r = vkBindImageMemory(device, depthImage, depthMemory, 0);
    VK_CHECKERROR(r);

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = depthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_D32_SFLOAT;
    viewInfo.subresourceRange = {
        .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };

    r = vkCreateImageView(device, &viewInfo, nullptr, &depthImageView);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, depthImageView, VK_OBJECT_TYPE_IMAGE_VIEW, "Shadow map image view");

    VkSamplerReductionModeCreateInfo reductionInfo = {};
    reductionInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO;
    reductionInfo.reductionMode = VK_SAMPLER_REDUCTION_MODE_MIN;

    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.pNext = &reductionInfo;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;

    r = vkCreateSampler(device, &samplerInfo, nullptr, &shadowSampler);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, shadowSampler, VK_OBJECT_TYPE_SAMPLER, "Shadow map sampler");
}

void ShadowMap::CreateRenderPass()
{
    VkResult r;

    VkAttachmentDescription depthAttachment = {};
    depthAttachment.format = VK_FORMAT_D32_SFLOAT;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef = {};
    depthRef.attachment = 0;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 0;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependency.dependencyFlags = 0;

    VkRenderPassCreateInfo passInfo = {};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    passInfo.attachmentCount = 1;
    passInfo.pAttachments = &depthAttachment;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    passInfo.dependencyCount = 1;
    passInfo.pDependencies = &dependency;

    r = vkCreateRenderPass(device, &passInfo, nullptr, &renderPass);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, renderPass, VK_OBJECT_TYPE_RENDER_PASS, "Shadow map render pass");

    VkFramebufferCreateInfo fbInfo = {};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &depthImageView;
    fbInfo.width = SHADOW_MAP_SIZE;
    fbInfo.height = SHADOW_MAP_SIZE;
    fbInfo.layers = 1;

    r = vkCreateFramebuffer(device, &fbInfo, nullptr, &framebuffer);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, framebuffer, VK_OBJECT_TYPE_FRAMEBUFFER, "Shadow map framebuffer");
}

void ShadowMap::CreatePipelineLayout()
{
    VkResult r;

    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = 16 * sizeof(float);

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 0;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    r = vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, pipelineLayout, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "Shadow map pipeline layout");
}

void ShadowMap::CreatePipelines(const ShaderManager *shaderManager)
{
    VkResult r;

    VkPipelineShaderStageCreateInfo shaderStage = {};
    shaderStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStage.module = shaderManager->GetStageInfo("ShadowMap").module;
    shaderStage.pName = "main";

    VkVertexInputBindingDescription binding = {};
    binding.binding = 0;
    binding.stride = sizeof(RgVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attribute = {};
    attribute.location = 0;
    attribute.binding = 0;
    attribute.format = VK_FORMAT_R32G32B32_SFLOAT;
    attribute.offset = offsetof(RgVertex, position);

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions = &attribute;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = (float)SHADOW_MAP_SIZE,
        .height = (float)SHADOW_MAP_SIZE,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };

    VkRect2D scissor = {
        .offset = {0, 0},
        .extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE},
    };

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    // Q2RTX culls front faces here because Q2 BSP geometry has a consistent
    // winding. The Q1 geometry upload has unreliable winding (the rasterizer
    // uses VK_CULL_MODE_NONE for the same reason), so culling would punch
    // spurious holes into the shadow map and the god rays would shine through
    // random walls. Render all faces: the depth test still keeps the nearest
    // (sun-facing) side, which is what a shadow map needs.
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 1;
    pipelineInfo.pStages = &shaderStage;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, pipeline, VK_OBJECT_TYPE_PIPELINE, "Shadow map pipeline");
}

void ShadowMap::CreateDescriptors()
{
    VkResult r;

    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;

    r = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descSetLayout);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, descSetLayout, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "Shadow map desc set layout");

    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;

    r = vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, descPool, VK_OBJECT_TYPE_DESCRIPTOR_POOL, "Shadow map desc pool");

    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &descSetLayout;

    r = vkAllocateDescriptorSets(device, &allocInfo, &descSet);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, descSet, VK_OBJECT_TYPE_DESCRIPTOR_SET, "Shadow map desc set");

    VkDescriptorImageInfo imageInfo = {};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = depthImageView;
    imageInfo.sampler = shadowSampler;

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void ShadowMap::DestroyAll()
{
    if (pipeline)        vkDestroyPipeline(device, pipeline, nullptr);
    if (pipelineLayout)  vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    if (framebuffer)     vkDestroyFramebuffer(device, framebuffer, nullptr);
    if (renderPass)      vkDestroyRenderPass(device, renderPass, nullptr);
    if (descSetLayout)   vkDestroyDescriptorSetLayout(device, descSetLayout, nullptr);
    if (descPool)        vkDestroyDescriptorPool(device, descPool, nullptr);
    if (shadowSampler)   vkDestroySampler(device, shadowSampler, nullptr);
    if (depthImageView)  vkDestroyImageView(device, depthImageView, nullptr);
    if (depthImage)      vkDestroyImage(device, depthImage, nullptr);
    if (depthMemory)     MemoryAllocator::FreeDedicated(device, depthMemory);

    pipeline       = VK_NULL_HANDLE;
    pipelineLayout = VK_NULL_HANDLE;
    framebuffer    = VK_NULL_HANDLE;
    renderPass     = VK_NULL_HANDLE;
    descSetLayout  = VK_NULL_HANDLE;
    descPool       = VK_NULL_HANDLE;
    descSet        = VK_NULL_HANDLE;
    shadowSampler  = VK_NULL_HANDLE;
    depthImageView = VK_NULL_HANDLE;
    depthImage     = VK_NULL_HANDLE;
    depthMemory    = VK_NULL_HANDLE;
}

void ShadowMap::OnShaderReload(const ShaderManager *shaderManager)
{
    if (pipeline)
    {
        vkDestroyPipeline(device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }

    CreatePipelines(shaderManager);
}

VkDescriptorSetLayout ShadowMap::GetDescSetLayout() const
{
    return descSetLayout;
}

VkDescriptorSet ShadowMap::GetDescSet() const
{
    return descSet;
}

bool ShadowMap::Render(VkCommandBuffer cmd,
                       const float sunDirection[3],
                       const float aabbMin[3], const float aabbMax[3],
                       const VertexCollector *staticCollector,
                       const VertexCollector *dynamicCollector,
                       float outViewProjection[16], float *outDepthScale)
{
    ComputeViewProjection(sunDirection, aabbMin, aabbMax, outViewProjection, outDepthScale);

    const auto staticDraws  = staticCollector ? staticCollector->GetGeometryDrawInfos() : std::vector<VertexCollector::GeometryDrawInfo>{};
    const auto dynamicDraws = dynamicCollector ? dynamicCollector->GetGeometryDrawInfos() : std::vector<VertexCollector::GeometryDrawInfo>{};

    if (staticDraws.empty() && dynamicDraws.empty())
    {
        return false;
    }

    CmdLabel label(cmd, "Shadow map");

    // transition depth image to be used as a depth attachment (contents discarded,
    // the render pass clears it anyway)
    {
        VkImageMemoryBarrier2 b = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            .srcAccessMask = 0,
            .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
            .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = depthImage,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };

        VkDependencyInfo dep = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &b,
        };

        svkCmdPipelineBarrier2KHR(cmd, &dep);
    }

    VkClearValue clearDepth = {};
    clearDepth.depthStencil.depth = 1.0f;

    VkRenderPassBeginInfo passInfo = {};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass = renderPass;
    passInfo.framebuffer = framebuffer;
    passInfo.renderArea = {.offset = {0, 0}, .extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE}};
    passInfo.clearValueCount = 1;
    passInfo.pClearValues = &clearDepth;

    vkCmdBeginRenderPass(cmd, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    VkViewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = (float)SHADOW_MAP_SIZE,
        .height = (float)SHADOW_MAP_SIZE,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {
        .offset = {0, 0},
        .extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE},
    };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16 * sizeof(float), outViewProjection);

    for (const auto &draw : staticDraws)
    {
        DrawGeometry(cmd, draw);
    }
    for (const auto &draw : dynamicDraws)
    {
        DrawGeometry(cmd, draw);
    }

    vkCmdEndRenderPass(cmd);

    // transition depth attachment -> shader read (for the god rays compute pass)
    {
        VkImageMemoryBarrier2 b = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = depthImage,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };

        VkDependencyInfo dep = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &b,
        };

        svkCmdPipelineBarrier2KHR(cmd, &dep);
    }

    return true;
}

void ShadowMap::DrawGeometry(VkCommandBuffer cmd, const VertexCollector::GeometryDrawInfo &draw)
{
    const VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &draw.vertexBuffer, &vertexOffset);

    if (draw.indexCount > 0)
    {
        vkCmdBindIndexBuffer(cmd, draw.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, draw.indexCount, 1, draw.firstIndex, (int32_t)draw.baseVertex, 0);
    }
    else
    {
        vkCmdDraw(cmd, draw.indexCount, 1, draw.baseVertex, 0);
    }
}

}
