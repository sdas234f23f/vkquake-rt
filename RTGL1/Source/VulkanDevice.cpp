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

#include "VulkanDevice.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "HaltonSequence.h"
#include "Matrix.h"
#include "RenderResolutionHelper.h"
#include "RgException.h"
#include "Utils.h"
#include "Generated/ShaderCommonC.h"

using namespace RTGL1;

VkCommandBuffer VulkanDevice::BeginFrame(const RgStartFrameInfo &startInfo)
{
    uint32_t frameIndex = currentFrameState.IncrementFrameIndexAndGet();

    if (!waitForOutOfFrameFence)
    {
        // wait for previous cmd with the same frame index
        Utils::WaitAndResetFence(device, frameFences[frameIndex]);
    }
    else
    {
        Utils::WaitAndResetFences(device, frameFences[frameIndex], outOfFrameFences[frameIndex]);
    }

    swapchain->RequestVsync(startInfo.requestVSync);
    swapchain->AcquireImage(imageAvailableSemaphores[frameIndex]);

    VkSemaphore semaphoreToWaitOnSubmit = imageAvailableSemaphores[frameIndex];


    // if out-of-frame cmd exist, submit it
    {
        VkCommandBuffer preFrameCmd = currentFrameState.GetPreFrameCmdAndRemove();
        if (preFrameCmd != VK_NULL_HANDLE)
        {
            // Signal inFrameSemaphore after completion.
            // Signal outOfFrameFences, but for the next frame
            // because we can't reset cmd pool with cmds (in this case 
            // it's preFrameCmd) that are in use.
            cmdManager->Submit(preFrameCmd,
                               semaphoreToWaitOnSubmit, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               inFrameSemaphores[frameIndex],
                               outOfFrameFences[(frameIndex + 1) % MAX_FRAMES_IN_FLIGHT]);

            // should wait other semaphore in this case
            semaphoreToWaitOnSubmit = inFrameSemaphores[frameIndex];

            waitForOutOfFrameFence = true;
        }
        else
        {
            waitForOutOfFrameFence = false;
        }
    }
    currentFrameState.SetSemaphore(semaphoreToWaitOnSubmit);


    if (startInfo.requestShaderReload)
    {
        shaderManager->ReloadShaders();
    }


    // reset cmds for current frame index
    cmdManager->PrepareForFrame(frameIndex);

    // clear the data that were created MAX_FRAMES_IN_FLIGHT ago
    worldSamplerManager->PrepareForFrame(frameIndex);
    genericSamplerManager->PrepareForFrame(frameIndex);
    textureManager->PrepareForFrame(frameIndex);
    cubemapManager->PrepareForFrame(frameIndex);
    rasterizer->PrepareForFrame(frameIndex);
    decalManager->PrepareForFrame(frameIndex);

    VkCommandBuffer cmd = cmdManager->StartGraphicsCmd();

    BeginCmdLabel(cmd, "Prepare for frame");

    // start dynamic geometry recording to current frame
    scene->PrepareForFrame(cmd, frameIndex);

    return cmd;
}

#define RG_SET_VEC3( dst, x, y, z ) \
    ( dst )[ 0 ] = ( x );           \
    ( dst )[ 1 ] = ( y );           \
    ( dst )[ 2 ] = ( z )

#define RG_SET_VEC3_A( dst, xyz )  \
    ( dst )[ 0 ] = ( xyz )[ 0 ]; \
    ( dst )[ 1 ] = ( xyz )[ 1 ]; \
    ( dst )[ 2 ] = ( xyz )[ 2 ]

#define RG_MAX_VEC3( dst, m ) \
    ( dst )[ 0 ] = std::max( ( dst )[ 0 ], ( m ) );  \
    ( dst )[ 1 ] = std::max( ( dst )[ 1 ], ( m ) );  \
    ( dst )[ 2 ] = std::max( ( dst )[ 2 ], ( m ) )

