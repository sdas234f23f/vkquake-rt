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

#include "RenderCubemap.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "CmdLabel.h"
#include "Matrix.h"
#include "RasterizedDataCollector.h"
#include "Utils.h"
#include "Generated/ShaderCommonC.h"


// high-res HDR cubemap, matching Q2RTX physical sky quality (1024^2, R16G16B16A16_SFLOAT)
constexpr VkFormat CUBEMAP_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT; 
constexpr VkFormat CUBEMAP_DEPTH_FORMAT = VK_FORMAT_D16_UNORM; 
constexpr uint32_t CUBEMAP_SIDE_SIZE = 1024;


namespace vkpt
{

struct RasterizedMultiviewPushConst
{
    float model[16];
    float color[4];
    uint32_t textureIndex;

    explicit RasterizedMultiviewPushConst(const RasterizedDataCollector::DrawInfo &info)
    {
        Matrix::ToMat4Transposed(model, info.transform);
        memcpy(color, info.color.Get(), 4 * sizeof(float));
        textureIndex = info.textureIndex;
    }
};

}



vkpt::RenderCubemap::RenderCubemap(
    VkDevice _device, 
    const std::shared_ptr<MemoryAllocator> &_allocator,
    const std::shared_ptr<ShaderManager> &_shaderManager,
    const std::shared_ptr<TextureManager> &_textureManager,
    const std::shared_ptr<GlobalUniform> &_uniform,
    const std::shared_ptr<SamplerManager> &_samplerManager,
    const std::shared_ptr<CommandBufferManager> &_cmdManager,
    const RgInstanceCreateInfo &_instanceInfo)
:
    device(_device),
    allocator(_allocator),
    pipelineLayout(VK_NULL_HANDLE),
    multiviewRenderPass(VK_NULL_HANDLE),
    cubemap{},
    cubemapDepth{},
    cubemapFramebuffer(VK_NULL_HANDLE),
    cubemapSize(CUBEMAP_SIDE_SIZE),
    descSetLayout(VK_NULL_HANDLE),
    descPool(VK_NULL_HANDLE),
    descSet(VK_NULL_HANDLE)
{
    CreatePipelineLayout(_textureManager->GetDescSetLayout(), _uniform->GetDescSetLayout());
    CreateRenderPass();
    InitPipelines(_shaderManager, cubemapSize, _instanceInfo.rasterizedVertexColorGamma);

    VkCommandBuffer cmd = _cmdManager->StartGraphicsCmd();
    CreateAttch(_allocator, cmd, cubemapSize, cubemap, false);
    CreateAttch(_allocator, cmd, cubemapSize, cubemapDepth, true);
    _cmdManager->Submit(cmd);
    _cmdManager->WaitGraphicsIdle();

    CreateFramebuffer(cubemapSize);
    CreateDescriptors(_samplerManager);

    CreateProceduralSkyParamsBuffer();
    CreateProceduralSkyDescriptors();
    CreateProceduralSkyPipelineLayout();
    CreateProceduralSkyPipeline(_shaderManager.get());
}

