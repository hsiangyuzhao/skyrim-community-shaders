// This file is rewritten from AMD's FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "ScreenSpaceRayTracing/ssrt_common.hlsli"

#if SHARC_UPDATE || SHARC_RENDER
#   define SHARC_ENABLE_64_BIT_ATOMICS 1
#   include "ScreenSpaceRayTracing/sharc/SharcCommon.h"
#endif

Texture2D<float4> HistoryTexture : register(t0);
Texture2D<float4> MotionVectorTexture : register(t1);
Texture2D<float4> ScreenColorTextureMips : register(t3);
Texture2D<float> DepthTexture : register(t4);
Texture2D<float> DepthTextureMips : register(t5);
Texture2DArray<float> NoiseTexture : register(t6);
#if defined(DYNAMIC_CUBEMAPS)
TextureCube<float3> EnvTexture : register(t7);
TextureCube<float3> EnvReflectionsTexture : register(t8);
#   if defined(SSGI)
Texture2D<float> SsgiAoTexture : register(t9);
#   endif
#   if defined(SKYLIGHTING)
#	    include "Skylighting/Skylighting.hlsli"
Texture3D<sh2> SkylightingProbeArray : register(t10);
Texture2DArray<float3> stbn_vec3_2Dx1D_128x128x64 : register(t11);
#   endif
#endif
Texture2D<float3> AlbedoTexture : register(t12);

// UAV register map (audit P6 / #20). Must stay in lockstep with the `uavs` arrays in
// ScreenSpaceRayTracing::DrawSSRTDiffuse / DrawSSRTSpecular:
//   u0                       radiance + confidence output       (all permutations)
//   u1     SSRT_SPECULAR     specular hit distance -> texHitDistance, consumed by
//                            Upscaling.cpp as the DLSS-RR specular guide
//   u1..u4 SHARC_*           hash entries / copy offsets / voxel data / voxel prev
// SSRT_SPECULAR and the SHARC permutations are mutually exclusive (SHARC only exists on
// the diffuse path), so both may claim u1.
// The former u1, SSRPDFOutput -> texHitPDF, had no consumer anywhere in the pipeline
// and is gone. That also resolves the old u2 double declaration in the specular
// permutation (SSRTHitDistanceOutput plus u_SharcHashEntriesBuffer), which only
// compiled because fxc strips the unused one.
RWTexture2D<float4> SSRColorOutput : register(u0);

#if defined(SSRT_SPECULAR)
RWTexture2D<float> SSRTHitDistanceOutput : register(u1);
#endif

#if SHARC_UPDATE || SHARC_RENDER
RWStructuredBuffer<uint2> u_SharcHashEntriesBuffer : register(u1);
RWStructuredBuffer<uint> u_HashCopyOffsetBuffer : register(u2);
RWStructuredBuffer<uint4> u_SharcVoxelDataBuffer : register(u3);
RWStructuredBuffer<uint4> u_SharcVoxelDataBufferPrev : register(u4);
#endif

cbuffer SSRTCB : register(b1)
{
    uint MaxSteps;
    uint MaxMips;
    uint UseDynamicCubemapsAsFallback;
    float Thickness;
    float NormalBias;
    float BRDFBias;
    float OcclusionStrength;
    float CubemapNormalization;
};

// (audit #21) Never defined by ScreenSpaceRayTracing::CompileComputeShaders, so this is
// always 0 in every shipped permutation: the engine depth buffer is not inverted (see
// SSRT_IS_FAR_PLANE in ssrt_common.hlsli). The guarded branches are kept only so the
// option remains available, and are unified on this single spelling -- one of them used
// to be `#ifdef SSRT_INVERTED_DEPTH_RANGE`, which never matched and therefore hid
// non-compiling code.
#define SSRT_OPTION_INVERTED_DEPTH 0

#define HIZ_MAX_ITERATIONS MaxSteps
#define HIZ_MIN_MIP 0
#define SSRT_FLOAT_MAX 3.402823466e+38
#define SSRT_DEPTH_HIERARCHY_MAX_MIP MaxMips
#if defined(SSRT_SPECULAR)
#   define SAMPLES_PER_PIXEL 1
#elif SHARC_UPDATE
#   define SAMPLES_PER_PIXEL 1
#else
#   define SAMPLES_PER_PIXEL DIFFUSE_SPP
#endif

// (audit #7s) Two coherent conventions exist for the Hi-Z traversal, and the code used
// to mix them:
//
//  * legacy (diffuse): `screen_size` is the *full* buffer extent, so cell boundaries are
//    computed on a 1/fullDim grid, and every depth fetch rescales the coordinate by
//    DynamicResolutionParams1.xy to land inside the pyramid's dynamic-resolution
//    sub-rect. Under DRS with ratio s < 1 (DLSS Quality s ~= 0.667) a cell is s times
//    smaller than a texel: the ray re-tests the same texel 1/s times, the effective
//    reach of MaxSteps shrinks by s, and the tile/mip decisions no longer line up with
//    texel boundaries.
//
//  * render resolution (specular): `screen_size` is the render extent, the grid matches
//    the pyramid's valid area exactly, and no rescale is needed anywhere.
//
// The second is correct. It is applied to the specular permutation only: the traversal
// grid determines hit rates, and changing it on the diffuse path would move the
// diffuse+fallback look the user currently depends on.
#if defined(SSRT_SPECULAR)
#   define SSRT_DEPTH_COORD_SCALE float2(1.0, 1.0)
#else
#   define SSRT_DEPTH_COORD_SCALE FrameBuffer::DynamicResolutionParams1.xy
#endif