void VulkanDevice::FillUniform(ShGlobalUniform *gu, const RgDrawFrameInfo &drawInfo) const
{
    const float IdentityMat4x4[16] =
    {
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        0,0,0,1
    };

    const float aspect = static_cast< float >( renderResolution.Width() ) / static_cast< float >( renderResolution.Height() );

    {
        memcpy( gu->viewPrev, gu->view, 16 * sizeof( float ) );
        memcpy( gu->projectionPrev, gu->projection, 16 * sizeof( float ) );

        memcpy( gu->view, drawInfo.view, 16 * sizeof( float ) );

        Matrix::MakeProjectionMatrix( gu->projection, 
                                      aspect, 
                                      drawInfo.fovYRadians, 
                                      drawInfo.cameraNear, 
                                      drawInfo.cameraFar );

        Matrix::Inverse( gu->invView, gu->view );
        Matrix::Inverse( gu->invProjection, gu->projection );

        memcpy( gu->cameraPositionPrev, gu->cameraPosition, 3 * sizeof( float ) );
        gu->cameraPosition[ 0 ] = gu->invView[ 12 ];
        gu->cameraPosition[ 1 ] = gu->invView[ 13 ];
        gu->cameraPosition[ 2 ] = gu->invView[ 14 ];
    }

    {
        static_assert( sizeof( gu->instanceGeomInfoOffset ) == sizeof( gu->instanceGeomInfoOffsetPrev ) );
        memcpy( gu->instanceGeomInfoOffsetPrev, gu->instanceGeomInfoOffset, sizeof( gu->instanceGeomInfoOffset ) );
    }

    {
        gu->frameId   = frameId;
        gu->timeDelta = static_cast< float >( std::max< double >( currentFrameTime - previousFrameTime, 0.001 ) );
        gu->time      = static_cast< float >( currentFrameTime );
    }

    {
        gu->renderWidth  = static_cast< float >( renderResolution.Width() );
        gu->renderHeight = static_cast< float >( renderResolution.Height() );
        // render width must be always even for checkerboarding!
        assert( ( int )gu->renderWidth % 2 == 0 );

        gu->upscaledRenderWidth  = static_cast< float >( renderResolution.UpscaledWidth() );
        gu->upscaledRenderHeight = static_cast< float >( renderResolution.UpscaledHeight() );

        RgFloat2D jitter = renderResolution.IsNvDlssEnabled() ? HaltonSequence::GetJitter_Halton23( frameId ) :
                           (renderResolution.IsAmdFsr2Enabled() || renderResolution.IsAmdFsr3Enabled()) ? FSR::GetJitter( renderResolution.GetResolutionState(), frameId ) :
                           RgFloat2D{ 0, 0 };

        gu->jitterX = jitter.data[ 0 ];
        gu->jitterY = jitter.data[ 1 ];
    }

    {
        gu->stopEyeAdaptation = drawInfo.disableEyeAdaptation;

        if( drawInfo.pTonemappingParams != nullptr )
        {
            gu->minLogLuminance     = drawInfo.pTonemappingParams->minLogLuminance;
            gu->maxLogLuminance     = drawInfo.pTonemappingParams->maxLogLuminance;
            gu->luminanceWhitePoint = drawInfo.pTonemappingParams->luminanceWhitePoint;
        }
        else
        {
            gu->minLogLuminance     = -3.9f;
            gu->maxLogLuminance     = -2.8f;
            gu->luminanceWhitePoint = 10.0f;
        }
    }

    {
        gu->lightCount     = scene->GetLightManager()->GetLightCount();
        gu->lightCountPrev = scene->GetLightManager()->GetLightCountPrev();

        gu->directionalLightExists = scene->GetLightManager()->DoesDirectionalLightExist();
    }

    {
        static_assert( sizeof( gu->skyCubemapRotationTransform ) == sizeof( IdentityMat4x4 ) && 
                       sizeof( IdentityMat4x4 ) == 16 * sizeof( float ), "Recheck skyCubemapRotationTransform sizes" );
        memcpy( gu->skyCubemapRotationTransform, IdentityMat4x4, 16 * sizeof( float ) );

        if( drawInfo.pSkyParams != nullptr )
        {
            const auto& sp = *drawInfo.pSkyParams;

            memcpy( gu->skyColorDefault, sp.skyColorDefault.data, sizeof( float ) * 3 );
            gu->skyColorMultiplier = sp.skyColorMultiplier;
            gu->skyColorSaturation = std::max( sp.skyColorSaturation, 0.0f );

            gu->skyType = sp.skyType == RG_SKY_TYPE_CUBEMAP ? SKY_TYPE_CUBEMAP :
                          sp.skyType == RG_SKY_TYPE_RASTERIZED_GEOMETRY ? SKY_TYPE_RASTERIZED_GEOMETRY :
                          sp.skyType == RG_SKY_TYPE_PROCEDURAL ? SKY_TYPE_PROCEDURAL :
                          SKY_TYPE_COLOR;

            gu->skyCubemapIndex = cubemapManager->IsCubemapValid( sp.skyCubemap ) ? sp.skyCubemap : RG_EMPTY_CUBEMAP;

            if( !Utils::IsAlmostZero( drawInfo.pSkyParams->skyCubemapRotationTransform ) )
            {
                Utils::SetMatrix3ToGLSLMat4( gu->skyCubemapRotationTransform, drawInfo.pSkyParams->skyCubemapRotationTransform );
            }
        }
        else
        {
            gu->skyColorDefault[ 0 ] = gu->skyColorDefault[ 1 ] = gu->skyColorDefault[ 2 ] = gu->skyColorDefault[ 3 ] = 1.0f;
            gu->skyColorMultiplier                                                                                    = 1.0f;
            gu->skyColorSaturation                                                                                    = 1.0f;
            gu->skyType                                                                                               = SKY_TYPE_COLOR;
            gu->skyCubemapIndex                                                                                       = RG_EMPTY_CUBEMAP;
        }

        RgFloat3D skyViewerPosition = drawInfo.pSkyParams ? drawInfo.pSkyParams->skyViewerPosition : RgFloat3D{ 0, 0, 0 };

        for( uint32_t i = 0; i < 6; i++ )
        {
            float* viewProjDst = &gu->viewProjCubemap[ 16 * i ];

            Matrix::GetCubemapViewProjMat( viewProjDst, i, skyViewerPosition.data, drawInfo.cameraNear, drawInfo.cameraFar );
        }
    }

    gu->debugShowFlags = 0;

    if( drawInfo.pDebugParams != nullptr )
    {
        RgDebugDrawFlags fs = drawInfo.pDebugParams->drawFlags;

        if( fs & RG_DEBUG_DRAW_ONLY_DIFFUSE_DIRECT_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_ONLY_DIRECT_DIFFUSE;
        }
        else if( fs & RG_DEBUG_DRAW_ONLY_DIFFUSE_INDIRECT_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_ONLY_INDIRECT_DIFFUSE;
        }
        else if( fs & RG_DEBUG_DRAW_ONLY_SPECULAR_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_ONLY_SPECULAR;
        }
        else if( fs & RG_DEBUG_DRAW_UNFILTERED_DIFFUSE_DIRECT_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_UNFILTERED_DIFFUSE;
        }
        else if( fs & RG_DEBUG_DRAW_UNFILTERED_DIFFUSE_INDIRECT_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_UNFILTERED_INDIRECT;
        }
        else if( fs & RG_DEBUG_DRAW_UNFILTERED_SPECULAR_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_UNFILTERED_SPECULAR;
        }

        if( fs & RG_DEBUG_DRAW_ALBEDO_WHITE_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_ALBEDO_WHITE;
        }
        if( fs & RG_DEBUG_DRAW_MOTION_VECTORS_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_MOTION_VECTORS;
        }
        if( fs & RG_DEBUG_DRAW_GRADIENTS_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_GRADIENTS;
        }
        if( fs & RG_DEBUG_DRAW_LIGHT_GRID_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_LIGHT_GRID;
        }
        if( fs & RG_DEBUG_DRAW_GOD_RAYS_BIT )
        {
            gu->debugShowFlags |= DEBUG_SHOW_FLAG_GOD_RAYS;
        }
    }

    // 4.7: the legacy RTGL1 render path was removed; the Q2RTX-style core is
    // the only renderer, so the shader-side switch is always enabled.
    gu->coreQ2RTX = 1u;

    if( drawInfo.pTexturesParams != nullptr )
    {
        gu->normalMapStrength      = drawInfo.pTexturesParams->normalMapStrength;
        gu->emissionMapBoost       = std::max( drawInfo.pTexturesParams->emissionMapBoost, 0.0f );
        gu->emissionMaxScreenColor = std::max( drawInfo.pTexturesParams->emissionMaxScreenColor, 0.0f );
        gu->squareInputRoughness   = !!drawInfo.pTexturesParams->squareInputRoughness;
        gu->minRoughness           = std::clamp( drawInfo.pTexturesParams->minRoughness, 0.0f, 1.0f );
    }
    else
    {
        gu->normalMapStrength      = 1.0f;
        gu->emissionMapBoost       = 100.0f;
        gu->emissionMaxScreenColor = 1.5f;
        gu->squareInputRoughness   = 1;
        gu->minRoughness           = 0.0f;
    }

    if( drawInfo.pIlluminationParams != nullptr )
    {
        gu->maxBounceShadowsLights     = drawInfo.pIlluminationParams->maxBounceShadows;
        gu->polyLightSpotlightFactor   = std::max( 0.0f, drawInfo.pIlluminationParams->polygonalLightSpotlightFactor );
        gu->indirSecondBounce          = !!drawInfo.pIlluminationParams->enableSecondBounceForIndirect;
        gu->lightIndexIgnoreFPVShadows = scene->GetLightManager()->GetLightIndexIgnoreFPVShadows( currentFrameState.GetFrameIndex(), drawInfo.pIlluminationParams->lightUniqueIdIgnoreFirstPersonViewerShadows );
        gu->cellWorldSize              = std::max( drawInfo.pIlluminationParams->cellWorldSize, 0.001f );
        gu->gradientMultDiffuse        = std::clamp( drawInfo.pIlluminationParams->directDiffuseSensitivityToChange, 0.0f, 1.0f );
        gu->gradientMultIndirect       = std::clamp( drawInfo.pIlluminationParams->indirectDiffuseSensitivityToChange, 0.0f, 1.0f );
        gu->gradientMultSpecular       = std::clamp( drawInfo.pIlluminationParams->specularSensitivityToChange, 0.0f, 1.0f );
    }
    else
    {
        gu->maxBounceShadowsLights     = 2;
        gu->polyLightSpotlightFactor   = 2.0f;
        gu->indirSecondBounce          = true;
        gu->lightIndexIgnoreFPVShadows = LIGHT_INDEX_NONE;
        gu->cellWorldSize              = 1.0f;
        gu->gradientMultDiffuse        = 0.5f;
        gu->gradientMultIndirect       = 0.2f;
        gu->gradientMultSpecular       = 0.5f;
    }

    if( drawInfo.pBloomParams != nullptr )
    {
        gu->bloomThreshold              = std::max( drawInfo.pBloomParams->inputThreshold, 0.0f );
        gu->bloomIntensity              = std::max( drawInfo.pBloomParams->bloomIntensity, 0.0f );
        gu->bloomEmissionMultiplier     = std::max( drawInfo.pBloomParams->bloomEmissionMultiplier, 0.0f );
    }
    else
    {
        gu->bloomThreshold              = 15.0f;
        gu->bloomIntensity              = 1.0f;
        gu->bloomEmissionMultiplier     = 64.0f;
    }

    static_assert(
        RG_MEDIA_TYPE_VACUUM == MEDIA_TYPE_VACUUM &&
        RG_MEDIA_TYPE_WATER == MEDIA_TYPE_WATER &&
        RG_MEDIA_TYPE_GLASS == MEDIA_TYPE_GLASS && 
        RG_MEDIA_TYPE_ACID == MEDIA_TYPE_ACID,
        "Interface and GLSL constants must be identical" );

    if( drawInfo.pReflectRefractParams != nullptr )
    {
        const auto& rr = *drawInfo.pReflectRefractParams;

        if( rr.typeOfMediaAroundCamera >= 0 && rr.typeOfMediaAroundCamera < MEDIA_TYPE_COUNT )
        {
            gu->cameraMediaType = rr.typeOfMediaAroundCamera;
        }
        else
        {
            gu->cameraMediaType = MEDIA_TYPE_VACUUM;
        }

        gu->reflectRefractMaxDepth     = std::min( 4u, rr.maxReflectRefractDepth );

        gu->indexOfRefractionGlass = std::max( 0.0f, rr.indexOfRefractionGlass );
        gu->indexOfRefractionWater = std::max( 0.0f, rr.indexOfRefractionWater );

        memcpy( gu->waterColorAndDensity, rr.waterColor.data, 3 * sizeof( float ) );
        gu->waterColorAndDensity[ 3 ] = 0.0f;

        memcpy( gu->acidColorAndDensity, rr.acidColor.data, 3 * sizeof( float ) );
        gu->acidColorAndDensity[ 3 ] = rr.acidDensity;

        gu->forceNoWaterRefraction            = !!rr.forceNoWaterRefraction;
        gu->waterWaveSpeed                    = rr.waterWaveSpeed;
        gu->waterWaveStrength                 = rr.waterWaveNormalStrength;
        gu->waterTextureDerivativesMultiplier = std::max( 0.0f, rr.waterWaveTextureDerivativesMultiplier );
        if( rr.waterTextureAreaScale < 0.0001f )
        {
            gu->waterTextureAreaScale = 1.0f;
        }
        else
        {
            gu->waterTextureAreaScale = rr.waterTextureAreaScale;
        }

        gu->noBackfaceReflForNoMediaChange = !!rr.disableBackfaceReflectionsForNoMediaChange;

        gu->twirlPortalNormal = !!rr.portalNormalTwirl;
    }
    else
    {
        gu->cameraMediaType        = MEDIA_TYPE_VACUUM;
        gu->reflectRefractMaxDepth = 2;

        gu->indexOfRefractionGlass = 1.52f;
        gu->indexOfRefractionWater = 1.33f;

        RG_SET_VEC3( gu->waterColorAndDensity, 0.3f, 0.73f, 0.63f );
        gu->waterColorAndDensity[ 3 ] = 0.0f;

        RG_SET_VEC3( gu->acidColorAndDensity, 0.0f, 0.66f, 0.55f );
        gu->acidColorAndDensity[ 3 ] = 10.0f;

        gu->forceNoWaterRefraction            = false;
        gu->waterWaveSpeed                    = 1.0f;
        gu->waterWaveStrength                 = 1.0f;
        gu->waterTextureDerivativesMultiplier = 1.0f;
        gu->waterTextureAreaScale             = 1.0f;

        gu->noBackfaceReflForNoMediaChange = false;

        gu->twirlPortalNormal = false;
    }

    gu->rayCullBackFaces  = rayCullBackFacingTriangles ? 1 : 0;
    gu->rayLength         = clamp( drawInfo.rayLength, 0.1f, ( float )MAX_RAY_LENGTH );
    gu->primaryRayMinDist = clamp( drawInfo.cameraNear, 0.001f, gu->rayLength );

    {
        gu->rayCullMaskWorld = 0;

        if( drawInfo.rayCullMaskWorld & RG_DRAW_FRAME_RAY_CULL_WORLD_0_BIT )
        {
            gu->rayCullMaskWorld |= INSTANCE_MASK_WORLD_0;
        }

        if( drawInfo.rayCullMaskWorld & RG_DRAW_FRAME_RAY_CULL_WORLD_1_BIT )
        {
            gu->rayCullMaskWorld |= INSTANCE_MASK_WORLD_1;
        }

        if( drawInfo.rayCullMaskWorld & RG_DRAW_FRAME_RAY_CULL_WORLD_2_BIT )
        {
            if( allowGeometryWithSkyFlag )
            {
                throw RgException( RG_WRONG_ARGUMENT, "RG_DRAW_FRAME_RAY_CULL_WORLD_2_BIT cannot be used, as RgInstanceCreateInfo::allowGeometryWithSkyFlag was true" );
            }

            gu->rayCullMaskWorld |= INSTANCE_MASK_WORLD_2;
        }

#if RAYCULLMASK_SKY_IS_WORLD2
        if( drawInfo.rayCullMaskWorld & RG_DRAW_FRAME_RAY_CULL_SKY_BIT )
        {
            if( !allowGeometryWithSkyFlag )
            {
                throw RgException( RG_WRONG_ARGUMENT, "RG_DRAW_FRAME_RAY_CULL_SKY_BIT cannot be used, as RgInstanceCreateInfo::allowGeometryWithSkyFlag was false" );
            }

            gu->rayCullMaskWorld |= INSTANCE_MASK_WORLD_2;
        }
#else
    #error Handle RG_DRAW_FRAME_RAY_CULL_SKY_BIT, if there is no WORLD_2
#endif


        if( allowGeometryWithSkyFlag )
        {
            gu->rayCullMaskWorld_Shadow = gu->rayCullMaskWorld & ( ~INSTANCE_MASK_WORLD_2 );
        }
        else
        {
            gu->rayCullMaskWorld_Shadow = gu->rayCullMaskWorld;
        }
    }

    gu->waterNormalTextureIndex = textureManager->GetWaterNormalTextureIndex();

    gu->cameraRayConeSpreadAngle = atanf( ( 2.0f * tanf( drawInfo.fovYRadians * 0.5f ) ) / ( float )renderResolution.Height() );

    if( Utils::IsAlmostZero( drawInfo.worldUpVector ) )
    {
        gu->worldUpVector[ 0 ] = 0.0f;
        gu->worldUpVector[ 1 ] = 1.0f;
        gu->worldUpVector[ 2 ] = 0.0f;
    }
    else
    {
        gu->worldUpVector[ 0 ] = drawInfo.worldUpVector.data[ 0 ];
        gu->worldUpVector[ 1 ] = drawInfo.worldUpVector.data[ 1 ];
        gu->worldUpVector[ 2 ] = drawInfo.worldUpVector.data[ 2 ];
    }

    if( drawInfo.pLightmapParams != nullptr )
    {
        gu->lightmapEnable = !!drawInfo.pLightmapParams->enableLightmaps;

        if( drawInfo.pLightmapParams->lightmapLayerIndex == 1 || drawInfo.pLightmapParams->lightmapLayerIndex == 2 )
        {
            gu->lightmapLayer = drawInfo.pLightmapParams->lightmapLayerIndex;
        }
        else
        {
            assert( 0 && "pLightMapLayerIndex must point to a value of 1 or 2. Others are invalidated" );
        }
    }
    else
    {
        gu->lightmapEnable = false;
        gu->lightmapLayer  = UINT8_MAX;
    }

    gu->lensFlareCullingInputCount = rasterizer->GetLensFlareCullingInputCount();
    gu->applyViewProjToLensFlares  = !lensFlareVerticesInScreenSpace;

    {
        gu->volumeCameraNear = std::max( drawInfo.cameraNear, 0.001f );
        gu->volumeCameraFar  = std::min(
            drawInfo.cameraFar,
            drawInfo.pVolumetricParams ? drawInfo.pVolumetricParams->volumetricFar : 100.0f );

        if( drawInfo.pVolumetricParams )
        {
            if( drawInfo.pVolumetricParams->enable )
            {
                gu->volumeEnableType = drawInfo.pVolumetricParams->useSimpleDepthBased
                                           ? VOLUME_ENABLE_SIMPLE
                                           : VOLUME_ENABLE_VOLUMETRIC;
            }
            else
            {
                gu->volumeEnableType = VOLUME_ENABLE_NONE;
            }
            gu->volumeScattering = drawInfo.pVolumetricParams->scaterring;
            gu->volumeSourceAsymmetry = std::clamp( drawInfo.pVolumetricParams->sourceAssymetry, -1.0f, 1.0f );

            RG_SET_VEC3_A( gu->volumeAmbient, drawInfo.pVolumetricParams->ambientColor.data );
            RG_MAX_VEC3( gu->volumeAmbient, 0.0f );

            RG_SET_VEC3_A( gu->volumeSourceColor, drawInfo.pVolumetricParams->sourceColor.data );
            RG_MAX_VEC3( gu->volumeSourceColor, 0.0f );

            RG_SET_VEC3_A( gu->volumeDirToSource, drawInfo.pVolumetricParams->sourceDirection.data );
            Utils::Negate( gu->volumeDirToSource );
            Utils::Normalize( gu->volumeDirToSource );
        }
        else
        {
            gu->volumeEnableType      = VOLUME_ENABLE_VOLUMETRIC;
            gu->volumeScattering      = 0.2f;
            gu->volumeSourceAsymmetry = 0.4f;
            RG_SET_VEC3( gu->volumeAmbient, 0.8f, 0.85f, 1.0f );
            RG_SET_VEC3( gu->volumeSourceColor, 0, 0, 0 );
            RG_SET_VEC3( gu->volumeDirToSource, 0, 1, 0 );
        }

        if( gu->volumeEnableType != VOLUME_ENABLE_NONE )
        {
            memcpy( gu->volumeViewProj_Prev, gu->volumeViewProj, 16 * sizeof( float ) );
            memcpy( gu->volumeViewProjInv_Prev, gu->volumeViewProjInv, 16 * sizeof( float ) );

            float volumeproj[ 16 ];
            Matrix::MakeProjectionMatrix( volumeproj,
                                          aspect,
                                          drawInfo.fovYRadians,
                                          gu->volumeCameraNear,
                                          gu->volumeCameraFar );

            Matrix::Multiply( gu->volumeViewProj, gu->view, volumeproj );
            Matrix::Inverse( gu->volumeViewProjInv, gu->volumeViewProj );
        }
    }

    gu->antiFireflyEnabled = !!drawInfo.forceAntiFirefly;

    // Q2RTX-style fog volumes (host data set via rgSetFogVolumes)
    {
        for (uint32_t i = 0; i < MAX_FOG_VOLUMES; i++)
        {
            // std140: scalar arrays have 16-byte element stride
            gu->fogIsActive[i * 4] = 0;
        }

        for (uint32_t i = 0; i < fogVolumeCount; i++)
        {
            const RgFogVolume &v = fogVolumes[i];

            if (v.halfExtinctionDistance <= 0.0f ||
                v.pointA.data[0] == v.pointB.data[0] ||
                v.pointA.data[1] == v.pointB.data[1] ||
                v.pointA.data[2] == v.pointB.data[2])
            {
                continue;
            }

            const float *pa = v.pointA.data;
            const float *pb = v.pointB.data;

            for (int axis = 0; axis < 3; axis++)
            {
                gu->fogMins[i * 4 + axis] = std::min(pa[axis], pb[axis]);
                gu->fogMaxs[i * 4 + axis] = std::max(pa[axis], pb[axis]);
            }
            gu->fogMins[i * 4 + 3] = 0.0f;
            gu->fogMaxs[i * 4 + 3] = 0.0f;

            gu->fogColor[i * 4 + 0] = v.color.data[0];
            gu->fogColor[i * 4 + 1] = v.color.data[1];
            gu->fogColor[i * 4 + 2] = v.color.data[2];
            gu->fogColor[i * 4 + 3] = 0.0f;

            // exp(-kx) = 0.5  =>  k = -ln(0.5) / x
            const float density = 0.69315f / v.halfExtinctionDistance;

            gu->fogDensity[i * 4 + 0] = 0.0f;
            gu->fogDensity[i * 4 + 1] = 0.0f;
            gu->fogDensity[i * 4 + 2] = 0.0f;

            if (1 <= v.softface && v.softface <= 6)
            {
                // linear density gradient along one axis (zero on the soft face)
                const int axis = (v.softface - 1) / 2;
                const float pos0 = (v.softface & 1) ? pa[axis] : pb[axis];
                const float pos1 = (v.softface & 1) ? pb[axis] : pa[axis];
                const float a = density / (pos1 - pos0);
                const float b = -pos0 * a;
                gu->fogDensity[i * 4 + axis] = a;
                gu->fogDensity[i * 4 + 3] = b;
            }
            else
            {
                gu->fogDensity[i * 4 + 3] = density;
            }

            // std140: scalar arrays have 16-byte element stride
            gu->fogIsActive[i * 4] = 1;
        }
    }
}

