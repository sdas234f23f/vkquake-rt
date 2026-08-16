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

#include "Q2Denoiser.h"

#include "Generated/ShaderCommonC.h"
#include "CmdLabel.h"
#include "Utils.h"

using namespace vkpt;

Q2Denoiser::Q2Denoiser(
    VkDevice _device,
    std::shared_ptr<Framebuffers> _framebuffers,
    const std::shared_ptr<const ShaderManager> &shaderManager,
    const std::shared_ptr<const GlobalUniform> &uniform,
    const std::shared_ptr<const ASManager> &asManager)
:
    device(_device),
    framebuffers(std::move(_framebuffers)),
    pipelineLayout(VK_NULL_HANDLE),
    gradientReproject(VK_NULL_HANDLE),
    gradientImg(VK_NULL_HANDLE),
    gradientAtrous{},
    adapter(VK_NULL_HANDLE),
    temporal(VK_NULL_HANDLE),
    atrousLF{},
    atrous{},
    interleave(VK_NULL_HANDLE),
    fog(VK_NULL_HANDLE),
    taau(VK_NULL_HANDLE)
{
    static_assert(sizeof(atrous) / sizeof(VkPipeline) == COMPUTE_SVGF_ATROUS_ITERATION_COUNT, "Wrong atrous pipeline count");
    static_assert(sizeof(atrousLF) / sizeof(VkPipeline) == COMPUTE_SVGF_ATROUS_ITERATION_COUNT, "Wrong atrousLF pipeline count");
    static_assert(sizeof(gradientAtrous) / sizeof(VkPipeline) == Q2_GRADIENT_ATROUS_ITERATION_COUNT, "Wrong gradient atrous pipeline count");

    (void)asManager;

    VkDescriptorSetLayout setLayouts[] =
    {
        framebuffers->GetDescSetLayout(),
        uniform->GetDescSetLayout(),
    };

    CreatePipelineLayout(setLayouts, std::size(setLayouts));
    CreatePipelines(shaderManager.get());
}

Q2Denoiser::~Q2Denoiser()
{
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);

    DestroyPipelines();
}

void Q2Denoiser::CreatePipelineLayout(VkDescriptorSetLayout *pSetLayouts, uint32_t setLayoutCount)
{
    // iteration for the LF a-trous pass (CmQ2AtrousLF.comp)
    VkPushConstantRange pushConstant = {};
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(uint32_t);

    VkPipelineLayoutCreateInfo plLayoutInfo = {};
    plLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plLayoutInfo.setLayoutCount = setLayoutCount;
    plLayoutInfo.pSetLayouts = pSetLayouts;
    plLayoutInfo.pushConstantRangeCount = 1;
    plLayoutInfo.pPushConstantRanges = &pushConstant;

    VkResult r = vkCreatePipelineLayout(device, &plLayoutInfo, nullptr, &pipelineLayout);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, pipelineLayout, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "Q2Denoiser pipeline layout");
}