float3 ProjectPosition(float3 origin, float4x4 mat)
{
    float4 projected = mul(mat, float4(origin, 1));
    projected.xyz /= projected.w;
    projected.xy = 0.5 * projected.xy + 0.5;
    projected.y = (1 - projected.y);
    return projected.xyz;
}

// Origin and direction must be in the same space and mat must be able to transform from that space into clip space.
float3 ProjectDirection(float3 origin, float3 direction, float3 screen_space_origin, float4x4 mat) 
{
    float3 offsetted = ProjectPosition(origin + direction, mat);
    return offsetted - screen_space_origin;
}

float3 InvProjectPosition(float3 coord, float4x4 mat) 
{
    coord.y = (1 - coord.y);
    coord.xy = 2 * coord.xy - 1;
    float4 projected = mul(mat, float4(coord, 1));
    projected.xyz /= projected.w;
    return projected.xyz;
}

float2 SSRT_GetMipResolution(float2 screen_dimensions, int mip_level)
{
    // (audit P9) exp2(-mip) instead of pow(0.5, mip): identical result (fxc lowers
    // pow(0.5, x) to exp2(x * log2(0.5)) anyway) but says what is meant and drops the
    // multiply.
    return screen_dimensions * exp2(-float(mip_level));
}

float SSRT_LoadDepth(int2 pixel_coordinate, int mip)
{
    return DepthTextureMips.Load(int3(pixel_coordinate, mip /* + pc.depth_mip_bias*/)).x;
}

float3 SSRT_ScreenSpaceToViewSpace(float3 screen_space_position, uint eyeIndex)
{
    return InvProjectPosition(screen_space_position, FrameBuffer::CameraProjInverse[eyeIndex]);
}

void SSRT_InitialAdvanceRay(float3     origin,
                                float3     direction,
                                float3     inv_direction,
                                float2     current_mip_resolution,
                                float2     current_mip_resolution_inv,
                                float2     floor_offset,
                                float2     uv_offset,
                                out float3 position,
                                out float  current_t)
{
    float2 current_mip_position = current_mip_resolution * origin.xy;

    // Intersect ray with the half box that is pointing away from the ray origin.
    float2 xy_plane = floor(current_mip_position) + floor_offset;
    xy_plane        = xy_plane * current_mip_resolution_inv + uv_offset;

    // o + d * t = p' => t = (p' - o) / d
    float2 t  = xy_plane * inv_direction.xy - origin.xy * inv_direction.xy;
    current_t = min(t.x, t.y);
    position  = origin + current_t * direction;
}

bool SSRT_AdvanceRay(float3       origin,
                         float3       direction,
                         float3       inv_direction,
                         float2       current_mip_position,
                         float2       current_mip_resolution_inv,
                         uint         current_mip_level,
                         float2       floor_offset,
                         float2       uv_offset,
                         float        surface_z,
                         // (audit P9) `thickness` used to be passed here and never read
                         inout float3 position,
                         inout float  current_t)
{
    // Create boundary planes
    float2 xy_plane        = floor(current_mip_position) + floor_offset;
    xy_plane               = xy_plane * current_mip_resolution_inv + uv_offset;
    float3 boundary_planes = float3(xy_plane, surface_z);

    // Intersect ray with the half box that is pointing away from the ray origin.
    // o + d * t = p' => t = (p' - o) / d
    float3 t = boundary_planes * inv_direction - origin * inv_direction;

    // Prevent using z plane when shooting out of the depth buffer.
#if SSRT_OPTION_INVERTED_DEPTH
    t.z = direction.z < 0 ? t.z : SSRT_FLOAT_MAX;
#else
    t.z = direction.z > 0 ? t.z : SSRT_FLOAT_MAX;
#endif

    // Choose nearest intersection with a boundary.
    float t_min = min(min(t.x, t.y), t.z);

#if SSRT_OPTION_INVERTED_DEPTH
    // Larger z means closer to the camera.
    bool above_surface = surface_z < position.z;
#else
    // Smaller z means closer to the camera.
    bool above_surface = surface_z > position.z;
#endif

    // Decide whether we are able to advance the ray until we hit the xy boundaries or if we had to clamp it at the surface.
    // We use the asuint comparison to avoid NaN / Inf logic, also we actually care about bitwise equality here to see if t_min is the t.z we fed into the min3 above.
    bool skipped_tile = asuint(t_min) != asuint(t.z) && above_surface;

    // Make sure to only advance the ray if we're still above the surface.
    current_t = above_surface ? t_min : current_t;

    // Advance ray
    position = origin + current_t * direction;

    return skipped_tile;
}