void VulkanDevice::Render(VkCommandBuffer cmd, const RgDrawFrameInfo &drawInfo)
{
    // end of "Prepare for frame" label
    EndCmdLabel(cmd);


    const uint32_t frameIndex = currentFrameState.GetFrameIndex();

    
    bool mipLodBiasUpdated = worldSamplerManager->TryChangeMipLodBias(frameIndex, renderResolution.GetMipLodBias());
    const RgFloat2D jitter = { uniform->GetData()->jitterX, uniform->GetData()->jitterY };

    textureManager->SubmitDescriptors(frameIndex, drawInfo.pTexturesParams, mipLodBiasUpdated);
    cubemapManager->SubmitDescriptors(frameIndex);


    // submit geometry and upload uniform after getting data from a scene
    scene->SubmitForFrame(cmd, frameIndex, uniform, 
                          uniform->GetData()->rayCullMaskWorld, 
                          allowGeometryWithSkyFlag,
                          drawInfo.disableRayTracedGeometry);


    framebuffers->PrepareForSize(renderResolution.GetResolutionState());
    

    if (!drawInfo.disableRasterization)
    {
        rasterizer->SubmitForFrame(cmd, frameIndex);

        // draw rasterized sky to albedo before tracing primary rays
        if (uniform->GetData()->skyType == RG_SKY_TYPE_RASTERIZED_GEOMETRY)
        {
            RgFloat3D skyViewerPosition = drawInfo.pSkyParams ? drawInfo.pSkyParams->skyViewerPosition : RgFloat3D{ 0,0,0 };

            rasterizer->DrawSkyToCubemap(cmd, frameIndex, textureManager, uniform);
            rasterizer->DrawSkyToAlbedo(cmd, frameIndex, textureManager, uniform->GetData()->view, skyViewerPosition.data, uniform->GetData()->projection, jitter, renderResolution);
        }
        // fill the sky cubemap with a procedural atmosphere (compute)
        else if (uniform->GetData()->skyType == RG_SKY_TYPE_PROCEDURAL)
        {
            RenderCubemap::ProceduralSkyParams p = {};

            // sky color follows the sun preset (rt_sky_light_*), sent by the host via skyColorDefault;
            // this keeps the sky tint independent from whether the sun light is enabled (rt_sun)
            p.sunColor[0] = uniform->GetData()->skyColorDefault[0];
            p.sunColor[1] = uniform->GetData()->skyColorDefault[1];
            p.sunColor[2] = uniform->GetData()->skyColorDefault[2];

            float sunColor[3], sunDir[3], sunAngularRadius = 0.0047f;
            if (scene->GetLightManager()->GetLastDirectionalLight(sunColor, sunDir, &sunAngularRadius))
            {
                // the directional light direction points FROM the sun toward the scene;
                // the sky shader expects the direction TOWARD the sun
                p.sunDirection[0] = -sunDir[0];
                p.sunDirection[1] = -sunDir[1];
                p.sunDirection[2] = -sunDir[2];
            }
            else
            {
                // no sun: fallback to a default elevation/azimuth (direction toward the sun)
                float d[3] = { 0.3f, 0.5f, 0.8f };
                const float len = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
                p.sunDirection[0] = d[0] / len;
                p.sunDirection[1] = d[1] / len;
                p.sunDirection[2] = d[2] / len;
            }
            p.sunColor[3] = sunAngularRadius;
            p.skyParams[0] = uniform->GetData()->skyColorMultiplier;
            p.skyParams[1] = uniform->GetData()->skyColorSaturation;
            p.skyParams[2] = 25.0f;  // sun disc intensity
            p.skyParams[3] = 0.025f; // display sun disc angular radius (rad), ~1.4 deg; physical 0.05 deg is sub-pixel

            // cloud params are packed into the otherwise-unused skyCubemapRotationTransform field
            // (keeps the public RG_* API unchanged):
            //   [0..2] cloud color rgb, [3] coverage, [4] density, [5] drift speed, [6] enabled
            p.cloudColor[3] = uniform->GetData()->time; // cloud animation time
            if (drawInfo.pSkyParams)
            {
                const float *c = &drawInfo.pSkyParams->skyCubemapRotationTransform.matrix[0][0];
                p.cloudColor[0] = c[0];
                p.cloudColor[1] = c[1];
                p.cloudColor[2] = c[2];
                p.cloudParams[0] = c[3];
                p.cloudParams[1] = c[4];
                p.cloudParams[2] = c[5];
                p.cloudParams[3] = c[6];
            }

            // per-face camera bases, matching Matrix::GetCubemapViewProjMat
            constexpr float PI = 3.14159265358979323846f;
            const float faceAngles[6][2] = {
                { 0.0f,        PI / 2.0f }, // POSITIVE_X
                { 0.0f,       -PI / 2.0f }, // NEGATIVE_X
                { -PI / 2.0f, 0.0f       }, // POSITIVE_Y
                {  PI / 2.0f, 0.0f       }, // NEGATIVE_Y
                { 0.0f,        0.0f      }, // POSITIVE_Z
                { 0.0f,        PI        }, // NEGATIVE_Z
            };

            float view[16];
            const float origin[3] = { 0.0f, 0.0f, 0.0f };
            for (uint32_t face = 0; face < 6; face++)
            {
                Matrix::GetViewMatrix(view, origin, faceAngles[face][0], faceAngles[face][1], 0.0f);

                // column-major columns of the view rotation: right, up, forward
                p.faceBasis[face * 3 + 0][0] = view[0];  p.faceBasis[face * 3 + 0][1] = view[4];  p.faceBasis[face * 3 + 0][2] = view[8];
                p.faceBasis[face * 3 + 1][0] = view[1];  p.faceBasis[face * 3 + 1][1] = view[5];  p.faceBasis[face * 3 + 1][2] = view[9];
                p.faceBasis[face * 3 + 2][0] = view[2];  p.faceBasis[face * 3 + 2][1] = view[6];  p.faceBasis[face * 3 + 2][2] = view[10];
            }

            rasterizer->GetRenderCubemap()->DrawProcedural(cmd, p);
        }
    }


    {
        lightGrid->Build(cmd, frameIndex, uniform, blueNoise, scene->GetLightManager());

        decalManager->SubmitForFrame(cmd, frameIndex);
        portalList->SubmitForFrame(cmd, frameIndex);

        const auto params = pathTracer->Bind( cmd,
                                              frameIndex,
                                              renderResolution.Width(),
                                              renderResolution.Height(),
                                              scene.get(),
                                              uniform.get(),
                                              textureManager.get(),
                                              framebuffers,
                                              restirBuffers,
                                              blueNoise.get(),
                                              cubemapManager.get(),
                                              rasterizer->GetRenderCubemap().get(),
                                              portalList.get(),
                                              volumetric.get() );

        pathTracer->TracePrimaryRays(params);

        // draw decals on top of primary surface
        decalManager->Draw(cmd, frameIndex, uniform, framebuffers, textureManager);

        // volumetric sunlight: render the shadow map and ray march god rays
        bool godRaysActive = false;
        GodRays::Params gr = {};
        {
            float sunColor[3], sunDir[3], sunAngularRadius = 0.0047f;
            if (!scene->GetLightManager()->GetLastDirectionalLight(sunColor, sunDir, &sunAngularRadius))
            {
                // No sun light (rt_sun 0): follow the sun that is visible in the
                // procedural sky (same fallback as DrawProcedural), so the
                // volumetric sun shafts match the visible sun (Q2RTX behavior).
                // sunDir is the light direction (FROM the sun toward the scene),
                // i.e. opposite of the sky's toward-sun fallback.
                const float towardSun[3] = { 0.3f, 0.5f, 0.8f };
                const float len = std::sqrt(towardSun[0]*towardSun[0] + towardSun[1]*towardSun[1] + towardSun[2]*towardSun[2]);
                sunDir[0] = -towardSun[0] / len;
                sunDir[1] = -towardSun[1] / len;
                sunDir[2] = -towardSun[2] / len;
                sunColor[0] = uniform->GetData()->skyColorDefault[0];
                sunColor[1] = uniform->GetData()->skyColorDefault[1];
                sunColor[2] = uniform->GetData()->skyColorDefault[2];
            }

            float aabbMin[3], aabbMax[3];
            if (scene->HasAABB())
            {
                scene->GetAABB(aabbMin, aabbMax);

                float shadowMapVP[16];
                float shadowMapDepthScale = 0.0f;

                const auto &staticCollector  = scene->GetASManager()->GetStaticCollector();
                const auto &dynamicCollector = scene->GetASManager()->GetDynamicCollector(frameIndex);

                if (shadowMap->Render(cmd, sunDir, aabbMin, aabbMax,
                                      staticCollector.get(), dynamicCollector.get(),
                                      shadowMapVP, &shadowMapDepthScale))
                {
                    // The god rays sun direction points TOWARD the sun (Q2RTX
                    // convention): the phase function peaks around the sun and
                    // the shadow bias pushes samples away from it.
                    gr.sunDirection[0] = -sunDir[0];
                    gr.sunDirection[1] = -sunDir[1];
                    gr.sunDirection[2] = -sunDir[2];
                    gr.sunColor[0] = sunColor[0];
                    gr.sunColor[1] = sunColor[1];
                    gr.sunColor[2] = sunColor[2];

                    for (int k = 0; k < 3; k++)
                    {
                        const float halfSize = std::max((aabbMax[k] - aabbMin[k]) * 0.5f, 1.0f);
                        gr.worldCenter[k] = (aabbMin[k] + aabbMax[k]) * 0.5f;
                        gr.worldHalfSizeInv[k] = 1.0f / halfSize;
                    }

                    memcpy(gr.shadowMapVP, shadowMapVP, 16 * sizeof(float));
                    gr.shadowMapDepthScale = shadowMapDepthScale;
                    gr.godRaysIntensity = 2.0f; // Q2RTX default (god_rays_intensity)
                    gr.godRaysEccentricity = 0.75f;
                    gr.godRaysEnabled = 1u;

                    godRays->Trace(cmd, frameIndex, gr, 0);
                    godRaysActive = true;
                }
            }
        }

        // Q2RTX-style reflection/refraction pass (phase 4.4.3). The legacy
        // RTGL1 refl/refr path was removed (4.7: only the Q2RTX renderer).
        if (uniform->GetData()->reflectRefractMaxDepth > 0)
        {
            pathTracer->TraceQ2ReflectionRefractionRays(params);
        }

        // Q2RTX-style god rays over the reflected/refracted segments. The refl
        // pass stores each reflected segment length in Q2GodRaysThroughputDist.w
        // and marks reflection pixels with negative Q2ViewDepth; this pass only
        // processes those pixels and accumulates on top of the primary result.
        if (godRaysActive)
        {
            godRays->Trace(cmd, frameIndex, gr, 1);
        }

        // Bilateral upscale of the half-res god rays to full resolution
        // (Q2RTX god_rays_filter.comp); runs after all trace passes.
        if (godRaysActive)
        {
            godRays->Filter(cmd, frameIndex);
        }

        scene->GetLightManager()->BarrierLightGrid(cmd, frameIndex);
        pathTracer->CalculateInitialReservoirs(params);
        pathTracer->TraceDirectllumination(params);
        pathTracer->TraceIndirectllumination(params);

        // The legacy screen-space volumetric was removed (4.7: only the Q2RTX
        // renderer); fog is handled by the traced fog volumes + god rays.
        pathTracer->CalculateGradientsSamples(params);

        // Q2RTX-style ASVGF denoiser (the legacy SVGF path was removed).
        q2Denoiser->Denoise(cmd, frameIndex, uniform);

        tonemapping->CalculateExposure(cmd, frameIndex, uniform);
    }

    imageComposition->PrepareForRaster( cmd, frameIndex, uniform.get() );

    if (!drawInfo.disableRasterization)
    {
        // draw rasterized geometry into the final image
        rasterizer->DrawToFinalImage(
            cmd,
            frameIndex,
            textureManager,
            uniform,
            tonemapping,
            volumetric,
            uniform->GetData()->view,
            uniform->GetData()->projection,
            jitter,
            renderResolution,
            drawInfo.pLensFlareParams );
    }

    // Q2RTX-style fog volumes are traced per ray in the primary/refl passes
    // and accumulated in Q2FogAccum, blended by the ASVGF compositing (denoised
    // and temporally consistent). The legacy post-pass was removed (4.7).
    imageComposition->Finalize(
        cmd, frameIndex, uniform.get(), tonemapping.get(), volumetric.get() );


    bool enableBloom = drawInfo.pBloomParams == nullptr || (drawInfo.pBloomParams != nullptr && drawInfo.pBloomParams->bloomIntensity > 0.0f);

    if (enableBloom)
    {
        bloom->Prepare(cmd, frameIndex, uniform, tonemapping);
    }


    FramebufferImageIndex accum = FramebufferImageIndex::FB_IMAGE_INDEX_FINAL;
    {
        // upscale the finalized (Q2RTX) image. FSR/DLSS remain available as
        // upscalers; the Q2RTX TAAU is the default fallback.
        if (renderResolution.IsNvDlssEnabled())
        {
            accum = nvDlss->Apply(cmd, frameIndex, 
                                               framebuffers, 
                                               renderResolution, 
                                               jitter);
        }
        else if (renderResolution.IsAmdFsr2Enabled() || renderResolution.IsAmdFsr3Enabled())
        {
            accum = amdFsr->Apply(cmd, frameIndex, 
                                                framebuffers, 
                                                renderResolution, 
                                                jitter, 
                                                uniform->GetData()->timeDelta, 
                                                drawInfo.cameraNear, 
                                                drawInfo.cameraFar, 
                                                drawInfo.fovYRadians );
        }
        else
        {
            // Q2RTX TAAU (default upscaler of the Q2RTX core)
            q2Denoiser->ApplyTAAU(cmd, frameIndex, uniform);
            accum = FramebufferImageIndex::FB_IMAGE_INDEX_UPSCALED_PING;
        }

        const RgExtent2D* pixelized = drawInfo.pRenderResolutionParams
                                          ? drawInfo.pRenderResolutionParams->pPixelizedRenderSize
                                          : nullptr;

        accum = framebuffers->BlitForEffects(
            cmd, frameIndex, accum, renderResolution.GetBlitFilter(), pixelized );
    }


    const CommonnlyUsedEffectArguments args = { cmd, frameIndex, framebuffers, uniform, renderResolution.UpscaledWidth(), renderResolution.UpscaledHeight(), (float)currentFrameTime };
    {
        if (renderResolution.IsDedicatedSharpeningEnabled())
        {
            accum = sharpening->Apply(
                cmd, frameIndex, framebuffers, renderResolution.UpscaledWidth(), renderResolution.UpscaledHeight(), accum,
                renderResolution.GetSharpeningTechnique(), renderResolution.GetSharpeningIntensity());
        }
        if (enableBloom)
        {
            accum = bloom->Apply(cmd, frameIndex, uniform, renderResolution.UpscaledWidth(), renderResolution.UpscaledHeight(), accum);
        }
        if (effectColorTint->Setup(args, drawInfo.postEffectParams.pColorTint))
        {
            accum = effectColorTint->Apply(args, accum);
        }
        if (effectInverseBW->Setup(args, drawInfo.postEffectParams.pInverseBlackAndWhite))
        {
            accum = effectInverseBW->Apply(args, accum);
        }
        if (effectHueShift->Setup(args, drawInfo.postEffectParams.pHueShift))
        {
            accum = effectHueShift->Apply(args, accum);
        }
        if (effectChromaticAberration->Setup(args, drawInfo.postEffectParams.pChromaticAberration))
        {
            accum = effectChromaticAberration->Apply(args, accum);
        }
        if (effectDistortedSides->Setup(args, drawInfo.postEffectParams.pDistortedSides))
        {
            accum = effectDistortedSides->Apply(args, accum);
        }
        if (effectWaves->Setup(args, drawInfo.postEffectParams.pWaves))
        {
            accum = effectWaves->Apply(args, accum);
        }
        if (effectRadialBlur->Setup(args, drawInfo.postEffectParams.pRadialBlur))
        {
            accum = effectRadialBlur->Apply(args, accum);
        }
    }

    // draw geometry such as HUD into an upscaled framebuf
    if (!drawInfo.disableRasterization)
    {
        rasterizer->DrawToSwapchain( cmd,
                                     frameIndex,
                                     accum,
                                     textureManager,
                                     uniform->GetData()->view,
                                     uniform->GetData()->projection,
                                     renderResolution.UpscaledWidth(),
                                     renderResolution.UpscaledHeight() );
    }

    // post-effect that work on swapchain geometry too
    {
        if (effectWipe->Setup(args, drawInfo.postEffectParams.pWipe, swapchain, frameId))
        {
            accum = effectWipe->Apply(args, blueNoise, accum);
        }
        if (drawInfo.postEffectParams.pCRT != nullptr && drawInfo.postEffectParams.pCRT->isActive)
        {
            effectCrtDemodulateEncode->Setup(args);
            accum = effectCrtDemodulateEncode->Apply(args, accum);

            effectCrtDecode->Setup(args);
            accum = effectCrtDecode->Apply(args, accum);
        }
    }

    // blit result image to present on a surface
    framebuffers->PresentToSwapchain( cmd, frameIndex, swapchain, accum, VK_FILTER_NEAREST );
}

