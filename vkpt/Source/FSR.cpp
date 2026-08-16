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

#include "FSR.h"

#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>

#include "RenderResolutionHelper.h"
#include "RgException.h"

#include <Windows.h>
#include <vector>

namespace
{
    void FsrMessageCallback(uint32_t type, const wchar_t* msg)
    {
        char buf[512];
        snprintf(buf, sizeof(buf), "[FSR] type=%u: %S\n", type, msg);
        OutputDebugStringA(buf);
    }

    FfxApiSurfaceFormat MapVkFormat(VkFormat fmt)
    {
        switch (fmt)
        {
        case VK_FORMAT_R16G16B16A16_SFLOAT:        return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
        case VK_FORMAT_R32G32B32A32_SFLOAT:        return FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT;
        case VK_FORMAT_R32G32_SFLOAT:              return FFX_API_SURFACE_FORMAT_R32G32_FLOAT;
        case VK_FORMAT_R32_SFLOAT:                 return FFX_API_SURFACE_FORMAT_R32_FLOAT;
        case VK_FORMAT_R16G16_SFLOAT:              return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
        case VK_FORMAT_R16_SFLOAT:                 return FFX_API_SURFACE_FORMAT_R16_FLOAT;
        case VK_FORMAT_R8G8B8A8_UNORM:             return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_UNORM:             return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:    return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:   return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
        case VK_FORMAT_R16G16_UINT:                return FFX_API_SURFACE_FORMAT_R16G16_UINT;
        default:                                   return FFX_API_SURFACE_FORMAT_UNKNOWN;
        }
    }

    constexpr vkpt::FramebufferImageIndex OUTPUT_IMAGE_INDEX = vkpt::FB_IMAGE_INDEX_UPSCALED_PONG;

    FfxApiResource ToFfxApiResource(
        vkpt::FramebufferImageIndex fbImage, uint32_t frameIndex,
        const vkpt::Framebuffers& framebuffers,
        const vkpt::ResolutionState& resolutionState)
    {
        auto [image, view, format, sz] = framebuffers.GetImageHandles(fbImage, frameIndex, resolutionState);

        FfxApiResource res = {};
        res.resource = (void*)image;
        res.description.type     = FFX_API_RESOURCE_TYPE_TEXTURE2D;
        res.description.format   = MapVkFormat(format);
        res.description.width    = sz.width;
        res.description.height   = sz.height;
        res.description.depth    = 1;
        res.description.mipCount = 1;
        res.description.flags    = FFX_API_RESOURCE_FLAGS_NONE;
        res.description.usage    = (fbImage == OUTPUT_IMAGE_INDEX)
            ? FFX_API_RESOURCE_USAGE_UAV
            : FFX_API_RESOURCE_USAGE_READ_ONLY;
        res.state = (fbImage == OUTPUT_IMAGE_INDEX)
            ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS
            : FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
        return res;
    }

    template <size_t N>
    void InsertBarriers(
        VkCommandBuffer cmd, uint32_t frameIndex,
        vkpt::Framebuffers& framebuffers,
        const vkpt::FramebufferImageIndex (&inputsAndOutput)[N],
        bool isBackwards)
    {
        assert(std::find(std::begin(inputsAndOutput), std::end(inputsAndOutput), OUTPUT_IMAGE_INDEX) != std::end(inputsAndOutput));

        VkImageMemoryBarrier2 barriers[N];
        for (size_t i = 0; i < N; i++)
        {
            auto& b = barriers[i];
            b = {};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b.dstAccessMask = inputsAndOutput[i] == OUTPUT_IMAGE_INDEX ? VK_ACCESS_2_SHADER_WRITE_BIT : VK_ACCESS_2_SHADER_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.newLayout = inputsAndOutput[i] == OUTPUT_IMAGE_INDEX ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = framebuffers.GetImage(inputsAndOutput[i], frameIndex);
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.baseMipLevel = 0;
            b.subresourceRange.levelCount = 1;
            b.subresourceRange.baseArrayLayer = 0;
            b.subresourceRange.layerCount = 1;

            if (isBackwards)
            {
                std::swap(b.srcStageMask, b.dstStageMask);
                std::swap(b.srcAccessMask, b.dstAccessMask);
                std::swap(b.oldLayout, b.newLayout);
                std::swap(b.srcQueueFamilyIndex, b.dstQueueFamilyIndex);
            }
        }

        VkDependencyInfoKHR depInfo = {};
        depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR;
        depInfo.imageMemoryBarrierCount = static_cast<uint32_t>(N);
        depInfo.pImageMemoryBarriers = barriers;

        vkpt::svkCmdPipelineBarrier2KHR(cmd, &depInfo);
    }
}