vkpt::RenderCubemap::~RenderCubemap()
{
    if (mappedProcSkyParams)
    {
        procSkyParamsBuffer.TryUnmap();
    }
    procSkyParamsBuffer.Destroy();

    vkDestroyDescriptorPool(device, procSkyDescPool, nullptr);
    vkDestroyDescriptorSetLayout(device, procSkyDescSetLayout, nullptr);
    vkDestroyPipelineLayout(device, procSkyPipelineLayout, nullptr);
    DestroyProceduralSkyPipelines();

    vkDestroyDescriptorPool(device, descPool, nullptr);
    vkDestroyDescriptorSetLayout(device, descSetLayout, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyRenderPass(device, multiviewRenderPass, nullptr);

    vkDestroyImage(device, cubemap.image, nullptr);
    vkDestroyImageView(device, cubemap.view, nullptr);
    vkFreeMemory(device, cubemap.memory, nullptr);

    vkDestroyImage(device, cubemapDepth.image, nullptr);
    vkDestroyImageView(device, cubemapDepth.view, nullptr);
    vkFreeMemory(device, cubemapDepth.memory, nullptr);

    vkDestroyFramebuffer(device, cubemapFramebuffer, nullptr);
}

void vkpt::RenderCubemap::OnShaderReload(const ShaderManager *shaderManager)
{
    pipelines->OnShaderReload( shaderManager );
    DestroyProceduralSkyPipelines();
    CreateProceduralSkyPipeline(shaderManager);
}

void vkpt::RenderCubemap::Draw(VkCommandBuffer cmd, uint32_t frameIndex,
                                const std::shared_ptr<RasterizedDataCollector> &skyDataCollector,
                                const std::shared_ptr<TextureManager> &textureManager,
                                const std::shared_ptr<GlobalUniform> &uniform)
{
    const auto &drawInfos = skyDataCollector->GetSkyDrawInfos();

    if (drawInfos.empty())
    {
        return;
    }

    VkBuffer vertexBuffer = skyDataCollector->GetVertexBuffer();
    VkBuffer indexBuffer = skyDataCollector->GetIndexBuffer();

    VkDescriptorSet descSets[] =
    {
        textureManager->GetDescSet(frameIndex),
        uniform->GetDescSet(frameIndex),
    };
    const uint32_t descSetCount = sizeof(descSets) / sizeof(descSets[0]);


    VkClearValue clearValues[2] = {};
    clearValues[0].color = {};
    clearValues[1].depthStencil.depth = 1.0f;


    VkRenderPassBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.renderPass = multiviewRenderPass;
    beginInfo.framebuffer = cubemapFramebuffer;
    beginInfo.renderArea.offset = { 0, 0 };
    beginInfo.renderArea.extent = { cubemapSize, cubemapSize };
    beginInfo.clearValueCount = 2;
    beginInfo.pClearValues = clearValues;

    vkCmdBeginRenderPass(cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);


    VkPipeline curPipeline = VK_NULL_HANDLE;
    BindPipelineIfNew(cmd, drawInfos[0], curPipeline);


    VkDeviceSize offset = 0;

    vkCmdBindDescriptorSets(
        cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines->GetPipelineLayout(), 0,
        descSetCount, descSets,
        0, nullptr);
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &offset);
    vkCmdBindIndexBuffer(cmd, indexBuffer, offset, VK_INDEX_TYPE_UINT32);


    for (const auto &info : drawInfos)
    {
        BindPipelineIfNew(cmd, info, curPipeline);

        // push const
        {
            RasterizedMultiviewPushConst push(info);

            vkCmdPushConstants(
                cmd, pipelines->GetPipelineLayout(),
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(push),
                &push);
        }

        // draw
        if (info.indexCount > 0)
        {
            vkCmdDrawIndexed(cmd, info.indexCount, 1, info.firstIndex, info.firstVertex, 0);
        }
        else
        {
            vkCmdDraw(cmd, info.vertexCount, 1, info.firstVertex, 0);
        }
    }

    vkCmdEndRenderPass(cmd);
}

VkDescriptorSetLayout vkpt::RenderCubemap::GetDescSetLayout() const
{
    return descSetLayout;
}

VkDescriptorSet vkpt::RenderCubemap::GetDescSet() const
{
    return descSet;
}

void vkpt::RenderCubemap::BindPipelineIfNew(VkCommandBuffer cmd, const RasterizedDataCollector::DrawInfo &info, VkPipeline &curPipeline)
{
    pipelines->BindPipelineIfNew(cmd, curPipeline, info.pipelineState, info.blendFuncSrc, info.blendFuncDst);
}


void vkpt::RenderCubemap::CreatePipelineLayout(VkDescriptorSetLayout texturesSetLayout, VkDescriptorSetLayout uniformSetLayout)
{
    VkDescriptorSetLayout setLayouts[] =
    {
        texturesSetLayout,
        uniformSetLayout
    };
    const uint32_t setLayoutCount = sizeof(setLayouts) / sizeof(setLayouts[0]);

    static_assert(sizeof(RasterizedMultiviewPushConst) == 16 * sizeof(float) + 4 * sizeof(float) + sizeof(uint32_t), "");

    VkPushConstantRange pushConst = {};
    pushConst.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConst.offset = 0;
    pushConst.size = sizeof(RasterizedMultiviewPushConst);

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConst;
    layoutInfo.setLayoutCount = setLayoutCount;
    layoutInfo.pSetLayouts = setLayouts;

    VkResult r = vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, pipelineLayout, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "Render cubemap pipeline layout");
}

