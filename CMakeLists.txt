cmake_minimum_required(VERSION 3.20)
project(vkquake LANGUAGES C)

# ============================================================================
# C Standard
# ============================================================================
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ============================================================================
# Options (matching VS project defines)
# ============================================================================
option(USE_CODEC_MP3    "Enable MP3 audio codec (libmad)"     ON)
option(USE_CODEC_VORBIS "Enable Vorbis audio codec"           ON)
option(USE_CODEC_WAVE   "Enable WAVE audio codec"             ON)
option(USE_CODEC_FLAC   "Enable FLAC audio codec"             ON)
option(USE_CODEC_OPUS   "Enable Opus audio codec"             ON)
option(USE_CODEC_XMP    "Enable XMP tracker music"            ON)
option(USE_CODEC_UMX    "Enable UMX (Unreal) music"           ON)
option(USE_CODEC_MIKMOD "Enable MikMod tracker music"         OFF)
option(USE_CODEC_MODPLUG "Enable ModPlug tracker music"       OFF)

option(ENABLE_MIMALLOC  "Use mimalloc allocator"              OFF)

# ============================================================================
# Platform detection
# ============================================================================
if(WIN32)
    set(VKQUAKE_PLATFORM_WINDOWS ON)
elseif(APPLE)
    set(VKQUAKE_PLATFORM_MACOS ON)
else()
    set(VKQUAKE_PLATFORM_LINUX ON)
endif()

# ============================================================================
# Compiler settings
# ============================================================================
include(CheckCCompilerFlag)

if(MSVC)
    # Match VS project: /W4, WX, no exceptions, multiprocessor
    add_compile_options(/W4 /WX /MP)
    add_compile_options(/wd4100 /wd4127 /wd4389 /wd4702 /wd4706 /wd5286)

    # VS project default defines
    add_compile_definitions(
        _CRT_NONSTDC_NO_DEPRECATE
        _CRT_SECURE_NO_WARNINGS
        _WINSOCK_DEPRECATED_NO_WARNINGS
    )

    # Disable exception handling (C project)
    add_compile_options($<$<COMPILE_LANGUAGE:C>:/EHsc->)

else()
    # GCC/Clang flags matching Makefile
    add_compile_options(-Wall -Wno-trigraphs -Wno-unused-function -Werror)

    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR NOT CMAKE_BUILD_TYPE)
        add_compile_options(-O3)
    endif()

    check_c_compiler_flag(-fweb COMPILER_HAS_FWEB)
    check_c_compiler_flag(-frename-registers COMPILER_HAS_FRENAME)
    check_c_compiler_flag(-fno-asynchronous-unwind-tables COMPILER_HAS_NO_ASYNC_UNWIND)
    check_c_compiler_flag(-fno-ident COMPILER_HAS_NO_IDENT)

    if(COMPILER_HAS_FWEB)
        add_compile_options(-fweb)
    endif()
    if(COMPILER_HAS_FRENAME)
        add_compile_options(-frename-registers)
    endif()
    if(COMPILER_HAS_NO_ASYNC_UNWIND)
        add_compile_options(-fno-asynchronous-unwind-tables)
    endif()
    if(COMPILER_HAS_NO_IDENT)
        add_compile_options(-fno-ident)
    endif()
endif()

# ============================================================================
# Codec compile definitions
# ============================================================================
function(_add_codec_define opt name)
    if(${opt})
        add_compile_definitions(${name})
    endif()
endfunction()

_add_codec_define(USE_CODEC_MP3    USE_CODEC_MP3)
_add_codec_define(USE_CODEC_VORBIS USE_CODEC_VORBIS)
_add_codec_define(USE_CODEC_WAVE   USE_CODEC_WAVE)
_add_codec_define(USE_CODEC_FLAC   USE_CODEC_FLAC)
_add_codec_define(USE_CODEC_OPUS   USE_CODEC_OPUS)
_add_codec_define(USE_CODEC_XMP    USE_CODEC_XMP)
_add_codec_define(USE_CODEC_UMX    USE_CODEC_UMX)
_add_codec_define(USE_CODEC_MIKMOD USE_CODEC_MIKMOD)
_add_codec_define(USE_CODEC_MODPLUG USE_CODEC_MODPLUG)