void VulkanDevice::EndFrame(VkCommandBuffer cmd)
{
    uint32_t frameIndex = currentFrameState.GetFrameIndex();
    VkSemaphore semaphoreToWait = currentFrameState.GetSemaphoreForWaitAndRemove();

    // submit command buffer, but wait until presentation engine has completed using image
    cmdManager->Submit(
        cmd, 
        semaphoreToWait,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 
        renderFinishedSemaphores[frameIndex],
        frameFences[frameIndex]);

    // present on a surface when rendering will be finished
    swapchain->Present(queues, renderFinishedSemaphores[frameIndex]);

    frameId++;
}



#pragma region RTGL1 interface implementation

void VulkanDevice::StartFrame(const RgStartFrameInfo *startInfo)
{
    if (currentFrameState.WasFrameStarted())
    {
        throw RgException(RG_FRAME_WASNT_ENDED);
    }

    if (startInfo == nullptr)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Argument is null");
    }

    VkCommandBuffer newFrameCmd = BeginFrame(*startInfo);
    currentFrameState.OnBeginFrame(newFrameCmd);
}

void VulkanDevice::DrawFrame(const RgDrawFrameInfo *drawInfo)
{
    if (!currentFrameState.WasFrameStarted())
    {
        throw RgException(RG_FRAME_WASNT_STARTED);
    }

    if (drawInfo == nullptr)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Argument is null");
    }

    VkCommandBuffer cmd = currentFrameState.GetCmdBuffer();

    previousFrameTime = currentFrameTime;
    currentFrameTime = drawInfo->currentTime;

    renderResolution.Setup(drawInfo->pRenderResolutionParams,
                           swapchain->GetWidth(), swapchain->GetHeight(), nvDlss);

    // Tell the FidelityFX framework which FSR version to use (FSR 2 or FSR 3.1),
    // but only when the technique actually changes (or on the first frame).
    if (!lastUpscaleTechnique.has_value() ||
        *lastUpscaleTechnique != renderResolution.GetUpscaleTechnique())
    {
        lastUpscaleTechnique = renderResolution.GetUpscaleTechnique();
        amdFsr->SetUpscaleVersion(*lastUpscaleTechnique);
    }

    textureManager->CheckForHotReload(cmd);

    if (renderResolution.Width() > 0 && renderResolution.Height() > 0)
    {
        FillUniform(uniform->GetData(), *drawInfo);
        Render(cmd, *drawInfo);
    }

    EndFrame(cmd);
    currentFrameState.OnEndFrame();
}

