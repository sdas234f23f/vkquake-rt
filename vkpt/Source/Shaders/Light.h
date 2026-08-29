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

#ifndef LIGHT_H_
#define LIGHT_H_

struct DirectionalLight
{
    vec3 direction;
    float angularRadius;
    vec3 color;
};

struct SphereLight
{
    vec3 center;
    float radius;
    vec3 color;
    // Emission normal (one-sided light-textured surfaces). (0,0,0) = full sphere.
    vec3 normal;
};

struct TriangleLight
{
    vec3 position[3];
    vec3 normal;
    float area;
    vec3 color;
};

#define MAX_TEXTURED_AREA_LIGHT_VERTS 8

// A receiver within this distance of the light polygon's plane is treated as
// coplanar with the emitter -> its contribution is culled. The light polygon
// lies exactly on the emitter's brush face, so receivers on the same plane ARE
// the emitter surface itself (or a coplanar neighbour face). Their true
// contribution is zero (grazing-angle geometry factor), but float error /
// affine-fit slop leaves a tiny nonzero dot(normal, lightToSurf) that explodes
// when the random sample lands close to the receiver -> the pixel-noise right
// in the middle of the light source.
#define TAL_SELF_ILLUMINATION_PLANE_EPS 0.05

// Capped rejection sampling for the luma mask: a uniform polygon sample is
// accepted with probability = mask, so accepted points are distributed
// proportional to the emissivity (importance sampling). The cap bounds the
// warp cost on very sparse masks; on exhaustion the exact uniform-mask
// estimator is used as an unbiased fallback (0.94^96 ~ 0.3% for a 6% mask).
#define TAL_MASK_REJECTION_TRIES 96

struct TexturedAreaLight
{
    // Affine map world = A*s + B*t + C (s,t = raw texture coords). The UV
    // polygon below maps through it to a world polygon that lies exactly on
    // the brush face geometry.
    vec3 A;
    vec3 B;
    vec3 C;
    vec3 normal;
    // Exact world-space polygon area.
    float area;
    // Emission (luma) texture index, packed as float bits. floatBitsToUint.
    float textureIndex;
    // Average emissivity of the luma mask over the UV polygon.
    float meanEmiss;
    int numVerts;
    // Convex polygon vertices in texture (S,T) space, in face vertex order.
    vec2 uvVerts[MAX_TEXTURED_AREA_LIGHT_VERTS];
    vec3 color;
};

struct SpotLight
{
    vec3 center;
    float radius;
    vec3 direction;
    float cosAngleInner;
    vec3 color;
    float cosAngleOuter;
};

DirectionalLight decodeAsDirectionalLight(const ShLightEncoded encoded)
{
    DirectionalLight l;
    l.direction = encoded.data_0.xyz;
    l.angularRadius = encoded.data_0.w;
    l.color = encoded.color;

    return l;
}

SphereLight decodeAsSphereLight(const ShLightEncoded encoded)
{
    SphereLight l;
    l.center = encoded.data_0.xyz;
    l.radius = encoded.data_0.w;
    l.color = encoded.color;
    l.normal = encoded.data_1.xyz;

    return l;
}

TriangleLight decodeAsTriangleLight(const ShLightEncoded encoded)
{
    TriangleLight l;
    l.position[0] = encoded.data_0.xyz;
    l.position[1] = encoded.data_1.xyz;
    l.position[2] = encoded.data_2.xyz;
    l.color = encoded.color;

    l.normal = vec3(
        encoded.data_0.w, 
        encoded.data_1.w, 
        encoded.data_2.w
    );
    // len is guaranteed to be > 0.0
    float len = length(l.normal);
    l.normal /= len;
    l.area = len * 0.5;

    return l;
}

