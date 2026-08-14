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
#include "IShaderDependency.h"
#include "MemoryAllocator.h"
#include "ShaderManager.h"
#include "VertexCollector.h"

namespace RTGL1
{

// Renders the world geometry into a depth-only shadow map from the sun's
// point of view. Used by god rays for volumetric sunlight.
// Ported from Quake 2 RTX (shadow_map.c / shadow_map.vert), GPL v2.
class ShadowMap : public IShaderDependency
{
public:
    ShadowMap(VkDevice device, std::shared_ptr<MemoryAllocator> &allocator,
              const std::shared_ptr<const ShaderManager> &shaderManager);
    ~ShadowMap();

    ShadowMap(const ShadowMap &other) = delete;
    ShadowMap(ShadowMap &&other) noexcept = delete;
    ShadowMap &operator=(const ShadowMap &other) = delete;
    ShadowMap &operator=(ShadowMap &&other) noexcept = delete;

    // Renders static+dynamic world geometry into the shadow map.
    // outViewProjection (column-major, 16 floats) and outDepthScale are used
    // by the god rays shader. Returns false if there is nothing to draw.
    bool Render(VkCommandBuffer cmd,
                const float sunDirection[3],
                const float aabbMin[3], const float aabbMax[3],
                const VertexCollector *staticCollector,
                const VertexCollector *dynamicCollector,
                float outViewProjection[16], float *outDepthScale);

    VkDescriptorSetLayout GetDescSetLayout() const;
    VkDescriptorSet GetDescSet() const;

    void OnShaderReload(const ShaderManager *shaderManager);

private:
    void CreateImage();
    void CreateRenderPass();
    void CreatePipelineLayout();
    void CreatePipelines(const ShaderManager *shaderManager);
    void CreateDescriptors();
    void DestroyAll();

    void DrawGeometry(VkCommandBuffer cmd, const VertexCollector::GeometryDrawInfo &draw);

private:
    // Q2RTX uses SHADOWMAP_SIZE 4096 (shader/constants.h). We used 2048, which
    // quantized the god rays shaft edges to twice the texel size and made the
    // shafts look like coarse "individual rays" instead of smooth beams.
    static constexpr uint32_t SHADOW_MAP_SIZE = 4096;

    VkDevice device;
    std::shared_ptr<MemoryAllocator> allocator;

    VkImage       depthImage    = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory  = VK_NULL_HANDLE;
    VkImageView   depthImageView = VK_NULL_HANDLE;
    VkSampler     shadowSampler = VK_NULL_HANDLE;

    VkRenderPass   renderPass   = VK_NULL_HANDLE;
    VkFramebuffer  framebuffer  = VK_NULL_HANDLE;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline       pipeline       = VK_NULL_HANDLE;

    VkDescriptorSetLayout descSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      descPool      = VK_NULL_HANDLE;
    VkDescriptorSet       descSet       = VK_NULL_HANDLE;
};

}