vkpt::FSR::FSR(VkDevice _device, VkPhysicalDevice _physDevice, UserPrint* pUserPrint)
    : m_device(_device)
    , m_physDevice(_physDevice)
    , m_pUserPrint(pUserPrint)
    , m_context(nullptr)
    , m_requestedTechnique(RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3)
    , m_technique(RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3)
    , m_renderWidth(0)
    , m_renderHeight(0)
    , m_displayWidth(0)
    , m_displayHeight(0)
    , m_hasSize(false)
{
}

vkpt::FSR::~FSR()
{
    DestroyContext();
}

void vkpt::FSR::SetUpscaleVersion(RgRenderUpscaleTechnique technique)
{
    if (technique != RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2 &&
        technique != RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3)
    {
        // FSR is not used, destroy the context (if any) and remember the technique
        m_requestedTechnique = technique;
        m_technique = technique;
        if (m_context)
        {
            vkDeviceWaitIdle(m_device);
            DestroyContext();
        }
        return;
    }

    if (technique == m_requestedTechnique && m_context)
    {
        return;
    }

    // The FidelityFX context is recreated on version change. Make sure the GPU
    // is no longer using the old context's resources before destroying them.
    vkDeviceWaitIdle(m_device);

    m_requestedTechnique = technique;
    RecreateContext();
}

void vkpt::FSR::OnFramebuffersSizeChange(const ResolutionState& resolutionState)
{
    m_renderWidth  = resolutionState.renderWidth;
    m_renderHeight = resolutionState.renderHeight;
    m_displayWidth  = resolutionState.upscaledWidth;
    m_displayHeight = resolutionState.upscaledHeight;
    m_hasSize = true;

    if (m_requestedTechnique == RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2 ||
        m_requestedTechnique == RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3)
    {
        RecreateContext();
    }
    else
    {
        DestroyContext();
    }
}

