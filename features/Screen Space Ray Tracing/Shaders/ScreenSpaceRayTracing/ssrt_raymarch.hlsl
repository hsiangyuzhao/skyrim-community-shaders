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

// (batch 11, item A) REBLUR's front-end packing helpers, so that the ray march can write u0
// straight into the IN_*_RADIANCE_HITDIST layout NRD expects instead of handing a second
// full-screen pass the job of converting it. Header only -- it declares no registers and every
// symbol in it is prefixed _NRD_ / NRD_ / REBLUR_, so it cannot collide with anything
// ssrt_common.hlsli brings in.
#include "NRD/NRDReblurSH.hlsli"

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

// (contact noise) Deterministic near-field occlusion for the diffuse cubemap fallback.
//
// WHY THE VOTE IS SUPPRESSED IN THE NEAR FIELD. The fallback's near-field darkening used to come
// entirely from the per-ray `occlusion` term below, which is a two-sample Monte-Carlo vote on a hard
// sign test (see the long note at the application site). No reformulation of a 2-spp binary vote can
// be quiet: estimating a smooth visibility field from two samples carries a +-0.5 quantisation error
// by construction, and on this path nothing downstream removes it -- the SVGF chain's edge stops
// collapse at exactly the depth and normal discontinuities a contact region is made of, and the user
// may have no anti-aliasing at all. The only fix is to stop estimating that field stochastically, so
// the vote hands the near field to a deterministic kernel instead.
//
// WHERE THAT KERNEL IS NOW. In Screen Space GI. This file used to include
// EnvironmentAmbient/EnvAmbient.hlsli and evaluate a ten-tap contact kernel itself, once per pixel,
// with a frame-static spiral and an LDS broadcast across the z slices -- one of three independent,
// unfiltered evaluations of the same signal in the pipeline. SSGI now runs it once per frame at full
// resolution with its own temporal accumulator and folds the result into the AO channel this file
// already reads (`ao *= 1 - SsgiAoTexture` at the application site), so the darkening arrives here
// with no code of its own, denoised, and identical to what every other consumer sees.
//
// The dependency on eatures/Environment Ambient/ went with it: that folder is non-CORE, so this
// file used to compile the whole change out on an SSRT-without-L1 install. It now needs only SSGI's
// own AO texture, which it was reading anyway.

// UAV register map (audit P6 / #20). Must stay in lockstep with the `uavs` arrays in
// ScreenSpaceRayTracing::DrawSSRTDiffuse / DrawSSRTSpecular:
//   u0                       radiance + confidence output       (all permutations)
//   u1     SSRT_SPECULAR     specular hit distance -> texHitDistance, consumed by
//                            Upscaling.cpp as the DLSS-RR specular guide
//   u1..u4 SHARC_*           hash entries / copy offsets / voxel data / voxel prev
//   u5     diffuse           raw hit confidence -> texSSRTDiffuseConfidence
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

// (ambient reinjection) A second, deliberately denoiser-independent home for the diffuse hit
// confidence.
//
// .w of SSRColorOutput carries the same number, but only until the SVGF chain runs: with
// EnableSVGF on, ssrt_temporal.hlsl overwrites .w with the luminance variance and the
// variance / a-trous passes keep it there, because that is the channel the edge-stopping
// function is steered by. The composite-side ambient reinjection needs the confidence *after*
// the denoiser has run, so it cannot read .w and cannot be threaded through four ping-ponging
// passes without colliding with the quantity they exist to compute.
//
// A dedicated R8_UNORM surface costs one byte per render texel (~8 MB of a 4K allocation
// against ~33 MB for each of the eight full-screen RGBA16F surfaces this feature already
// holds) and its 1/255 quantisation is an order of magnitude below the residual noise of the
// spatial smoothing this value gets in ssrt_diffuse_composite.hlsl. Being UNORM also means a
// read is a [0,1] value by construction, so no consumer needs its own finiteness test.
#if !defined(SSRT_SPECULAR) && !SHARC_UPDATE
RWTexture2D<float> SSRTConfidenceOutput : register(u5);

// (batch 1, item 2) The diffuse per-pixel hit distance, encoded as a correlation length in
// texels, reciprocally encoded as t / (t + SSRT_HITT_REF_TEXELS) -- see that constant in
// ssrt_common.hlsli.
// Read by the diffuse permutation of ssrt_spatial.hlsl to size its kernel per pixel.
//
// A slot of its own rather than a channel of an existing surface, for the reasons the audit
// already established for the confidence pair:
//   * .w of SSRColorOutput carries the confidence until the SVGF chain runs, and from
//     ssrt_temporal.hlsl onwards it carries the luminance variance -- the one channel the whole
//     a-trous edge-stop is steered by. There is no third meaning available there.
//   * texSSRTDiffuseConfidence is R8_UNORM and full. Widening it to R8G8 would cost the same
//     two bytes per texel as a second R8 surface while entangling the denoiser's kernel sizing
//     with the ambient-reinjection path that DeferredCompositeCS reads, so the surfaces stay
//     separate.
// u6, because u1..u4 belong to the SHARC buffers on the permutation that has them and u5 to the
// confidence. Seven UAVs, against the eight a cs_5_0 dispatch may bind.
RWTexture2D<float> SSRTDiffuseHitDistanceOutput : register(u6);
#endif

#if SHARC_UPDATE || SHARC_RENDER
RWStructuredBuffer<uint2> u_SharcHashEntriesBuffer : register(u1);
RWStructuredBuffer<uint> u_HashCopyOffsetBuffer : register(u2);
RWStructuredBuffer<uint4> u_SharcVoxelDataBuffer : register(u3);
RWStructuredBuffer<uint4> u_SharcVoxelDataBufferPrev : register(u4);
#endif

// (batch 36b) The constant buffer moved to ssrt_cb.hlsli, shared with the batch 36b composite and
// unpack permutations that read its later rows.
#include "ScreenSpaceRayTracing/ssrt_cb.hlsli"

#if defined(SSRT_CHECKERBOARD)
// (batch 36b) Checkerboard debug view: red where this frame traced diffuse, green where it traced
// specular. u7 is free on both checkerboard permutations (no SHARC buffers there).
RWTexture2D<unorm float4> CheckerDebugOutput : register(u7);
#endif

// (audit #21) Never defined by ScreenSpaceRayTracing::CompileComputeShaders, so this is
// always 0 in every shipped permutation: the engine depth buffer is not inverted (see
// SSRT_IS_FAR_PLANE in ssrt_common.hlsli). The guarded branches are kept only so the
// option remains available, and are unified on this single spelling -- one of them used
// to be `#ifdef SSRT_INVERTED_DEPTH_RANGE`, which never matched and therefore hid
// non-compiling code.
#define SSRT_OPTION_INVERTED_DEPTH 0

#define HIZ_MAX_ITERATIONS MaxSteps
// The traversal starts at mip 0: every lane, checkerboard ones included, traces a real
// full-resolution pixel and walks the same cell grid the full-resolution pass walks.
#define HIZ_MIN_MIP 0
// The self-intersection radius in SSRT_ValidateHit, in full-resolution texels.
#define SSRT_SELF_HIT_TEXELS 2.f
#define SSRT_FLOAT_MAX 3.402823466e+38
#define SSRT_DEPTH_HIERARCHY_MAX_MIP MaxMips
#if defined(SSRT_SPECULAR)
#   define SAMPLES_PER_PIXEL 1
#elif SHARC_UPDATE
#   define SAMPLES_PER_PIXEL 1
#else
#   define SAMPLES_PER_PIXEL DIFFUSE_SPP
#endif