bool VulkanDevice::IsSuspended() const
{
    if (!swapchain)
    {
        return false;
    }

    return !swapchain->IsExtentOptimal();
}

bool RTGL1::VulkanDevice::IsRenderUpscaleTechniqueAvailable(RgRenderUpscaleTechnique technique) const
{
    switch (technique)
    {
        case RG_RENDER_UPSCALE_TECHNIQUE_NEAREST:
        case RG_RENDER_UPSCALE_TECHNIQUE_LINEAR:
            return true;
        case RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2:
        case RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR3:
            return FSR::IsUpscaleVersionAvailable(technique);
        case RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS:
            return nvDlss->IsDlssAvailable();
        default:
            throw RgException(RG_WRONG_ARGUMENT, "Incorrect technique was passed to rgIsRenderUpscaleTechniqueAvailable");
    }
}

void VulkanDevice::Print(const char *pMessage) const
{
    userPrint->Print(pMessage);
}


void VulkanDevice::UploadGeometry(const RgGeometryUploadInfo *uploadInfo)
{
    using namespace std::string_literals;

    if (uploadInfo == nullptr)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Argument is null");
    }

    if (uploadInfo->pVertices == nullptr || uploadInfo->vertexCount == 0)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Incorrect vertex data");
    }

    if ((uploadInfo->pIndices == nullptr && uploadInfo->indexCount != 0) ||
        (uploadInfo->pIndices != nullptr && uploadInfo->indexCount == 0))
    {
        throw RgException(RG_WRONG_ARGUMENT, "Incorrect index data");
    }

    if (uploadInfo->geomType != RG_GEOMETRY_TYPE_STATIC &&
        uploadInfo->geomType != RG_GEOMETRY_TYPE_STATIC_MOVABLE &&
        uploadInfo->geomType != RG_GEOMETRY_TYPE_DYNAMIC &&

        uploadInfo->passThroughType != RG_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE &&
        uploadInfo->passThroughType != RG_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED &&
        uploadInfo->passThroughType != RG_GEOMETRY_PASS_THROUGH_TYPE_MIRROR &&
        uploadInfo->passThroughType != RG_GEOMETRY_PASS_THROUGH_TYPE_PORTAL &&
        uploadInfo->passThroughType != RG_GEOMETRY_PASS_THROUGH_TYPE_WATER_ONLY_REFLECT &&
        uploadInfo->passThroughType != RG_GEOMETRY_PASS_THROUGH_TYPE_WATER_REFLECT_REFRACT &&
        uploadInfo->passThroughType != RG_GEOMETRY_PASS_THROUGH_TYPE_GLASS_REFLECT_REFRACT &&
        uploadInfo->passThroughType != RG_GEOMETRY_PASS_THROUGH_TYPE_ACID_REFLECT_REFRACT &&

        uploadInfo->visibilityType != RG_GEOMETRY_VISIBILITY_TYPE_WORLD_0 &&
        uploadInfo->visibilityType != RG_GEOMETRY_VISIBILITY_TYPE_WORLD_1 &&
        uploadInfo->visibilityType != RG_GEOMETRY_VISIBILITY_TYPE_WORLD_2 &&
        uploadInfo->visibilityType != RG_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON &&
        uploadInfo->visibilityType != RG_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON_VIEWER && 
        uploadInfo->visibilityType != RG_GEOMETRY_VISIBILITY_TYPE_SKY)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Incorrect type of ray traced geometry");
    }

    if (allowGeometryWithSkyFlag)
    {
        if (uploadInfo->visibilityType == RG_GEOMETRY_VISIBILITY_TYPE_WORLD_2)
        {
            throw RgException(RG_WRONG_ARGUMENT, "Geometry with RG_GEOMETRY_VISIBILITY_TYPE_WORLD_2 cannot be used, as RgInstanceCreateInfo::allowGeometryWithSkyFlag was true");
        }
    }
    else
    {
        if (uploadInfo->visibilityType == RG_GEOMETRY_VISIBILITY_TYPE_SKY)
        {
            throw RgException(RG_WRONG_ARGUMENT, "Geometry with RG_GEOMETRY_VISIBILITY_TYPE_SKY cannot be used, as RgInstanceCreateInfo::allowGeometryWithSkyFlag was false");
        }
    }

    if ((uploadInfo->flags & RG_GEOMETRY_UPLOAD_REFL_REFR_ALBEDO_MULTIPLY_BIT) != 0 &&
        (uploadInfo->flags & RG_GEOMETRY_UPLOAD_REFL_REFR_ALBEDO_ADD_BIT) != 0)
    {
        throw RgException(RG_WRONG_ARGUMENT, "RG_GEOMETRY_UPLOAD_REFL_REFR_ALBEDO_MULTIPLY_BIT and RG_GEOMETRY_UPLOAD_REFL_REFR_ALBEDO_ADD_BIT must be set separately");
    }

    if (scene->DoesUniqueIDExist(uploadInfo->uniqueID))
    {
        throw RgException(RG_WRONG_ARGUMENT, "Geometry with ID="s + std::to_string(uploadInfo->uniqueID) + " already exists");
    }

    if (uploadInfo->pPortalIndex != nullptr && uploadInfo->passThroughType != RG_GEOMETRY_PASS_THROUGH_TYPE_PORTAL)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Geometry's pPortalIndex is non-null, but geometry is not marked as portal");
    }

    if (uploadInfo->pPortalIndex == nullptr && uploadInfo->passThroughType == RG_GEOMETRY_PASS_THROUGH_TYPE_PORTAL)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Geometry is marked as portal, but pPortalIndex is null");
    }

    if (uploadInfo->pPortalIndex && *(uploadInfo->pPortalIndex) >= PORTAL_MAX_COUNT)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Geometry's portal index must be in [0, 62]");
    }

    scene->Upload(currentFrameState.GetFrameIndex(), *uploadInfo);
}