void Q2Denoiser::CreatePipelines(const ShaderManager *shaderManager)
{
    VkResult r;

    uint32_t iteration = 0;

    VkSpecializationMapEntry specEntry = {};
    specEntry.constantID = 0;
    specEntry.offset = 0;
    specEntry.size = sizeof(uint32_t);

    VkSpecializationInfo specInfo = {};
    specInfo.mapEntryCount = 1;
    specInfo.pMapEntries = &specEntry;
    specInfo.dataSize = sizeof(uint32_t);
    specInfo.pData = &iteration;

    // Q2RTX-style gradient pipeline (produces Q2GradLF / Q2GradHFSpec for the
    // temporal pass): reproject + gradient img + gradient atrous (7 iterations).
    {
        VkComputePipelineCreateInfo plInfo = {};
        plInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        plInfo.layout = pipelineLayout;
        plInfo.stage = shaderManager->GetStageInfo("CQ2GradientReproject");

        r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &plInfo, nullptr, &gradientReproject);
        VK_CHECKERROR(r);

        SET_DEBUG_NAME(device, gradientReproject, VK_OBJECT_TYPE_PIPELINE, "Q2 ASVGF gradient reproject pipeline");
    }

    {
        VkComputePipelineCreateInfo plInfo = {};
        plInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        plInfo.layout = pipelineLayout;
        plInfo.stage = shaderManager->GetStageInfo("CQ2GradientImg");

        r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &plInfo, nullptr, &gradientImg);
        VK_CHECKERROR(r);

        SET_DEBUG_NAME(device, gradientImg, VK_OBJECT_TYPE_PIPELINE, "Q2 ASVGF gradient img pipeline");
    }

    {
        const char *debugNames[Q2_GRADIENT_ATROUS_ITERATION_COUNT] =
        {
            "Q2 ASVGF gradient atrous iteration #0 pipeline",
            "Q2 ASVGF gradient atrous iteration #1 pipeline",
            "Q2 ASVGF gradient atrous iteration #2 pipeline",
            "Q2 ASVGF gradient atrous iteration #3 pipeline",
            "Q2 ASVGF gradient atrous iteration #4 pipeline",
            "Q2 ASVGF gradient atrous iteration #5 pipeline",
            "Q2 ASVGF gradient atrous iteration #6 pipeline",
        };

        VkComputePipelineCreateInfo plInfo = {};
        plInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        plInfo.layout = pipelineLayout;
        plInfo.stage = shaderManager->GetStageInfo("CQ2GradientAtrous");

        for (uint32_t i = 0; i < Q2_GRADIENT_ATROUS_ITERATION_COUNT; i++)
        {
            r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &plInfo, nullptr, &gradientAtrous[i]);
            VK_CHECKERROR(r);

            SET_DEBUG_NAME(device, gradientAtrous[i], VK_OBJECT_TYPE_PIPELINE, debugNames[i]);
        }
    }

    {
        VkComputePipelineCreateInfo plInfo = {};
        plInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        plInfo.layout = pipelineLayout;
        plInfo.stage = shaderManager->GetStageInfo("CQ2Adapter");

        r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &plInfo, nullptr, &adapter);
        VK_CHECKERROR(r);

        SET_DEBUG_NAME(device, adapter, VK_OBJECT_TYPE_PIPELINE, "Q2 ASVGF adapter pipeline");
    }

    {
        VkComputePipelineCreateInfo plInfo = {};
        plInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        plInfo.layout = pipelineLayout;
        plInfo.stage = shaderManager->GetStageInfo("CQ2Temporal");

        r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &plInfo, nullptr, &temporal);
        VK_CHECKERROR(r);

        SET_DEBUG_NAME(device, temporal, VK_OBJECT_TYPE_PIPELINE, "Q2 ASVGF temporal pipeline");
    }

    // LF a-trous (push constant iteration)
    {
        const char *debugNames[COMPUTE_SVGF_ATROUS_ITERATION_COUNT] =
        {
            "Q2 ASVGF LF atrous iteration #0 pipeline",
            "Q2 ASVGF LF atrous iteration #1 pipeline",
            "Q2 ASVGF LF atrous iteration #2 pipeline",
            "Q2 ASVGF LF atrous iteration #3 pipeline",
        };

        VkComputePipelineCreateInfo plInfo = {};
        plInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        plInfo.layout = pipelineLayout;
        plInfo.stage = shaderManager->GetStageInfo("CQ2AtrousLF");

        for (uint32_t i = 0; i < COMPUTE_SVGF_ATROUS_ITERATION_COUNT; i++)
        {
            r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &plInfo, nullptr, &atrousLF[i]);
            VK_CHECKERROR(r);

            SET_DEBUG_NAME(device, atrousLF[i], VK_OBJECT_TYPE_PIPELINE, debugNames[i]);
        }
    }

    // HF/SPEC atrous (spec constant iteration)
    {
        const char *debugNames[COMPUTE_SVGF_ATROUS_ITERATION_COUNT] =
        {
            "Q2 ASVGF atrous iteration #0 pipeline",
            "Q2 ASVGF atrous iteration #1 pipeline",
            "Q2 ASVGF atrous iteration #2 pipeline",
            "Q2 ASVGF atrous iteration #3 pipeline",
        };

        VkComputePipelineCreateInfo plInfo = {};
        plInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        plInfo.layout = pipelineLayout;
        plInfo.stage = shaderManager->GetStageInfo("CQ2Atrous");
        plInfo.stage.pSpecializationInfo = &specInfo;

        for (uint32_t i = 0; i < COMPUTE_SVGF_ATROUS_ITERATION_COUNT; i++)
        {
            iteration = i;

            r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &plInfo, nullptr, &atrous[i]);
            VK_CHECKERROR(r);

            SET_DEBUG_NAME(device, atrous[i], VK_OBJECT_TYPE_PIPELINE, debugNames[i]);
        }
    }

    {
        VkComputePipelineCreateInfo plInfo = {};
        plInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        plInfo.layout = pipelineLayout;
        plInfo.stage = shaderManager->GetStageInfo("CQ2Interleave");

        r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &plInfo, nullptr, &interleave);
        VK_CHECKERROR(r);

        SET_DEBUG_NAME(device, interleave, VK_OBJECT_TYPE_PIPELINE, "Q2 ASVGF interleave pipeline");
    }

    {
        VkComputePipelineCreateInfo plInfo = {};
        plInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        plInfo.layout = pipelineLayout;
        plInfo.stage = shaderManager->GetStageInfo("CQ2Fog");

        r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &plInfo, nullptr, &fog);
        VK_CHECKERROR(r);

        SET_DEBUG_NAME(device, fog, VK_OBJECT_TYPE_PIPELINE, "Q2 fog volumes pipeline");
    }

    {
        VkComputePipelineCreateInfo plInfo = {};
        plInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        plInfo.layout = pipelineLayout;
        plInfo.stage = shaderManager->GetStageInfo("CQ2TAAU");

        r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &plInfo, nullptr, &taau);
        VK_CHECKERROR(r);

        SET_DEBUG_NAME(device, taau, VK_OBJECT_TYPE_PIPELINE, "Q2 TAAU pipeline");
    }
}