TexturedAreaLight decodeAsTexturedAreaLight(const ShLightEncoded encoded)
{
    TexturedAreaLight l;
    l.A = encoded.data_0.xyz;
    l.textureIndex = encoded.data_0.w;
    l.B = encoded.data_1.xyz;
    l.meanEmiss = encoded.data_1.w;
    l.C = encoded.data_2.xyz;
    l.numVerts = clamp(int(encoded.data_2.w), 0, MAX_TEXTURED_AREA_LIGHT_VERTS);
    l.uvVerts[0] = encoded.data_3.xy;
    l.uvVerts[1] = encoded.data_3.zw;
    l.uvVerts[2] = encoded.data_4.xy;
    l.uvVerts[3] = encoded.data_4.zw;
    l.uvVerts[4] = encoded.data_5.xy;
    l.uvVerts[5] = encoded.data_5.zw;
    l.uvVerts[6] = encoded.data_6.xy;
    l.uvVerts[7] = encoded.data_6.zw;
    l.normal = encoded.data_7.xyz;
    l.area = encoded.data_7.w;
    l.color = encoded.color;

    return l;
}

SpotLight decodeAsSpotLight(const ShLightEncoded encoded)
{
    SpotLight l;
    l.center = encoded.data_0.xyz;
    l.radius = encoded.data_0.w;
    l.direction = encoded.data_1.xyz;
    l.color = encoded.color;
    l.cosAngleInner = encoded.data_2.x;
    l.cosAngleOuter = encoded.data_2.y;
    
    return l;
}

float getPolySpotFactor(const vec3 lightNormal, const vec3 lightToSurf)
{
    float ll = max(dot(lightNormal, lightToSurf), 0.0);
    return pow(ll, globalUniform.polyLightSpotlightFactor);
}

float getSpotFactor(float cosA, float cosAInner, float cosAOuter)
{
    return square(smoothstep(cosAOuter, cosAInner, cosA));
}

float isSphereInFront(const vec3 planeNormal, const vec3 planePos, const vec3 sphereCenter, float sphereRadius)
{
    return float(dot(planeNormal, sphereCenter - planePos) > -sphereRadius);
}



// Veach, E. Robust Monte Carlo Methods for Light Transport Simulation
// The change of variables from solid angle measure to area integration measure
// Note: but without |dot(surfNormal, surfaceToLight)|
float getGeometryFactor(const vec3 lightNormal, const vec3 lightToSurface, float surfaceToLightDistance)
{
    return abs(dot(lightNormal, lightToSurface)) / square(surfaceToLightDistance);
}
float getGeometryFactorClamped(const vec3 lightNormal, const vec3 lightToSurface, float surfaceToLightDistance)
{
    return max(0.0, dot(lightNormal, lightToSurface)) * safePositiveRcp(square(surfaceToLightDistance));
}

float safeSolidAngle(float a)
{
    return a > 0.0 && !isnan(a) && !isinf(a) ? clamp(a, 0.0, 4.0 * M_PI) : 0.0;
}

float calcSolidAngleForSphere(float sphereRadius, float distanceToSphereCenter)
{
    // solid angle here is the spherical cap area on a unit sphere
    float sinTheta = sphereRadius / max(sphereRadius, distanceToSphereCenter);
    float cosTheta = sqrt(1.0 - sinTheta * sinTheta);
    return safeSolidAngle(2 * M_PI * (1.0 - cosTheta));
}

float calcSolidAngleForArea(float area, const vec3 areaPosition, const vec3 areaNormal, const vec3 surfPosition)
{
    const DirectionAndLength areaLightToSurf = calcDirectionAndLength(areaPosition, surfPosition);
    // from area measure to solid angle measure
    return safeSolidAngle(area * getGeometryFactor(areaNormal, areaLightToSurf.dir, areaLightToSurf.len));
}



float getLightColorWeight(const vec3 color)
{
    return clamp(getLuminance(color) * 0.1 + 0.9, 1.0, 10.0);
}

float getDirectionalLightWeight(const SphereLight l, const vec3 cellCenter, float cellRadius)
{
    return 
        getLightColorWeight(l.color);
}

float getSphereLightWeight(const SphereLight l, const vec3 cellCenter, float cellRadius)
{
    return 
        getLightColorWeight(l.color) * 
        calcSolidAngleForSphere(l.radius, max(length(l.center - cellCenter), cellRadius));
}