# ============================================================================
# Find Vulkan
#   On Windows, the project bundles vulkan-1.lib + headers in Windows/misc/
#   but prefer the system Vulkan SDK if installed.
# ============================================================================
if(VKQUAKE_PLATFORM_WINDOWS)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(VK_BUNDLED_LIB_DIR "${CMAKE_SOURCE_DIR}/Windows/misc/x64")
    else()
        set(VK_BUNDLED_LIB_DIR "${CMAKE_SOURCE_DIR}/Windows/misc/x86")
    endif()
    set(VK_BUNDLED_INC_DIR "${CMAKE_SOURCE_DIR}/Windows/misc/include")

    # Try system Vulkan SDK first, fall back to bundled
    find_package(Vulkan QUIET)
    if(NOT Vulkan_FOUND)
        message(STATUS "Vulkan SDK not found, using bundled Vulkan from Windows/misc/")
        add_library(Vulkan::Vulkan SHARED IMPORTED)
        set_target_properties(Vulkan::Vulkan PROPERTIES
            IMPORTED_LOCATION "${VK_BUNDLED_LIB_DIR}/vulkan-1.lib"
            INTERFACE_INCLUDE_DIRECTORIES "${VK_BUNDLED_INC_DIR}"
        )
        set(Vulkan_FOUND TRUE)
    endif()
else()
    find_package(Vulkan REQUIRED)
endif()

# ============================================================================
# SDL2 — bundled on Windows
# ============================================================================
if(VKQUAKE_PLATFORM_WINDOWS)
    set(SDL2_ROOT "${CMAKE_SOURCE_DIR}/Windows/SDL2")
    set(SDL2_INCLUDE_DIR "${SDL2_ROOT}/include")

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(SDL2_LIB_DIR "${SDL2_ROOT}/lib64")
    else()
        set(SDL2_LIB_DIR "${SDL2_ROOT}/lib")
    endif()

    # SDL2.lib is an import library for SDL2.dll
    add_library(SDL2::SDL2 SHARED IMPORTED)
    set_target_properties(SDL2::SDL2 PROPERTIES
        IMPORTED_LOCATION "${SDL2_LIB_DIR}/SDL2.dll"
        IMPORTED_IMPLIB   "${SDL2_LIB_DIR}/SDL2.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIR}"
    )

    # SDL2main.lib is a tiny static lib providing WinMain
    add_library(SDL2::SDL2main STATIC IMPORTED)
    set_target_properties(SDL2::SDL2main PROPERTIES
        IMPORTED_LOCATION "${SDL2_LIB_DIR}/SDL2main.lib"
    )
else()
    find_package(SDL2 REQUIRED)
endif()

# ============================================================================
# Windows bundled codec libraries
# ============================================================================
if(VKQUAKE_PLATFORM_WINDOWS)
    set(CODECS_ROOT "${CMAKE_SOURCE_DIR}/Windows/codecs")

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(CODECS_LIB_DIR "${CODECS_ROOT}/x64")
    else()
        set(CODECS_LIB_DIR "${CODECS_ROOT}/x86")
    endif()
    set(CODECS_INC_DIR "${CODECS_ROOT}/include")

    # Helper: create an imported DLL library from bundled .lib (import lib) + .dll
    function(_add_codec_dll target_name lib_file dll_file)
        add_library(${target_name} SHARED IMPORTED)
        set_target_properties(${target_name} PROPERTIES
            IMPORTED_LOCATION "${CODECS_LIB_DIR}/${dll_file}"
            IMPORTED_IMPLIB   "${CODECS_LIB_DIR}/${lib_file}"
            INTERFACE_INCLUDE_DIRECTORIES "${CODECS_INC_DIR}"
        )
    endfunction()

    if(USE_CODEC_MP3)
        _add_codec_dll(codec::mad "libmad.lib" "libmad-0.dll")
    endif()
    if(USE_CODEC_VORBIS)
        _add_codec_dll(codec::vorbis     "libvorbis.lib"      "libvorbis-0.dll")
        _add_codec_dll(codec::vorbisfile "libvorbisfile.lib"  "libvorbisfile-3.dll")
        _add_codec_dll(codec::ogg        "libogg.lib"         "libogg-0.dll")
    endif()
    if(USE_CODEC_FLAC)
        _add_codec_dll(codec::FLAC "libFLAC.lib" "libFLAC-8.dll")
    endif()
    if(USE_CODEC_OPUS)
        _add_codec_dll(codec::opus     "libopus.lib"     "libopus-0.dll")
        _add_codec_dll(codec::opusfile "libopusfile.lib" "libopusfile-0.dll")
    endif()
    if(USE_CODEC_XMP)
        _add_codec_dll(codec::xmp "libxmp.lib" "libxmp.dll")
    endif()
    if(USE_CODEC_MIKMOD)
        _add_codec_dll(codec::mikmod "libmikmod.lib" "libmikmod-3.dll")
    endif()
endif()

