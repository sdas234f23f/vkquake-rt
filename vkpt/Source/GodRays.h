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
#include "IShaderDependency.h"
#include "MemoryAllocator.h"
#include "ShaderManager.h"

namespace vkpt
{

class Framebuffers;
class GlobalUniform;
class BlueNoise;
class ShadowMap;

// Volumetric sunlight ("god rays") — ray marches through the shadow map.
// Ported from Quake 2 RTX (god_rays.c / god_rays.comp), GPL v2.
class GodRays : public IShaderDependency
{
public:
    struct Params
    {
        float sunDirection[4];
        float sunColor[4];
        float worldCenter[4];
        float worldHalfSizeInv[4];
        float shadowMapVP[16];
        float shadowMapDepthScale;
        float godRaysIntensity;
        float godRaysEccentricity;
        uint32_t godRaysEnabled;
        float _padding;
    };

public:
    GodRays(VkDevice device, std::shared_ptr<MemoryAllocator> &allocator,
            const std::shared_ptr<Framebuffers> &framebuffers,
            const std::shared_ptr<const ShaderManager> &shaderManager,
            const std::shared_ptr<const GlobalUniform> &uniform,
            const std::shared_ptr<const BlueNoise> &blueNoise,
            const std::shared_ptr<const ShadowMap> &shadowMap);
    ~GodRays();

    GodRays(const GodRays &other) = delete;
    GodRays(GodRays &&other) noexcept = delete;
    GodRays &operator=(const GodRays &other) = delete;
    GodRays &operator=(GodRays &&other) noexcept = delete;

    // passIndex: 0 = primary rays, 1 = reflection/refraction rays (the
    // reflected segment god rays are accumulated on top of the primary result).
    void Trace(VkCommandBuffer cmd, uint32_t frameIndex,
               const Params &params, uint32_t passIndex = 0);

    // Bilateral upscale of the half-resolution result to full resolution
    // (Q2RTX god_rays_filter.comp). Call after all Trace passes.
    void Filter(VkCommandBuffer cmd, uint32_t frameIndex);

    void OnShaderReload(const ShaderManager *shaderManager);

private:
    void CreateParamsBuffer();
    void CreateDescriptors();
    void CreatePipelineLayout();
    void CreatePipelines(const ShaderManager *shaderManager);
    void DestroyPipelines();

private:
    VkDevice device;
    std::shared_ptr<MemoryAllocator> allocator;
    std::shared_ptr<Framebuffers> framebuffers;
    std::shared_ptr<const GlobalUniform> uniform;
    std::shared_ptr<const BlueNoise> blueNoise;
    std::shared_ptr<const ShadowMap> shadowMap;

    Buffer paramsBuffer;
    void *mappedParams = nullptr;

    VkDescriptorSetLayout paramsDescSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      paramsDescPool      = VK_NULL_HANDLE;
    VkDescriptorSet       paramsDescSet       = VK_NULL_HANDLE;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline       tracePipeline  = VK_NULL_HANDLE;
    VkPipeline       filterPipeline = VK_NULL_HANDLE;
};

}