void vkpt::RenderCubemap::CreateRenderPass()
{
    const int attchCount = 2;
    VkAttachmentDescription attchs[attchCount] = {};

    auto &colorAttch = attchs[0];
    colorAttch.format = CUBEMAP_FORMAT;
    colorAttch.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttch.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttch.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttch.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttch.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttch.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    colorAttch.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    auto &depthAttch = attchs[1];
    depthAttch.format = CUBEMAP_DEPTH_FORMAT; 
    depthAttch.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttch.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttch.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttch.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttch.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttch.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttch.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;


    VkAttachmentReference colorRef = {};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef = {};
    depthRef.attachment = 1;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;


    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;


    // cubemap, 6 faces
    uint32_t viewMask = 0b00111111;
    int32_t viewOffset = 0;

    VkRenderPassMultiviewCreateInfo multiview = {};
    multiview.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
    multiview.subpassCount = 1;
    multiview.pViewMasks = &viewMask;
    multiview.dependencyCount = 1;
    multiview.pViewOffsets = &viewOffset;
    // no correlation between cubemap faces
    multiview.correlationMaskCount = 0;
    multiview.pCorrelationMasks = nullptr;


    VkRenderPassCreateInfo passInfo = {};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    passInfo.pNext = &multiview;
    passInfo.attachmentCount = attchCount;
    passInfo.pAttachments = attchs;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    passInfo.dependencyCount = 1;
    passInfo.pDependencies = &dependency;

    VkResult r = vkCreateRenderPass(device, &passInfo, nullptr, &multiviewRenderPass);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, multiviewRenderPass, VK_OBJECT_TYPE_RENDER_PASS, "Render cubemap multiview render pass");
}

void vkpt::RenderCubemap::InitPipelines(const std::shared_ptr<ShaderManager> &shaderManager, uint32_t sideSize, bool applyVertexColorGamma)
{
    VkViewport viewport = {};
    viewport.x = viewport.y = 0;
    viewport.width = viewport.height = (float)sideSize;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissors = {};
    scissors.offset = { 0, 0 };
    scissors.extent = { sideSize, sideSize };


    pipelines = std::make_shared< RasterizerPipelines >( device,
                                                         pipelineLayout,
                                                         multiviewRenderPass,
                                                         shaderManager.get(),
                                                         "VertDefaultMultiview",
                                                         "FragSky",
                                                         0,
                                                         applyVertexColorGamma );
    pipelines->DisableDynamicState(viewport, scissors);
}