# ============================================================================
# RTGL1 (RayTracedGL1) — REQUIRED
# ============================================================================
if(NOT DEFINED RTGL1_SDK_PATH)
    set(RTGL1_SDK_PATH "$ENV{RTGL1_SDK_PATH}" CACHE PATH
        "Path to RayTracedGL1 SDK (must contain Include/RTGL1/RTGL1.h)")
endif()

if(NOT RTGL1_SDK_PATH)
    message(FATAL_ERROR
        "RTGL1_SDK_PATH is not set. This project requires RayTracedGL1.\n"
        "Set via: -DRTGL1_SDK_PATH=<path> or RTGL1_SDK_PATH environment variable.\n"
        "The SDK should contain: Include/RTGL1/RTGL1.h and Build/RayTracedGL1.lib")
endif()

if(NOT EXISTS "${RTGL1_SDK_PATH}/Include/RTGL1/RTGL1.h")
    message(FATAL_ERROR
        "RTGL1 header not found: ${RTGL1_SDK_PATH}/Include/RTGL1/RTGL1.h\n"
        "Make sure RTGL1_SDK_PATH points to a valid RayTracedGL1 SDK.")
endif()

add_library(RTGL1::RTGL1 SHARED IMPORTED)
set_target_properties(RTGL1::RTGL1 PROPERTIES
    IMPORTED_IMPLIB   "${RTGL1_SDK_PATH}/Build/RayTracedGL1.lib"
    IMPORTED_LOCATION "${RTGL1_SDK_PATH}/Build/RayTracedGL1.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${RTGL1_SDK_PATH}/Include"
)

message(STATUS "RTGL1 SDK: ${RTGL1_SDK_PATH}")

# ============================================================================
# Source files
# ============================================================================

# --- Core sources (all platforms) ---
set(CORE_SOURCES
    Quake/bgmusic.c
    Quake/cfgfile.c
    Quake/chase.c
    Quake/cl_demo.c
    Quake/cl_input.c
    Quake/cl_main.c
    Quake/cl_parse.c
    Quake/cl_tent.c
    Quake/cmd.c
    Quake/common.c
    Quake/console.c
    Quake/crc.c
    Quake/cvar.c
    Quake/gl_draw.c
    Quake/gl_fog.c
    Quake/gl_heap.c
    Quake/gl_mesh.c
    Quake/gl_model.c
    Quake/gl_refrag.c
    Quake/gl_rlight.c
    Quake/gl_rmain.c
    Quake/gl_rmisc.c
    Quake/gl_screen.c
    Quake/gl_sky.c
    Quake/gl_texmgr.c
    Quake/gl_warp.c
    Quake/host.c
    Quake/host_cmd.c
    Quake/image.c
    Quake/keys.c
    Quake/mathlib.c
    Quake/mdfour.c
    Quake/mem.c
    Quake/menu.c
    Quake/miniz.c
    Quake/palette.c
    Quake/pr_cmds.c
    Quake/pr_edict.c
    Quake/pr_exec.c
    Quake/pr_ext.c
    Quake/r_alias.c
    Quake/r_brush.c
    Quake/r_part.c
    Quake/r_part_fte.c
    Quake/r_sprite.c
    Quake/r_world.c
    Quake/rt_material.c
    Quake/rt_pkz.c
    Quake/sbar.c
    Quake/snd_codec.c
    Quake/snd_dma.c
    Quake/snd_mem.c
    Quake/snd_mix.c
    Quake/snd_wave.c
    Quake/strlcat.c
    Quake/strlcpy.c
    Quake/sv_main.c
    Quake/sv_move.c
    Quake/sv_phys.c
    Quake/sv_user.c
    Quake/tasks.c
    Quake/view.c
    Quake/wad.c
    Quake/world.c
)

# --- SDL layer (all platforms) ---
set(SDL_SOURCES
    Quake/cd_sdl.c
    Quake/gl_vidsdl.c
    Quake/in_sdl.c
    Quake/main_sdl.c
    Quake/snd_sdl.c
)

# --- Networking (all platforms) ---
set(NET_COMMON_SOURCES
    Quake/net_dgrm.c
    Quake/net_loop.c
    Quake/net_main.c
)

# --- Codec sources ---
set(CODEC_SOURCES)
if(USE_CODEC_MP3)
    list(APPEND CODEC_SOURCES Quake/snd_mp3.c Quake/snd_mp3tag.c)
endif()
if(USE_CODEC_VORBIS)
    list(APPEND CODEC_SOURCES Quake/snd_vorbis.c)
endif()
if(USE_CODEC_FLAC)
    list(APPEND CODEC_SOURCES Quake/snd_flac.c)
endif()
if(USE_CODEC_OPUS)
    list(APPEND CODEC_SOURCES Quake/snd_opus.c)