void Q2Denoiser::DestroyPipelines()
{
    vkDestroyPipeline(device, gradientReproject, nullptr);
    vkDestroyPipeline(device, gradientImg, nullptr);
    vkDestroyPipeline(device, adapter, nullptr);
    vkDestroyPipeline(device, temporal, nullptr);
    vkDestroyPipeline(device, interleave, nullptr);
    vkDestroyPipeline(device, fog, nullptr);
    vkDestroyPipeline(device, taau, nullptr);

    for (VkPipeline &p : gradientAtrous)
    {
        vkDestroyPipeline(device, p, nullptr);
        p = VK_NULL_HANDLE;
    }

    for (VkPipeline &p : atrousLF)
    {
        vkDestroyPipeline(device, p, nullptr);
        p = VK_NULL_HANDLE;
    }

    for (VkPipeline &p : atrous)
    {
        vkDestroyPipeline(device, p, nullptr);
        p = VK_NULL_HANDLE;
    }

    for (VkPipeline &p : gradientAtrous)
    {
        vkDestroyPipeline(device, p, nullptr);
        p = VK_NULL_HANDLE;
    }

    gradientReproject = VK_NULL_HANDLE;
    gradientImg = VK_NULL_HANDLE;
    adapter = VK_NULL_HANDLE;
    temporal = VK_NULL_HANDLE;
    interleave = VK_NULL_HANDLE;
    fog = VK_NULL_HANDLE;
    taau = VK_NULL_HANDLE;
}

void Q2Denoiser::OnShaderReload(const ShaderManager *shaderManager)
{
    DestroyPipelines();
    CreatePipelines(shaderManager);
}

