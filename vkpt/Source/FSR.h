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

#pragma once

#include "Framebuffers.h"
#include "UserFunction.h"

/// Opaque FidelityFX API context handle (ffx_api/ffx_api.h: typedef void* ffxContext)
using ffxContext = void*;

namespace vkpt
{
    class RenderResolutionHelper;

    class FSR : public IFramebuffersDependency
    {
    public:
        FSR(VkDevice device, VkPhysicalDevice physDevice, UserPrint* pUserPrint);
        ~FSR() override;

        FSR(const FSR& other) = delete;
        FSR(FSR&& other) noexcept = delete;
        FSR& operator=(const FSR& other) = delete;
        FSR& operator=(FSR&& other) noexcept = delete;

        // Selects which FSR version to use. Recreates the FidelityFX context if the version changed.
        void SetUpscaleVersion(RgRenderUpscaleTechnique technique);

        void OnFramebuffersSizeChange(const ResolutionState &resolutionState) override;

        FramebufferImageIndex Apply(
            VkCommandBuffer cmd, uint32_t frameIndex,
            const std::shared_ptr<Framebuffers> &framebuffers,
            const RenderResolutionHelper &renderResolution,
            RgFloat2D jitterOffset,
            float timeDelta,
            float nearPlane, float farPlane, float fovVerticalRad);

        static RgFloat2D GetJitter(const ResolutionState &resolutionState, uint32_t frameId);

        // Checks whether the requested FSR version (AMD_FSR2 / AMD_FSR3) is present in the FidelityFX DLL.
        static bool IsUpscaleVersionAvailable(RgRenderUpscaleTechnique technique);

    private:
        void RecreateContext();
        void DestroyContext();
        static uint64_t FindVersionId(bool preferFsr3);

        VkDevice        m_device;
        VkPhysicalDevice m_physDevice;
        UserPrint*      m_pUserPrint;

        ffxContext      m_context;
        // Version requested by the application (never changed by fallback)
        RgRenderUpscaleTechnique m_requestedTechnique;
        // Version actually used (may differ from requested after a fallback)
        RgRenderUpscaleTechnique m_technique;

        uint32_t        m_renderWidth;
        uint32_t        m_renderHeight;
        uint32_t        m_displayWidth;
        uint32_t        m_displayHeight;

        bool            m_hasSize;

        static inline ffxContext s_contextForJitter = nullptr;
    };
}
