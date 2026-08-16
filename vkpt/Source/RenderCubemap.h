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

#pragma once

#include "Common.h"
#include "Buffer.h"
#include "GlobalUniform.h"
#include "MemoryAllocator.h"
#include "RasterizedDataCollector.h"
#include "RasterizerPipelines.h"
#include "TextureManager.h"

namespace vkpt
{

class RenderCubemap : public IShaderDependency
{
public:
    // params for the procedural sky compute pass (std140)
    struct ProceduralSkyParams
    {
        float faceBasis[18][4]; // 6 faces * (right, up, forward)
        float sunDirection[4];
        float sunColor[4];      // xyz color, w = sun angular radius (rad)
        float skyParams[4];     // x = multiplier, y = tint strength, z = sun disc intensity, w = sun disc radius
        float cloudColor[4];    // xyz = cloud color, w = cloud time (s)
        float cloudParams[4];   // x = coverage, y = density, z = drift speed, w = enabled
    };

public:
    RenderCubemap(VkDevice device,
                  const std::shared_ptr<MemoryAllocator> &allocator,
                  const std::shared_ptr<ShaderManager> &shaderManager,
                  const std::shared_ptr<TextureManager> &textureManager,
                  const std::shared_ptr<GlobalUniform> &uniform,
                  const std::shared_ptr<SamplerManager> &samplerManager,
                  const std::shared_ptr<CommandBufferManager> &cmdManager,
                  const RgInstanceCreateInfo &instanceInfo);
    ~RenderCubemap() override;

    RenderCubemap(const RenderCubemap &other) = delete;
    RenderCubemap(RenderCubemap &&other) noexcept = delete;
    RenderCubemap &operator=(const RenderCubemap &other) = delete;
    RenderCubemap &operator=(RenderCubemap &&other) noexcept = delete;

    // Draw to a cubemap
    void Draw(VkCommandBuffer cmd, uint32_t frameIndex,
               const std::shared_ptr< RasterizedDataCollector >& skyDataCollector,
              const std::shared_ptr<TextureManager> &textureManager,
              const std::shared_ptr<GlobalUniform> &uniform);

    // Fill the cubemap with a procedural atmospheric sky (compute)
    void DrawProcedural(VkCommandBuffer cmd, const ProceduralSkyParams &params);

    VkDescriptorSetLayout GetDescSetLayout() const;
    VkDescriptorSet GetDescSet() const;

    void OnShaderReload(const ShaderManager *shaderManager) override;
    

private:
    struct Attachment
    {
        VkImage image;
        VkImageView view;
        VkDeviceMemory memory;
    };

private:
    void CreatePipelineLayout(VkDescriptorSetLayout texturesSetLayout, VkDescriptorSetLayout uniformSetLayout);
    void CreateRenderPass();
    void InitPipelines(const std::shared_ptr<ShaderManager> &shaderManager, uint32_t sideSize, bool applyVertexColorGamma);
    void CreateAttch(const std::shared_ptr<MemoryAllocator> &allocator, VkCommandBuffer cmd, uint32_t sideSize, Attachment &result, bool isDepth);
    void CreateFramebuffer(uint32_t sideSize);
    void CreateDescriptors(const std::shared_ptr<SamplerManager> &samplerManager);

    void BindPipelineIfNew(VkCommandBuffer cmd, const RasterizedDataCollector::DrawInfo &info, VkPipeline &curPipeline);

    // procedural sky (compute)
    void CreateProceduralSkyPipelineLayout();
    void CreateProceduralSkyDescriptors();
    void CreateProceduralSkyParamsBuffer();
    void CreateProceduralSkyPipeline(const ShaderManager *shaderManager);
    void DestroyProceduralSkyPipelines();

private:
    VkDevice device;
    std::shared_ptr<MemoryAllocator> allocator;

    VkPipelineLayout pipelineLayout;
    std::shared_ptr<RasterizerPipelines> pipelines;

    VkRenderPass multiviewRenderPass;

    Attachment cubemap;
    Attachment cubemapDepth;

    VkFramebuffer cubemapFramebuffer;

    uint32_t cubemapSize;

    VkDescriptorSetLayout descSetLayout;
    VkDescriptorPool descPool;
    VkDescriptorSet descSet;

    // procedural sky (compute)
    Buffer procSkyParamsBuffer;
    void *mappedProcSkyParams = nullptr;

    VkDescriptorSetLayout procSkyDescSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      procSkyDescPool      = VK_NULL_HANDLE;
    VkDescriptorSet       procSkyDescSet       = VK_NULL_HANDLE;

    VkPipelineLayout procSkyPipelineLayout = VK_NULL_HANDLE;
    VkPipeline       procSkyPipeline       = VK_NULL_HANDLE;
};

}