void VulkanDevice::UpdateGeometryTransform(const RgUpdateTransformInfo *updateInfo)
{
    if (updateInfo == nullptr)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Argument is null");
    }

    scene->UpdateTransform(*updateInfo);
}

void RTGL1::VulkanDevice::UpdateGeometryTexCoords(const RgUpdateTexCoordsInfo *updateInfo)
{
    if (updateInfo == nullptr)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Argument is null");
    }

    scene->UpdateTexCoords(*updateInfo);
}

void VulkanDevice::UploadRasterizedGeometry(const RgRasterizedGeometryUploadInfo *pUploadInfo,
                                                const float *pViewProjection, const RgViewport *pViewport)
{
    if (pUploadInfo == nullptr)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Argument is null");
    }

    if (pUploadInfo->renderType != RG_RASTERIZED_GEOMETRY_RENDER_TYPE_DEFAULT &&
        pUploadInfo->renderType != RG_RASTERIZED_GEOMETRY_RENDER_TYPE_SWAPCHAIN &&
        pUploadInfo->renderType != RG_RASTERIZED_GEOMETRY_RENDER_TYPE_SKY)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Incorrect render type of rasterized geometry");
    }

    if (pUploadInfo->pVertices == nullptr || pUploadInfo->vertexCount == 0)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Vertex data / count is null");
    }

    if ((pUploadInfo->pIndices == nullptr && pUploadInfo->indexCount != 0) ||
        (pUploadInfo->pIndices != nullptr && pUploadInfo->indexCount == 0))
    {
        throw RgException(RG_WRONG_ARGUMENT, "Index data / count must be both not null or null");
    }

    rasterizer->Upload(currentFrameState.GetFrameIndex(), *pUploadInfo, pViewProjection, pViewport);
}