// (audit #7 / #17) `screen_size` is the *render* extent, on both permutations.
//
// Two conventions used to coexist here, gated on SSRT_SPECULAR through a
// SSRT_DEPTH_COORD_SCALE macro:
//
//  * legacy (diffuse): `screen_size` was the full buffer extent, so cell boundaries were
//    computed on a 1/fullDim grid while every depth fetch rescaled its coordinate by
//    DynamicResolutionParams1.xy to land inside the pyramid's dynamic-resolution
//    sub-rect. Under DRS with ratio s < 1 (DLSS Quality s ~= 0.667) a cell is s times
//    smaller than a texel: the ray re-tests the same texel 1/s times, the effective reach
//    of MaxSteps shrinks by s, and the tile/mip decisions stop lining up with texel
//    boundaries -- so the mip pyramid, whose whole purpose is to skip empty space a tile
//    at a time, is stepping through fractions of single texels.
//
//  * render resolution (specular, from freewins): `screen_size` is the render extent, so
//    the traversal grid coincides with the pyramid's valid area at every mip
//    (valid extent of mip m == renderDim >> m) and no rescale is needed anywhere.
//
// Only the second is coherent, and it is now the only one. The macro and its gate are
// gone, and diffuse and specular walk the same cell grid.
//
// It also settles audit #17, which flags two lengths in SSRT_ValidateHit as
// resolution-dependent. Only one of them is:
//
//  * the self-intersection radius `2 / screen_size` is a texel count in the same [0,1]
//    render-normalised uv space as `hit.xy`, so on the full extent it measured 2s ~= 1.33
//    render texels rather than 2. Unifying the convention makes it mean what it says.
//  * the border vignette's `fov = 0.01 * float2(screen_size.y / screen_size.x, 1)` is
//    *not* affected: DRS scales both extents by the same ratio, so the quotient -- and
//    with it the vignette band in uv -- is identical under either convention. Nothing to
//    fix, and nothing changes.

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
        // (audit #7) No rescale: `screen_size` is the render extent, so a texel coordinate
        // on this mip's traversal grid is already a texel coordinate inside the pyramid's
        // valid area at this mip.
        float  surface_z            = SSRT_LoadDepth(current_mip_position, current_mip);
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
    int2  texel_coords = int2(screen_size * hit.xy);  // (audit #7) render extent, no rescale
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
    //
    // (audit #5) This branch used to write `occlusion = 1 - confidence`, and a
    // self-intersection lands on the origin's own surface, so distance ~= 0, confidence
    // ~= 1, occlusion ~= 0 -- full occlusion. Downstream that is
    // `ao = lerp(1.0, occlusion, OcclusionStrength)` with OcclusionStrength defaulting to
    // 1.0, then `envColor *= MultiBounceAO(albedo, ao)`: the cubemap fallback gets
    // multiplied to black as well, so a failed ray does not merely miss, it darkens the
    // pixel. At DiffuseSPP = 2 half the samples self-intersecting halves the ambient.
    //
    // A ray that never left its own texel is a *failure to trace*, not evidence of an
    // occluder, so it must leave occlusion at the 1.0 the function entry establishes and
    // let the pixel fall through to the fallback unattenuated. Back-face hits above
    // remain the only occlusion source, which is the one case where the ray really did
    // run into geometry.
    //
    // This is also where the contact darkening the user currently likes came from: with
    // #4 fixed the branch fires far less often, and the hair/foliage darkening is
    // expected to come back through the legitimate path instead -- rays that now advance
    // properly, hit the hair, and return its dark screen radiance.
    float2 manhattan_dist = abs(hit.xy - uv);
    if ((manhattan_dist.x < (SSRT_SELF_HIT_TEXELS / screen_size.x)) && (manhattan_dist.y < (SSRT_SELF_HIT_TEXELS / screen_size.y)))
    {
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

// (S3.10) NUMBER OF ARRAY SLICES IN noise.dds. Read out of the file's own DX10 header, not
// assumed: dxgiFormat 80 (BC4_UNORM), 128x128, 8 mips, arraySize 64. That figure matters
// because the blue-noise path this constant serves was previously written -- and commented out
// -- as `NoiseTexture[uint3(coord, 0)]` paired with `NoiseTexture[uint3(coord, 64)]`, and slice
// 64 does not exist in a 64-slice array. An out-of-range Load returns 0, so the y coordinate of
// every sample would have been a constant and the second dimension of the hemisphere sample a
// pure spatial ramp. That is almost certainly why the path was abandoned rather than debugged.
#define SSRT_NOISE_SLICE_MASK 63u

// (S3.10) Second tap offset for the y coordinate.
//
// noise.dds is a *scalar* blue-noise array, so a 2D sample needs two values. Taking them from
// two positions within the same slice is the standard construction: a blue-noise field's
// autocorrelation is negligible beyond a few texels, so two taps this far apart are effectively
// independent while each remains blue-noise distributed *across the screen*, which is the
// property that buys the perceptual win. Coprime with 128 in both axes so the pairing does not
// degenerate on any lattice the tiling can produce.
#define SSRT_NOISE_TAP2_OFFSET uint2(61, 37)

float2 SampleRandomVector2DBaked(uint2 pixel, uint index, uint numSamples) {
    // (diagnostic T2) The frame counter is the *only* thing that makes this pixel's sample
    // directions differ from frame to frame, so replacing it with a constant turns the 2-spp
    // Monte-Carlo estimate into a fixed, screen-space-locked pattern. That is precisely the
    // discriminator the audit needs: smearing caused by an upscaler clamping a *changing*
    // stochastic signal along motion must vanish, while smearing caused by the SVGF temporal
    // pass's own reprojection must be unaffected. Diagnostic only -- frozen noise is noise a
    // denoiser cannot average away.
    const uint noisePhase = FreezeNoisePhase != 0 ? 0u : SharedData::FrameCount;

    // (S3.10) The scramble Hammersley16 is given: blue noise by default, hash white noise as
    // the A/B alternative.
    //
    // WHY THIS IS THE RIGHT SEAM. Hammersley16 uses its argument as a per-pixel Cranley-Patterson
    // shift on the first coordinate and an XOR scramble on the second, so the argument decides
    // *where in the sequence* each pixel starts while the sequence itself keeps the intra-pixel
    // stratification of the N samples. Replacing the whole function with a raw texture fetch --
    // which is what the commented-out code did -- would have thrown that stratification away and
    // handed every one of a pixel's N samples the same direction. Only the scramble changes here.
    //
    // WHAT IT BUYS. The error of a 2-spp estimate does not get smaller; it gets *rearranged*.
    // A hash gives neighbouring pixels independent scrambles, so the error field is white --
    // energy spread evenly across all spatial frequencies, including the low ones that read as
    // blotching and that no small denoiser kernel and no upscaler can remove. A blue-noise
    // scramble pushes that energy into the high frequencies, where every subsequent stage --
    // the a-trous kernel, REBLUR's spatial pass, the upscaler, and the eye's own contrast
    // sensitivity falloff -- attenuates it. Same samples, same cost (one or two texture taps
    // against two pcg3d hashes, which is a wash), materially quieter picture.
    //
    // WHY THE PHASE STILL ADVANCES PER FRAME. The third axis of this array is the temporal one,
    // so the slice index carries the per-frame phase. Freezing it would give a fixed screen-space
    // pattern -- which is exactly what the T2 diagnostic is for and exactly what REBLUR's
    // temporal accumulation must not be handed, because it averages over frames and can only
    // average away something that changes.
    uint2 xi;
    if (UseBlueNoise != 0) {
        const uint2 coord = uint2(pixel.x & 127u, pixel.y & 127u);
        const uint2 coord2 = (coord + SSRT_NOISE_TAP2_OFFSET) & 127u;
        // One slice per (frame, sample) pair, so the N samples of a frame are decorrelated from
        // each other as well as from the previous frame's.
        const uint slice = (noisePhase * numSamples + index) & SSRT_NOISE_SLICE_MASK;
        // BC4 carries 8 bits, so the fetch quantises to 1/255 and is shifted into the high byte
        // of the 16-bit scramble Hammersley16 expects. The low byte being zero costs nothing
        // that matters: it makes the per-pixel shift a multiple of 1/256, an order of magnitude
        // finer than the angular resolution a 2-spp hemisphere estimate can resolve.
        const uint nx = (uint)(NoiseTexture[uint3(coord, slice)] * 255.0f + 0.5f);
        const uint ny = (uint)(NoiseTexture[uint3(coord2, slice)] * 255.0f + 0.5f);
        xi = uint2(nx, ny) << 8u;
    } else {
        int3 seed = int3(pixel.xy, 0);
        seed.z = Random::pcg3d(int3(seed.xy, noisePhase)).x;
        xi = Random::pcg3d(seed).xy / 0x10000;
    }
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
// (batch 1, item 2) The per-sample encoded hit distance, resolved by the same z == 0 lane and
// across the same barrier as `samples`. Same y * 8 + x slot mapping for the same LDS banking
// reason; at one dword per slot the eight threads of a row are contiguous.
//
// A parallel array rather than a fifth channel because `samples` is a float4 with all four
// channels spoken for (rgb radiance plus the confidence the resolve also has to average).
// 4 bytes per lane per sample: 512 bytes at the default DiffuseSPP 2, 4096 at the slider's
// maximum of 16, on top of the 2048 / 16384 `samples` already costs.
groupshared float hitNorms[64][SAMPLES_PER_PIXEL];
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

// (guard G2) Last gate before the radiance leaves the ray march and becomes the input of
// the SVGF chain, whose history textures are persistent and -- until G8 -- never cleared.
// Every path that can produce a non-finite value converges here: the kMAIN sample (G1
// covers the read itself, this covers the arithmetic applied after it), the cubemap
// fallback's normalisation ratio (G7), the skylighting product, and the per-SPP average
// below. Sanitising once at the write is cheaper than auditing each contributor and is
// what makes "one bad frame" recoverable instead of permanent.
//
// The clamp is on the colour channels only. .w is confidence (diffuse) or confidence
// (specular) and is a [0,1] quantity; filterNaN/filterInf are the whole guard it needs.
//
// No-op on healthy data: filterNaN is the identity on any ordered float, filterInf the
// identity on any value below infinity, and min(x, 128) the identity for the entire
// legitimate radiance range (see SSRT_MAX_RADIANCE). Bit-exact, not merely close.
float4 SSRT_SanitiseRadianceOutput(float4 color)
{
    color = filterNaN(color);
    color = filterInf(color);
    color.rgb = min(color.rgb, SSRT_MAX_RADIANCE);
    return color;
}

// ============================================================================================
// (batch 11, item A) REBLUR front-end packing, folded in from the retired ssrt_nrd_pack.hlsl.
//
// WHY THE STORAGE ROUND TRIPS ARE REPRODUCED EXPLICITLY. The pack pass read its inputs back
// out of textures the ray march had just written: the radiance out of an R16G16B16A16_FLOAT
// surface, the diffuse hit-distance encoding out of an R8_UNORM one. Computing the same
// quantities from the full-precision locals instead would produce a *different* (in fact
// slightly better) number, and "slightly better" is not what this change is for -- the point is
// to delete two full-screen passes at no visual cost whatsoever. So the two conversions are
// applied by hand, which makes the arithmetic below bit-identical to what the pack pass
// computed and keeps the burden of proof on arithmetic rather than on perception.
//
// Both conversions are exactly specified, which is what makes this work at all:
//   * FLOAT -> FLOAT16 is IEEE half with round-to-nearest-even, and the f32tof16 / f16tof32
//     intrinsic pair is that same conversion.
//   * FLOAT -> UNORM8 is round-to-nearest-even of clamp(x, 0, 1) * 255, and HLSL's round()
//     compiles to DXBC round_ne -- nearest *even*, not C's ties-away-from-zero.
float3 SSRT_Fp16RoundTrip3(float3 v)
{
    return float3(f16tof32(f32tof16(v.x)), f16tof32(f32tof16(v.y)), f16tof32(f32tof16(v.z)));
}

float SSRT_Unorm8RoundTrip(float v)
{
    return round(saturate(v) * 255.0f) / 255.0f;
}

// Verbatim from ssrt_nrd_pack.hlsl's ScreenToViewDepth, including the sentinel: sky and far
// plane resolve to a viewZ far outside NRD's denoisingRange, so REBLUR treats the pixel as "no
// surface". Deliberately *not* expressed through view_space_ray.z, which is a different
// construction (Hi-Z mip 0 through CameraProjInverse) and would not reproduce the same bits.
float SSRT_NRDViewZ(float screenDepth)
{
    if (screenDepth >= 1.0 - 1e-6 || screenDepth <= 0.0)
        return 3.402823466e+38;
    return (SharedData::CameraData.w / (-screenDepth * SharedData::CameraData.z + SharedData::CameraData.x));
}

float3 SSRT_NRDHitDistParams()
{
    return float3(NRDHitDistA, NRDHitDistB, NRDHitDistC);
}

// (guard G7) Non-finite guard on the CubemapNormalization brightness ratio.
//
// The ratio is directionalAmbientLuminance / max(envLuminance, 1e-4); a non-finite
// DirectionalAmbient produces Inf, at which point the surrounding
// `lerp(envColor, envColor * ratio, CubemapNormalization)` is *worse* than a plain
// multiply, because at the default CubemapNormalization = 0 the lerp evaluates
// envColor + 0 * (Inf - envColor) = 0 * Inf = NaN. The feature being switched off does not
// protect it.
//
// Deliberately NOT a magnitude ceiling. The numerator (DALC luminance scaled by
// ReflectionNormalisationScale) and the denominator (the cubemap's mip-15 average
// luminance) are not comparable quantities -- bridging that scale gap is the entire job of
// CubemapNormalization -- and in regions with no direct light the honest ratio routinely
// runs far past any small constant. A first version of this guard capped the ratio at 16
// and measurably darkened the fallback in exactly those regions. Magnitude overflow is
// already contained downstream by G2's SSRT_MAX_RADIANCE ceiling, so the only thing that
// must be stopped here is non-finiteness itself; 1.0 as the fallback multiplies envColor
// by exactly nothing. Identity (bit-exact) for every finite ratio.
float SSRT_CubemapNormalizationRatio(float ambientLuminance, float envLuminance)
{
    float ratio = ambientLuminance / max(envLuminance, 1e-4);
    return isFiniteSafe(ratio) ? ratio : 1.0f;
}

#if defined(DYNAMIC_CUBEMAPS) && defined(SSGI)
// (batch 36b) The fallbacks' AO, in SSGI's convention (occlusion). t9 is Screen Space GI's AO, read in
// place as always -- or, under SSRT's denoiser AO tiers, texSSRTAo. That surface is written by the
// diffuse composite, i.e. after every ray march that runs before it, so those passes read last
// frame's at this pixel's motion-reprojected position (AoFetchReprojected). Same reprojection as
// ReprojectHit: uv plus the motion vector, point-loaded, previous frame's dynamic-resolution ratio.
float SSRT_FallbackAoOcclusion(uint2 px, float2 uv)
{
    int2 aoPx = int2(px);
    [branch] if (AoFetchReprojected != 0)
    {
        const int2 motionMax = int2(SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy) - 1;
        const float2 prevUV = uv + MotionVectorTexture[clamp(int2(px), int2(0, 0), motionMax)].xy;
        const float2 prevExtent = SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.zw;
        aoPx = clamp(int2(prevUV * prevExtent), int2(0, 0), int2(prevExtent) - 1);
    }
    return saturate(SsgiAoTexture[aoPx].x);
}
#endif

[numthreads(8, 8, SAMPLES_PER_PIXEL)] void main(uint3 groupID : SV_GroupID,
                                                uint3 groupThreadID : SV_GroupThreadID,
                                                uint3 DTid : SV_DispatchThreadID)
{
    // (audit #7) The render extent, on every permutation -- the traversal grid, the
    // pyramid's valid area and the dispatch all agree on it. Matches the renderDim in
    // ssrt_preprocess_depth.hlsl and Util::ConvertToDynamic on the C++ side, so the
    // truncation lands on the same texel.
    const uint2 screen_size = uint2(SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy);
    uint2 coords = DTid.xy;
#if defined(SSRT_SPECULAR)
    uint sample_id = 0;
#else
    uint sample_id = groupThreadID.z;
#endif
    float3 debug;

    float4 outColor = float4(0, 0, 0, 0);

    // `SSRT_GBUFFER_COORDS` is the full-resolution pixel this lane traces for and `SSRT_NOISE_COORDS`
    // seeds its sampling sequences. Macros rather than locals so the token stream fxc sees is
    // `coords.xy` itself on every full-resolution permutation.
#if defined(SSRT_CHECKERBOARD)
    // (batch 36b) REBLUR checkerboard. `coords` is the compact coordinate: the dispatch is
    // ceil(width / 2) wide and the packed output is written there, i.e. tightly into the left half of
    // the full-size NRD input, which is NRD's own layout for checkerboard signals
    // (NRDSettings.h, CheckerboardMode). The full-resolution pixel this lane traces is the one of the
    // horizontal pair that REBLUR will read this frame:
    //
    //   NRD: checkerboard = (x ^ y ^ frameIndex) & 1  (MathLib Sequence::CheckerBoard)
    //        diffuse has data where checkerboard == gDiffCheckerboard, specular where
    //        checkerboard == gSpecCheckerboard, and it reads both at (x >> 1, y)
    //        (REBLUR_TemporalAccumulation.cs.hlsl, diffHasData / specHasData).
    //   REBLUR_DIFFUSE_SPECULAR with CheckerboardMode::BLACK: gDiffCheckerboard = 0,
    //        gSpecCheckerboard = 1 (Reblur.cpp).
    //   So diffuse owns x with (x & 1) == ((y + frameIndex) & 1) -- the low bit of an XOR is the
    //   low bit of the sum -- and specular owns the other one. Every pixel gets exactly one of the
    //   two signals per frame, and the other one the next frame.
    //
    // NRDFrameIndex is the very value REBLUR receives as gFrameIndex (CommonSettings::frameIndex).
#   if defined(SSRT_SPECULAR)
    const uint checkerParity = ((coords.y + NRDFrameIndex) & 1u) ^ 1u;
#   else
    const uint checkerParity = (coords.y + NRDFrameIndex) & 1u;
#   endif
    const uint2 gbufferCoords = uint2((coords.x << 1) | checkerParity, coords.y);
#   define SSRT_GBUFFER_COORDS gbufferCoords
    // The noise pattern was authored for the full-resolution grid, and a checkerboard lane really is
    // a full-resolution pixel, so it is seeded exactly as the full-resolution pass would seed it.
#   define SSRT_NOISE_COORDS gbufferCoords
#else
#   define SSRT_GBUFFER_COORDS coords.xy
#   define SSRT_NOISE_COORDS coords.xy
#endif

    float2 uv = float2(SSRT_GBUFFER_COORDS + 0.5) * SharedData::BufferDim.zw * FrameBuffer::DynamicResolutionParams2.xy;
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
    float depth = DepthTexture[SSRT_GBUFFER_COORDS].x;
    const bool is_far_plane = SSRT_IS_FAR_PLANE(depth);

    float3 normalVS;
    float roughness;
    GetNormalRoughness(SSRT_GBUFFER_COORDS, normalVS, roughness);
    // (batch 11, item A) The un-clamped roughness, which is what the retired pack pass fed
    // REBLUR_FrontEnd_GetNormHitDist: it read the G-buffer itself and computed
    // saturate(1 - glossiness), where the clamp below would have lifted a mirror-smooth pixel to
    // 0.02 and moved _NRD_GetSpecMagicCurve with it. GetNormalRoughness returns exactly
    // 1 - glossiness and glossiness comes out of a UNORM texture, so the value here is in [0, 1]
    // by construction and the pack's saturate() was a no-op -- i.e. this is the same number.
    const float nrdFrontEndRoughness = roughness;
    roughness = clamp(roughness, 0.02f, 1.0f);

#if !defined(SSRT_SPECULAR)
    float3 albedo = AlbedoTexture[SSRT_GBUFFER_COORDS].xyz;
#endif

    bool is_mirror = IsMirrorReflection(roughness);
    int most_detailed_mip = HIZ_MIN_MIP;
    float2 mip_resolution = SSRT_GetMipResolution(screen_size, most_detailed_mip);
    float z = SSRT_LoadDepth(uv * mip_resolution, most_detailed_mip);  // (audit #7) no rescale
    float3 screen_uv_space_ray_origin = float3(uv, z);
    float3 view_space_ray = ScreenSpaceToViewSpace(screen_uv_space_ray_origin, FrameBuffer::CameraProjInverse[eyeIndex]);
#if !defined(SSRT_SPECULAR) && !SHARC_UPDATE
    // (batch 1, item 2) The world-space length the encoding's half-way point stands for, i.e.
    // SSRT_HITT_REF_TEXELS texels measured in game units at this pixel's depth. The per-sample
    // encode below is then one mad and one divide: u = L / (L + hitDistRefWorld).
    //
    // texelWorldSize = viewZ * 2 / (P00 * renderWidth): the world width of one render texel at
    // this pixel's depth, taken from the projection and the render extent rather than from an
    // assumed FOV. That is the same construction ssrt_temporal.hlsl's plane tolerance uses, and
    // CameraProj is the field ProjectPosition / ProjectDirection below already depend on every
    // frame, so nothing new is being trusted. Computed here, before the grazing-angle origin
    // bias moves view_space_ray: the bias is 1.4 game units at z = 1000 and at most ~14 at
    // grazing, i.e. under 2% of a reference length that only has to be right to a texel.
    //
    // abs() on both terms so neither axis-sign convention can matter, and the max() floor keeps
    // the reference length positive for a degenerate projection or a point on the near plane. A
    // pixel there encodes u ~ 1, i.e. "treat this as a distant hit", which is the direction that
    // leaves the kernel at its unmodified width.
#   if defined(VR)
    const float hitDistWidthPerEye = float(screen_size.x) * 0.5f;  // each eye owns half the buffer
#   else
    const float hitDistWidthPerEye = float(screen_size.x);
#   endif
    const float hitDistTexelWorld = abs(view_space_ray.z) * 2.0f /
                                    max(abs(FrameBuffer::CameraProj[eyeIndex][0][0]) * hitDistWidthPerEye, 1e-6f);
    const float hitDistRefWorld = max(hitDistTexelWorld * SSRT_HITT_REF_TEXELS, 1e-6f);
    // (batch 36b) This pixel's REBLUR viewZ, for the per-sample visibility below (HitDistIsVisibility).
    const float nrdVisViewZ = SSRT_NRDViewZ(depth);
#endif
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
    float3 view_space_reflected_direction = SampleReflectionVector(view_space_ray_direction, view_space_surface_normal, roughness, SSRT_NOISE_COORDS, sample_id, SAMPLES_PER_PIXEL, pdf);
    screen_uv_space_ray_origin = ProjectPosition(view_space_ray, FrameBuffer::CameraProj[eyeIndex]);
    float3 screen_space_ray_direction = ProjectDirection(view_space_ray, view_space_reflected_direction, screen_uv_space_ray_origin, FrameBuffer::CameraProj[eyeIndex]);
    float3 world_space_reflected_direction = mul(FrameBuffer::CameraViewInverse[eyeIndex], float4(view_space_reflected_direction, 0)).xyz;
    float3 world_space_origin = mul(FrameBuffer::CameraViewInverse[eyeIndex], float4(view_space_ray, 1)).xyz;
    float world_ray_length = 0.0;
    // (audit #7) `screen_size` is already the render extent, i.e. the dispatch's own
    // bound, so the rescale is gone. The `coords >= int2(0, 0)` half is gone with it:
    // `coords` is uint2, so it was vacuously true, and comparing it against an int2 was
    // two of the file's signed/unsigned warnings (X3203).
    bool valid_ray = all(SSRT_GBUFFER_COORDS < screen_size) && !is_far_plane;  // (audit P1)
#if defined(SSRT_SPECULAR)
    // (batch 28) Skip the march where the GGX lobe is wide enough that the prefiltered cubemap
    // is already the same answer. Joining valid_ray rather than returning early is deliberate:
    // this is the one gate every non-hit path already funnels through, so a skipped pixel ends
    // on confidence 0 and the composite falls back exactly as it does for a ray that left the
    // screen -- no new path, no new state.
    //
    // Correct by construction in a way the diffuse side never was: vanilla specular already
    // comes from the cubemap, along the same reflection vector and in the same units, so this
    // picks the cheaper estimator of one quantity rather than deleting one estimator and hoping
    // another covers it. The cubemap has no parallax, though, which is why the gate belongs at
    // high roughness only -- a polished floor at roughness 0.05 would show the difference at once.
    valid_ray = valid_ray && roughness <= SpecularMaxRoughness;
#endif
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

#if !defined(SSRT_SPECULAR) && !SHARC_UPDATE
    // (batch 1, item 2) "As distant as the encoding can say", i.e. the full kernel, and it is
    // the value every path that does not produce a screen-space hit ends on:
    //
    //   * a ray that missed. Its radiance came from the cubemap / skylighting fallback, i.e.
    //     from the environment at effectively infinite distance, whose irradiance field varies
    //     over the scale of the sky rather than of local geometry. Wide averaging there is both
    //     safe and wanted -- it is the "远命中/miss = 大核敢抹" half of the mechanism.
    //   * a ray whose hit was rejected by SSRT_ValidateHit (confidence 0), including the
    //     back-face case. There is no screen-space radiance for that direction either, so the
    //     value that reaches the frame is again the fallback's.
    //   * the SHARC cache hit below, which answers from the world-space radiance cache and never
    //     runs a screen-space march at all.
    //   * a far-plane or out-of-bounds lane, whose pixel the whole denoiser chain early-outs on.
    //
    // Writing 0 for these instead would be the exact inverse of the intent: it would pin the
    // narrowest kernel onto precisely the fallback-dominated, lowest-variance, most
    // spatially-smooth regions of the screen, and leave the full kernel only where the signal
    // has real geometric structure.
    float hit_norm = 1.0f;
#endif
#if SSRT_USE_SAMPLE_LDS
    samples[SSRT_SAMPLE_SLOT][sample_id] = 0.f;
    hitNorms[SSRT_SAMPLE_SLOT][sample_id] = 1.0f;
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
#if !defined(SSRT_SPECULAR) && !SHARC_UPDATE
        // (batch 1, item 2) Encode this sample's hit distance, here and not later: `confidence`
        // is overwritten twice further down -- the cubemap fallback sets it to 1 and the ambient
        // reinjection folds the back-face occlusion into it -- and what this test needs is the
        // raw "did the march stop on real screen-space geometry" answer.
        //
        // A positive validated confidence means there *is* geometry at world_ray_length, so the
        // correlation length of the light this sample carries is that distance. Zero means the
        // radiance is the fallback's and the default 1.0 stands; see its declaration.
        //
        // u = L / (L + refWorld) rather than a linear scale, for the reason recorded at
        // SSRT_HITT_REF_TEXELS: the correlation length spans four orders of magnitude across an
        // exterior and an 8-bit linear encoding saturates over most of the screen. The divisor
        // is >= refWorld > 0 for any non-negative L, so the divide needs no guard, and the
        // result is in [0, 1) for every finite L -- strictly below the 1.0 a genuine miss
        // writes, which is what keeps "no hit" distinguishable from "a very long hit".
        //
        // (guard, same discipline as G2/G4) The finiteness test is a bit test rather than
        // isfinite() for the reason recorded at isFiniteSafe, and it is not decoration:
        // world_ray_length comes out of a projection divide, and Inf / Inf is NaN, which a UNORM
        // store would resolve to an implementation-defined value -- most likely 0, i.e. the
        // *narrowest* kernel on a pixel we know nothing about. Falling back to the 1.0 default
        // keeps a broken reconstruction on the "leave the kernel alone" side.
        if (confidence > 0.0f && isFiniteSafe(world_ray_length))
            hit_norm = world_ray_length / (world_ray_length + hitDistRefWorld);
        // (batch 36b) REBLUR's own AO convention for the hit-distance channel, used whenever
        // something downstream reads that channel as a visibility (efficiency mode's confidence,
        // the denoiser AO). Per sample: the coverage this ray claims -- its validated hit plus the
        // back-face evidence ambient reinjection already counts -- attenuated by how far away the
        // geometry is on REBLUR's own hit-distance curve, (A + B * viewZ) at roughness 1. The
        // channel then carries the mean of (1 - visCoverage), which is the "normalized hit distance
        // averaged over samples" NRD asks for (a miss is 1). The batch 11 texel encoding above stays
        // for every other configuration, bit for bit.
        float visCoverage = 0.0f;
        float proximity = 1.0f;
        [branch] if (HitDistIsVisibility != 0 && isFiniteSafe(world_ray_length))
        {
            proximity = 1.0f - REBLUR_FrontEnd_GetNormHitDist(world_ray_length, nrdVisViewZ, SSRT_NRDHitDistParams(), 1.0f);
            visCoverage = saturate(confidence + (1.0f - occlusion) * OcclusionStrength) * proximity;
        }
#endif
        float3 sampleColor = 0;
#if defined(SSRT_SPECULAR)
        // (batch 36b) Efficiency mode traces specular before this frame's merged REBLUR dispatch,
        // i.e. before this frame's image exists, so the hit takes its colour from last frame's
        // texColor at the hit's motion-reprojected position. A hit whose previous position fell off
        // screen has no colour to give and is treated as a miss (the fallback below covers it).
        float2 hitColorUV = hit.xy * FrameBuffer::DynamicResolutionParams1.xy;
        [branch] if ((RaymarchFlags & SSRT_RAYMARCH_FLAG_PREV_FRAME_COLOR) != 0 && confidence > 0.0f)
        {
            float2 prevHitUV;
            ReprojectHit(MotionVectorTexture, hit, eyeIndex, prevHitUV);
            if (any(prevHitUV < 0.0f) || any(prevHitUV > 1.0f))
                confidence = 0.0f;
            // .zw: last frame's dynamic-resolution ratio, which is what texColor was written at.
            hitColorUV = prevHitUV * FrameBuffer::DynamicResolutionParams1.zw;
        }
#endif
        if (confidence > 0.0f)
        {
#if defined(SSRT_SPECULAR)
            sampleColor = ScreenColorTextureMips.SampleLevel(LinearSampler, hitColorUV, 0).xyz;
#else
            sampleColor = ScreenColorTextureMips.SampleLevel(LinearSampler, hit.xy * FrameBuffer::DynamicResolutionParams1.xy, 0).xyz;
#endif
            sampleColor = Color::IrradianceToLinear(sampleColor);
            // (guard G1) The radiance source is kMAIN, i.e. the accumulated output of every
            // other feature in the deferred chain. SSRT has no control over what lands
            // there, and a single non-finite texel produced anywhere upstream is otherwise
            // read verbatim into the GI signal and from there into the SVGF history, which
            // has no way of ever getting rid of it. Sanitise on the way in, at the same
            // point and in the same order as ScreenSpaceGI does for its own radiance
            // (radianceDisocc.cs.hlsl:139-140).
            //
            // Placed *after* IrradianceToLinear rather than before, so it also catches an
            // Inf manufactured by the gamma/linear transform itself out of a merely huge
            // finite input. On finite, in-range radiance both calls are the identity:
            // filterNaN's ISNAN test is false for any ordered value and filterInf's
            // exponent test is false for any value below the float32 infinity, so every
            // healthy pixel keeps its exact bit pattern.
            sampleColor = filterNaN(sampleColor);
            sampleColor = filterInf(sampleColor);
#if !defined(SSRT_SPECULAR)
            sampleColor *= SharedData::ssrtSettings.DiffuseMult;
#else
            sampleColor *= SharedData::ssrtSettings.SpecularMult;
            hit_distance = world_ray_length;
#endif
        }
        // NdotV is the cosine between the surface normal and the direction *towards the
        // camera*. view_space_ray is the view-space surface position, i.e. camera->surface
        // (see :758), so its normalized form points away from the camera and the previous
        // dot(normalize(view_space_ray), N) was <= 0 for every visible surface - saturate
        // pinned it to 0 and GetSpecularOcclusionFromAmbientOcclusion below always saw a
        // fully grazing view. Negate the pre-bias ray direction (:787) to get the
        // surface->camera vector; the result now lands in (0, 1] as intended.
        const float NdotV = saturate(dot(-view_space_ray_direction, view_space_surface_normal));
#if defined(DYNAMIC_CUBEMAPS) && !defined(SSRT_SPECULAR) && !SHARC_UPDATE
        // (batch 8, cubemap fill) The (radiance, weight) pair the beta fill contributes. It has to
        // live out here because the two halves of it are produced and spent in different blocks:
        // the cubemap estimate is only built inside the fallback block below, and the weight can
        // only be applied after the ambient-reinjection block further down has finished deciding
        // what the reported confidence is.
        //
        // Both stay at zero unless the fallback block actually ran *and* beta is non-zero, and
        // that coupling is the point rather than tidiness: the block is skipped for any ray that
        // already reports confidence >= 0.999, and a weight applied without a colour to go with it
        // would take vanilla ambient away and put nothing in its place.
        float3 ambientFillColor = 0.0;
        float ambientFillBlend = 0.0;
#endif
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

                sh2 skylighting = Skylighting::sample(SharedData::skylightingSettings, SkylightingProbeArray, stbn_vec3_2Dx1D_128x128x64, SSRT_NOISE_COORDS, positionMS.xyz, world_space_reflected_direction);
                // (guard G6) max(0, z) makes the argument the zero vector whenever the
                // surface normal points away from +Z *and* has no xy component -- a
                // downward-facing horizontal surface, i.e. the underside of any overhang,
                // plus every pixel whose normal G-buffer texel is cleared or garbage.
                // normalize(0) is 0/0 = NaN in all three components, and this vector feeds
                // the skylighting cosine lobe, so the NaN reaches envColor, sampleColor and
                // the SVGF history.
                //
                // 1e-6 instead of 0 is below the resolution of the quantity it feeds: for
                // any nonzero xy the renormalised z becomes 1e-6 (was exactly 0) and the xy
                // pair is scaled by 1/sqrt(1 + 1e-12), a relative change of 5e-13 that is
                // four orders of magnitude under the float32 epsilon and therefore rounds
                // to the identical bit pattern. SphericalHarmonics::EvaluateCosineLobe is
                // linear in z, so the SH coefficient moves by ~1e-6 of its own scale.
                float3 skylightingNormal = normalize(float3(world_space_normal.xy, max(1e-6, world_space_normal.z)));
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
                envColor = lerp(envNoSkyColor, envNoSkyColor * SSRT_CubemapNormalizationRatio(directionalAmbientLuminance, envLuminance), CubemapNormalization);  // (guard G7)
                envColor += skyColor * skylightingDiffuse;
            } else {
                envLuminance = Color::RGBToLuminance(EnvReflectionsTexture.SampleLevel(LinearSampler, world_space_reflected_direction, 15));
                envColor = lerp(envColor, envColor * SSRT_CubemapNormalizationRatio(directionalAmbientLuminance, envLuminance), CubemapNormalization);  // (guard G7)
            }
#   else
            envLuminance = Color::RGBToLuminance(EnvReflectionsTexture.SampleLevel(LinearSampler, world_space_reflected_direction, 15).xyz);
            envColor = lerp(envColor, envColor * SSRT_CubemapNormalizationRatio(directionalAmbientLuminance, envLuminance), CubemapNormalization);  // (guard G7)
#   endif
            envColor = Color::IrradianceToLinear(envColor);
#   if defined(SSGI) && !defined(SSRT_SPECULAR)
            // (contact noise) THE FIX. Near-field hits stop voting, and Screen Space GI's
            // deterministic contact pass takes over the range they used to own.
            //
            // What the vote actually is. SSRT_ValidateHit leaves `occlusion` at 1 on every path but
            // one: a hit taken from behind sets `occlusion = 1 - confidence`, and `confidence` is the
            // *thickness* confidence, which is ~1 for any clean intersection at any range. So a
            // back-face hit is not a graded occlusion measurement, it is a switch: one ray reports
            // ~0 and `MultiBounceAO(albedo, ~0)` multiplies this ray's whole cubemap ambient to
            // black, while a ray that missed reports 1 and keeps all of it. With DIFFUSE_SPP = 2 the
            // pixel's ambient therefore resolves to one of {0, half, full}, and the ray directions
            // are reseeded from SharedData::FrameCount every frame (SampleRandomVector2DBaked), so
            // which one it lands on is redrawn 60 times a second. That is the flicker: a
            // full-amplitude three-level vote on a quantity that is geometrically smooth.
            //
            // Why the near field is where it hurts. Back-face hits need thin or tightly-folded
            // geometry to be common -- hair strands, foliage, cloth against skin -- and the sign
            // test `dot(hit_normal, ray_direction) > 0` is closest to its own boundary exactly
            // there, so the two rays of a contact pixel routinely land on opposite sides of it.
            //
            // Why softening the test instead would not work. The vote is a two-sample estimate of a
            // smooth visibility field; its quantisation error is +-1/2 by construction, whatever
            // shape the payload has. Reducing the payload's amplitude reduces the flicker without
            // removing it, and it removes the contact darkening along with it -- the darkening and
            // the flicker are the same number. The estimator has to stop being stochastic.
            //
            // What replaces it, and where it enters. SSGI's contact AO pass: ten depth taps on a
            // golden-angle spiral at its Contact Radius, cosine-weighted with a quadratic range
            // falloff, temporally accumulated over eight frames behind a current-frame neighbourhood
            // fence. It reaches this expression through `SsgiAoTexture` a few lines below -- the same
            // multiply that has always applied SSGI's occlusion here -- so there is nothing to
            // evaluate on this path and no per-ray or per-pixel cost at all. Deterministic frame to
            // frame, and readable without a denoiser or an upscaler behind it, which the vote was
            // not, and which is why turning SVGF's own smoothing up cannot fix this.
            //
            // Why the darkening comes out at least as strong, not weaker. The vote only darkened the
            // rays that happened to hit a back face; a ray that missed kept the *full* ambient even
            // in a tight crease. The kernel applies to the pixel, so every ray of a contact pixel is
            // attenuated, and at tight contact it reaches full occlusion at strength 1 by its own
            // calibration. Hair pressed against a face gets the same depth of shadow it had on its
            // darkest frames, on every frame.
            //
            // Why this cannot double-count.
            //  * Against the vote: the vote is suppressed for exactly the hits inside the kernel's
            //    radius, and the kernel's falloff is exactly zero at and beyond that radius. The two
            //    partition the range rather than overlapping it. Hits beyond the radius keep voting
            //    unchanged -- a ray that went behind a wall two metres away is still evidence, and
            //    it is not what was reported.
            //  * Against the traced radiance: the AO built here multiplies `envColor` only, before
            //    the `lerp(envColor, sampleColor, confidence)` below. A ray that found nearby geometry
            //    already returns that geometry's light instead of the environment's, so darkening it
            //    too would count the occlusion twice.
            //  * Against the other consumers of the contact term: there is now exactly one
            //    evaluation of it in the pipeline and every consumer reads the same texel, so
            //    `ao *= 1 - SsgiAo` below applies it once here whatever else the frame is doing.
            //
            // The radius comes from the pass's own setting, so there is one contact radius in the
            // system and the handover cannot drift out of alignment with the kernel's falloff.
            // Converted to the game units `world_ray_length` is in.
            //
            // The suppression is conditioned on that pass actually running: handing the near field to
            // a term that does not exist would leave that range shaded by nothing at all, which would
            // be strictly worse than the flicker. SsgiContactAoActive is 0 whenever Screen Space GI is
            // absent, disabled, or has its contact pass switched off, and the vote comes straight back
            // -- an SSRT-only or SSGI-off install therefore keeps P2.1 behaviour exactly rather than
            // losing the darkening and keeping the noise.
            //
            // Gated on `defined(SSGI)` rather than on the cbuffer flag alone because without SSGI
            // there is no AO texture to carry the replacement in the first place; with SSGI absent
            // this whole island compiles to the `#else` line, byte for byte what it was before.
            //
            // `!defined(SSRT_SPECULAR)` is inherited from the gate this replaced, deliberately. The
            // specular permutation does pick up the contact term -- its own `ao *= 1 - SsgiAo`
            // below is the same line -- but its `occlusion` vote is a *one*-sample estimate that
            // then goes through GetSpecularOcclusionFromAmbientOcclusion, and the handover radius
            // was never calibrated against that. Suppressing the vote there is a separate,
            // look-changing decision about the specular path, not part of moving the kernel.
            const float contactRange = max(SharedData::ssrtSettings.SsgiContactRadius, 0.1) / GAME_UNIT_TO_CM;
            const bool handOverNearField = SharedData::ssrtSettings.SsgiContactAoActive != 0 && world_ray_length < contactRange;
            const float voteOcclusion = handOverNearField ? 1.0 : occlusion;
            float ao = lerp(1.0, voteOcclusion, OcclusionStrength);
#   else
            float ao = lerp(1.0, occlusion, OcclusionStrength);
#   endif
#   if defined(SSGI)
            ao *= 1 - SSRT_FallbackAoOcclusion(SSRT_GBUFFER_COORDS, uv);  // (batch 36b) SSGI's or the denoiser's
#   endif
#   if defined(SSRT_SPECULAR)
            ao = GetSpecularOcclusionFromAmbientOcclusion(NdotV, ao, roughness);
            envColor *= ao;
            sampleColor.xyz = lerp(envColor, sampleColor.xyz, confidence);
            confidence = 1;
#   else
            // (batch 8, cubemap fill) The fork between the legacy fallback and the beta fill.
            //
            // Gated on beta rather than on AmbientReinjection so that the *literal* zero used by
            // the bit-identity harness collapses the whole `if` to its else side: at
            // CubemapFillBlend == 0 what fxc sees is textually the sequence this block always
            // was. C++ only ever sends a non-zero beta while reinjection is on, so gating on the
            // one value covers both conditions (see the cbuffer declaration).
            //
            // WHY THE FILL'S OCCLUSION IS NOT THE FALLBACK'S `ao`. The fallback multiplies the
            // per-ray back-face vote into the environment colour, because in that mode the vote
            // is the *only* way the geometry can darken the pixel. Under reinjection the vote is
            // already spent as coverage a few lines below -- `confidence += (1 - occlusion) *
            // OcclusionStrength` -- with zero radiance attached to it, which is the same
            // darkening expressed as a weight instead of a multiplier. Multiplying it in here as
            // well would count one piece of evidence twice, and quadratically: at
            // OcclusionStrength 1 a back-face ray would contribute weight (1 - 1) = 0 *and* a
            // colour multiplied by MultiBounceAO(albedo, 0). So the fill carries SSGI's occlusion
            // only, and the vote reaches the frame exactly once, as coverage.
            //
            // The re-read of SsgiAoTexture is the same texel `ao` already sampled; fxc common-
            // subexpressions the two loads, and at beta == 0 this side does not exist at all.
            [branch] if (CubemapFillBlend > 0.0) {
                float fillAo = 1.0;
#       if defined(SSGI)
                fillAo = 1 - SSRT_FallbackAoOcclusion(SSRT_GBUFFER_COORDS, uv);
#       endif
                ambientFillColor = envColor * Color::MultiBounceAO(albedo, fillAo);
                ambientFillBlend = CubemapFillBlend;
            } else {
                float3 multiBounceAO = Color::MultiBounceAO(albedo, ao);
                envColor *= multiBounceAO;
                sampleColor.xyz = lerp(envColor, sampleColor.xyz, confidence);
                confidence = 1;
            }
#   endif
        }
#endif

#if !defined(SSRT_SPECULAR) && !SHARC_UPDATE
        // (ambient reinjection) Turn this sample into a matched (radiance, coverage) pair, so
        // that the composite can spend the *rest* of the hemisphere on the vanilla ambient
        // without counting any direction twice.
        //
        // Two things change, and only in this mode:
        //
        //  * The radiance is weighted by the confidence that produced it. Without reinjection
        //    the estimator is deliberately indicator-weighted -- any hit that clears the
        //    validation threshold contributes its *full* radiance while reporting only its
        //    partial confidence -- which is harmless when the miss fraction is filled by the
        //    cubemap fallback (that path does its own `lerp(envColor, sampleColor, confidence)`
        //    and then reports confidence 1). Here .w is the weight the composite subtracts
        //    ambient with, so radiance and weight have to be the same weight or a soft hit
        //    adds more light than the ambient it displaces.
        //
        //  * A back-face hit counts towards the coverage. SSRT_ValidateHit returns confidence 0
        //    for a ray that hit geometry from behind but reports `occlusion = 1 - confidence`,
        //    i.e. it is the one rejection path that carries positive evidence: the ray did run
        //    into something, we simply have no screen-space radiance for its far side. Folding
        //    that evidence back in as coverage with zero radiance is what keeps the contact and
        //    corner darkening the cubemap fallback used to supply through `MultiBounceAO(albedo,
        //    ao)` -- without it a blocked direction would fall back to *full* ambient, which is
        //    the exact opposite of what the geometry says. OcclusionStrength keeps its meaning:
        //    it is the fraction of that evidence that is allowed to darken the pixel, matching
        //    the `ao = lerp(1.0, occlusion, OcclusionStrength)` the fallback path applies.
        //
        // SSGI's ambient occlusion is deliberately *not* folded in here even though the fallback
        // path multiplies it in: DeferredCompositeCS already shapes the re-added ambient with
        // Color::MultiBounceAO of the same SSGI AO, so doing it here as well would apply it
        // twice.
        [branch] if (SharedData::ssrtSettings.AmbientReinjection != 0) {
            // (batch 36b) ProximityCoverage (efficiency mode, deviation 3): the confidence the
            // composite subtracts the ambient with is the merged denoiser's visibility, i.e. the
            // distance-attenuated coverage computed above, so the radiance has to carry the same
            // weight or a far hit would add its light without displacing the ambient it stands for.
            [branch] if (ProximityCoverage != 0) {
                sampleColor *= confidence * proximity;
                confidence = visCoverage;
            } else {
                sampleColor *= confidence;
                confidence = saturate(confidence + (1.0 - occlusion) * OcclusionStrength);
            }
#   if defined(DYNAMIC_CUBEMAPS)
            // (batch 8, cubemap fill) Spend the beta fill, and spend it *here* -- after the line
            // above has settled what fraction of the hemisphere this sample claims to have
            // answered, because the fill is defined on the fraction it did not.
            //
            // THE ALGEBRA, and why the composite needs no change. Write c for the validated
            // confidence, o for the occlusion out-parameter, S for OcclusionStrength, L for the
            // traced radiance and F for `ambientFillColor`. The two lines above report
            //
            //     radiance   = c * L
            //     confidence = c_rep = saturate(c + (1 - o) * S)
            //
            // and DeferredCompositeCS turns that into `out = direct + (1 - c_rep) * A + c * L`
            // with A the vanilla ambient. So (1 - c_rep) is exactly the weight A is carrying, and
            // it is that weight -- not (1 - c) -- that beta has to split. Reporting
            //
            //     radiance   = c * L + w * F,   w = (1 - c_rep) * beta
            //     confidence = c_rep + w
            //
            // makes the composite's own subtraction produce
            //
            //     out = direct + (1 - c_rep - w) * A + c * L + w * F
            //         = direct + c * L + (1 - c_rep) * ((1 - beta) * A + beta * F)
            //         = direct + c * L + (1 - c_rep) * lerp(A, F, beta)
            //
            // i.e. the target formula, with the fill weights still summing to exactly the
            // unresolved fraction. That is what keeps this from reintroducing the asymmetry the
            // ambient-coupling audit is about: the removal and every one of its replacements are
            // still one partition of one hemisphere, per pixel, from one estimator.
            //
            // Splitting (1 - c_rep) rather than (1 - c) is forced, not chosen. Beta applied to
            // (1 - c) would leave A weighted by (1 - c_rep - beta * (1 - c)), which goes negative
            // wherever the back-face vote fired, and a negative ambient weight is not a slightly
            // wrong look -- it is light created out of nothing (or clamped, and then the weights
            // no longer sum to one). The visible consequence is that beta = 1 is a shade darker
            // than the legacy fallback on back-face-heavy geometry, which is the correct
            // direction: that evidence is being honoured once instead of not at all.
            [branch] if (ambientFillBlend > 0.0) {
                const float fillWeight = (1.0 - confidence) * ambientFillBlend;
                sampleColor += ambientFillColor * fillWeight;
                confidence = saturate(confidence + fillWeight);
            }
#   endif
            // (batch 36b) The fill partitions the same hemisphere, so under ProximityCoverage it has to
            // reach the denoised channel too, or the composite would keep the ambient it replaced.
            if (ProximityCoverage != 0)
                visCoverage = confidence;
        }
#endif

#if SSRT_USE_SAMPLE_LDS
        samples[SSRT_SAMPLE_SLOT][sample_id] = float4(sampleColor, confidence);
#   if !SHARC_UPDATE
        // (batch 1, item 2) The texel encoding, or (batch 36b) the REBLUR visibility of this sample.
        hitNorms[SSRT_SAMPLE_SLOT][sample_id] = HitDistIsVisibility != 0 ? 1.0f - visCoverage : hit_norm;
#   endif
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
    outColor = SSRT_SanitiseRadianceOutput(localSample);  // (guard G2)
    // (batch 11, item A) Written on both paths and unchanged: this is the R32_FLOAT surface
    // Upscaling.cpp hands DLSS-RR as its specular hit-distance guide, so it is not the pack
    // pass's private input and cannot be traded away for the packed layout.
#if defined(SSRT_CHECKERBOARD)
    // (batch 36b) Not written here: half the pixels have no specular sample this frame, so the
    // efficiency-mode unpack rebuilds this R32 surface for every pixel from REBLUR's denoised
    // hit distance (ssrt_nrd_unpack.hlsl, SSRT_UNPACK_SPEC_EFFICIENCY).
    if ((RaymarchFlags & SSRT_RAYMARCH_FLAG_CHECKER_DEBUG) != 0 && all(SSRT_GBUFFER_COORDS < screen_size))
        CheckerDebugOutput[SSRT_GBUFFER_COORDS] = float4(0, 1, 0, 1);
#else
    SSRTHitDistanceOutput[coords.xy] = hit_distance;
#endif
    if (NRDFrontEndPack != 0) {
        // The retired pack pass read hit_distance back out of the R32_FLOAT surface above --
        // an exact round trip, so no re-quantisation is owed here, unlike the diffuse case.
        // 65536 is the ray march's "no hit" sentinel, which GetNormHitDist saturates to 1.
        const float nrdViewZ = SSRT_NRDViewZ(depth);
        const float normHitDist = REBLUR_FrontEnd_GetNormHitDist(
            hit_distance, nrdViewZ, SSRT_NRDHitDistParams(), nrdFrontEndRoughness);
        SSRColorOutput[coords.xy] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(
            SSRT_Fp16RoundTrip3(outColor.rgb), normHitDist, true);
    } else {
        SSRColorOutput[coords.xy] = outColor;
    }
#elif SHARC_UPDATE
#else

    if (sample_id == 0) {
        outColor = 0.f;
        // (batch 1, item 2) Averaged alongside the radiance and the confidence, in the same
        // loop and across the same barrier.
        //
        // The *encoded* value is what gets averaged, not the raw distance, and that ordering is
        // the whole reason the encoding exists. Its consumer is linear in this number -- the
        // a-trous window's exponent is beta = strength * (1 - value) -- so averaging the encoded
        // samples computes E[f(L)], which is the mean kernel width the pixel's own rays call
        // for. Averaging raw distances first and encoding afterwards would compute f(E[L]), and
        // those are not the same thing at all where it matters: one ray hitting a wall 20 units
        // away while the other misses gives (0.06 + 1.00) / 2 = 0.53, "half the light is
        // contact, half is sky, so use half the kernel", where the raw mean of 20 and the miss
        // sentinel is dominated by the sentinel and reports the full kernel.
        float hitNormSum = 0.f;
        for (int i = 0; i < SAMPLES_PER_PIXEL; ++i) {
            outColor.xyz += samples[SSRT_SAMPLE_SLOT][i].xyz;
            outColor.w += samples[SSRT_SAMPLE_SLOT][i].w;
            hitNormSum += hitNorms[SSRT_SAMPLE_SLOT][i];
        }
        outColor.xyz /= SAMPLES_PER_PIXEL;
        outColor.w = saturate(outColor.w / SAMPLES_PER_PIXEL);
        // (guard G2) After the average, not before: a single poisoned SPP slot turns the
        // whole sum non-finite, so the useful place to cut is the resolved value.
        outColor = SSRT_SanitiseRadianceOutput(outColor);
        // (batch 11, item A) The encoded hit distance, hoisted out of the store below because
        // the packed path needs the same number.
        const float hitNormMean = saturate(hitNormSum / SAMPLES_PER_PIXEL);
        if (NRDFrontEndPack != 0) {
            // Every line here is the retired ssrt_nrd_pack.hlsl, moved. The reciprocal
            // texel-space encoding is decoded back to world units through the *pack's* own
            // texel-footprint construction, not through hitDistTexelWorld above: the two are
            // different expressions (this one takes viewZ from the depth buffer and the render
            // width un-truncated, where the encode side used view_space_ray.z and the truncated
            // screen_size), and reproducing the consumer's arithmetic rather than the producer's
            // is what keeps the result bit-identical to what REBLUR was fed before.
            const float u = SSRT_Unorm8RoundTrip(hitNormMean);
            const float tTexels = SSRT_HITT_REF_TEXELS * u / max(1.0 - u, 1e-4);
            const float nrdViewZ = SSRT_NRDViewZ(depth);
            const float nrdRenderWidth = SharedData::BufferDim.x * FrameBuffer::DynamicResolutionParams1.x;
            // viewZ is clamped before the footprint multiply: a sky pixel carries the 3.4e38
            // sentinel, and 2 * 3.4e38 overflows to +inf, which a u = 0 texel would then turn
            // into 0 * inf = NaN. 1e7 game units is far beyond the denoising range, so the clamp
            // changes nothing for any pixel REBLUR actually reads.
            const float nrdTexelWorld = min(abs(nrdViewZ), 1e7) * 2.0 /
                                        max(abs(FrameBuffer::CameraProj[0][0][0]) * nrdRenderWidth, 1e-6);
            float normHitDist = REBLUR_FrontEnd_GetNormHitDist(
                tTexels * nrdTexelWorld, nrdViewZ, SSRT_NRDHitDistParams(), 1.0);
            // (batch 36b) The samples already carry REBLUR's normalized visibility, so the mean is
            // the front-end value as it stands. Floored at NRD_EPS like GetNormHitDist's own result.
            if (HitDistIsVisibility != 0)
                normHitDist = max(hitNormMean, NRD_EPS);
            SSRColorOutput[coords.xy] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(
                SSRT_Fp16RoundTrip3(outColor.xyz), normHitDist, true);
        } else {
            SSRColorOutput[coords.xy] = outColor;
        }
        // (ambient reinjection) The same value SSRColorOutput.w carries, on a surface the
        // denoiser does not touch. Written unconditionally -- including the plain 0 a
        // far-plane or out-of-bounds lane resolves to, so the surface stays deterministic
        // for every texel the dispatch covers and a sky pixel keeps its full vanilla
        // ambient. Already saturated above and sanitised by G2, so the UNORM store cannot
        // see a NaN.
        SSRTConfidenceOutput[SSRT_GBUFFER_COORDS] = outColor.w;
        // (batch 1, item 2) Written unconditionally, sky and out-of-bounds lanes included, so
        // the surface is deterministic for every texel the dispatch covers -- those lanes
        // resolve to the 1.0 default, which the denoiser never reads because it early-outs on
        // the far plane. The value is already in [0, 1] by construction (every contributing
        // sample was saturated), so the UNORM store cannot clip and cannot see a NaN.
        // (batch 11, item A) Still written on the packed path, and deliberately so. It costs one
        // byte per texel, it is what the Buffer Viewer shows for this surface, and it is the one
        // thing that keeps the entry honest rather than displaying whatever the last SVGF frame
        // left there. hitNormMean is the value the store always carried, hoisted above.
        SSRTDiffuseHitDistanceOutput[SSRT_GBUFFER_COORDS] = hitNormMean;
#   if defined(SSRT_CHECKERBOARD)
        if ((RaymarchFlags & SSRT_RAYMARCH_FLAG_CHECKER_DEBUG) != 0 && all(SSRT_GBUFFER_COORDS < screen_size))
            CheckerDebugOutput[SSRT_GBUFFER_COORDS] = float4(1, 0, 0, 1);
#   endif
    }
#endif
}