float getTriangleLightWeight(const TriangleLight l, const vec3 cellCenter, float cellRadius)
{
    const vec3 triCenter = 
        l.position[0] / 3.0 +
        l.position[1] / 3.0 +
        l.position[2] / 3.0;

    const float aprxTriRadius = 
        length(l.position[0] - triCenter) / 3.0 +
        length(l.position[1] - triCenter) / 3.0 +
        length(l.position[2] - triCenter) / 3.0;

    return 
        getLightColorWeight(l.color) * 
        calcSolidAngleForSphere(aprxTriRadius, max(length(triCenter - cellCenter), cellRadius)) *
        isSphereInFront(l.normal, triCenter, cellCenter, cellRadius);
}

vec3 texturedAreaLightWorldPos(const TexturedAreaLight l, const vec2 uv)
{
    return l.C + l.A * uv.x + l.B * uv.y;
}

// World-space center of the polygon (average of the UV verts mapped to world).
vec3 getTexturedAreaLightCenter(const TexturedAreaLight l)
{
    vec2 uvCenter = vec2(0.0);
    for (int i = 0; i < l.numVerts; i++)
    {
        uvCenter += l.uvVerts[i];
    }
    uvCenter /= max(float(l.numVerts), 1.0);
    return texturedAreaLightWorldPos(l, uvCenter);
}

// Sample a point uniformly inside a convex polygon given by its UV verts. The
// polygon is fanned from vert 0; u1 selects a fan triangle by its area CDF and
// u2 warps a uniform square inside it (same warp as sampleTriangle). Uniform in
// UV space -> uniform in world through the affine map (constant Jacobian), so
// the area pdf is exactly 1 / worldArea.
vec2 sampleConvexPolygon(const vec2 verts[MAX_TEXTURED_AREA_LIGHT_VERTS], int numVerts, float u1, float u2)
{
    if (numVerts < 3)
    {
        return verts[0];
    }

    float triArea[MAX_TEXTURED_AREA_LIGHT_VERTS - 2];
    float totalArea = 0.0;
    for (int i = 0; i < numVerts - 2; i++)
    {
        const vec2 e1 = verts[i + 1] - verts[0];
        const vec2 e2 = verts[i + 2] - verts[0];
        triArea[i] = 0.5 * abs(e1.x * e2.y - e1.y * e2.x);
        totalArea += triArea[i];
    }
    totalArea = max(totalArea, 1e-8);

    // select the fan triangle by area CDF
    float r = u1 * totalArea;
    int t = numVerts - 3;
    float acc = 0.0;
    for (int i = 0; i < numVerts - 2; i++)
    {
        acc += triArea[i];
        if (r <= acc)
        {
            t = i;
            break;
        }
    }

    // re-map r into [0,1] inside the selected triangle
    const float accBefore = acc - triArea[t];
    const float uTri = clamp((r - accBefore) / max(triArea[t], 1e-8), 0.0, 1.0);

    // square-to-triangle warp (same as sampleTriangle)
    const float beta  = 1.0 - sqrt(uTri);
    const float gamma = (1.0 - beta) * u2;
    const float alpha = 1.0 - beta - gamma;

    return alpha * verts[0] + beta * verts[t + 1] + gamma * verts[t + 2];
}

float getTexturedAreaLightWeight(const TexturedAreaLight l, const vec3 cellCenter, float cellRadius)
{
    const vec3 center = getTexturedAreaLightCenter(l);

    // Bounding radius of the polygon for solid-angle weighting.
    float aprxRadius = 0.0;
    for (int i = 0; i < l.numVerts; i++)
    {
        aprxRadius = max(aprxRadius, length(texturedAreaLightWorldPos(l, l.uvVerts[i]) - center));
    }

    return 
        getLightColorWeight(l.color) * l.meanEmiss *
        calcSolidAngleForSphere(aprxRadius, max(length(center - cellCenter), cellRadius)) *
        isSphereInFront(l.normal, center, cellCenter, cellRadius);
}

float getSpotLightWeight(const SpotLight l, const vec3 cellCenter, float cellRadius)
{
    return 
        getLightColorWeight(l.color) * 
        calcSolidAngleForSphere(l.radius, max(length(l.center - cellCenter), cellRadius)) *
        isSphereInFront(l.direction, l.center, cellCenter, cellRadius);
}




struct LightSample
{
    vec3 position;
    vec3 color;
    float dw;
};

LightSample emptyLightSample()
{
    LightSample r;
    r.position = vec3(0);
    r.color = vec3(0);
    r.dw = 0;
    return r;
}

