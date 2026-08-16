# ============================================================================
# RTGL1 (RayTracedGL1) renderer, vendored into vkquake (phase 4.7: decouple
# from RTGL1.dll). Built as a STATIC library and linked into vkquake.
# The public RG_* API stays an internal interface (plain function calls).
# ============================================================================
cmake_minimum_required(VERSION 3.20)

set(RTGL1_CXX_STANDARD 20)

# The vendored source is a copy of RayTracedGL1/Source + Include + Shaders.
set(RTGL1_PUBLIC_HEADERS
    Include/RTGL1/RTGL1.h
    Include/RTGL1/RTGL1A.h)

set(RTGL1_SOURCES
    Source/RTGL1.cpp
    Source/RTGL1A.cpp
    Source/VulkanDevice.cpp
    Source/VulkanDevice_Init.cpp
    Source/Buffer.cpp
    Source/Scene.cpp
    Source/PhysicalDevice.cpp
    Source/Queues.cpp
    Source/Swapchain.cpp
    Source/GlobalUniform.cpp
    Source/CommandBufferManager.cpp
    Source/ShaderManager.cpp
    Source/RayTracingPipeline.cpp
    Source/VertexCollector.cpp
    Source/ASManager.cpp
    Source/VertexCollectorFilter.cpp
    Source/ASBuilder.cpp
    Source/ScratchBuffer.cpp
    Source/Utils.cpp
    Source/PathTracer.cpp
    Source/Common.cpp
    Source/Matrix.cpp
    Source/Rasterizer.cpp
    Source/RasterizedDataCollector.cpp
    Source/Vma/vk_mem_alloc_imp.cpp
    Source/ImageLoader.cpp
    Source/TextureManager.cpp
    Source/MemoryAllocator.cpp
    Source/SamplerManager.cpp
    Source/TextureOverrides.cpp
    Source/TextureDescriptors.cpp
    Source/TextureUploader.cpp
    Source/VertexCollectorFilterType.cpp
    Source/Generated/ShaderCommonCFramebuf.cpp
    Source/Framebuffers.cpp
    Source/BlueNoise.cpp
    Source/ImageComposition.cpp
    Source/Tonemapping.cpp
    Source/LightManager.cpp
    Source/AutoBuffer.cpp
    Source/ASComponent.cpp
    Source/CubemapManager.cpp
    Source/CubemapUploader.cpp
    Source/GeomInfoManager.cpp
    Source/VertexPreprocessing.cpp
    Source/RasterizerPipelines.cpp
    Source/RenderCubemap.cpp
    Source/DepthCopying.cpp
    Source/RasterPass.cpp
    Source/SwapchainPass.cpp
    Source/UserFunction.cpp
    Source/RgException.cpp
    Source/Bloom.cpp
    Source/Sharpening.cpp
    Source/DLSS.cpp
    Source/HaltonSequence.cpp
    Source/LensFlares.cpp
    Source/DecalManager.cpp
    Source/EffectBase.cpp
    Source/LightGrid.cpp
    Source/Stb/stb_image.cpp
    Source/ImageLoaderDev.cpp
    Source/PortalList.cpp
    Source/TextureObserver.cpp
    Source/Volumetric.cpp
    Source/ShadowMap.cpp
    Source/GodRays.cpp
    Source/Q2Denoiser.cpp
)

set(RTGL1_KTX_FOLDER Source/KTX/lib)
set(RTGL1_KTX_SOURCES
    "${RTGL1_KTX_FOLDER}/texture.c"
    "${RTGL1_KTX_FOLDER}/texture1.c"
    "${RTGL1_KTX_FOLDER}/texture2.c"
    "${RTGL1_KTX_FOLDER}/hashlist.c"
    "${RTGL1_KTX_FOLDER}/checkheader.c"
    "${RTGL1_KTX_FOLDER}/swap.c"
    "${RTGL1_KTX_FOLDER}/memstream.c"
    "${RTGL1_KTX_FOLDER}/filestream.c"
    "${RTGL1_KTX_FOLDER}/vkloader.c"
    "${RTGL1_KTX_FOLDER}/vk_funcs.c"
    "${RTGL1_KTX_FOLDER}/basisu/zstd/zstd.c"
    "${RTGL1_KTX_FOLDER}/dfdutils/createdfd.c"
    "${RTGL1_KTX_FOLDER}/dfdutils/interpretdfd.c"
    "${RTGL1_KTX_FOLDER}/dfdutils/queries.c"
    "${RTGL1_KTX_FOLDER}/dfdutils/vk2dfd.c"
)