// Requires origin and direction of the ray to be in screen space [0, 1] x [0, 1]
float3 SSRT_HierarchicalRaymarch(float3 origin, float3 direction, bool is_mirror, float2 screen_size, int most_detailed_mip, float roughness, float thickness,
                                     uint max_traversal_intersections, out bool valid_hit, out uint _num_iters) {
    const float3 inv_direction = abs(direction) > float(1.0e-12) ? float(1.0) / direction : SSRT_FLOAT_MAX;

    // Start on mip with highest detail.
    int current_mip = most_detailed_mip;

    // Could recompute these every iteration, but it's faster to hoist them out and update them.
    float2 current_mip_resolution     = SSRT_GetMipResolution(screen_size, current_mip);
    float2 current_mip_resolution_inv = rcp(current_mip_resolution);

    // Offset to the bounding boxes uv space to intersect the ray with the center of the next pixel.
    // This means we ever so slightly over shoot into the next region.
    float2 uv_offset = 0.005 * exp2(most_detailed_mip) / screen_size;
    uv_offset        = direction.xy < 0 ? -uv_offset : uv_offset;

    // Offset applied depending on current mip resolution to move the boundary to the left/right upper/lower border depending on ray direction.
    float2 floor_offset = direction.xy < 0 ? 0 : 1;


    // Initially advance ray to avoid immediate self intersections.
    float  current_t;
    float3 position;
    SSRT_InitialAdvanceRay(origin, direction, inv_direction, current_mip_resolution, current_mip_resolution_inv, floor_offset, uv_offset, position, current_t);

    _num_iters                     = uint(0);
    while (_num_iters < max_traversal_intersections && current_mip >= most_detailed_mip) {
        if (any(position.xy > float2(1.0, 1.0)) || any(position.xy < float2(0.0, 0.0))) break;
        // (audit #21) Was `#ifdef SSRT_INVERTED_DEPTH_RANGE` -- a third spelling of the
        // macro the rest of the file calls SSRT_OPTION_INVERTED_DEPTH -- guarding
        // `f32(1.0e-6)`, which is not HLSL (it is a leftover from the FidelityFX source
        // this was ported from). Never compiled because the name never matched; unified
        // and made legal so enabling the option is not an instant build break.
#if SSRT_OPTION_INVERTED_DEPTH
        if (position.z < float(1.0e-6)) break;
#else
        if (position.z > float(1.0) - float(1.0e-6)) break;
#endif

        float2 current_mip_position = current_mip_resolution * position.xy;
        float  surface_z            = SSRT_LoadDepth(current_mip_position * SSRT_DEPTH_COORD_SCALE, current_mip);  // (audit #7s)
        bool skipped_tile =
            SSRT_AdvanceRay(origin, direction, inv_direction, current_mip_position, current_mip_resolution_inv, current_mip, floor_offset, uv_offset, surface_z, position, current_t);
        bool nextMipIsOutOfRange = skipped_tile && (current_mip >= SSRT_DEPTH_HIERARCHY_MAX_MIP);
        if (!nextMipIsOutOfRange)
        {
            current_mip += skipped_tile ? 1 : -1;
            current_mip_resolution *= skipped_tile ? 0.5 : 2;
            current_mip_resolution_inv *= skipped_tile ? 2 : 0.5;
        }
        ++_num_iters;
    }

    // (audit #9) `_num_iters <= max_traversal_intersections` was tautologically true --
    // the loop condition is `<`, so the counter can never exceed the limit -- which made
    // every ray report a hit. Rays that ran out of iterations mid-flight, left the
    // screen, or reached the far plane were then handed to SSRT_ValidateHit(), where a
    // far-plane stop in particular yields distance ~= 0 against the equally-far surface
    // depth and therefore confidence ~= 1, i.e. sky colour sampled as if it were a real
    // hit.
    //
    // The traversal descends below `most_detailed_mip` only via the
    // `current_mip += skipped_tile ? 1 : -1` step taken when a tile is *not* skipped at
    // the finest level, which is precisely the definition of an intersection. Every
    // other exit -- the two `break`s and iteration exhaustion -- leaves
    // current_mip >= most_detailed_mip because the loop condition still held. So the
    // final mip alone is an exact hit/miss discriminator, and unlike an iteration-count
    // test it does not reject a genuine hit found on the very last iteration.
    valid_hit = (current_mip < most_detailed_mip);

    return position;
}

float SSRT_ValidateHit(float3 hit, float2 uv, float3 world_space_ray_direction, float2 screen_size, float depth_buffer_thickness, uint eyeIndex, out float occlusion)
{
    occlusion = 1.f;

    // Reject hits outside the view frustum
    if ((hit.x < 0.0f) || (hit.y < 0.0f) || (hit.x > 1.0f) || (hit.y > 1.0f))
    {
        return 0.0f;
    }

    // Don't lookup radiance from the background.
    int2  texel_coords = int2(screen_size * hit.xy * SSRT_DEPTH_COORD_SCALE);
    // (audit #3) Validate against mip 0 -- the same level the traversal descends to
    // (HIZ_MIN_MIP == 0), and the true per-pixel depth.
    //
    // The 2x2 min of mip 1 that used to be read here measures the surface's *own* depth
    // gradient rather than the ray/surface separation the thickness test is about. A
    // clean intersection leaves the ray exactly on the depth plane of the texel it
    // stopped in, so against mip 0 it scores distance ~= 0 and confidence ~= 1; against
    // the 2x2 min of the neighbourhood it scores the gradient across that
    // neighbourhood, which on a grazing surface is ~9 game units at 1920 px / z = 1000 /
    // 80 deg incidence and rises with z and with the incidence angle -- more than the
    // former 5-15 unit thickness, i.e. confidence <= 0.1 over exactly the grazing ground
    // and terrain that fills most of a Skyrim frame.
    //
    // (The FidelityFX source this was ported from reads `texel_coords / 2, 1` because
    // there the traversal grid is half the depth hierarchy's resolution; here it is mip 0
    // itself.)
    //
    // Now unconditional: freewins applied it to the specular permutation only, because at
    // the time the diffuse look depended on the fallback path this starves. Unfreezing it
    // is the point of this change -- see F5 in the spec for the expected visual shift.
    float surface_z = SSRT_LoadDepth(texel_coords, 0);

    // (audit #6) Restored from the author's own fix in e59b35a75. Without it a ray that
    // stops on a background texel is validated against an equally-far surface depth,
    // yielding distance ~= 0 and confidence ~= 1 -- so sky/background colour is sampled
    // as a real hit, while GetNormalRoughness() below reads the cleared normal G-buffer
    // and turns the back-face test into a coin flip. The visible result is flickering
    // sky-coloured light leaks. The threshold stays a margin rather than an equality test
    // so a ray that stops one texel short of a sky silhouette is rejected too.
    static const float SKY_DEPTH_THRESHOLD = 1e-4;
#if SSRT_OPTION_INVERTED_DEPTH
    if (surface_z < SKY_DEPTH_THRESHOLD)
#else
    if (surface_z > (1.0 - SKY_DEPTH_THRESHOLD))
#endif
    {
        // Leaves occlusion at 1 (no occlusion), so the ray falls through to the cubemap
        // fallback unattenuated rather than being darkened.
        return 0;
    }

    float3 view_space_surface = SSRT_ScreenSpaceToViewSpace(float3(hit.xy, surface_z), eyeIndex);
    float3 view_space_hit     = SSRT_ScreenSpaceToViewSpace(hit, eyeIndex);
    float  distance           = length(view_space_surface - view_space_hit);

    // We accept all hits that are within a reasonable minimum distance below the surface.
    // Add constant in linear space to avoid growing of the reflections toward the reflected objects.
    float confidence = 1.0f - smoothstep(0.0f, depth_buffer_thickness, distance);
    confidence *= confidence;

    // We check if we hit the surface from the back, these should be rejected.
    float3 hit_normalVS;
    float hit_roughness;
    GetNormalRoughness(texel_coords, hit_normalVS, hit_roughness);
    float3 hit_normal = normalize(mul(FrameBuffer::CameraViewInverse[eyeIndex], float4(hit_normalVS, 0)).xyz);
    if (dot(hit_normal, world_space_ray_direction) > 0)
    {
        occlusion = 1 - confidence;
        return 0;
    }

    // Reject the hit if we didnt advance the ray significantly to avoid immediate self reflection
    float2 manhattan_dist = abs(hit.xy - uv);
    if ((manhattan_dist.x < (2.f / screen_size.x)) && (manhattan_dist.y < (2.f / screen_size.y)))
    {
        occlusion = 1 - confidence;
        return 0;
    }

    // Fade out hits near the screen borders
    float2 fov      = 0.01 * float2(screen_size.y / screen_size.x, 1);
    float2 border   = smoothstep(float2(0.0f, 0.0f), fov, hit.xy) * (1 - smoothstep(float2(1.0f, 1.0f) - fov, float2(1.0f, 1.0f), hit.xy));
    float  vignette = border.x * border.y;

    return vignette * confidence;
}

