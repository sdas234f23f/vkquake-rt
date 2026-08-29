// Copyright (c) 2021-2022 Sultim Tsyrendashiev
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



// This file was originally a raygen shader. But G-buffer decals are drawn
// on primary surfaces, but not in perfect reflections/refractions. Because
// 

// Must be defined:
// - either RAYGEN_PRIMARY_SHADER or RAYGEN_REFL_REFR_SHADER or Q2_REFL_REFR_SHADER
// - MATERIAL_MAX_ALBEDO_LAYERS

#if defined(RAYGEN_PRIMARY_SHADER) && defined(RAYGEN_REFL_REFR_SHADER)
    #error Only one of RAYGEN_PRIMARY_SHADER and RAYGEN_REFL_REFR_SHADER must be defined
#endif
#if !defined(RAYGEN_PRIMARY_SHADER) && !defined(RAYGEN_REFL_REFR_SHADER) && !defined(Q2_REFL_REFR_SHADER)
    #error RAYGEN_PRIMARY_SHADER, RAYGEN_REFL_REFR_SHADER or Q2_REFL_REFR_SHADER must be defined
#endif
#ifndef MATERIAL_MAX_ALBEDO_LAYERS
    #error MATERIAL_MAX_ALBEDO_LAYERS is not defined
#endif 



#define DESC_SET_TLAS 0
#define DESC_SET_FRAMEBUFFERS 1
#define DESC_SET_GLOBAL_UNIFORM 2
#define DESC_SET_VERTEX_DATA 3
#define DESC_SET_TEXTURES 4
#define DESC_SET_RANDOM 5
#define DESC_SET_LIGHT_SOURCES 6
#define DESC_SET_CUBEMAPS 7
#define DESC_SET_RENDER_CUBEMAP 8
#define DESC_SET_PORTALS 9
#define DESC_SET_RAY_STATS 11
#define LIGHT_SAMPLE_METHOD (LIGHT_SAMPLE_METHOD_NONE)
#include "RaygenCommon.h"
#include "Q2Fog.h"

vec2 getMotionVectorForUpscaler(const vec2 motionCurToPrev)
{
    return motionCurToPrev;
}

vec2 getMotionForInfinitePoint(const ivec2 pix)
{
    // treat as a point with .w=0, i.e. at infinite distance
    vec3 rayDir = getRayDir(getPixelUVWithJitter(pix));

    vec3 viewSpacePosCur   = mat3(globalUniform.view)     * rayDir;
    vec3 viewSpacePosPrev  = mat3(globalUniform.viewPrev) * rayDir;

    vec3 clipSpacePosCur   = mat3(globalUniform.projection)     * viewSpacePosCur;
    vec3 clipSpacePosPrev  = mat3(globalUniform.projectionPrev) * viewSpacePosPrev;

    // don't divide by .w
    vec3 ndcCur            = clipSpacePosCur.xyz;
    vec3 ndcPrev           = clipSpacePosPrev.xyz;

    vec2 screenSpaceCur    = ndcCur.xy  * 0.5 + 0.5;
    vec2 screenSpacePrev   = ndcPrev.xy * 0.5 + 0.5;

    return screenSpacePrev - screenSpaceCur;
}

// Q2RTX-style path tracer G-buffer, written on the new Q2 core path only.
// These feed the Q2 ASVGF gradient pipeline (phase 4.4.2) and the Q2RTX
// reflections (phase 4.4.3). Q2ViewDepth is the ray distance and is NEGATIVE
// for reflection/refraction surfaces (Q2RTX reflect_refract convention) so
// the ASVGF filters don't bleed across reflection boundaries.
void storeQ2GBuffer(
    const ivec2 pix,
    const vec3 baseColor, float specularFactor,
    float metallic, float roughness,
    float depth,
    float halfConeAngle, float distToLight,
    const vec3 transparentColor, float transparentAlpha,
    const vec4 fogAccum,
    const uint cluster)
{
    if (globalUniform.coreQ2RTX == 0)
    {
        return;
    }

    imageStore(framebufQ2ViewDepth,          pix, vec4(depth));
    imageStore(framebufQ2BaseColor,          pix, vec4(baseColor, specularFactor));
    imageStore(framebufQ2Metallic,           pix, vec4(metallic, roughness, 0.0, 0.0));
    imageStore(framebufQ2BounceThroughput,   pix, vec4(1.0, 1.0, 1.0, halfConeAngle));
    imageStore(framebufQ2Transparent,        pix, vec4(transparentColor, transparentAlpha));
    imageStore(framebufQ2GodRaysThroughputDist, pix, vec4(1.0, 1.0, 1.0, distToLight));
    imageStore(framebufQ2RngSeed,            pix, uvec4(getRandomSeed(pix, globalUniform.frameId)));
    imageStore(framebufQ2FogAccum,           pix, fogAccum);
    // BSP cluster of the hit surface (Q2RTX IMG_PT_CLUSTER_A). ~0u for sky.
    imageStore(framebufQ2Cluster,            pix, uvec4(cluster));
}