void RTGL1::VulkanDevice::UploadLensFlare(const RgLensFlareUploadInfo *pUploadInfo)
{
    if (pUploadInfo == nullptr)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Argument is null");
    }

    if (pUploadInfo->indexCount == 0 || pUploadInfo->pIndices == nullptr)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Lens flare index data and count must not be null");
    }

    if (pUploadInfo->vertexCount == 0 || pUploadInfo->pVertices == nullptr)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Lens flare vertex data and count must not be null");
    }

    rasterizer->UploadLensFlare(currentFrameState.GetFrameIndex(), *pUploadInfo);
}

void RTGL1::VulkanDevice::UploadDecal(const RgDecalUploadInfo *pUploadInfo)
{
    if (pUploadInfo == nullptr)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Argument is null");
    }

    decalManager->Upload(currentFrameState.GetFrameIndex(), *pUploadInfo, textureManager);
}

void RTGL1::VulkanDevice::UploadPortal(const RgPortalUploadInfo *pUploadInfo)
{
    if (pUploadInfo == nullptr)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Argument is null");
    }

    portalList->Upload(currentFrameState.GetFrameIndex(), *pUploadInfo);
}

void VulkanDevice::SubmitStaticGeometries()
{
    scene->SubmitStatic();
}

void VulkanDevice::StartNewStaticScene()
{
    scene->StartNewStatic();
}