bool IsMirrorReflection(float roughness)
{
    return roughness < 0.1;
}

float3 Sample_GGX_VNDF_Ellipsoid(float3 Ve, float alpha_x, float alpha_y, float U1, float U2) { return SampleGGXVNDF(Ve, alpha_x, alpha_y, U1, U2); }

float3 Sample_GGX_VNDF_Hemisphere(float3 Ve, float alpha, float U1, float U2) { return Sample_GGX_VNDF_Ellipsoid(Ve, alpha, alpha, U1, U2); }

float3x3 CreateTBN(float3 N) {
    float3 U;
    if (abs(N.z) > 0.0) {
        float k = sqrt(N.y * N.y + N.z * N.z);
        U.x     = 0.0;
        U.y     = -N.z / k;
        U.z     = N.y / k;
    } else {
        float k = sqrt(N.x * N.x + N.y * N.y);
        U.x     = N.y / k;
        U.y     = -N.x / k;
        U.z     = 0.0;
    }

    float3x3 TBN;
    TBN[0] = U;
    TBN[1] = cross(N, U);
    TBN[2] = N;
    return transpose(TBN);
}

#define GOLDEN_RATIO 1.61803398875f

float2 SampleRandomVector2DBaked(uint2 pixel, uint index, uint numSamples) {
    // int2   coord = int2(pixel.x & 127u, pixel.y & 127u);
    // float2 xi    = float2(NoiseTexture[uint3(coord, 0)].x, NoiseTexture[uint3(coord, 64)].x);
    // float2 u     = float2(fmod(xi.x + (((int)(pixel.x / 128)) & 0xFFu) * GOLDEN_RATIO, 1.0f), fmod(xi.y + (((int)(pixel.y / 128)) & 0xFFu) * GOLDEN_RATIO, 1.0f));
    // return u;
    int3 seed = int3(pixel.xy, 0);
    seed.z = Random::pcg3d(int3(seed.xy, SharedData::FrameCount)).x;
    uint2 xi = Random::pcg3d(seed).xy / 0x10000;
    float2 E = Hammersley16(index, numSamples, xi);
#if defined(SSRT_SPECULAR)
    E.y = lerp(E.y, 0, BRDFBias);
#endif
    return E;
}

float3 SampleReflectionVector(float3 view_direction, float3 normal, float roughness, int2 dispatch_thread_id, uint index, uint numSamples, out float pdf) {
    if (roughness < 0.001f) {
        pdf = 1.0f;
        return reflect(view_direction, normal);
    }
    float3x3 tbn_transform = CreateTBN(normal);
    float3   view_direction_tbn = mul(-view_direction, tbn_transform);
    float2   u = SampleRandomVector2DBaked(dispatch_thread_id, index, numSamples);
    // float3   sampled_normal_tbn = Sample_GGX_VNDF_Hemisphere(view_direction_tbn, roughness, u.x, u.y);
#if defined(SSRT_SPECULAR)
    float4   sampled_normal_tbn = ImportanceSampleGGX(u, roughness * roughness * roughness * roughness);
#else
    float4   sampled_normal_tbn = CosineSampleHemisphereConcentric(u);
#endif
#ifdef PERFECT_REFLECTIONS
    sampled_normal_tbn.xyz = float3(0, 0, 1); // Overwrite normal sample to produce perfect reflection.
#endif
#if defined(SSRT_SPECULAR)
    float3 reflected_direction_tbn = reflect(-view_direction_tbn, sampled_normal_tbn.xyz);
#else
    float3 reflected_direction_tbn = sampled_normal_tbn.xyz;
#endif
    // Transform reflected_direction back to the initial space.
    float3x3 inv_tbn_transform = transpose(tbn_transform);
    pdf = sampled_normal_tbn.w;
    return mul(reflected_direction_tbn, inv_tbn_transform);
}