void vkpt::RenderCubemap::CreateAttch(
    const std::shared_ptr<MemoryAllocator> &allocator,
    VkCommandBuffer cmd,
    uint32_t sideSize, Attachment &result, bool isDepth)
{
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    imageInfo.format = isDepth ? CUBEMAP_DEPTH_FORMAT : CUBEMAP_FORMAT;
    imageInfo.extent = { sideSize, sideSize, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 6;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = isDepth ?
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT :
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult r = vkCreateImage(device, &imageInfo, nullptr, &result.image);
    VK_CHECKERROR(r);
    SET_DEBUG_NAME(device, result.image, VK_OBJECT_TYPE_IMAGE, isDepth ? "Render cubemap depth image" : "Render cubemap image");


    // allocate dedicated memory
    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device, result.image, &memReqs);

    result.memory = allocator->AllocDedicated(memReqs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, MemoryAllocator::AllocType::DEFAULT, isDepth ? "Render cubemap depth memory" : "Render cubemap image memory");

    if (result.memory == VK_NULL_HANDLE)
    {
        vkDestroyImage(device, result.image, nullptr);
        result.image = VK_NULL_HANDLE;

        return;
    }

    r = vkBindImageMemory(device, result.image, result.memory, 0);
    VK_CHECKERROR(r);


    // create image view
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format = isDepth ? CUBEMAP_DEPTH_FORMAT : CUBEMAP_FORMAT;
    viewInfo.subresourceRange = {};
    viewInfo.subresourceRange.aspectMask = isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 6;
    viewInfo.image = result.image;

    r = vkCreateImageView(device, &viewInfo, nullptr, &result.view);
    VK_CHECKERROR(r);
    SET_DEBUG_NAME(device, result.view, VK_OBJECT_TYPE_IMAGE_VIEW, isDepth ? "Render cubemap depth image view" : "Render cubemap image view");


    // make transition from undefined manually, so initialLayout can be specified
    VkImageMemoryBarrier imageBarrier = {};
    imageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imageBarrier.image = result.image;
    imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.srcAccessMask = 0;
    if (isDepth)
    {
        imageBarrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        imageBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    else
    {
        imageBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        imageBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    }
    imageBarrier.subresourceRange.baseMipLevel = 0;
    imageBarrier.subresourceRange.levelCount = 1;
    imageBarrier.subresourceRange.baseArrayLayer = 0;
    imageBarrier.subresourceRange.layerCount = 6;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
        0, nullptr,
        0, nullptr,
        1, &imageBarrier);
}

void vkpt::RenderCubemap::CreateFramebuffer(uint32_t sideSize)
{
    if (cubemap.image == VK_NULL_HANDLE || cubemap.view == VK_NULL_HANDLE ||
        cubemapDepth.image == VK_NULL_HANDLE || cubemapDepth.view == VK_NULL_HANDLE)
    {
        return;
    }

    VkImageView attchs[] =
    {
        cubemap.view,
        cubemapDepth.view,
    };

    VkFramebufferCreateInfo fbInfo = {};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = multiviewRenderPass;
    fbInfo.attachmentCount = 2;
    fbInfo.pAttachments = attchs;
    fbInfo.width = sideSize;
    fbInfo.height = sideSize;
    fbInfo.layers = 1;

    VkResult r = vkCreateFramebuffer(device, &fbInfo, nullptr, &cubemapFramebuffer);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, cubemapFramebuffer, VK_OBJECT_TYPE_FRAMEBUFFER, "Render cubemap framebuffer");
}

void vkpt::RenderCubemap::CreateDescriptors(const std::shared_ptr<SamplerManager> &samplerManager)
{
    VkDescriptorSetLayoutBinding binding = {};

    binding.binding = BINDING_RENDER_CUBEMAP;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_ALL;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;

    VkResult r = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descSetLayout);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, descSetLayout, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "Render cubemap Desc set layout");


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

    SET_DEBUG_NAME(device, descPool, VK_OBJECT_TYPE_DESCRIPTOR_POOL, "Render cubemap Desc pool");


    VkDescriptorSetAllocateInfo setInfo = {};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setInfo.descriptorPool = descPool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &descSetLayout;

    r = vkAllocateDescriptorSets(device, &setInfo, &descSet);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, descSet, VK_OBJECT_TYPE_DESCRIPTOR_SET, "Render cubemap desc set");


    VkDescriptorImageInfo img = {};
    img.sampler = samplerManager->GetSampler(RG_SAMPLER_FILTER_LINEAR, RG_SAMPLER_ADDRESS_MODE_REPEAT, RG_SAMPLER_ADDRESS_MODE_REPEAT);
    img.imageView = cubemap.view;
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet wrt = {};
    wrt.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wrt.dstSet = descSet;
    wrt.dstBinding = BINDING_RENDER_CUBEMAP;
    wrt.dstArrayElement = 0;
    wrt.descriptorCount = 1;
    wrt.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wrt.pImageInfo = &img;

    vkUpdateDescriptorSets(device, 1, &wrt, 0, nullptr);
}

void vkpt::RenderCubemap::CreateProceduralSkyParamsBuffer()
{
    procSkyParamsBuffer.Init(
        allocator,
        sizeof(ProceduralSkyParams),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        "Procedural sky params buffer");

    mappedProcSkyParams = procSkyParamsBuffer.Map();
    if (mappedProcSkyParams)
    {
        memset(mappedProcSkyParams, 0, sizeof(ProceduralSkyParams));
    }
}