void storeSky(
    const ivec2 pix, const vec3 rayDir, bool calculateSkyAndStoreToAlbedo, const vec3 throughput,
#ifdef RAYGEN_PRIMARY_SHADER
    float firstHitDepthNDC,
#else
    bool wasSplit,
#endif
    const vec4 fogAccum )
{
    imageStore(framebufIsSky, pix, ivec4(1));

    {
        vec3 albedo;
        
        if (calculateSkyAndStoreToAlbedo)
        {
            albedo = getSkyPrimary(rayDir);
        }
        else
        {
            // was already in G-buffer after rasterization pass
            albedo = imageLoad(framebufAlbedo, getRegularPixFromCheckerboardPix(pix)).rgb;
        }
            
        imageStore(framebufAlbedo, getRegularPixFromCheckerboardPix(pix), vec4(albedo, 0.0));

        // Q2RTX-style G-buffer (sky = empty surface, env color in transparent)
        storeQ2GBuffer(pix, albedo, 0.0, 0.0, 1.0, MAX_RAY_LENGTH * 2.0, 0.0, MAX_RAY_LENGTH * 2.0, albedo, 1.0, fogAccum, ~0u);
    }

    vec2 m = getMotionForInfinitePoint(pix);

    imageStoreNormal(                       pix, vec3(0.0));
    imageStoreNormalGeometry(               pix, vec3(0.0));
    imageStore(framebufMetallicRoughness,   pix, vec4(0.0));
    imageStore(framebufDepthWorld,          pix, vec4(MAX_RAY_LENGTH * 2.0));
    imageStore(framebufMotion,              pix, vec4(m, 0.0, 0.0));
    imageStore(framebufSurfacePosition,     pix, vec4(SURFACE_POSITION_INCORRECT));
    imageStore(framebufVisibilityBuffer,    pix, vec4(UINT32_MAX));
    imageStore(framebufViewDirection,       pix, vec4(rayDir, 0.0));
    imageStore(framebufScreenEmisRT,        getRegularPixFromCheckerboardPix( pix ), vec4( 0.0 ) );
    imageStore(framebufAcidFogRT,           getRegularPixFromCheckerboardPix( pix ), vec4( 0.0 ) );
#ifdef RAYGEN_PRIMARY_SHADER
    imageStore(framebufPrimaryToReflRefr,   pix, uvec4(0, 0, PORTAL_INDEX_NONE, 0));
    imageStore(framebufDepthGrad,           pix, vec4(0.0));
    imageStore(framebufDepthNdc,            getRegularPixFromCheckerboardPix(pix), vec4(clamp(firstHitDepthNDC, 0.0, 1.0)));
    imageStore(framebufMotionDlss,          getRegularPixFromCheckerboardPix(pix), vec4(getMotionVectorForUpscaler(m), 0.0, 0.0));
    imageStore(framebufThroughput,          pix, vec4(throughput, 0.0));
#else
    imageStore(framebufThroughput,          pix, vec4(throughput, wasSplit ? 1.0 : -1.0));
#endif
}

uint getNewRayMedia(int i, uint prevMedia, uint geometryInstanceFlags)
{
    // if camera is not in vacuum, assume that new media is vacuum
    if (i == 0 && globalUniform.cameraMediaType != MEDIA_TYPE_VACUUM)
    {
       return MEDIA_TYPE_VACUUM;
    }

    return getMediaTypeFromFlags(geometryInstanceFlags);
}

vec3 getWaterNormal(const RayCone rayCone, const vec3 rayDir, const vec3 normalGeom, const vec3 position, bool wasPortal)
{
    const mat3 basis = getONB(normalGeom);
    const vec2 baseUV = vec2(dot(position, basis[0]), dot(position, basis[1])); 


    // how much vertical flow to apply
    float verticality = 1.0 - abs(dot(normalGeom, globalUniform.worldUpVector.xyz));

    // project basis[0] and basis[1] on up vector
    vec2 flowSpeedVertical = 10 * vec2(dot(basis[0], globalUniform.worldUpVector.xyz), 
                                       dot(basis[1], globalUniform.worldUpVector.xyz));

    vec2 flowSpeedHorizontal = vec2(1.0);


    const float uvScale = 0.05 / globalUniform.waterTextureAreaScale;
    vec2 speed0 = uvScale * mix(flowSpeedHorizontal, flowSpeedVertical, verticality) * globalUniform.waterWaveSpeed;
    vec2 speed1 = -0.9 * speed0 * mix(1.0, -0.1, verticality);


    // for texture sampling
    float derivU = globalUniform.waterTextureDerivativesMultiplier * 0.5 * uvScale * getWaterDerivU(rayCone, rayDir, normalGeom);

    // make water sharper if visible through the portal
    if (wasPortal)
    {
        derivU *= 0.1;
    }


    vec2 uv0 = uvScale * baseUV + globalUniform.time * speed0;
    vec3 n0 = getTextureSampleDerivU(globalUniform.waterNormalTextureIndex, uv0, derivU).xyz;
    n0.xy = n0.xy * 2.0 - vec2(1.0);


    vec2 uv1 = 0.8 * uvScale * baseUV + globalUniform.time * speed1;
    vec3 n1 = getTextureSampleDerivU(globalUniform.waterNormalTextureIndex, uv1, derivU).xyz;
    n1.xy = n1.xy * 2.0 - vec2(1.0);


    vec2 uv2 = 0.1 * (uvScale * baseUV + speed0 * sin(globalUniform.time * 0.5));
    vec3 n2 = getTextureSampleDerivU(globalUniform.waterNormalTextureIndex, uv2, derivU).xyz;
    n2.xy = n2.xy * 2.0 - vec2(1.0);


    const float strength = globalUniform.waterWaveStrength;

    const vec3 n = normalize(vec3(0, 0, 1) + strength * (0.25 * n0 + 0.2 * n1 + 0.1 * n2));
    return basis * n;   
}

mat3 lookAt(const vec3 forward, const vec3 worldUp)
{
    vec3 right = cross(forward, worldUp);
    vec3 up = cross(right, forward);

    return mat3(right, up, forward);
}

vec3 getPortalNormal(const vec3 baseNormal, const vec3 inWorldOffset)
{
    if (globalUniform.twirlPortalNormal == 0)
    {
        return -baseNormal;
    }

    float phaseScale = 3;
    float timeScale = 3;
    float waveScale = 0.01;
    float tm = mod(timeScale * globalUniform.time, M_PI * 2);

    const mat3 inLookAt_Plain = lookAt(-baseNormal, globalUniform.worldUpVector.xyz);
    const vec2 localOffset_Plain = vec2(dot(inWorldOffset, inLookAt_Plain[0]), 
                                        dot(inWorldOffset, inLookAt_Plain[1]));

    float distance = length(localOffset_Plain);
    float angle = atan(localOffset_Plain.y, localOffset_Plain.x);

    float phase = sin(phaseScale * sqrt(distance) + angle + tm) + 1.0;
    phase *= waveScale;
    // less weight around center
    phase *= clamp(distance / 20, 0, 1); 

    vec3 localN = { phase, phase, 1.0 };

    return inLookAt_Plain * normalize(localN);
}