float3 ScreenSpaceToWorldSpace(float3 screen_space_position, float4x4 invViewProj)
{
    return InvProjectPosition(screen_space_position, invViewProj);
}

float3 ScreenSpaceToViewSpace(float3 screen_uv_coord, float4x4 invProj)
{
    return InvProjectPosition(screen_uv_coord, invProj);
}

// (audit P4) `samples` only ever needs to be shared when several rays per pixel are
// spread across the thread group's z slices and thread z == 0 sums them up. The
// specular permutation and the SHARC update pass both run one sample per pixel, so
// there the whole LDS round trip -- and the group-wide barrier that goes with it --
// is pure overhead; they use a plain local instead.
#if defined(SSRT_SPECULAR) || SHARC_UPDATE
#   define SSRT_USE_SAMPLE_LDS 0
#else
#   define SSRT_USE_SAMPLE_LDS 1
#endif

#if SSRT_USE_SAMPLE_LDS
// (audit P4) Index as y * 8 + x, not x * 8 + y. LDS is banked on consecutive dwords,
// so with x * 8 + y the eight threads of a row (fixed y, x = 0..7) land 8 * sizeof(float4)
// = 32 dwords apart and collide on the same bank, serialising the access eight ways.
// y * 8 + x makes a row contiguous. This is a pure bijective remap of the slots --
// every thread still owns exactly one slot and the z == 0 reduction reads the same
// slots it did before -- so it is bit-for-bit output preserving.
groupshared float4 samples[64][SAMPLES_PER_PIXEL];
#   define SSRT_SAMPLE_SLOT (groupThreadID.y * 8 + groupThreadID.x)
#endif

// (audit P4 / #10) `groupshared float4 weights[64][SAMPLES_PER_PIXEL]` and the
// LocalBRDF() that fed it are gone: nothing ever read the array back, so the specular
// path never actually applied any BRDF/pdf weighting, and the write cost half of the
// group's LDS budget (hurting occupancy) for nothing. Deleting it also retires the
// swapped L/N arguments at the single call site (audit #10) permanently. Reintroducing
// BRDF weighting is a separate, look-changing piece of work.

#if SHARC_UPDATE
uint Hash(uint2 pos, uint seed)
{
    uint hash = pos.x + pos.y * 8 + seed * 64;
    hash = hash * 1103515245u + 12345u;
    return hash;
}
bool ShouldProcessPixel(uint2 GroupThreadID, uint FrameCount)
{
    uint hash = Hash(GroupThreadID, FrameCount);
    return (hash % 4) == 0;
}
#endif

[numthreads(8, 8, SAMPLES_PER_PIXEL)] void main(uint3 groupID : SV_GroupID,
                                                uint3 groupThreadID : SV_GroupThreadID,
                                                uint3 DTid : SV_DispatchThreadID)
{
    // (audit #7s) See SSRT_DEPTH_COORD_SCALE: specular traverses a render-resolution
    // cell grid, diffuse keeps the legacy full-resolution grid plus per-fetch rescale.
#if defined(SSRT_SPECULAR)
    uint2 screen_size = uint2(SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy);
#else
    uint2 screen_size = SharedData::BufferDim.xy;
#endif
    uint2 coords = DTid.xy;
#if defined(SSRT_SPECULAR)
    uint sample_id = 0;
#else
    uint sample_id = groupThreadID.z;
#endif
    float3 debug;

    float4 outColor = float4(0, 0, 0, 0);

    float2 uv = float2(coords.xy + 0.5) * SharedData::BufferDim.zw * FrameBuffer::DynamicResolutionParams2.xy;
    uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);

    // (audit P1) Sky / far-plane early-out.
    // Nothing computed for a far-plane pixel can reach the frame: the diffuse
    // composite multiplies the SSRT result by the albedo G-buffer, which is cleared
    // to 0 wherever no deferred geometry was rasterised (Sky.hlsl writes colour /
    // motion vectors / normals only), and the specular path is consumed through the
    // same albedo-gated deferred composite. Today those pixels still run the full
    // 128-step Hi-Z traversal plus the cubemap + skylighting fallback on a garbage
    // normal, and (because `occlusion` defaults to 1 whenever the ray leaves the
    // screen) frequently emit *full* cubemap radiance with confidence 1 -- which the
    // SVGF spatial filter then bleeds onto the geometry along sky silhouettes.
    // Folding this into `valid_ray` skips the traversal and the fallback and writes a
    // plain 0 instead.
    // NOTE: deliberately *not* an early `return`. The diffuse permutation
    // synchronises `samples[]` across the SAMPLES_PER_PIXEL thread-group z-slices with
    // GroupMemoryBarrierWithGroupSync(); bailing out of a subset of the group's
    // (x, y) lanes would make that barrier non-uniform. Writing 0 (instead of leaving
    // the target untouched) also keeps the ping-pong denoiser textures deterministic.
    float depth = DepthTexture[coords.xy].x;
    const bool is_far_plane = SSRT_IS_FAR_PLANE(depth);

    float3 normalVS;
    float roughness;
    GetNormalRoughness(coords.xy, normalVS, roughness);
    roughness = clamp(roughness, 0.02f, 1.0f);