# Win32 Vulkan surface is required by the host (SDL window).
add_compile_definitions(
    RG_USE_SURFACE_WIN32
    VK_USE_PLATFORM_WIN32_KHR
    NOMINMAX
    # Static build: the RG_* functions are plain internal calls, not exports.
    # RTGL1.h turns RGAPI into an empty macro when RG_STATIC is defined.
    RG_STATIC
    # KTX-Software
    KHRONOS_STATIC
    LIBKTX
    # FSR 2 / FSR 3.1 via prebuilt AMD-signed FidelityFX API DLL
    RG_USE_FSR3
)

if(WIN32)
    # DLSS stays optional (off by default, matching the current RTGL1 build)
    option(RG_WITH_NVIDIA_DLSS "Build RTGL1 with Nvidia DLSS" OFF)
    if(RG_WITH_NVIDIA_DLSS)
        add_compile_definitions(RG_USE_NVIDIA_DLSS)
        if(DEFINED ENV{DLSS_SDK_PATH})
            target_include_directories(RayTracedGL1 PRIVATE "$ENV{DLSS_SDK_PATH}/include")
            add_compile_definitions(NV_WINDOWS)
            if(CMAKE_BUILD_TYPE STREQUAL "Debug")
                target_link_libraries(RayTracedGL1 PRIVATE "$ENV{DLSS_SDK_PATH}/lib/Windows_x86_64/x86_64/nvsdk_ngx_d_dbg.lib")
            else()
                target_link_libraries(RayTracedGL1 PRIVATE "$ENV{DLSS_SDK_PATH}/lib/Windows_x86_64/x86_64/nvsdk_ngx_d.lib")
            endif()
        else()
            message(FATAL_ERROR "RG_WITH_NVIDIA_DLSS is ON but DLSS_SDK_PATH is not set")
        endif()
    endif()
endif()

add_library(RayTracedGL1 STATIC
    ${RTGL1_SOURCES}
    ${RTGL1_KTX_SOURCES}
    ${RTGL1_PUBLIC_HEADERS}
)

target_compile_features(RayTracedGL1 PRIVATE cxx_std_20)

# Keep the vendored renderer warnings under control (the host build uses /WX).
if(MSVC)
    target_compile_options(RayTracedGL1 PRIVATE /W3 /EHsc /WX- /MP)
    target_compile_options(RayTracedGL1 PRIVATE /wd4244 /wd4267 /wd4018 /wd4305 /wd4996 /wd4100 /wd4505 /wd5287 /wd4005)
else()
    target_compile_options(RayTracedGL1 PRIVATE -w)
endif()

# KTX includes
target_include_directories(RayTracedGL1 PRIVATE
    "Source/KTX/include"
    "Source/KTX/other_include"
    "Source/KTX/lib/basisu/zstd"
)

# Public include dir (RTGL1/RTGL1.h) — needed by the host and by RTGL1 sources
target_include_directories(RayTracedGL1 PUBLIC "Include")

# Vulkan (found in the parent scope)
target_link_libraries(RayTracedGL1 PUBLIC Vulkan::Vulkan)

# FSR 2 / FSR 3.1 — prebuilt AMD-signed FidelityFX API DLL
if(WIN32)
    set(RTGL1_FFX_API_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Source/FidelityFX_SDK_1.1.4/src")
    target_sources(RayTracedGL1 PRIVATE "Source/FSR.cpp")
    target_include_directories(RayTracedGL1 PRIVATE "${RTGL1_FFX_API_DIR}/ffx-api/include")
    target_link_libraries(RayTracedGL1 PUBLIC "${RTGL1_FFX_API_DIR}/PrebuiltSignedDLL/amd_fidelityfx_vk.lib")
    # amd_fidelityfx_vk.dll is copied next to vkquake.exe in the root CMakeLists
endif()