endif()
if(USE_CODEC_XMP)
    list(APPEND CODEC_SOURCES Quake/snd_xmp.c)
endif()
if(USE_CODEC_MIKMOD)
    list(APPEND CODEC_SOURCES Quake/snd_mikmod.c)
endif()
if(USE_CODEC_MODPLUG)
    list(APPEND CODEC_SOURCES Quake/snd_modplug.c)
endif()
if(USE_CODEC_UMX)
    list(APPEND CODEC_SOURCES Quake/snd_umx.c)
endif()

# --- Platform-specific sources (matching vcxproj for Windows) ---
if(VKQUAKE_PLATFORM_WINDOWS)
    set(PLATFORM_SOURCES
        Quake/net_win.c
        Quake/net_wins.c
        Quake/net_wipx.c
        Quake/pl_win.c
        Quake/sys_sdl_win.c
    )
    set(PLATFORM_RESOURCE Windows/vkQuake.rc)
elseif(VKQUAKE_PLATFORM_LINUX)
    set(PLATFORM_SOURCES
        Quake/net_bsd.c
        Quake/net_udp.c
        Quake/pl_linux.c
        Quake/sys_sdl_unix.c
    )
    set(PLATFORM_RESOURCE "")
elseif(VKQUAKE_PLATFORM_MACOS)
    set(PLATFORM_SOURCES
        Quake/net_bsd.c
        Quake/net_udp.c
        Quake/pl_osx.m
        Quake/sys_sdl_unix.c
    )
    set(PLATFORM_RESOURCE "")
endif()