#if !defined(SSRT_SPECULAR)
    float3 albedo = AlbedoTexture[coords.xy].xyz;
#endif

    bool is_mirror = IsMirrorReflection(roughness);
    int most_detailed_mip = HIZ_MIN_MIP;
    float2 mip_resolution = SSRT_GetMipResolution(screen_size, most_detailed_mip);
    float z = SSRT_LoadDepth(uv * mip_resolution * SSRT_DEPTH_COORD_SCALE, most_detailed_mip);  // (audit #7s)
    float3 screen_uv_space_ray_origin = float3(uv, z);
    float3 view_space_ray = ScreenSpaceToViewSpace(screen_uv_space_ray_origin, FrameBuffer::CameraProjInverse[eyeIndex]);
    float3 world_space_normal = normalize(mul(FrameBuffer::CameraViewInverse[eyeIndex], float4(normalVS, 0)).xyz);
    float3 view_space_surface_normal = normalVS;
    float3 view_space_ray_direction = normalize(view_space_ray);
    // (audit #4) Offsetting purely along the surface normal produces almost no *depth*
    // offset on a grazing surface, which is precisely where one is needed.
    //
    // View space here is right-handed with +z forward and the depth buffer is not
    // inverted (see SSRT_IS_FAR_PLANE). The old `N * NormalBias * z * GAME_UNIT_TO_M`
    // moves the origin by 0.001428 * z * N, whose NDC depth component is
    // ~ n * 0.001428 * N.z / z: at n = 15, z = 1000 that is 2.1e-5 * N.z. On a grazing
    // surface |N.z| -> 0 and the offset vanishes, while the same surface's per-pixel NDC
    // depth gradient reaches ~1e-4. The very first `above_surface = surface_z >
    // position.z` test then fails, current_t never advances, the mip walks down to -1 and
    // the traversal returns position ~= origin -- a self-intersection reported as a hit
    // one texel from the origin. That is the mechanism behind the "grazing ground is
    // black and noisy" symptom, and (via #5) behind the accidental contact darkening.
    //
    // Two corrections, both scoped to grazing angles so the near-normal behaviour the
    // contact regions depend on is untouched:
    //
    //  * tilt the offset direction from N towards the camera by mixing in
    //    -view_space_ray_direction. Motion towards the camera reduces view z directly,
    //    independent of the incidence angle. At near-normal incidence N ~= -D already, so
    //    normalize(N - D) ~= N and nothing changes; at grazing it becomes a 45-degree
    //    blend with a full-strength depth component. Normalising is a deliberate
    //    departure from the audit's reference expression, which leaves the raw difference
    //    (length 2 at normal incidence, 1.41 at grazing) and would silently double the
    //    bias everywhere -- weakening exactly the short-range contact hits that carry
    //    the hair/foliage look.
    //
    //  * scale by 1 / max(|N.D|, 0.1). The depth gradient the bias has to clear grows as
    //    tan(incidence) ~ 1 / |N.D|, so this tracks it instead of fighting it, capped at
    //    10x. At z = 1000 the offset goes from 1.4 game units of which almost none is
    //    depth, to ~14 units almost all of which is -- about three texels of grazing
    //    gradient, enough for the first advance to clear the surface.
    const float view_space_normal_dot_ray = dot(view_space_surface_normal, view_space_ray_direction);
    float3 view_space_bias_direction = view_space_surface_normal - view_space_ray_direction;
    // Degenerate only for N == D, i.e. a normal facing exactly away from the camera; the
    // rsqrt form keeps it finite without a branch.
    view_space_bias_direction *= rsqrt(max(dot(view_space_bias_direction, view_space_bias_direction), 1e-8));
    view_space_ray += view_space_bias_direction * NormalBias * view_space_ray.z * GAME_UNIT_TO_M / max(abs(view_space_normal_dot_ray), 0.1);
    float pdf;
    float3 view_space_reflected_direction = SampleReflectionVector(view_space_ray_direction, view_space_surface_normal, roughness, coords, sample_id, SAMPLES_PER_PIXEL, pdf);
    screen_uv_space_ray_origin = ProjectPosition(view_space_ray, FrameBuffer::CameraProj[eyeIndex]);
    float3 screen_space_ray_direction = ProjectDirection(view_space_ray, view_space_reflected_direction, screen_uv_space_ray_origin, FrameBuffer::CameraProj[eyeIndex]);
    float3 world_space_reflected_direction = mul(FrameBuffer::CameraViewInverse[eyeIndex], float4(view_space_reflected_direction, 0)).xyz;
    float3 world_space_origin = mul(FrameBuffer::CameraViewInverse[eyeIndex], float4(view_space_ray, 1)).xyz;
    float world_ray_length = 0.0;
    // (audit #7s) screen_size * SSRT_DEPTH_COORD_SCALE is the render-resolution extent
    // under both conventions: fullDim * ratio for diffuse, renderDim * 1 for specular.
    bool valid_ray = all(coords < int2(screen_size * SSRT_DEPTH_COORD_SCALE)) && all(coords >= int2(0, 0)) && !is_far_plane;  // (audit P1)
#if SHARC_UPDATE
    valid_ray = valid_ray && ShouldProcessPixel(coords.xy, SharedData::FrameCount);
#endif
    uint hit_counter = 0;
    float3 hit = float3(0.0, 0.0, 0.0);
    float confidence = 0.0;
    float3 world_space_hit = float3(0.0, 0.0, 0.0);
    float3 world_space_ray = float3(0.0, 0.0, 0.0);

    float4 positionWS = float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1);
	positionWS = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], positionWS);
	positionWS.xyz = positionWS.xyz / positionWS.w;