void Q2Denoiser::GradientReproject(
    VkCommandBuffer cmd, uint32_t frameIndex,
    const std::shared_ptr<const GlobalUniform> &uniform)
{
    typedef FramebufferImageIndex FI;

    VkDescriptorSet sets[] =
    {
        framebuffers->GetDescSet(frameIndex),
        uniform->GetDescSet(frameIndex)
    };

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                        pipelineLayout,
                        0, std::size(sets), sets,
                        0, nullptr);

    const uint32_t wgGradX = Utils::GetWorkGroupCount(uniform->GetData()->renderWidth / COMPUTE_ASVGF_STRATA_SIZE, COMPUTE_GRADIENT_ATROUS_GROUP_SIZE_X);
    const uint32_t wgGradY = Utils::GetWorkGroupCount(uniform->GetData()->renderHeight / COMPUTE_ASVGF_STRATA_SIZE, COMPUTE_GRADIENT_ATROUS_GROUP_SIZE_X);

    CmdLabel label(cmd, "Q2 ASVGF gradient reproject (before lighting)");

    // Reads the previous frame G-buffer + gradient data and patches the current
    // frame G-buffer + RNG seed at gradient sample pixels (see CmQ2GradientReproject.comp),
    // so the lighting passes that run next re-trace those pixels with the
    // previous frame's random number sequence (Q2RTX asvgf_gradient_reproject).
    FI fs[] =
    {
        // current frame inputs (written by primary / reflections / god rays)
        FI::FB_IMAGE_INDEX_Q2_VIEW_DEPTH,
        FI::FB_IMAGE_INDEX_NORMAL_GEOMETRY,
        FI::FB_IMAGE_INDEX_MOTION,
        // previous frame inputs
        FI::FB_IMAGE_INDEX_Q2_GRAD_SMPL_POS_PREV,
        FI::FB_IMAGE_INDEX_Q2_VIEW_DEPTH_PREV,
        FI::FB_IMAGE_INDEX_Q2_COLOR_H_F_PREV,
        FI::FB_IMAGE_INDEX_Q2_COLOR_SPEC_PREV,
        // NOTE: Q2_RNG_SEED_PREV is the LAST framebuffer in the list, and
        // FrameIndexToFBIndex(i, frameIndex) = i + frameIndex for swappable
        // pairs -> it would resolve to i+2 (out of range) at frameIndex 1.
        // The prev RNG seed image is barriered via Q2_RNG_SEED (current),
        // which resolves to the physical prev image at frameIndex 1.
        FI::FB_IMAGE_INDEX_Q2_BASE_COLOR_PREV,
        FI::FB_IMAGE_INDEX_Q2_METALLIC_PREV,
        FI::FB_IMAGE_INDEX_NORMAL_PREV,
        FI::FB_IMAGE_INDEX_METALLIC_ROUGHNESS_PREV,
        FI::FB_IMAGE_INDEX_SURFACE_POSITION_PREV,
        FI::FB_IMAGE_INDEX_VIEW_DIRECTION_PREV,
        FI::FB_IMAGE_INDEX_ALBEDO_PREV,
        // current frame outputs (patched G-buffer)
        FI::FB_IMAGE_INDEX_Q2_GRAD_SMPL_POS,
        FI::FB_IMAGE_INDEX_Q2_GRAD_H_F_SPEC_PING,
        FI::FB_IMAGE_INDEX_Q2_RNG_SEED,
        FI::FB_IMAGE_INDEX_Q2_BASE_COLOR,
        FI::FB_IMAGE_INDEX_Q2_METALLIC,
        FI::FB_IMAGE_INDEX_NORMAL,
        FI::FB_IMAGE_INDEX_METALLIC_ROUGHNESS,
        FI::FB_IMAGE_INDEX_SURFACE_POSITION,
        FI::FB_IMAGE_INDEX_VIEW_DIRECTION,
        FI::FB_IMAGE_INDEX_ALBEDO,
    };
    framebuffers->BarrierMultiple(cmd, frameIndex, fs);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gradientReproject);
    vkCmdDispatch(cmd, wgGradX, wgGradY, 1);
}