bool isBackface(const vec3 normalGeom, const vec3 rayDir)
{
    return dot(normalGeom, -rayDir) < 0.0;
}

vec3 getNormal(const vec3 position, const vec3 normalFromMap, const vec3 normalGeom, const RayCone rayCone, const vec3 rayDir, bool isWater, bool wasPortal)
{
    if (isWater)
    {
        vec3 n = normalGeom;

        if (isBackface(normalGeom, rayDir))
        {
            n *= -1;
        }

        return getWaterNormal(rayCone, rayDir, n, position, wasPortal);
    }
    else
    {
        vec3 n = normalFromMap;

        if (isBackface(normalGeom, rayDir))
        {
           n *= -1;
        }

        return normalize(n);
    }
}

#ifdef RAYGEN_PRIMARY_SHADER
void main() 
{
    const ivec2 regularPix = ivec2(gl_LaunchIDEXT.xy);
    const ivec2 pix = getCheckerboardPix(regularPix);
    const vec2 inUV = getPixelUVWithJitter(regularPix);

    const vec3 cameraOrigin = globalUniform.cameraPosition.xyz;
    const vec3 cameraRayDir = getRayDir(inUV);
    const vec3 cameraRayDirAX = getRayDirAX(inUV);
    const vec3 cameraRayDirAY = getRayDirAY(inUV);

    const uint randomSeed = getRandomSeed(pix, globalUniform.frameId);
    
    
    const ShPayload primaryPayload = tracePrimaryRay(cameraOrigin, cameraRayDir);
    rayStatsAdd(RAY_STATS_CATEGORY_PRIMARY, 1);


    const uint currentRayMedia = globalUniform.cameraMediaType;


    // was no hit
    if (!doesPayloadContainHitInfo(primaryPayload))
    {
        vec3 throughput = vec3(1.0);
        // throughput *= getMediaTransmittance(currentRayMedia, pow(abs(dot(cameraRayDir, globalUniform.worldUpVector.xyz)), -3));

        // Q2RTX-style fog over the primary segment, extended to the end of the volumes
        vec4 q2FogAccum = vec4(0);
        if (globalUniform.coreQ2RTX != 0)
        {
            uvec4 q2SkyFog1, q2SkyFog2;
            q2FindFogVolumes(cameraOrigin, cameraRayDir, 0.0, 1e6, q2SkyFog1, q2SkyFog2);
            q2FogAccum = q2SegmentFog(q2SkyFog1, q2SkyFog2, 1e6);
        }

        // if sky is a rasterized geometry, it was already rendered to albedo framebuf 
        storeSky(pix, cameraRayDir, globalUniform.skyType != SKY_TYPE_RASTERIZED_GEOMETRY, throughput, MAX_RAY_LENGTH * 2.0, q2FogAccum);
        return;
    }


    vec2 motionCurToPrev;
    float motionDepthLinearCurToPrev;
    vec2 gradDepth;
    float firstHitDepthNDC;
    float firstHitDepthLinear;
    float screenEmission;
    const ShHitInfo h = getHitInfoPrimaryRay(primaryPayload, cameraOrigin, cameraRayDirAX, cameraRayDirAY, motionCurToPrev, motionDepthLinearCurToPrev, gradDepth, firstHitDepthNDC, firstHitDepthLinear, screenEmission);


    vec3 throughput = vec3(1.0);
    throughput *= getMediaTransmittance(currentRayMedia, firstHitDepthLinear);


    imageStore(framebufIsSky,               pix, ivec4(0));
    imageStore(framebufAlbedo,              getRegularPixFromCheckerboardPix(pix), vec4(h.albedo, 0.0));
    imageStore(framebufScreenEmisRT,        getRegularPixFromCheckerboardPix(pix), vec4((globalUniform.debugShowFlags & DEBUG_SHOW_FLAG_LUMA) != 0 ? vec3(screenEmission) : h.albedo * screenEmission * throughput , 0.0));
    imageStore(framebufAcidFogRT,           getRegularPixFromCheckerboardPix(pix), vec4(getGlowingMediaFog(currentRayMedia, firstHitDepthLinear), 0));
    imageStoreNormal(                       pix, h.normal);
    imageStoreNormalGeometry(               pix, h.normalGeom);
    imageStore(framebufMetallicRoughness,   pix, vec4(h.metallic, h.roughness, 0, 0));
    imageStore(framebufDepthWorld,          pix, vec4(firstHitDepthLinear));
    // depth gradients is not 2d, to remove vertical/horizontal artifacts
    imageStore(framebufDepthGrad,           pix, vec4(length(gradDepth)));
    imageStore(framebufMotion,              pix, vec4(motionCurToPrev, motionDepthLinearCurToPrev, 0.0));
    imageStore(framebufSurfacePosition,     pix, vec4(h.hitPosition, uintBitsToFloat(h.instCustomIndex)));
    imageStore(framebufVisibilityBuffer,    pix, packVisibilityBuffer(primaryPayload));
    imageStore(framebufViewDirection,       pix, vec4(cameraRayDir, 0.0));
    imageStore(framebufThroughput,          pix, vec4(throughput, 0.0));

    // save some info for refl/refr shader
    imageStore(framebufPrimaryToReflRefr,   pix, uvec4(h.geometryInstanceFlags, primaryPayload.instIdAndIndex, h.portalIndex, 0));

    // save info for rasterization and upscalers (FSR/DLSS), but only about primary surface,
    // as reflections/refraction only may be losely represented via rasterization
    imageStore(framebufDepthNdc,            getRegularPixFromCheckerboardPix(pix), vec4(clamp(firstHitDepthNDC, 0.0, 1.0)));
    imageStore(framebufMotionDlss,          getRegularPixFromCheckerboardPix(pix), vec4(getMotionVectorForUpscaler(motionCurToPrev), 0.0, 0.0));

    // Q2RTX-style G-buffer. specular factor = dielectric F0 (0.04) to metal albedo.
    // Accumulate the fog over the primary segment (Q2RTX approach).
    uvec4 q2Fog1, q2Fog2;
    q2FindFogVolumes(cameraOrigin, cameraRayDir, 0.0, firstHitDepthLinear, q2Fog1, q2Fog2);
    const vec4 q2FogAccum = q2SegmentFog(q2Fog1, q2Fog2, firstHitDepthLinear);
    storeQ2GBuffer(pix, h.albedo, mix(0.04, 1.0, h.metallic), h.metallic, h.roughness,
                   firstHitDepthLinear, 0.5 * length(cameraRayDir - cameraRayDirAX), firstHitDepthLinear,
                   vec3(0.0), 0.0, q2FogAccum, h.cluster);
}
#endif


