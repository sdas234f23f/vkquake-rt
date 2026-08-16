// Copyright (c) 2022 Sultim Tsyrendashiev
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

#include "LightGrid.h"

#include "CmdLabel.h"
#include "Utils.h"
#include "Generated/ShaderCommonC.h"

vkpt::LightGrid::LightGrid(
    VkDevice _device,
    const std::shared_ptr<ShaderManager> &_shaderManager,
    const std::shared_ptr<GlobalUniform> &_uniform,
    const std::shared_ptr<BlueNoise> &_blueNoise,
    const std::shared_ptr<LightManager> &_lightManager
)
    : device(_device)
    , pipelineLayout(VK_NULL_HANDLE)
    , q2ListBuildPipeline(VK_NULL_HANDLE)
{
    VkDescriptorSetLayout setLayouts[] =
    {
        _uniform->GetDescSetLayout(),
        _blueNoise->GetDescSetLayout(),
        _lightManager->GetDescSetLayout()
    };

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = std::size(setLayouts);
    layoutInfo.pSetLayouts = setLayouts;

    VkResult r = vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, pipelineLayout, VK_OBJECT_TYPE_PIPELINE_LAYOUT, "Light grid pipeline layout");


    CreatePipelines(_shaderManager.get());
}

vkpt::LightGrid::~LightGrid()
{
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    DestroyPipelines();
}

void vkpt::LightGrid::Q2Build(
    VkCommandBuffer cmd, uint32_t frameIndex,
    const std::shared_ptr<GlobalUniform> &uniform,
    const std::shared_ptr<BlueNoise> &blueNoise,
    const std::shared_ptr<LightManager> &lightManager)
{
    CmdLabel label(cmd, "Q2 light list build");

    VkDescriptorSet sets[] =
    {
        uniform->GetDescSet(frameIndex),
        blueNoise->GetDescSet(),
        lightManager->GetDescSet(frameIndex),
    };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        pipelineLayout,
        0, std::size(sets), sets,
        0, nullptr);

    uint32_t wgCountX = Utils::GetWorkGroupCount(static_cast<uint32_t>(Q2_LIGHT_LIST_CELL_COUNT), COMPUTE_Q2_LIGHT_LIST_GROUP_SIZE_X);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, q2ListBuildPipeline);
    vkCmdDispatch(cmd, wgCountX, 1, 1);
}

void vkpt::LightGrid::OnShaderReload(const ShaderManager* shaderManager)
{
    DestroyPipelines();
    CreatePipelines(shaderManager);
}

void vkpt::LightGrid::CreatePipelines(const ShaderManager* shaderManager)
{
    VkComputePipelineCreateInfo plInfo = {};
    plInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    plInfo.layout = pipelineLayout;
    plInfo.stage = shaderManager->GetStageInfo("CQ2LightListBuild");

    VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &plInfo, nullptr, &q2ListBuildPipeline);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, q2ListBuildPipeline, VK_OBJECT_TYPE_PIPELINE, "Q2 light list build pipeline");
}

void vkpt::LightGrid::DestroyPipelines()
{
    vkDestroyPipeline(device, q2ListBuildPipeline, nullptr);
    q2ListBuildPipeline = VK_NULL_HANDLE;
}