void Q2Denoiser::Denoise(
    VkCommandBuffer cmd, uint32_t frameIndex,
    const std::shared_ptr<const GlobalUniform> &uniform)
{
    typedef FramebufferImageIndex FI;

    // bind desc sets
    VkDescriptorSet sets[] =
    {
        framebuffers->GetDescSet(frameIndex),
        uniform->GetDescSet(frameIndex)
    };

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                        pipelineLayout,
                        0, std::size(sets), sets,
                        0, nullptr);

    const uint32_t wgX = Utils::GetWorkGroupCount(uniform->GetData()->renderWidth, 16);
    const uint32_t wgY = Utils::GetWorkGroupCount(uniform->GetData()->renderHeight, 16);

    // CmQ2Temporal.comp uses a 15x15 workgroup (Q2RTX GROUP_SIZE)
    const uint32_t wgTX = Utils::GetWorkGroupCount(uniform->GetData()->renderWidth, 15);
    const uint32_t wgTY = Utils::GetWorkGroupCount(uniform->GetData()->renderHeight, 15);

    const uint32_t wgGradX = Utils::GetWorkGroupCount(uniform->GetData()->renderWidth / COMPUTE_ASVGF_STRATA_SIZE, COMPUTE_GRADIENT_ATROUS_GROUP_SIZE_X);
    const uint32_t wgGradY = Utils::GetWorkGroupCount(uniform->GetData()->renderHeight / COMPUTE_ASVGF_STRATA_SIZE, COMPUTE_GRADIENT_ATROUS_GROUP_SIZE_X);

    // ReSTIR outputs -> Q2RTX ASVGF channel format
    {
        CmdLabel label(cmd, "Q2 ASVGF adapter");

        FI fs[] =
        {
            FI::FB_IMAGE_INDEX_UNFILTERED_DIRECT,
            FI::FB_IMAGE_INDEX_UNFILTERED_SPECULAR,
            FI::FB_IMAGE_INDEX_UNFILTERED_INDIRECT_S_H_R,
            FI::FB_IMAGE_INDEX_UNFILTERED_INDIRECT_S_H_G,
            FI::FB_IMAGE_INDEX_UNFILTERED_INDIRECT_S_H_B,
        };
        framebuffers->BarrierMultiple(cmd, frameIndex, fs);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, adapter);
        vkCmdDispatch(cmd, wgX, wgY, 1);
    }

    {
        CmdLabel label(cmd, "Q2 ASVGF gradient img");

        FI fs[] =
        {
            FI::FB_IMAGE_INDEX_Q2_GRAD_SMPL_POS,
            FI::FB_IMAGE_INDEX_Q2_GRAD_H_F_SPEC_PING,
            FI::FB_IMAGE_INDEX_Q2_GRAD_L_F_PING,
            FI::FB_IMAGE_INDEX_Q2_HIST_COLOR_L_F_S_H_PREV,
            FI::FB_IMAGE_INDEX_Q2_COLOR_L_F_S_H,
            FI::FB_IMAGE_INDEX_Q2_COLOR_H_F,
            FI::FB_IMAGE_INDEX_Q2_COLOR_SPEC,
            FI::FB_IMAGE_INDEX_MOTION,
        };
        framebuffers->BarrierMultiple(cmd, frameIndex, fs);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gradientImg);
        vkCmdDispatch(cmd, wgGradX, wgGradY, 1);
    }

    {
        CmdLabel label(cmd, "Q2 ASVGF gradient atrous");

        for (uint32_t i = 0; i < Q2_GRADIENT_ATROUS_ITERATION_COUNT; i++)
        {
            // input ping/pong for this iteration (see CmQ2GradientAtrous.comp)
            FI fs[] =
            {
                (i % 2 == 0) ? FI::FB_IMAGE_INDEX_Q2_GRAD_L_F_PING : FI::FB_IMAGE_INDEX_Q2_GRAD_L_F_PONG,
                (i % 2 == 0) ? FI::FB_IMAGE_INDEX_Q2_GRAD_H_F_SPEC_PING : FI::FB_IMAGE_INDEX_Q2_GRAD_H_F_SPEC_PONG,
            };
            framebuffers->BarrierMultiple(cmd, frameIndex, fs);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gradientAtrous[i]);
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t), &i);
            vkCmdDispatch(cmd, wgGradX, wgGradY, 1);
        }
    }

    // temporal accumulation
    {
        CmdLabel label(cmd, "Q2 ASVGF temporal");

        FI fs[] =
        {
            FI::FB_IMAGE_INDEX_Q2_COLOR_L_F_S_H,
            FI::FB_IMAGE_INDEX_Q2_COLOR_L_F_C_O_C_G,
            FI::FB_IMAGE_INDEX_Q2_COLOR_H_F,
            FI::FB_IMAGE_INDEX_Q2_COLOR_SPEC,
            FI::FB_IMAGE_INDEX_MOTION,
            FI::FB_IMAGE_INDEX_DEPTH_WORLD,
            FI::FB_IMAGE_INDEX_DEPTH_GRAD,
            FI::FB_IMAGE_INDEX_NORMAL,
            FI::FB_IMAGE_INDEX_NORMAL_GEOMETRY,
            FI::FB_IMAGE_INDEX_METALLIC_ROUGHNESS,
            FI::FB_IMAGE_INDEX_THROUGHPUT,
            FI::FB_IMAGE_INDEX_Q2_GRAD_L_F_PONG,
            FI::FB_IMAGE_INDEX_Q2_GRAD_H_F_SPEC_PONG,
        };
        framebuffers->BarrierMultiple(cmd, frameIndex, fs);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, temporal);
        vkCmdDispatch(cmd, wgTX, wgTY, 1);
    }

    // LF a-trous at 1/3 resolution
    {
        CmdLabel label(cmd, "Q2 ASVGF LF atrous");

        for (uint32_t i = 0; i < COMPUTE_SVGF_ATROUS_ITERATION_COUNT; i++)
        {
            // input ping/pong for this iteration (see CmQ2AtrousLF.comp)
            FI fs[] =
            {
                (i % 2 == 0) ? FI::FB_IMAGE_INDEX_Q2_ATROUS_PING_L_F_S_H : FI::FB_IMAGE_INDEX_Q2_ATROUS_PONG_L_F_S_H,
                (i % 2 == 0) ? FI::FB_IMAGE_INDEX_Q2_ATROUS_PING_L_F_C_O_C_G : FI::FB_IMAGE_INDEX_Q2_ATROUS_PONG_L_F_C_O_C_G,
            };
            framebuffers->BarrierMultiple(cmd, frameIndex, fs);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, atrousLF[i]);
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t), &i);
            vkCmdDispatch(cmd, wgGradX, wgGradY, 1);
        }
    }

    // HF/SPEC atrous + compositing
    {
        CmdLabel label(cmd, "Q2 ASVGF atrous");

        for (uint32_t i = 0; i < COMPUTE_SVGF_ATROUS_ITERATION_COUNT; i++)
        {
            // inputs for this iteration (see CmQ2Atrous.comp)
            switch (i)
            {
                case 0:
                {
                    FI fs[] =
                    {
                        FI::FB_IMAGE_INDEX_Q2_ATROUS_PING_H_F,
                        FI::FB_IMAGE_INDEX_Q2_ATROUS_PING_SPEC,
                        FI::FB_IMAGE_INDEX_Q2_ATROUS_PING_MOMENTS,
                    };
                    framebuffers->BarrierMultiple(cmd, frameIndex, fs);
                    break;
                }
                case 1:
                {
                    FI fs[] =
                    {
                        FI::FB_IMAGE_INDEX_Q2_HIST_COLOR_H_F,
                        FI::FB_IMAGE_INDEX_Q2_ATROUS_PONG_SPEC,
                        FI::FB_IMAGE_INDEX_Q2_ATROUS_PONG_MOMENTS,
                    };
                    framebuffers->BarrierMultiple(cmd, frameIndex, fs);
                    break;
                }
                case 2:
                {
                    FI fs[] =
                    {
                        FI::FB_IMAGE_INDEX_Q2_ATROUS_PING_H_F,
                        FI::FB_IMAGE_INDEX_Q2_ATROUS_PING_SPEC,
                        FI::FB_IMAGE_INDEX_Q2_ATROUS_PING_MOMENTS,
                    };
                    framebuffers->BarrierMultiple(cmd, frameIndex, fs);
                    break;
                }
                case 3:
                {
                    FI fs[] =
                    {
                        FI::FB_IMAGE_INDEX_Q2_ATROUS_PONG_H_F,
                        FI::FB_IMAGE_INDEX_Q2_ATROUS_PONG_SPEC,
                        FI::FB_IMAGE_INDEX_Q2_ATROUS_PONG_MOMENTS,
                        FI::FB_IMAGE_INDEX_Q2_ATROUS_PING_L_F_S_H,
                        FI::FB_IMAGE_INDEX_Q2_ATROUS_PING_L_F_C_O_C_G,
                        FI::FB_IMAGE_INDEX_Q2_HIST_COLOR_L_F_S_H,
                        FI::FB_IMAGE_INDEX_Q2_HIST_COLOR_L_F_C_O_C_G,
                        FI::FB_IMAGE_INDEX_Q2_FOG_ACCUM,
                    };
                    framebuffers->BarrierMultiple(cmd, frameIndex, fs);
                    break;
                }
                default:
                {
                    assert(0);
                    return;
                }
            }

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, atrous[i]);
            vkCmdDispatch(cmd, wgX, wgY, 1);
        }
    }

    // checkerboard interleave -> PreFinal (regular layout)
    {
        CmdLabel label(cmd, "Q2 ASVGF interleave");

        FI fs[] =
        {
            FI::FB_IMAGE_INDEX_Q2_COLOR,
            FI::FB_IMAGE_INDEX_THROUGHPUT,
        };
        framebuffers->BarrierMultiple(cmd, frameIndex, fs);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, interleave);
        vkCmdDispatch(cmd, wgX, wgY, 1);
    }
}

