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
//
// Ported from Q2RTX (GPL v2) shader/light_lists.h, adapted to the vkpt
// framework: the per-BSP-cluster light lists (q2LightListOffsets /
// q2LightListLights, built on the CPU from the PVS and uploaded each frame)
// are used verbatim like Q2RTX, and the Q2RTX polygon light sampling is
// replaced by vkpt's ShLightEncoded (sphere/spot/triangle).

#ifndef Q2_LIGHT_LISTS_H_
#define Q2_LIGHT_LISTS_H_

#define Q2_MAX_BRUTEFORCE_SAMPLING 8

uint q2GetClusterLightCount(const uint cluster)
{
    return q2LightListOffsets[cluster + 1] - q2LightListOffsets[cluster];
}

uint q2GetClusterLight(const uint cluster, const uint slot)
{
    return q2LightListLights[q2LightListOffsets[cluster] + slot];
}

bool q2GetIsGradient(const ivec2 pix)
{
    const uint u = texelFetch(framebufQ2GradSmplPos_Sampler, pix / Q2_GRAD_DWN, 0).r;
    if (u == 0u)
    {
        return false;
    }

    const ivec2 gradStrataPos = ivec2(
        (u >> (Q2_STRATUM_OFFSET_SHIFT * 0)) & Q2_STRATUM_OFFSET_MASK,
        (u >> (Q2_STRATUM_OFFSET_SHIFT * 1)) & Q2_STRATUM_OFFSET_MASK);

    return all(equal(gradStrataPos, pix % Q2_GRAD_DWN));
}

float q2RoughnessSquareToSpecPower(const float alpha)
{
    return max(0.01, 2.0 / (alpha * alpha + 1e-4) - 2.0);
}

uint q2GetPrimaryDirectionSide(const vec3 n)
{
    const vec3 a = abs(n);
    if (a.x >= a.y && a.x >= a.z) return n.x >= 0.0 ? 0u : 1u;
    if (a.y >= a.z)                 return n.y >= 0.0 ? 2u : 3u;
    return                             n.z >= 0.0 ? 4u : 5u;
}

uint q2GetLightStatsAddr(const uint cluster, const uint slot, const uint side)
{
    uint addr = cluster;
    addr = addr * uint(Q2_LIGHT_LIST_MAX_PER_CELL) + slot;
    addr = addr * uint(Q2_LIGHT_LIST_STATS_SIDES) + side;
    addr = addr * 2u;
    return addr;
}

void q2AccumulateLightStats(const uint cluster, const uint slot, const vec3 n, const float vis)
{
    const uint frameSlot = globalUniform.frameId % uint(Q2_LIGHT_LIST_STATS_BUFFERS);
    const uint statsFrameBase = frameSlot
        * uint(Q2_MAX_CLUSTERS) * uint(Q2_LIGHT_LIST_MAX_PER_CELL)
        * uint(Q2_LIGHT_LIST_STATS_SIDES) * 2u;
    const uint side = q2GetPrimaryDirectionSide(n);
    const uint addr = statsFrameBase + q2GetLightStatsAddr(cluster, slot, side);
    atomicAdd(q2LightStats[addr + (vis > 0.5 ? 0u : 1u)], 1u);
}

float q2Phong(vec3 n, vec3 L, vec3 V, float phongExp)
{
    return pow(max(dot(reflect(-L, n), V), 0.0), phongExp);
}

float q2SphericalTriArea(mat3 positions, vec3 p, vec3 n, vec3 V, float phongExp, float phongScale, float phongWeight)
{
    positions[0] = positions[0] - p;
    positions[1] = positions[1] - p;
    positions[2] = positions[2] - p;

    vec3 g = cross(positions[1] - positions[0], positions[2] - positions[0]);
    if (dot(n, positions[0]) <= 0 && dot(n, positions[1]) <= 0 && dot(n, positions[2]) <= 0)
        return 0;
    if (dot(g, positions[0]) >= 0 && dot(g, positions[1]) >= 0 && dot(g, positions[2]) >= 0)
        return 0;

    vec3 L = normalize(positions * vec3(1.0 / 3.0));
    float specular = q2Phong(n, L, V, phongExp) * phongScale;
    float brdf = mix(1.0, specular, phongWeight);

    vec3 A = normalize(positions[0]);
    vec3 B = normalize(positions[1]);
    vec3 C = normalize(positions[2]);

    float area = 2 * atan(abs(dot(A, cross(B, C))), 1 + dot(A, B) + dot(B, C) + dot(A, C));
    return max(area - 1e-5, 0.0) * brdf;
}