#if SSRT_USE_SAMPLE_LDS
    samples[SSRT_SAMPLE_SLOT][sample_id] = 0.f;
#else
    float4 localSample = 0.f;  // (audit P4) single sample per pixel, no LDS needed
#endif
#if defined(SSRT_SPECULAR)
    float hit_distance = 65536;  // "no hit"; fed to DLSS-RR as the specular hit distance
#endif

#if SHARC_RENDER
    SharcParameters sharcParameters;

    sharcParameters.gridParameters.cameraPosition = FrameBuffer::CameraPosAdjust[0].xyz;
    sharcParameters.gridParameters.sceneScale = GAME_UNIT_TO_M;
    sharcParameters.gridParameters.logarithmBase = SHARC_GRID_LOGARITHM_BASE;
    sharcParameters.gridParameters.levelBias = SHARC_GRID_LEVEL_BIAS;

    sharcParameters.hashMapData.capacity = 0x100000;
    sharcParameters.hashMapData.hashEntriesBuffer = u_SharcHashEntriesBuffer;
#if !SHARC_ENABLE_64_BIT_ATOMICS
    sharcParameters.hashMapData.lockBuffer = u_HashCopyOffsetBuffer;
#endif

    sharcParameters.voxelDataBuffer = u_SharcVoxelDataBuffer;
    sharcParameters.voxelDataBufferPrev = u_SharcVoxelDataBufferPrev;

    SharcHitData hitData;
    hitData.positionWorld = positionWS.xyz + FrameBuffer::CameraPosAdjust[0].xyz;
    hitData.normalWorld = world_space_normal;

    float3 sharcColor = 0;

    if (valid_ray && SharcGetCachedRadiance(sharcParameters, hitData, sharcColor, true))
    {
        samples[SSRT_SAMPLE_SLOT][sample_id] = float4(sharcColor, 1);
    }
    else