void VulkanDevice::UploadDirectionalLight(const RgDirectionalLightUploadInfo *pLightInfo)
{
    if (pLightInfo == nullptr)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Argument is null");
    }

    scene->UploadLight(currentFrameState.GetFrameIndex(), *pLightInfo);
}

void VulkanDevice::UploadSphericalLight(const RgSphericalLightUploadInfo *pLightInfo)
{
    if (pLightInfo == nullptr)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Argument is null");
    }

    scene->UploadLight(currentFrameState.GetFrameIndex(), *pLightInfo);
}

void VulkanDevice::UploadSpotlight(const RgSpotLightUploadInfo *pLightInfo)
{
    if (pLightInfo == nullptr)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Argument is null");
    }

    scene->UploadLight(currentFrameState.GetFrameIndex(), *pLightInfo);
}

void RTGL1::VulkanDevice::UploadPolygonalLight(const RgPolygonalLightUploadInfo *pLightInfo)
{
    if (pLightInfo == nullptr)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Argument is null");
    }

    scene->UploadLight(currentFrameState.GetFrameIndex(), *pLightInfo);
}

void VulkanDevice::SetFogVolumes(uint32_t count, const RgFogVolume *pVolumes)
{
    if (count == 0 || pVolumes == nullptr)
    {
        fogVolumeCount = 0;
        fogVolumes = {};
        return;
    }

    count = std::min(count, static_cast<uint32_t>(RG_MAX_FOG_VOLUMES));

    for (uint32_t i = 0; i < count; i++)
    {
        fogVolumes[i] = pVolumes[i];
    }
    fogVolumeCount = count;
}

void VulkanDevice::CreateMaterial(const RgMaterialCreateInfo *createInfo, RgMaterial *result)
{
    if (createInfo == nullptr)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Argument is null");
    }

    *result = textureManager->CreateMaterial
    (
        currentFrameState.GetCmdBufferForMaterials(cmdManager),
        currentFrameState.GetFrameIndex(),
        *createInfo
    );
}

void VulkanDevice::CreateAnimatedMaterial(const RgAnimatedMaterialCreateInfo *createInfo, RgMaterial *result)
{
    if (createInfo == nullptr)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Argument is null");
    }

    if (createInfo->frameCount == 0)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Animated materials must have non-zero amount of frames");
    }

    *result = textureManager->CreateAnimatedMaterial
    (
        currentFrameState.GetCmdBufferForMaterials(cmdManager),
        currentFrameState.GetFrameIndex(),
        *createInfo
    );
}

void VulkanDevice::ChangeAnimatedMaterialFrame(RgMaterial animatedMaterial, uint32_t frameIndex)
{
    if (!currentFrameState.WasFrameStarted())
    {
        throw RgException(RG_FRAME_WASNT_STARTED);
    }

    bool wasChanged = textureManager->ChangeAnimatedMaterialFrame(animatedMaterial, frameIndex);
}

void VulkanDevice::UpdateMaterial(const RgMaterialUpdateInfo *updateInfo)
{
    if (!currentFrameState.WasFrameStarted())
    {
        throw RgException(RG_FRAME_WASNT_STARTED);
    }

    if (updateInfo == nullptr)
    {
        throw RgException(RG_WRONG_ARGUMENT, "Argument is null");
    }

    bool wasUpdated = textureManager->UpdateMaterial(currentFrameState.GetCmdBuffer(), *updateInfo);
}

void VulkanDevice::DestroyMaterial(RgMaterial material)
{
    textureManager->DestroyMaterial(currentFrameState.GetFrameIndex(), material);
}
void VulkanDevice::CreateSkyboxCubemap(const RgCubemapCreateInfo *createInfo, RgCubemap *result)
{
    *result = cubemapManager->CreateCubemap(currentFrameState.GetCmdBufferForMaterials(cmdManager), currentFrameState.GetFrameIndex(), *createInfo);
}
void VulkanDevice::DestroyCubemap(RgCubemap cubemap)
{
    cubemapManager->DestroyCubemap(currentFrameState.GetFrameIndex(), cubemap);
}
#pragma endregion 

