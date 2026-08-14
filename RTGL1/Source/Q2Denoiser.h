// Copyright (c) 2020-2021 Sultim Tsyrendashiev
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

#include "Denoiser.h"

namespace RTGL1
{

// New Q2RTX-style denoiser: full ASVGF (gradient atrous, temporal, LF atrous at
// 1/3 res, HF/SPEC atrous + compositing) fed from the RTGL1 ReSTIR outputs via
// CmQ2Adapter.comp, followed by checkerboard interleave into PreFinal.
//
// The ASVGF uses the Q2RTX channel format (LF luma-SH YCoCg, HF/SPEC packed
// RGBE). The gradients are provided by the RTGL1 gradient estimation
// (DISPingGradient), which is computed here by running the legacy ASVGF
// gradient atrous pass.
class Q2Denoiser : public IShaderDependency
{
public:
    Q2Denoiser(
        VkDevice device,
        std::shared_ptr<Framebuffers> framebuffers,
        const std::shared_ptr<const ShaderManager> &shaderManager,
        const std::shared_ptr<const GlobalUniform> &uniform,
        const std::shared_ptr<const ASManager> &asManager);
    ~Q2Denoiser();

    Q2Denoiser(const Q2Denoiser &other) = delete;
    Q2Denoiser(Q2Denoiser &&other) noexcept = delete;
    Q2Denoiser &operator=(const Q2Denoiser &other) = delete;
    Q2Denoiser &operator=(Q2Denoiser &&other) noexcept = delete;

    void Denoise(
        VkCommandBuffer cmd, uint32_t frameIndex,
        const std::shared_ptr<const GlobalUniform> &uniform);
    // TAAU upscaler for the new core path. Takes the tonemapped Final at render
    // resolution and writes UpscaledPing at the upscaled resolution.
    void ApplyTAAU(
        VkCommandBuffer cmd, uint32_t frameIndex,
        const std::shared_ptr<const GlobalUniform> &uniform);

    // Q2RTX-style fog volumes: blends the final HDR image toward the fog color.
    void ApplyFog(
        VkCommandBuffer cmd, uint32_t frameIndex,
        const std::shared_ptr<const GlobalUniform> &uniform);

    void OnShaderReload(const ShaderManager *shaderManager) override;

private:
    void CreatePipelineLayout(VkDescriptorSetLayout *pSetLayouts, uint32_t setLayoutCount);
    void CreatePipelines(const ShaderManager *shaderManager);
    void DestroyPipelines();

private:
    // Number of a-trous iterations of the Q2RTX-style gradient filter
    // (CmQ2GradientAtrous.comp). 7 like Q2RTX: LF in all, HF/SPEC in the
    // first 3, LF normalized in the last one.
    static constexpr uint32_t Q2_GRADIENT_ATROUS_ITERATION_COUNT = 7;

    VkDevice device;

    std::shared_ptr<Framebuffers> framebuffers;

    VkPipelineLayout pipelineLayout;

    // Q2RTX-style gradient pipeline (produces Q2GradLF / Q2GradHFSpec,
    // consumed by the temporal pass): reproject + gradient img + gradient atrous.
    VkPipeline gradientReproject;
    VkPipeline gradientImg;
    VkPipeline gradientAtrous[Q2_GRADIENT_ATROUS_ITERATION_COUNT];

    VkPipeline adapter;
    VkPipeline temporal;
    VkPipeline atrousLF[4];
    VkPipeline atrous[4];
    VkPipeline interleave;
    VkPipeline fog;
    VkPipeline taau;
};

}