#ifdef RAYGEN_REFL_REFR_SHADER
void main() 
{
    if (globalUniform.reflectRefractMaxDepth == 0)
    {
        return;
    }


    const ivec2 regularPix = ivec2(gl_LaunchIDEXT.xy);
    const ivec2 pix = getCheckerboardPix(regularPix);
    const vec2 inUV = getPixelUVWithJitter(regularPix);

    const vec3 cameraRayDir = getRayDir(inUV);
    
    if (isSkyPix(pix))
    {
        return;
    }



    // restore state from primary shader
    const uvec3 primaryToReflRefrBuf        = texelFetch(framebufPrimaryToReflRefr_Sampler, pix, 0).rgb;
    ShHitInfo h;
    h.albedo                                = texelFetch(framebufAlbedo_Sampler, getRegularPixFromCheckerboardPix(pix), 0).rgb;
    h.hitPosition                           = texelFetch(framebufSurfacePosition_Sampler, pix, 0).xyz;
    h.geometryInstanceFlags                 = primaryToReflRefrBuf.r;
    h.portalIndex                           = primaryToReflRefrBuf.b;
    h.normalGeom                            = texelFetchNormalGeometry(pix);
    h.normal                                = texelFetchNormal(pix);
    h.roughness                             = texelFetch( framebufMetallicRoughness_Sampler, pix, 0 ).g;
    const vec3  motionBuf                   = texelFetch(framebufMotion_Sampler, pix, 0).rgb;
    vec2        motionCurToPrev             = motionBuf.rg;
    float       motionDepthLinearCurToPrev  = motionBuf.b;
    float       firstHitDepthLinear         = texelFetch(framebufDepthWorld_Sampler, pix, 0).r;
    vec3        screenEmission              = texelFetch(framebufScreenEmisRT_Sampler, getRegularPixFromCheckerboardPix(pix), 0).rgb;
    vec3        acidFog                     = texelFetch(framebufAcidFogRT_Sampler, getRegularPixFromCheckerboardPix(pix), 0).rgb;
    vec3        throughput                  = texelFetch(framebufThroughput_Sampler, pix, 0).rgb;
    ShPayload currentPayload;
    currentPayload.instIdAndIndex           = primaryToReflRefrBuf.g;

    // Q2RTX-style accumulated fog from the primary pass; the reflection
    // segments are blended on top of it below (nearest fog in front).
    vec4 q2FogAccum = texelFetch(framebufQ2FogAccum_Sampler, pix, 0);



    RayCone rayCone;
    rayCone.width = 0;
    rayCone.spreadAngle = globalUniform.cameraRayConeSpreadAngle;

    float fullPathLength = firstHitDepthLinear;
    // length of the last reflected/refracted segment (used by the god rays
    // reflection pass to march only along the reflected segment, Q2RTX-style)
    float q2LastSegmentLen = 0.0;
    vec3 prevHitPosition = h.hitPosition;
    bool wasSplit = false;
    bool wasPortal = false;
    vec3 virtualPos = h.hitPosition;
    vec3 rayDir = cameraRayDir;
    uint currentRayMedia = globalUniform.cameraMediaType;
    // if there was no hitinfo from refl/refr, preserve primary hitinfo
    bool hitInfoWasOverwritten = false;


    propagateRayCone(rayCone, firstHitDepthLinear);



    for (int i = 0; i < globalUniform.reflectRefractMaxDepth; i++)
    {
        const uint instIndex = unpackInstanceIdAndCustomIndex(currentPayload.instIdAndIndex).y;


        bool isPixOdd = isCheckerboardPixOdd(pix) != 0;


        uint newRayMedia = getNewRayMedia(i, currentRayMedia, h.geometryInstanceFlags);

        bool isPortal = isPortalFromFlags(h.geometryInstanceFlags) && h.portalIndex != PORTAL_INDEX_NONE;
        bool toRefract = isRefractFromFlags(h.geometryInstanceFlags);
    #if SHIPPING_HACK
        bool toReflect = isReflectFromFlags( h.geometryInstanceFlags );
    #else
        bool toReflect = h.roughness < globalUniform.minRoughness ||
                         isReflectFromFlags( h.geometryInstanceFlags );
    #endif


        if (!toReflect && !toRefract && !isPortal)
        {
            break;
        }


        const float curIndexOfRefraction = getIndexOfRefraction(currentRayMedia);
        const float newIndexOfRefraction = getIndexOfRefraction(newRayMedia);

        const vec3 normal = getNormal(
            h.hitPosition,
            h.normal,
            h.normalGeom,
            rayCone,
            rayDir,
            !isPortal && ( newRayMedia == MEDIA_TYPE_WATER || currentRayMedia == MEDIA_TYPE_WATER ||
                           newRayMedia == MEDIA_TYPE_ACID || currentRayMedia == MEDIA_TYPE_ACID ),
            wasPortal );


        bool delaySplitOnNextTime = false;
            
        if ((h.geometryInstanceFlags & GEOM_INST_FLAG_NO_MEDIA_CHANGE) != 0)
        {
            // apply small new media transmittance, and ignore the media (but not the refraction indices)
            throughput *= getMediaTransmittance(newRayMedia, 1.0);
            newRayMedia = currentRayMedia;
            
            // if reflections are disabled if viewing from inside of NO_MEDIA_CHANGE geometry
            delaySplitOnNextTime = (globalUniform.noBackfaceReflForNoMediaChange != 0) && isBackface(h.normalGeom, rayDir);
        }

           

        vec3 rayOrigin = h.hitPosition;
        bool doSplit = !wasSplit;
        bool doRefraction;
        vec3 refractionDir;
        float F;

        if (delaySplitOnNextTime)
        {
            doSplit = false;
            // force refraction for all pixels
            toRefract = true;
            isPixOdd = true;
        }
        
        if (toRefract && calcRefractionDirection(curIndexOfRefraction, newIndexOfRefraction, rayDir, normal, refractionDir))
        {
            doRefraction = isPixOdd;
            F = getFresnelSchlick(curIndexOfRefraction, newIndexOfRefraction, -rayDir, normal);
        }
        else
        {
            // total internal reflection
            doRefraction = false;
            doSplit = false;
            F = 1.0;
        }
        
        if (doRefraction)
        {
            rayDir = refractionDir;
            throughput *= (1 - F);

            // change media
            currentRayMedia = newRayMedia;
        }
        else if (isPortal)
        {
            const ShPortalInstance portal = g_portals[h.portalIndex];

            const vec3 inCenter = portal.inPosition.xyz;
            const vec3 inWorldOffset = h.hitPosition - inCenter;

            mat3 inLookAt = lookAt(getPortalNormal(normal, inWorldOffset), globalUniform.worldUpVector.xyz);

            const vec3 outCenter = portal.outPosition.xyz;
            const mat3 outLookAt = lookAt(portal.outDirection.xyz, 
                                          portal.outUp.xyz);

            // to local space; then to world space but at portal output
            rayDir = outLookAt * (transpose(inLookAt) * rayDir);

            const vec2 localOffset = vec2(dot(inWorldOffset, inLookAt[0]), 
                                          dot(inWorldOffset, inLookAt[1]));

            rayOrigin = outCenter + localOffset.x * outLookAt[0] + localOffset.y * outLookAt[1];

            wasPortal = true;
        }
        else
        {
            rayDir = reflect(rayDir, normal);
            throughput *= F;
        }

        if (doSplit)
        {
            throughput *= 2;
            wasSplit = true;
        }


        if ((h.geometryInstanceFlags & GEOM_INST_FLAG_REFL_REFR_ALBEDO_MULT) != 0)
        {
            throughput *= h.albedo;
        }
        else if ((h.geometryInstanceFlags & GEOM_INST_FLAG_REFL_REFR_ALBEDO_ADD) != 0)
        {
            throughput += h.albedo;
        }


        currentPayload = traceReflectionRefractionRay(rayOrigin, rayDir, instIndex, h.geometryInstanceFlags, doRefraction);
        rayStatsAdd(RAY_STATS_CATEGORY_REFLECTION_REFRACTION, 1);

        
        if (!doesPayloadContainHitInfo(currentPayload))
        {
            throughput *= getMediaTransmittance(currentRayMedia, pow(abs(dot(rayDir, globalUniform.worldUpVector.xyz)), -3));

            // add the fog over this (missed, sky) segment to the accumulated fog
            uvec4 q2SegFog1, q2SegFog2;
            q2FindFogVolumes(rayOrigin, rayDir, 0.0, 1e6, q2SegFog1, q2SegFog2);
            q2FogAccum = q2AlphaBlendPremultiplied(q2FogAccum, q2SegmentFog(q2SegFog1, q2SegFog2, 1e6));

            storeSky(pix, rayDir, true, throughput, wasSplit, q2FogAccum);
            return;  
        }

        float rayLen;
        float emis;

        h = getHitInfoWithRayCone_ReflectionRefraction(
            currentPayload, rayCone, 
            rayOrigin, rayDir, cameraRayDir, 
            virtualPos, 
            rayLen, 
            motionCurToPrev, motionDepthLinearCurToPrev,
            emis
        );

        // Accumulate the fog along this reflection/refraction segment
        uvec4 q2SegFog1, q2SegFog2;
        q2FindFogVolumes(rayOrigin, rayDir, 0.0, rayLen, q2SegFog1, q2SegFog2);
        q2FogAccum = q2AlphaBlendPremultiplied(q2FogAccum, q2SegmentFog(q2SegFog1, q2SegFog2, rayLen));

        hitInfoWasOverwritten = true;
        throughput *= getMediaTransmittance(currentRayMedia, rayLen);
        propagateRayCone(rayCone, rayLen);
        fullPathLength += rayLen;
        q2LastSegmentLen = rayLen;
        prevHitPosition = h.hitPosition;
        screenEmission += h.albedo * emis * throughput;
        acidFog += getGlowingMediaFog(currentRayMedia, rayLen) * (doSplit ? 2.0 : 1.0);
    }


    if (!hitInfoWasOverwritten)
    {
        return;
    }


    imageStore(framebufIsSky,               pix, ivec4(0));
    imageStore(framebufAlbedo,              getRegularPixFromCheckerboardPix(pix), vec4(h.albedo, 0.0));
    imageStore(framebufScreenEmisRT,        getRegularPixFromCheckerboardPix(pix), vec4(screenEmission + ( globalUniform.cameraMediaType != MEDIA_TYPE_ACID ? acidFog * 0.05 : vec3( 0.0 ) ), 0.0));
    imageStore(framebufAcidFogRT,           getRegularPixFromCheckerboardPix(pix), vec4(acidFog, 0));
    imageStoreNormal(                       pix, h.normal);
    imageStoreNormalGeometry(               pix, h.normalGeom);
    imageStore(framebufMetallicRoughness,   pix, vec4(h.metallic, h.roughness, 0, 0));
    imageStore(framebufDepthWorld,          pix, vec4(fullPathLength));
    imageStore(framebufMotion,              pix, vec4(motionCurToPrev, motionDepthLinearCurToPrev, 0.0));
    imageStore(framebufSurfacePosition,     pix, vec4(h.hitPosition, uintBitsToFloat(h.instCustomIndex)));
    imageStore(framebufVisibilityBuffer,    pix, packVisibilityBuffer(currentPayload));
    imageStore(framebufViewDirection,       pix, vec4(rayDir, 0.0));
    imageStore(framebufThroughput,          pix, vec4(throughput, wasSplit ? 1.0 : -1.0));

    // Q2RTX-style G-buffer. Negative depth so the ASVGF filters don't bleed
    // across reflection/refraction boundaries; the half-cone angle for the
    // accumulated-cone LOD is taken from the primary pass (Q2RTX convention).
    const float q2HalfConeAngle = texelFetch(framebufQ2BounceThroughput_Sampler, pix, 0).w;
    storeQ2GBuffer(pix, h.albedo, mix(0.04, 1.0, h.metallic), h.metallic, h.roughness,
                   -fullPathLength, q2HalfConeAngle, q2LastSegmentLen,
                   vec3(0.0), 0.0, q2FogAccum, h.cluster);
}
#endif