LightSample sampleDirectionalLight(const DirectionalLight l, const vec3 surfPosition, const vec2 pointRnd)
{
    vec3 lightNormal;
    {
        const float diskRadiusAtUnit = sin(max(0.01, l.angularRadius));
        const vec2 disk = sampleDisk(diskRadiusAtUnit, pointRnd.x, pointRnd.y);
        const mat3 basis = getONB(l.direction);

        lightNormal = normalize(l.direction + basis[0] * disk.x + basis[1] * disk.y);
    }

    LightSample r;
    r.position = surfPosition - lightNormal * MAX_RAY_LENGTH;
    r.color = l.color;
    r.dw = 1.0;
    
    return r;
}

LightSample sampleSphereLight(const SphereLight l, const vec3 surfPosition, const vec2 pointRnd)
{
    const DirectionAndLength toLightCenter = calcDirectionAndLength(surfPosition, l.center);

    // sample hemisphere visible to the surface point
    float ltHsOneOverPdf;
    const vec3 lightNormal = sampleOrientedHemisphere(-toLightCenter.dir, pointRnd.x, pointRnd.y, ltHsOneOverPdf);

    LightSample r;
    r.position = l.center + lightNormal * l.radius;
    r.color = l.color;
    r.dw = calcSolidAngleForSphere(l.radius, toLightCenter.len);

    return r;
}

LightSample sampleTriangleLight(const TriangleLight l, const vec3 surfPosition, const vec2 pointRnd)
{
    LightSample r;
    r.position = sampleTriangle(l.position[0], l.position[1], l.position[2], pointRnd.x, pointRnd.y);
    r.color = l.color * getPolySpotFactor(l.normal, normalize(surfPosition - r.position));
    r.dw = calcSolidAngleForArea(l.area, r.position, l.normal, surfPosition);

    return r;
}

LightSample sampleTexturedAreaLight(const TexturedAreaLight l, const vec3 surfPosition, const vec2 pointRnd)
{
    LightSample r;

    const uint textureIndex = floatBitsToUint(l.textureIndex);

    // Cull receivers coplanar with the emitter (self-illumination noise); does
    // not depend on the sampled point, so it runs before the sampling loop.
    if (abs(dot(l.normal, surfPosition - getTexturedAreaLightCenter(l))) < TAL_SELF_ILLUMINATION_PLANE_EPS)
    {
        return emptyLightSample();
    }

    vec2 uv = sampleConvexPolygon(l.uvVerts, l.numVerts, pointRnd.x, pointRnd.y);
    float mask = 1.0;
    bool accepted = true;

    if (textureIndex != 0u)
    {
        // Luma importance sampling: choose the point proportional to the mask
        // by rejection sampling, so nearly every sample lands on a lit texel
        // instead of wasting most rays on the black parts of a sparse mask.
        // The acceptance probability is the mask itself, so the density of
        // accepted points is mask / M with M = meanEmiss * area (the polygon
        // covers the whole texture); the mask then cancels out of the estimate
        // and each accepted sample contributes the full radiance scaled by M.
        // On cap exhaustion the exact uniform-mask estimator below is used as
        // an unbiased (if higher-variance) fallback. The retry stream is
        // derived from the point RNG, so gradient-reprojected pixels retrace
        // the same accepted point as the previous frame.
        const uint maskSeed = wellonsLowBias32(
            floatBitsToUint(pointRnd.x) ^ floatBitsToUint(pointRnd.y));

        accepted = false;
        for (int tryIdx = 0; tryIdx < TAL_MASK_REJECTION_TRIES; tryIdx++)
        {
            const uint salt = uint(tryIdx) * 3u;
            const vec2 tryRnd = vec2(
                rnd16(maskSeed, salt),
                rnd16(maskSeed, salt + 1u));
            uv = sampleConvexPolygon(l.uvVerts, l.numVerts, tryRnd.x, tryRnd.y);
            // Explicit mip 0: implicit texture() derivatives in a raygen shader
            // are quad-group differences that go wild when adjacent rays hit
            // different surfaces, so the mask gets sampled at a random/blurred
            // mip level -> the whole texture seems to emit and the LOD jumps
            // frame to frame (flicker). The primary pass reads this same RME
            // with ray-cone grads; here a fixed texel-aligned read keeps the
            // mask identical to the luma debug overlay.
            mask = getTextureSampleLod(textureIndex, uv, 0.0).b;
            if (rnd16(maskSeed, salt + 2u) < mask)
            {
                accepted = true;
                break;
            }
        }
    }

    r.position = texturedAreaLightWorldPos(l, uv);

    const DirectionAndLength lightToSurf = calcDirectionAndLength(r.position, surfPosition);

    if (accepted)
    {
        // Accepted: the accepted-point density mask/(meanEmiss*area) cancels
        // the mask, so the contribution is full radiance scaled by the mean
        // emissivity. Same expected value as the uniform-mask estimator for a
        // full-coverage polygon (E[mask] = meanEmiss), but with (near) zero
        // variance on the mask itself.
        r.color = l.color;
        r.dw = safeSolidAngle(l.meanEmiss * l.area * getGeometryFactorClamped(l.normal, lightToSurf.dir, lightToSurf.len));
    }
    else
    {
        // Fallback (cap reached): exact uniform-mask estimator, mask applied
        // directly. Unbiased, just higher variance.
        r.color = l.color * mask;
        r.dw = safeSolidAngle(l.area * getGeometryFactorClamped(l.normal, lightToSurf.dir, lightToSurf.len));
    }

    return r;
}