void vkpt::RenderCubemap::CreateProceduralSkyDescriptors()
{
    VkResult r;

    VkDescriptorSetLayoutBinding bindings[2] = {};

    // 0: cubemap storage image
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // 1: params buffer
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;

    r = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &procSkyDescSetLayout);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, procSkyDescSetLayout, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, "Procedural sky desc set layout");

    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;

    r = vkCreateDescriptorPool(device, &poolInfo, nullptr, &procSkyDescPool);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, procSkyDescPool, VK_OBJECT_TYPE_DESCRIPTOR_POOL, "Procedural sky desc pool");

    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = procSkyDescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &procSkyDescSetLayout;

    r = vkAllocateDescriptorSets(device, &allocInfo, &procSkyDescSet);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, procSkyDescSet, VK_OBJECT_TYPE_DESCRIPTOR_SET, "Procedural sky desc set");

    VkDescriptorImageInfo imgInfo = {};
    imgInfo.imageView = cubemap.view;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorBufferInfo bufInfo = {};
    bufInfo.buffer = procSkyParamsBuffer.GetBuffer();
    bufInfo.offset = 0;
    bufInfo.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet writes[2] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = procSkyDescSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &imgInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = procSkyDescSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &bufInfo;

    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
}

void vkpt::RenderCubemap::CreateProceduralSkyPipelineLayout()
{
    VkResult r;

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &procSkyDescSetLayout;

    r = vkCreatePipelineLayout(device, &layoutInfo, nullptr, &procSkyPipelineLayout);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, procSkyPipelineLayout, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "Procedural sky pipeline layout");
}

void vkpt::RenderCubemap::CreateProceduralSkyPipeline(const ShaderManager *shaderManager)
{
    VkResult r;

    VkComputePipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.layout = procSkyPipelineLayout;
    pipelineInfo.stage = shaderManager->GetStageInfo("CProceduralSky");

    r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &procSkyPipeline);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, procSkyPipeline, VK_OBJECT_TYPE_PIPELINE, "Procedural sky pipeline");
}

void vkpt::RenderCubemap::DestroyProceduralSkyPipelines()
{
    if (procSkyPipeline)
    {
        vkDestroyPipeline(device, procSkyPipeline, nullptr);
        procSkyPipeline = VK_NULL_HANDLE;
    }
}

void vkpt::RenderCubemap::DrawProcedural(VkCommandBuffer cmd, const ProceduralSkyParams &inParams)
{
    CmdLabel label(cmd, "Procedural sky");

    ProceduralSkyParams params = inParams;

    // Clouds off: freeze the animation time so the cached sky isn't re-rendered
    // every frame (only when sun/sky params change).
    // Clouds on: keep the raw time -> the sky re-renders every frame (smooth per-frame drift).
    if (params.cloudParams[3] <= 0.5f)
    {
        params.cloudColor[3] = 0.0f;
    }

    if (mappedProcSkyParams)
    {
        // no changes since the last render - keep the cached cubemap
        if (memcmp(mappedProcSkyParams, &params, sizeof(ProceduralSkyParams)) == 0)
        {
            return;
        }

        memcpy(mappedProcSkyParams, &params, sizeof(ProceduralSkyParams));
    }

    // cubemap: SHADER_READ_ONLY -> GENERAL
    {
        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.image = cubemap.image;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 6;

        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
            0, nullptr,
            0, nullptr,
            1, &barrier);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, procSkyPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, procSkyPipelineLayout,
                            0, 1, &procSkyDescSet, 0, nullptr);

    const uint32_t wgX = Utils::GetWorkGroupCount(cubemapSize, 16);
    const uint32_t wgY = Utils::GetWorkGroupCount(cubemapSize, 16);
    vkCmdDispatch(cmd, wgX, wgY, 6);

    // cubemap: GENERAL -> SHADER_READ_ONLY
    {
        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.image = cubemap.image;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 6;

        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
            0, nullptr,
            0, nullptr,
            1, &barrier);
    }
}