uint64_t vkpt::FSR::FindVersionId(bool preferFsr3)
{
    ffxQueryDescGetVersions q = {};
    q.header.type       = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType    = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device            = nullptr; // Vulkan backend ignores this
    uint64_t count      = 0;
    q.outputCount       = &count;

    if (ffxQuery(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0)
    {
        return 0;
    }

    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    q.versionIds   = ids.data();
    q.versionNames = names.data();

    if (ffxQuery(nullptr, &q.header) != FFX_API_RETURN_OK)
    {
        return 0;
    }

    // Version names are like "2.3.3" (FSR2) or "3.1.4" (FSR3.1). Match by the major version.
    for (uint64_t i = 0; i < count; i++)
    {
        const char* name = names[i] ? names[i] : "";
        if (preferFsr3)
        {
            if (name[0] == '3')
            {
                return ids[i];
            }
        }
        else
        {
            if (name[0] == '2')
            {
                return ids[i];
            }
        }
    }

    return 0;
}

bool vkpt::FSR::IsUpscaleVersionAvailable(RgRenderUpscaleTechnique technique)
{
    switch (technique)
    {
        case RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2:
            return FindVersionId(false) != 0;
        case RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3:
            return FindVersionId(true) != 0;
        default:
            return false;
    }
}

void vkpt::FSR::RecreateContext()
{
    DestroyContext();

    if (!m_hasSize)
    {
        return;
    }

    bool preferFsr3 = (m_requestedTechnique == RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3);
    uint64_t versionId = FindVersionId(preferFsr3);

    if (versionId == 0)
    {
        // Requested version is not present in the DLL - fallback to the other one
        const char* requested = preferFsr3 ? "FSR 3.1" : "FSR 2";
        const char* fallback  = preferFsr3 ? "FSR 2" : "FSR 3.1";

        char buf[256];
        snprintf(buf, sizeof(buf), "FSR: %s is not available, falling back to %s", requested, fallback);
        OutputDebugStringA(buf);
        if (m_pUserPrint)
        {
            m_pUserPrint->Print(buf);
        }

        versionId = FindVersionId(!preferFsr3);
        if (versionId == 0)
        {
            const char* msg = "FSR: no FSR provider found in the FidelityFX DLL";
            OutputDebugStringA(msg);
            if (m_pUserPrint)
            {
                m_pUserPrint->Print(msg);
            }
            return;
        }

        m_technique = preferFsr3 ? RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2
                                 : RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3;
    }
    else
    {
        m_technique = m_requestedTechnique;
    }

    // Explicitly tell the FidelityFX framework which FSR version to use,
    // instead of letting it pick the "best" provider by itself.
    ffxOverrideVersion overrideDesc = {};
    overrideDesc.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    overrideDesc.versionId   = versionId;

    ffxCreateBackendVKDesc backendDesc = {};
    backendDesc.header.type      = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backendDesc.vkDevice         = m_device;
    backendDesc.vkPhysicalDevice = m_physDevice;
    backendDesc.vkDeviceProcAddr = vkGetDeviceProcAddr;

    ffxCreateContextDescUpscale upscaleDesc = {};
    upscaleDesc.header.type   = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    upscaleDesc.header.pNext  = &overrideDesc.header;
    overrideDesc.header.pNext = &backendDesc.header;
    upscaleDesc.flags         = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    upscaleDesc.maxRenderSize  = { m_renderWidth, m_renderHeight };
    upscaleDesc.maxUpscaleSize = { m_displayWidth, m_displayHeight };
    upscaleDesc.fpMessage      = FsrMessageCallback;

    ffxReturnCode_t r = ffxCreateContext(&m_context, &upscaleDesc.header, nullptr);
    if (r != FFX_API_RETURN_OK)
    {
        m_context = nullptr;
        throw RgException(RG_GRAPHICS_API_ERROR, "Failed to create FSR context");
    }

    // Update static context for GetJitter (which is a static method)
    s_contextForJitter = m_context;

    // Query the provider that the DLL actually attached to this context -
    // verifies that the requested FSR version was really selected.
    ffxQueryGetProviderVersion pv = {};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    ffxQuery(&m_context, &pv.header);

    const char* requestedName = m_technique == RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2 ? "FSR 2" : "FSR 3.1";

    char buf[256];
    snprintf(buf, sizeof(buf), "FSR: requested %s, provider \"%s\" (id=0x%llx)",
        requestedName, pv.versionName ? pv.versionName : "unknown",
        (unsigned long long)pv.versionId);
    OutputDebugStringA(buf);
    if (m_pUserPrint)
    {
        m_pUserPrint->Print(buf);
    }
}

void vkpt::FSR::DestroyContext()
{
    if (m_context)
    {
        ffxDestroyContext(&m_context, nullptr);
        m_context = nullptr;
    }
    s_contextForJitter = nullptr;
}

vkpt::FramebufferImageIndex vkpt::FSR::Apply(
    VkCommandBuffer cmd, uint32_t frameIndex,
    const std::shared_ptr<Framebuffers>& framebuffers,
    const RenderResolutionHelper& renderResolution,
    RgFloat2D jitterOffset,
    float timeDelta,
    float nearPlane, float farPlane, float fovVerticalRad)
{
    if (!m_context)
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            OutputDebugStringA("[FSR] Apply SKIPPED - no context\n");
        }
        return FB_IMAGE_INDEX_FINAL;
    }

    using FI = vkpt::FramebufferImageIndex;

    FI rs[] =
    {
        FI::FB_IMAGE_INDEX_FINAL,
        FI::FB_IMAGE_INDEX_DEPTH_NDC,
        FI::FB_IMAGE_INDEX_MOTION_DLSS,
        OUTPUT_IMAGE_INDEX
    };
    InsertBarriers(cmd, frameIndex, *framebuffers, rs, false);

    ffxDispatchDescUpscale info = {};
    info.header.type       = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    info.commandList       = cmd;
    info.color             = ToFfxApiResource(FI::FB_IMAGE_INDEX_FINAL,           frameIndex, *framebuffers, renderResolution.GetResolutionState());
    info.depth             = ToFfxApiResource(FI::FB_IMAGE_INDEX_DEPTH_NDC,       frameIndex, *framebuffers, renderResolution.GetResolutionState());
    info.motionVectors     = ToFfxApiResource(FI::FB_IMAGE_INDEX_MOTION_DLSS,     frameIndex, *framebuffers, renderResolution.GetResolutionState());
    info.exposure          = {};
    info.reactive          = {};
    info.transparencyAndComposition = {};
    info.output            = ToFfxApiResource(OUTPUT_IMAGE_INDEX, frameIndex, *framebuffers, renderResolution.GetResolutionState());
    info.jitterOffset.x    = jitterOffset.data[0];
    info.jitterOffset.y    = jitterOffset.data[1];
    info.motionVectorScale.x = static_cast<float>(renderResolution.GetResolutionState().renderWidth);
    info.motionVectorScale.y = static_cast<float>(renderResolution.GetResolutionState().renderHeight);
    info.renderSize.width  = renderResolution.GetResolutionState().renderWidth;
    info.renderSize.height = renderResolution.GetResolutionState().renderHeight;
    info.upscaleSize.width  = m_displayWidth;
    info.upscaleSize.height = m_displayHeight;
    info.enableSharpening  = false;
    info.sharpness         = 0.0f;
    info.frameTimeDelta    = timeDelta;
    info.preExposure       = 1.0f;
    info.reset             = false;
    info.cameraNear        = nearPlane;
    info.cameraFar         = farPlane;
    info.cameraFovAngleVertical = fovVerticalRad;
    info.viewSpaceToMetersFactor = 1.0f;
    info.flags             = 0;

    ffxReturnCode_t r = ffxDispatch(&m_context, &info.header);
    if (r != FFX_API_RETURN_OK)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "[FSR] ffxDispatch FAILED: r=%u\n", (unsigned)r);
        OutputDebugStringA(buf);
        return FB_IMAGE_INDEX_FINAL;
    }

    InsertBarriers(cmd, frameIndex, *framebuffers, rs, true);

    return OUTPUT_IMAGE_INDEX;
}