float q2LightSelectionMass(const ShLightEncoded encoded, const vec3 p, const vec3 n, const vec3 V,
                           const float phongExp, const float phongScale, const float phongWeight)
{
    if (encoded.lightType == LIGHT_TYPE_TRIANGLE)
    {
        const TriangleLight l = decodeAsTriangleLight(encoded);
        mat3 positions;
        positions[0] = l.position[0];
        positions[1] = l.position[1];
        positions[2] = l.position[2];
        return q2SphericalTriArea(positions, p, n, V, phongExp, phongScale, phongWeight);
    }
    else if (encoded.lightType == LIGHT_TYPE_TEXTURED_AREA)
    {
        const TexturedAreaLight l = decodeAsTexturedAreaLight(encoded);
        const vec3 center = getTexturedAreaLightCenter(l);
        const DirectionAndLength centerToSurf = calcDirectionAndLength(center, p);
        float sa = safeSolidAngle(l.area * getGeometryFactorClamped(l.normal, centerToSurf.dir, centerToSurf.len));
        return sa * max(l.meanEmiss, 0.0);
    }
    else
    {
        const float dist = max(length(p - encoded.data_0.xyz), encoded.data_0.w);
        float sa = calcSolidAngleForSphere(encoded.data_0.w, dist);

        if (encoded.lightType == LIGHT_TYPE_SPOT)
        {
            const SpotLight l = decodeAsSpotLight(encoded);
            const vec3 toLight = normalize(encoded.data_0.xyz - p);
            sa *= getSpotFactor(max(dot(l.direction, toLight), 0.0), l.cosAngleInner, l.cosAngleOuter);
        }

        return sa;
    }
}

void q2SampleClusterLights(
    const uint cluster, const vec3 p, const vec3 n, const vec3 V,
    const float phongExp, const float phongScale, const float phongWeight,
    const bool isGradient, const vec3 rng,
    out uint outLightIndex, out uint outSlot, out float outPdf)
{
    outLightIndex = LIGHT_INDEX_NONE;
    outSlot = 0u;
    outPdf = 0.0;

    const int lightCount = int(q2GetClusterLightCount(cluster));
    if (lightCount <= 0)
    {
        return;
    }

    const int listBase = int(q2LightListOffsets[cluster]);

    const float partitions = ceil(float(lightCount) / float(Q2_MAX_BRUTEFORCE_SAMPLING));
    float r0 = rng.x * partitions;
    const int fpart = int(min(floor(r0), partitions - 1.0));
    r0 -= float(fpart);
    const int stride = int(partitions);
    const int listStart = listBase + fpart;

    const uint frameSlot = globalUniform.frameId % uint(Q2_LIGHT_LIST_STATS_BUFFERS);
    const uint statsSlot = isGradient
        ? (frameSlot + uint(Q2_LIGHT_LIST_STATS_BUFFERS) - 2u) % uint(Q2_LIGHT_LIST_STATS_BUFFERS)
        : (frameSlot + uint(Q2_LIGHT_LIST_STATS_BUFFERS) - 1u) % uint(Q2_LIGHT_LIST_STATS_BUFFERS);
    const uint statsFrameBase = statsSlot
        * uint(Q2_MAX_CLUSTERS) * uint(Q2_LIGHT_LIST_MAX_PER_CELL)
        * uint(Q2_LIGHT_LIST_STATS_SIDES) * 2u;
    const uint side = q2GetPrimaryDirectionSide(n);

    float masses[Q2_MAX_BRUTEFORCE_SAMPLING];
    float massSum = 0.0;

    for (int i = 0; i < Q2_MAX_BRUTEFORCE_SAMPLING; i++)
    {
        const int nIdx = listStart + i * stride;
        if (nIdx >= listBase + lightCount)
        {
            break;
        }

        const int slot = nIdx - listBase;
        const uint li = q2GetClusterLight(cluster, uint(slot));

        if (li < uint(LIGHT_ARRAY_REGULAR_LIGHTS_OFFSET) ||
            li >= uint(LIGHT_ARRAY_REGULAR_LIGHTS_OFFSET) + globalUniform.lightCount)
        {
            masses[i] = 0.0;
            continue;
        }

        const ShLightEncoded l = lightSources[li];

        float m = q2LightSelectionMass(l, p, n, V, phongExp, phongScale, phongWeight);
        m *= abs(getLuminance(l.color));

        if (m > 0.0)
        {
            const uint statsAddr = statsFrameBase + q2GetLightStatsAddr(cluster, uint(slot), side);
            const uint numHits = q2LightStats[statsAddr];
            const uint numMisses = q2LightStats[statsAddr + 1];
            const uint numTotal = numHits + numMisses;

            if (numTotal > 0)
            {
                m *= max(float(numHits) / float(numTotal), 0.1);
            }
        }

        massSum += m;
        masses[i] = m;
    }

    if (massSum <= 0.0)
    {
        return;
    }

    float r = r0 * massSum;
    const float totalMassScaled = massSum * partitions;
    float pdf = 0.0;
    int selectedSlot = -1;

    for (int i = 0; i < Q2_MAX_BRUTEFORCE_SAMPLING; i++)
    {
        const int nIdx = listStart + i * stride;
        if (nIdx >= listBase + lightCount)
        {
            break;
        }

        pdf = masses[i];
        r -= pdf;
        if (r <= 0.0)
        {
            selectedSlot = nIdx - listBase;
            break;
        }
    }

    if (selectedSlot < 0)
    {
        return;
    }

    outLightIndex = q2GetClusterLight(cluster, uint(selectedSlot));
    outSlot = uint(selectedSlot);
    outPdf = pdf / totalMassScaled;
}

#endif