void Q2Denoiser::ApplyTAAU(
    VkCommandBuffer cmd, uint32_t frameIndex,
    const std::shared_ptr<const GlobalUniform> &uniform)
{
    typedef FramebufferImageIndex FI;

    VkDescriptorSet sets[] =
    {
        framebuffers->GetDescSet(frameIndex),
        uniform->GetDescSet(frameIndex)
    };

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                        pipelineLayout,
                        0, std::size(sets), sets,
                        0, nullptr);

    CmdLabel label(cmd, "Q2 TAAU");

    FI fs[] =
    {
        FI::FB_IMAGE_INDEX_FINAL,
        FI::FB_IMAGE_INDEX_MOTION_DLSS,
        FI::FB_IMAGE_INDEX_Q2_TAA_HISTORY,
    };
    framebuffers->BarrierMultiple(cmd, frameIndex, fs);

    const uint32_t wgX = Utils::GetWorkGroupCount(uniform->GetData()->upscaledRenderWidth, 16);
    const uint32_t wgY = Utils::GetWorkGroupCount(uniform->GetData()->upscaledRenderHeight, 16);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, taau);
    vkCmdDispatch(cmd, wgX, wgY, 1);
}

void Q2Denoiser::ApplyFog(
    VkCommandBuffer cmd, uint32_t frameIndex,
    const std::shared_ptr<const GlobalUniform> &uniform)
{
    typedef FramebufferImageIndex FI;

    VkDescriptorSet sets[] =
    {
        framebuffers->GetDescSet(frameIndex),
        uniform->GetDescSet(frameIndex)
    };

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                        pipelineLayout,
                        0, std::size(sets), sets,
                        0, nullptr);

    CmdLabel label(cmd, "Q2 fog volumes");

    FI fs[] =
    {
        FI::FB_IMAGE_INDEX_FINAL,
        FI::FB_IMAGE_INDEX_DEPTH_WORLD,
    };
    framebuffers->BarrierMultiple(cmd, frameIndex, fs);

    const uint32_t wgX = Utils::GetWorkGroupCount(uniform->GetData()->renderWidth, 16);
    const uint32_t wgY = Utils::GetWorkGroupCount(uniform->GetData()->renderHeight, 16);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fog);
    vkCmdDispatch(cmd, wgX, wgY, 1);
}