RgFloat2D vkpt::FSR::GetJitter(const ResolutionState& resolutionState, uint32_t frameId)
{
    if (!s_contextForJitter)
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            OutputDebugStringA("[FSR] GetJitter SKIPPED - no context\n");
        }
        return { 0, 0 };
    }

    ffxQueryDescUpscaleGetJitterPhaseCount phaseCountDesc = {};
    phaseCountDesc.header.type     = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    phaseCountDesc.renderWidth     = resolutionState.renderWidth;
    phaseCountDesc.displayWidth    = resolutionState.upscaledWidth;
    int32_t phaseCount = 0;
    phaseCountDesc.pOutPhaseCount  = &phaseCount;
    ffxQuery(&s_contextForJitter, &phaseCountDesc.header);

    if (phaseCount <= 0)
    {
        return { 0, 0 };
    }

    RgFloat2D jitter = {};
    ffxQueryDescUpscaleGetJitterOffset offsetDesc = {};
    offsetDesc.header.type  = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    offsetDesc.index        = static_cast<int32_t>(frameId % phaseCount);
    offsetDesc.phaseCount   = phaseCount;
    offsetDesc.pOutX        = &jitter.data[0];
    offsetDesc.pOutY        = &jitter.data[1];
    ffxQuery(&s_contextForJitter, &offsetDesc.header);

    return jitter;
}