#endif
    if (valid_ray)
    {
        bool valid_hit;
        bool go_through_thin = false;
        uint numIterations;
        float thickness  = Thickness  + roughness * 10.0;
        hit = SSRT_HierarchicalRaymarch(screen_uv_space_ray_origin,
                                            screen_space_ray_direction,
                                            is_mirror,
                                            screen_size,
                                            most_detailed_mip,
                                            roughness,
                                            thickness,
                                            HIZ_MAX_ITERATIONS,
                                            valid_hit, numIterations);

        world_space_hit  = ScreenSpaceToWorldSpace(hit, FrameBuffer::CameraViewProjInverse[eyeIndex]);
        world_space_ray  = world_space_hit - world_space_origin.xyz;
        world_ray_length = length(world_space_ray);
        // (audit #9) MUST be initialised. `occlusion` is an out parameter of
        // SSRT_ValidateHit, which is only reached on the true side of the ternary below;
        // it used to be dead code that valid_hit was always true so the variable was
        // always assigned. Now that misses genuinely skip the call, an uninitialised
        // read would feed garbage into `ao = lerp(1.0, occlusion, OcclusionStrength)`.
        // 1.0 = "no occlusion", matching what SSRT_ValidateHit returns for a hit
        // rejected outside the view frustum, so a miss falls back cleanly.
        float occlusion = 1.f;
        confidence       = valid_hit ? SSRT_ValidateHit(hit,
                                                      uv,
                                                      world_space_ray,
                                                      screen_size,
                                                      thickness,
                                                      eyeIndex,
                                                      occlusion
                                                      )
                                     : 0;
        float3 sampleColor = 0;
        if (confidence > 0.0f)
        {
            // float2 projUV;
            // ReprojectHit(MotionVectorTexture, LinearSampler, hit, eyeIndex, projUV);

            sampleColor = ScreenColorTextureMips.SampleLevel(LinearSampler, hit.xy * FrameBuffer::DynamicResolutionParams1.xy, 0).xyz;
            sampleColor = Color::IrradianceToLinear(sampleColor);
#if !defined(SSRT_SPECULAR)
            sampleColor *= SharedData::ssrtSettings.DiffuseMult;
#else
            sampleColor *= SharedData::ssrtSettings.SpecularMult;
            hit_distance = world_ray_length;
#endif
        }
        const float NdotV = saturate(dot(normalize(view_space_ray), view_space_surface_normal));
#if defined(DYNAMIC_CUBEMAPS) && !SHARC_UPDATE
        if (UseDynamicCubemapsAsFallback != 0 && (confidence < 0.999f))
        {
#   if defined(SSRT_SPECULAR)            
            const uint sampleMip = 0;
#   else
            const uint sampleMip = 2;
#   endif
            float directionalAmbientLuminance = Color::RGBToLuminance(max(0.0, mul(SharedData::DirectionalAmbient, float4(world_space_reflected_direction, 1.0)))) * Color::ReflectionNormalisationScale;
            float envLuminance;
            // Fallback to dynamic cubemaps
            float3 envColor = EnvReflectionsTexture.SampleLevel(LinearSampler, world_space_reflected_direction, sampleMip);
#	if defined(SKYLIGHTING)
            if (!SharedData::InInterior)
            {
                float3 positionMS = positionWS.xyz;

                sh2 skylighting = Skylighting::sample(SharedData::skylightingSettings, SkylightingProbeArray, stbn_vec3_2Dx1D_128x128x64, coords.xy, positionMS.xyz, world_space_reflected_direction);
                float3 skylightingNormal = normalize(float3(world_space_normal.xy, max(0, world_space_normal.z)));
                float skylightingDiffuse = SphericalHarmonics::FuncProductIntegral(skylighting, SphericalHarmonics::EvaluateCosineLobe(skylightingNormal)) / Math::PI;
                skylightingDiffuse = saturate(skylightingDiffuse);

                skylightingDiffuse = lerp(1.0, skylightingDiffuse, Skylighting::getFadeOutFactor(positionMS.xyz));

                skylightingDiffuse *= 1.0 + saturate(world_space_normal.z) * (1.0 - SharedData::skylightingSettings.MinDiffuseVisibility);

                skylightingDiffuse = Skylighting::mixDiffuse(SharedData::skylightingSettings, skylightingDiffuse);
#       if defined(SSRT_SPECULAR)
                skylightingDiffuse = GetSpecularOcclusionFromAmbientOcclusion(NdotV, skylightingDiffuse, roughness);
#       endif
                float3 envNoSkyColor = EnvTexture.SampleLevel(LinearSampler, world_space_reflected_direction, sampleMip);
                float3 envSkyColor = envColor;
                float3 skyColor = max(envSkyColor - envNoSkyColor, 0);
                envLuminance = Color::RGBToLuminance(EnvTexture.SampleLevel(LinearSampler, world_space_reflected_direction, 15));
                envColor = lerp(envNoSkyColor, envNoSkyColor * (directionalAmbientLuminance / max(envLuminance, 1e-4)), CubemapNormalization);
                envColor += skyColor * skylightingDiffuse;
            } else {
                envLuminance = Color::RGBToLuminance(EnvReflectionsTexture.SampleLevel(LinearSampler, world_space_reflected_direction, 15));
                envColor = lerp(envColor, envColor * (directionalAmbientLuminance / max(envLuminance, 1e-4)), CubemapNormalization);
            }
#   else
            envLuminance = Color::RGBToLuminance(EnvReflectionsTexture.SampleLevel(LinearSampler, world_space_reflected_direction, 15).xyz);
            envColor = lerp(envColor, envColor * (directionalAmbientLuminance / max(envLuminance, 1e-4)), CubemapNormalization);
#   endif
            envColor = Color::IrradianceToLinear(envColor);
            float ao = lerp(1.0, occlusion, OcclusionStrength);
#   if defined(SSGI)
            ao *= 1 - saturate(SsgiAoTexture[coords.xy].x);
#   endif
#   if defined(SSRT_SPECULAR)
            ao = GetSpecularOcclusionFromAmbientOcclusion(NdotV, ao, roughness);
            envColor *= ao;
#   else
            float3 multiBounceAO = Color::MultiBounceAO(albedo, ao);
            envColor *= multiBounceAO;
#   endif
            sampleColor.xyz = lerp(envColor, sampleColor.xyz, confidence);
            confidence = 1;
        }
#endif
#if SSRT_USE_SAMPLE_LDS
        samples[SSRT_SAMPLE_SLOT][sample_id] = float4(sampleColor, confidence);
#else
        localSample = float4(sampleColor, confidence);
#endif

#if SHARC_UPDATE
        if (confidence > 0.99f)
        {
            SharcParameters sharcParameters;

            sharcParameters.gridParameters.cameraPosition = FrameBuffer::CameraPosAdjust[0].xyz;
            sharcParameters.gridParameters.sceneScale = GAME_UNIT_TO_M;
            sharcParameters.gridParameters.logarithmBase = SHARC_GRID_LOGARITHM_BASE;
            sharcParameters.gridParameters.levelBias = SHARC_GRID_LEVEL_BIAS;

            sharcParameters.hashMapData.capacity = 0x100000;
            sharcParameters.hashMapData.hashEntriesBuffer = u_SharcHashEntriesBuffer;
#   if !SHARC_ENABLE_64_BIT_ATOMICS
            sharcParameters.hashMapData.lockBuffer = u_HashCopyOffsetBuffer;
#   endif

            sharcParameters.voxelDataBuffer = u_SharcVoxelDataBuffer;
            sharcParameters.voxelDataBufferPrev = u_SharcVoxelDataBufferPrev;

            SharcState sharcState;
            for (int i = 0; i < 4; ++i) {
                sharcState.cacheIndices[i] = 0;
                sharcState.sampleWeights[i] = 0;
            }
            sharcState.pathLength = 0;

            SharcHitData hitData;
            hitData.positionWorld = positionWS.xyz + FrameBuffer::CameraPosAdjust[0].xyz;
            hitData.normalWorld = world_space_normal;

            float random = Random::InterleavedGradientNoise(uv, SharedData::FrameCount);
            SharcUpdateHit(sharcParameters, sharcState, hitData, sampleColor, random);
        }
#endif
    }
#if SSRT_USE_SAMPLE_LDS
    // Publish this z slice's sample to the other slices of the same pixel before the
    // z == 0 lane sums them. Only reachable when SAMPLES_PER_PIXEL > 1 actually needs
    // cross-slice sharing; the specular and SHARC-update permutations skip both the
    // LDS round trip and this barrier (audit P4).
    GroupMemoryBarrierWithGroupSync();
#endif

#if defined(SSRT_SPECULAR)
    outColor = localSample;
    SSRColorOutput[coords.xy] = outColor;
    SSRTHitDistanceOutput[coords.xy] = hit_distance;
#elif SHARC_UPDATE
#else

    if (sample_id == 0) {
        outColor = 0.f;
        for (int i = 0; i < SAMPLES_PER_PIXEL; ++i) {
            outColor.xyz += samples[SSRT_SAMPLE_SLOT][i].xyz;
            outColor.w += samples[SSRT_SAMPLE_SLOT][i].w;
        }
        outColor.xyz /= SAMPLES_PER_PIXEL;
        outColor.w = saturate(outColor.w / SAMPLES_PER_PIXEL);
        SSRColorOutput[coords.xy] = outColor;
    }
#endif
}