#ifdef Q2_REFL_REFR_SHADER
// Q2RTX-style reflection/refraction pass (separate raygen, used on the Q2 core
// path). Ported from Q2RTX reflect_refract.rgen and adapted to the vkpt
// framework:
//   - material kinds come from the vkpt geometry instance flags for now (they
//     will switch to the Q2RTX .mat kinds together with the material system
//     port); screens/security cameras do not exist in Quake 1 and are skipped,
//   - the ray is traced with the vkpt payload/trace helper and the hit surface
//     is evaluated with the vkpt material code,
//   - the result is written to BOTH the vkpt G-buffer and the Q2RTX-style
//     G-buffer (negative view depth for reflections/refractions).
void main()
{
    if (globalUniform.reflectRefractMaxDepth == 0)
    {
        return;
    }

    const ivec2 regularPix = ivec2(gl_LaunchIDEXT.xy);
    const ivec2 pix = getCheckerboardPix(regularPix);
    const vec2 inUV = getPixelUVWithJitter(regularPix);
    const vec3 cameraRayDir = getRayDir(inUV);

    if (isSkyPix(pix))
    {
        return;
    }

    // restore state from primary shader
    const uvec3 primaryToReflRefrBuf = texelFetch(framebufPrimaryToReflRefr_Sampler, pix, 0).rgb;
    ShHitInfo h;
    h.albedo                            = texelFetch(framebufAlbedo_Sampler, getRegularPixFromCheckerboardPix(pix), 0).rgb;
    h.hitPosition                       = texelFetch(framebufSurfacePosition_Sampler, pix, 0).xyz;
    h.geometryInstanceFlags             = primaryToReflRefrBuf.r;
    h.portalIndex                       = primaryToReflRefrBuf.b;
    h.normalGeom                        = texelFetchNormalGeometry(pix);
    h.normal                            = texelFetchNormal(pix);
    h.metallic                          = texelFetch(framebufMetallicRoughness_Sampler, pix, 0).r;
    h.roughness                         = texelFetch(framebufMetallicRoughness_Sampler, pix, 0).g;
    const vec3  motionBuf               = texelFetch(framebufMotion_Sampler, pix, 0).rgb;
    vec2        motionCurToPrev         = motionBuf.rg;
    float       motionDepthLinearCurToPrev = motionBuf.b;
    const float firstHitDepthLinear     = texelFetch(framebufDepthWorld_Sampler, pix, 0).r;
    vec3        screenEmission          = texelFetch(framebufScreenEmisRT_Sampler, getRegularPixFromCheckerboardPix(pix), 0).rgb;
    vec3        acidFog                 = texelFetch(framebufAcidFogRT_Sampler, getRegularPixFromCheckerboardPix(pix), 0).rgb;
    vec3        throughput              = texelFetch(framebufThroughput_Sampler, pix, 0).rgb;
    ShPayload currentPayload;
    currentPayload.instIdAndIndex       = primaryToReflRefrBuf.g;

    // Q2RTX-style G-buffer from the primary pass
    const vec4 q2BaseColor              = texelFetch(framebufQ2BaseColor_Sampler, pix, 0);
    const float q2HalfConeAngle         = texelFetch(framebufQ2BounceThroughput_Sampler, pix, 0).w;
    vec4 q2Transparent                  = texelFetch(framebufQ2Transparent_Sampler, pix, 0);
    vec4 q2FogAccum                     = texelFetch(framebufQ2FogAccum_Sampler, pix, 0);

    RayCone rayCone;
    rayCone.width = 0;
    rayCone.spreadAngle = globalUniform.cameraRayConeSpreadAngle;

    float fullPathLength = firstHitDepthLinear;
    float q2LastSegmentLen = 0.0;
    vec3 prevHitPosition = h.hitPosition;
    bool wasSplit = false;
    bool wasPortal = false;
    vec3 virtualPos = h.hitPosition;
    vec3 rayDir = cameraRayDir;
    uint currentRayMedia = globalUniform.cameraMediaType;
    bool hitInfoWasOverwritten = false;

    propagateRayCone(rayCone, firstHitDepthLinear);

    for (int i = 0; i < globalUniform.reflectRefractMaxDepth; i++)
    {
        const uint instIndex = unpackInstanceIdAndCustomIndex(currentPayload.instIdAndIndex).y;
        bool isPixOdd = isCheckerboardPixOdd(pix) != 0;

        uint newRayMedia = getNewRayMedia(i, currentRayMedia, h.geometryInstanceFlags);
        bool isPortal = isPortalFromFlags(h.geometryInstanceFlags) && h.portalIndex != PORTAL_INDEX_NONE;

        // Material kinds, Q2RTX-style. vkpt flags for now; will switch to the
        // Q2RTX .mat kinds together with the material system port (phase 4.5).
        // (primary_is_chrome: explicit REFLECT flag; smooth surfaces reflect
        // too, like the legacy pass, to avoid losing glossy reflections.)
        const bool primaryIsWater  = (h.geometryInstanceFlags & GEOM_INST_FLAG_MEDIA_TYPE_WATER) != 0;
        const bool primaryIsSlime  = (h.geometryInstanceFlags & GEOM_INST_FLAG_MEDIA_TYPE_ACID) != 0;
        const bool primaryIsGlass  = (h.geometryInstanceFlags & GEOM_INST_FLAG_MEDIA_TYPE_GLASS) != 0;
        const bool primaryIsChrome = (h.geometryInstanceFlags & GEOM_INST_FLAG_REFLECT) != 0;
        const bool primaryIsSmooth = h.roughness < globalUniform.minRoughness;

        if (!primaryIsWater && !primaryIsSlime && !primaryIsGlass && !primaryIsChrome && !primaryIsSmooth && !isPortal)
        {
            break;
        }

        const vec3 normal = getNormal(h.hitPosition, h.normal, h.normalGeom, rayCone, rayDir,
                                      !isPortal && (newRayMedia == MEDIA_TYPE_WATER || currentRayMedia == MEDIA_TYPE_WATER ||
                                                    newRayMedia == MEDIA_TYPE_ACID || currentRayMedia == MEDIA_TYPE_ACID),
                                      wasPortal);

        vec3 rayOrigin = h.hitPosition;
        bool doSplit = !wasSplit;
        bool doRefraction = false;
        int correctMotionVector = 0; // 1 = reflection, 2 = refraction

        if (isPortal)
        {
            const ShPortalInstance portal = g_portals[h.portalIndex];

            const vec3 inCenter = portal.inPosition.xyz;
            const vec3 inWorldOffset = h.hitPosition - inCenter;

            mat3 inLookAt = lookAt(getPortalNormal(normal, inWorldOffset), globalUniform.worldUpVector.xyz);

            const vec3 outCenter = portal.outPosition.xyz;
            const mat3 outLookAt = lookAt(portal.outDirection.xyz,
                                          portal.outUp.xyz);

            // to local space; then to world space but at portal output
            rayDir = outLookAt * (transpose(inLookAt) * rayDir);

            const vec2 localOffset = vec2(dot(inWorldOffset, inLookAt[0]),
                                          dot(inWorldOffset, inLookAt[1]));

            rayOrigin = outCenter + localOffset.x * outLookAt[0] + localOffset.y * outLookAt[1];

            wasPortal = true;
            doSplit = false;
        }
        else if (primaryIsWater || primaryIsSlime)
        {
            // Q2RTX water/slime: IOR 1.34, Fresnel, checkerboard split.
            const float ior = getIndexOfRefraction(primaryIsWater ? MEDIA_TYPE_WATER : MEDIA_TYPE_ACID);
            const vec3 reflected = reflect(rayDir, normal);
            const float nDotV = abs(dot(rayDir, normal));

            if (currentRayMedia == MEDIA_TYPE_WATER || currentRayMedia == MEDIA_TYPE_ACID)
            {
                // Looking up from under water/slime: adjusted N.V, TIR.
                const vec3 refracted = refract(rayDir, normal, ior);
                float ndv = 1.0 - (1.0 - nDotV) * 3.0;
                if (ndv <= 0.0 || dot(refracted, refracted) == 0.0)
                {
                    // Total internal reflection - single ray
                    rayDir = reflected;
                    correctMotionVector = 1;
                }
                else
                {
                    const float f = pow(1.0 - ndv, 5.0);
                    doRefraction = (i == 0) ? isPixOdd : isPixOdd;
                    if (doRefraction)
                    {
                        rayDir = refracted;
                        throughput *= (1.0 - f);
                        currentRayMedia = MEDIA_TYPE_VACUUM;
                        correctMotionVector = 2;
                    }
                    else
                    {
                        rayDir = reflected;
                        throughput *= f;
                        correctMotionVector = 1;
                    }
                    if (doSplit)
                    {
                        throughput *= 2.0;
                    }
                }
            }
            else
            {
                // Looking down on the water/slime surface
                const vec3 refracted = refract(rayDir, normal, 1.0 / ior);
                const float F = 0.1 + 0.9 * pow(1.0 - nDotV, 5.0);
                doSplit = (i == 0);
                doRefraction = isPixOdd;
                if (doRefraction)
                {
                    rayDir = refracted;
                    throughput *= (1.0 - F);
                    currentRayMedia = newRayMedia; // enter water/slime
                    correctMotionVector = 2;
                }
                else
                {
                    rayDir = reflected;
                    throughput *= F;
                    correctMotionVector = 1;
                }
                if (doSplit)
                {
                    throughput *= 2.0;
                }
            }
        }
        else if (primaryIsGlass)
        {
            // Q2RTX glass: IOR 1.52, thin glass (dual refraction), split.
            const float ior = getIndexOfRefraction(MEDIA_TYPE_GLASS);
            vec3 glassGeomN = h.normalGeom;
            vec3 glassN = normal;

            float gnDotV = dot(rayDir, glassGeomN);
            if (gnDotV > 0)
            {
                glassGeomN = -glassGeomN;
                glassN = -glassN;
                gnDotV = -gnDotV;
            }

            const float nDotV = dot(rayDir, -glassN);
            const vec3 reflected = reflect(rayDir, glassN);
            float F = 0.05 + 0.95 * pow(1.0 - abs(nDotV), 5.0);
            if (dot(reflected, glassGeomN) < 0.01)
            {
                F = 0.0;
            }

            doSplit = (i == 0) && (F > 0.0);
            doRefraction = isPixOdd;
            if (doRefraction)
            {
                // infinitely thin glass: dual refraction (in via normal, out via flat geo normal)
                const vec3 refr1 = refract(rayDir, glassN, 1.0 / ior);
                const vec3 refr2 = refract(refr1, glassGeomN, ior);
                if (length(refr2) > 0.0)
                {
                    rayDir = refr2;
                }
                throughput *= (1.0 - F);
                throughput *= q2BaseColor.rgb;
                currentRayMedia = MEDIA_TYPE_VACUUM; // Q2RTX thin glass: no media change
                correctMotionVector = 2;
            }
            else
            {
                rayDir = reflected;
                throughput *= F;
                correctMotionVector = 1;
            }

            if (abs(dot(glassN, glassGeomN)) < 0.99999)
            {
                correctMotionVector = 0;
            }

            if (doSplit)
            {
                throughput *= 2.0;
            }
        }
        else
        {
            // chrome / smooth surface: reflection
            if (primaryIsChrome)
            {
                throughput *= h.albedo;
            }
            rayDir = reflect(rayDir, normal);
            correctMotionVector = 1;
        }

        if (doSplit)
        {
            wasSplit = true;
        }

        currentPayload = traceReflectionRefractionRay(rayOrigin, rayDir, instIndex, h.geometryInstanceFlags, doRefraction);
        rayStatsAdd(RAY_STATS_CATEGORY_REFLECTION_REFRACTION, 1);

        if (!doesPayloadContainHitInfo(currentPayload))
        {
            // Reflection/refraction ray hit the sky: store an empty surface,
            // blend the environment into the accumulated transparency and use
            // negative depth (Q2RTX convention). Prefilter the env by the
            // surface roughness so rough reflections can't catch the sun disc
            // as a firefly; sharp surfaces (chrome/glass) keep LOD ~0.
            const vec3 env = getSkyFiltered(rayDir, h.roughness * (SKY_MIP_COUNT - 1.0));
            q2Transparent = q2AlphaBlendPremultiplied(vec4(env * throughput, 1.0), q2Transparent);

            uvec4 q2SegFog1, q2SegFog2;
            q2FindFogVolumes(rayOrigin, rayDir, 0.0, 1e6, q2SegFog1, q2SegFog2);
            q2FogAccum = q2AlphaBlendPremultiplied(q2SegmentFog(q2SegFog1, q2SegFog2, 1e6), q2FogAccum);

            storeSky(pix, rayDir, true, throughput, wasSplit, q2FogAccum);
            // override the Q2RTX-style G-buffer: empty surface, negative depth,
            // environment blended into transparent (Q2RTX reflect_refract sky).
            storeQ2GBuffer(pix, vec3(0.0), 0.0, 0.0, 1.0, -MAX_RAY_LENGTH * 2.0, q2HalfConeAngle, MAX_RAY_LENGTH * 2.0,
                           q2Transparent.rgb, q2Transparent.a, q2FogAccum, ~0u);
            return;
        }

        float rayLen;
        float emis;

        h = getHitInfoWithRayCone_ReflectionRefraction(
            currentPayload, rayCone,
            rayOrigin, rayDir, cameraRayDir,
            virtualPos,
            rayLen,
            motionCurToPrev, motionDepthLinearCurToPrev,
            emis
        );

        // Accumulate the fog along this reflection/refraction segment
        uvec4 q2SegFog1, q2SegFog2;
        q2FindFogVolumes(rayOrigin, rayDir, 0.0, rayLen, q2SegFog1, q2SegFog2);
        q2FogAccum = q2AlphaBlendPremultiplied(q2SegmentFog(q2SegFog1, q2SegFog2, rayLen), q2FogAccum);

        hitInfoWasOverwritten = true;
        throughput *= getMediaTransmittance(currentRayMedia, rayLen);
        propagateRayCone(rayCone, rayLen);
        fullPathLength += rayLen;
        q2LastSegmentLen = rayLen;
        prevHitPosition = h.hitPosition;
        screenEmission += h.albedo * emis * throughput;
        acidFog += getGlowingMediaFog(currentRayMedia, rayLen) * (doSplit ? 2.0 : 1.0);
    }


    if (!hitInfoWasOverwritten)
    {
        return;
    }

    // Store the vkpt G-buffer (same as the legacy reflection pass)
    imageStore(framebufIsSky,               pix, ivec4(0));
    imageStore(framebufAlbedo,              getRegularPixFromCheckerboardPix(pix), vec4(h.albedo, 0.0));
    imageStore(framebufScreenEmisRT,        getRegularPixFromCheckerboardPix(pix), vec4(screenEmission + ( globalUniform.cameraMediaType != MEDIA_TYPE_ACID ? acidFog * 0.05 : vec3( 0.0 ) ), 0.0));
    imageStore(framebufAcidFogRT,           getRegularPixFromCheckerboardPix(pix), vec4(acidFog, 0));
    imageStoreNormal(                       pix, h.normal);
    imageStoreNormalGeometry(               pix, h.normalGeom);
    imageStore(framebufMetallicRoughness,   pix, vec4(h.metallic, h.roughness, 0, 0));
    imageStore(framebufDepthWorld,          pix, vec4(fullPathLength));
    imageStore(framebufMotion,              pix, vec4(motionCurToPrev, motionDepthLinearCurToPrev, 0.0));
    imageStore(framebufSurfacePosition,     pix, vec4(h.hitPosition, uintBitsToFloat(h.instCustomIndex)));
    imageStore(framebufVisibilityBuffer,    pix, packVisibilityBuffer(currentPayload));
    imageStore(framebufViewDirection,       pix, vec4(rayDir, 0.0));
    imageStore(framebufThroughput,          pix, vec4(throughput, wasSplit ? 1.0 : -1.0));

    // Q2RTX-style G-buffer. Negative depth so the ASVGF filters don't bleed
    // across reflection/refraction boundaries.
    storeQ2GBuffer(pix, h.albedo, mix(0.04, 1.0, h.metallic), h.metallic, h.roughness,
                   -fullPathLength, q2HalfConeAngle, q2LastSegmentLen,
                   q2Transparent.rgb, q2Transparent.a, q2FogAccum, h.cluster);
}
#endif