LightSample sampleSpotLight(const SpotLight l, const vec3 surfPosition, const vec2 pointRnd)
{
    LightSample r;
    {
        const vec2 disk = sampleDisk(l.radius, pointRnd.x, pointRnd.y);
        const mat3 basis = getONB(l.direction);

        r.position = l.center + basis[0] * disk.x + basis[1] * disk.y;
    }

    const DirectionAndLength toLightCenter = calcDirectionAndLength(surfPosition, l.center);
    const float cosA = max(dot(l.direction, -toLightCenter.dir), 0.0);
    
    r.color = l.color * getSpotFactor(cosA, l.cosAngleInner, l.cosAngleOuter);
    r.dw = calcSolidAngleForSphere(l.radius, toLightCenter.len);

    return r;
}



float getLightWeight(const ShLightEncoded encoded, const vec3 cellCenter, float cellRadius)
{
    switch (encoded.lightType)
    {
        case LIGHT_TYPE_DIRECTIONAL:       return getDirectionalLightWeight    (decodeAsSphereLight          (encoded), cellCenter, cellRadius);
        case LIGHT_TYPE_SPHERE:            return getSphereLightWeight         (decodeAsSphereLight          (encoded), cellCenter, cellRadius);
        case LIGHT_TYPE_TRIANGLE:          return getTriangleLightWeight       (decodeAsTriangleLight        (encoded), cellCenter, cellRadius);
        case LIGHT_TYPE_SPOT:              return getSpotLightWeight           (decodeAsSpotLight            (encoded), cellCenter, cellRadius);
        case LIGHT_TYPE_TEXTURED_AREA:     return getTexturedAreaLightWeight   (decodeAsTexturedAreaLight    (encoded), cellCenter, cellRadius);
        default:                           return 0.0;
    }
}

LightSample sampleLight(const ShLightEncoded encoded, const vec3 surfPosition, const vec2 pointRnd)
{
    switch (encoded.lightType)
    {
        case LIGHT_TYPE_DIRECTIONAL:       return sampleDirectionalLight       (decodeAsDirectionalLight     (encoded), surfPosition, pointRnd);
        case LIGHT_TYPE_SPHERE:            return sampleSphereLight            (decodeAsSphereLight          (encoded), surfPosition, pointRnd);
        case LIGHT_TYPE_TRIANGLE:          return sampleTriangleLight          (decodeAsTriangleLight        (encoded), surfPosition, pointRnd);
        case LIGHT_TYPE_SPOT:              return sampleSpotLight              (decodeAsSpotLight            (encoded), surfPosition, pointRnd);
        case LIGHT_TYPE_TEXTURED_AREA:     return sampleTexturedAreaLight      (decodeAsTexturedAreaLight    (encoded), surfPosition, pointRnd);
        default:                           return emptyLightSample();
    }
}

#endif // LIGHT_H_