# --- mimalloc (optional) ---
set(MIMALLOC_SOURCES)
if(ENABLE_MIMALLOC)
    file(GLOB MIMALLOC_FILES Quake/mimalloc/*.c)
    list(FILTER MIMALLOC_FILES EXCLUDE REGEX ".*alloc-override-osx\\.c$")
    set(MIMALLOC_SOURCES ${MIMALLOC_FILES})
    add_compile_definitions(USE_MIMALLOC)
endif()

# ============================================================================
# Combine all sources
# ============================================================================
set(ALL_SOURCES
    ${CORE_SOURCES}
    ${SDL_SOURCES}
    ${NET_COMMON_SOURCES}
    ${CODEC_SOURCES}
    ${PLATFORM_SOURCES}
    ${MIMALLOC_SOURCES}
)

if(PLATFORM_RESOURCE)
    list(APPEND ALL_SOURCES ${PLATFORM_RESOURCE})
endif()

# ============================================================================
# Create executable
# ============================================================================
add_executable(vkquake ${ALL_SOURCES})

# ============================================================================
# Include directories
# ============================================================================
target_include_directories(vkquake PRIVATE
    ${CMAKE_SOURCE_DIR}/Quake
)

if(VKQUAKE_PLATFORM_WINDOWS)
    target_include_directories(vkquake PRIVATE
        ${CODECS_INC_DIR}
        ${SDL2_INCLUDE_DIR}
    )
endif()

# ============================================================================
# Compile definitions (per-target)
# ============================================================================
target_compile_definitions(vkquake PRIVATE
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

if(VKQUAKE_PLATFORM_WINDOWS)
    target_compile_definitions(vkquake PRIVATE WIN32 _WINDOWS)
elseif(VKQUAKE_PLATFORM_LINUX)
    target_compile_definitions(vkquake PRIVATE LINUX)
endif()

# MSVC Release: /GS- (no buffer security check)
if(MSVC)
    target_compile_options(vkquake PRIVATE $<$<CONFIG:Release>:/GS->)
endif()

# ============================================================================
# Link libraries
# ============================================================================
target_link_libraries(vkquake PRIVATE Vulkan::Vulkan)
target_link_libraries(vkquake PRIVATE RTGL1::RTGL1)

if(VKQUAKE_PLATFORM_WINDOWS)
    target_link_libraries(vkquake PRIVATE SDL2::SDL2 SDL2::SDL2main)
else()
    target_link_libraries(vkquake PRIVATE SDL2::SDL2)
endif()

if(VKQUAKE_PLATFORM_WINDOWS)
    target_link_libraries(vkquake PRIVATE ws2_32 winmm)
elseif(VKQUAKE_PLATFORM_LINUX)
    target_link_libraries(vkquake PRIVATE m dl pthread)
elseif(VKQUAKE_PLATFORM_MACOS)
    target_link_libraries(vkquake PRIVATE m dl pthread)
endif()

# Codec libs
if(VKQUAKE_PLATFORM_WINDOWS)
    if(USE_CODEC_MP3)
        target_link_libraries(vkquake PRIVATE codec::mad)
    endif()
    if(USE_CODEC_VORBIS)
        target_link_libraries(vkquake PRIVATE codec::vorbisfile codec::vorbis codec::ogg)
    endif()
    if(USE_CODEC_FLAC)
        target_link_libraries(vkquake PRIVATE codec::FLAC)
    endif()
    if(USE_CODEC_OPUS)
        target_link_libraries(vkquake PRIVATE codec::opusfile codec::opus)
    endif()
    if(USE_CODEC_XMP)
        target_link_libraries(vkquake PRIVATE codec::xmp)
    endif()
    if(USE_CODEC_MIKMOD)
        target_link_libraries(vkquake PRIVATE codec::mikmod)
    endif()
else()
    if(USE_CODEC_MP3)
        find_library(MAD_LIB mad REQUIRED)
        target_link_libraries(vkquake PRIVATE ${MAD_LIB})
    endif()
    if(USE_CODEC_VORBIS)
        find_package(Vorbis REQUIRED)
        target_link_libraries(vkquake PRIVATE Vorbis::Vorbis Vorbis::Vorbisfile)
        find_package(OGG REQUIRED)
        target_link_libraries(vkquake PRIVATE OGG::OGG)
    endif()
    if(USE_CODEC_FLAC)
        find_package(FLAC REQUIRED)
        target_link_libraries(vkquake PRIVATE FLAC::FLAC)
    endif()
    if(USE_CODEC_OPUS)
        find_package(Opus REQUIRED)
        find_package(Opusfile REQUIRED)
        target_link_libraries(vkquake PRIVATE Opus::Opus Opusfile::Opusfile)
    endif()
    if(USE_CODEC_XMP)
        find_library(XMP_LIB xmp REQUIRED)
        target_link_libraries(vkquake PRIVATE ${XMP_LIB})
    endif()
    if(USE_CODEC_MIKMOD)
        find_library(MIKMOD_LIB mikmod REQUIRED)
        target_link_libraries(vkquake PRIVATE ${MIKMOD_LIB})
    endif()
endif()

# ============================================================================
# Windows: subsystem and resource file
# ============================================================================
if(VKQUAKE_PLATFORM_WINDOWS)
    set_target_properties(vkquake PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
    if(MSVC)
        target_link_options(vkquake PRIVATE /SUBSYSTEM:WINDOWS)
    endif()
endif()

# ============================================================================
# Post-build: copy DLLs (Windows)
# ============================================================================
if(VKQUAKE_PLATFORM_WINDOWS)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(DLL_CODEC_DIR "${CODECS_ROOT}/x64")
        set(DLL_SDL_DIR "${SDL2_ROOT}/lib64")
    else()
        set(DLL_CODEC_DIR "${CODECS_ROOT}/x86")
        set(DLL_SDL_DIR "${SDL2_ROOT}/lib")
    endif()

    # Codec DLLs
    set(CODEC_DLLS "")
    if(USE_CODEC_MP3)
        list(APPEND CODEC_DLLS "libmad-0.dll")
    endif()
    if(USE_CODEC_VORBIS)
        list(APPEND CODEC_DLLS "libvorbis-0.dll" "libvorbisfile-3.dll" "libogg-0.dll")
    endif()
    if(USE_CODEC_FLAC)
        list(APPEND CODEC_DLLS "libFLAC-8.dll")
    endif()
    if(USE_CODEC_OPUS)
        list(APPEND CODEC_DLLS "libopus-0.dll" "libopusfile-0.dll")
    endif()
    if(USE_CODEC_XMP)
        list(APPEND CODEC_DLLS "libxmp.dll")
    endif()
    if(USE_CODEC_MIKMOD)
        list(APPEND CODEC_DLLS "libmikmod-3.dll")
    endif()

    foreach(dll IN LISTS CODEC_DLLS)
        add_custom_command(TARGET vkquake POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DLL_CODEC_DIR}/${dll}"
                "$<TARGET_FILE_DIR:vkquake>"
            COMMENT "Copying ${dll}..."
        )
    endforeach()

    # SDL2 DLL
    add_custom_command(TARGET vkquake POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DLL_SDL_DIR}/SDL2.dll"
            "$<TARGET_FILE_DIR:vkquake>"
        COMMENT "Copying SDL2.dll..."
    )

    # RTGL1 DLL
    add_custom_command(TARGET vkquake POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RTGL1_SDK_PATH}/Build/RayTracedGL1.dll"
            "$<TARGET_FILE_DIR:vkquake>"
        COMMENT "Copying RayTracedGL1.dll..."
    )
endif()
