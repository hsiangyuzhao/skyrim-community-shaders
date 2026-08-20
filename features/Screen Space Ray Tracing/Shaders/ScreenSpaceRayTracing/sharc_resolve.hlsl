/*
 * Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

#include "Common/FrameBuffer.hlsli"
#include "Common/Game.hlsli"
#include "Common/SharedData.hlsli"

#define SHARC_ENABLE_64_BIT_ATOMICS 1
#include "ScreenSpaceRayTracing/sharc/SharcCommon.h"

#define LINEAR_BLOCK_SIZE 256

// (audit P6) u1..u4, matching ssrt_raymarch.hlsl and the `uavs` array in
// ScreenSpaceRayTracing::DrawSSRTDiffuse -- this pass is dispatched with the same
// bindings as the SHARC update/render raymarch. Was u2..u5, above the now-deleted
// SSRPDFOutput (u1) and SSRTHitDistanceOutput (u2).
RWStructuredBuffer<uint2> u_SharcHashEntriesBuffer : register(u1);
RWStructuredBuffer<uint> u_HashCopyOffsetBuffer : register(u2);
RWStructuredBuffer<uint4> u_SharcVoxelDataBuffer : register(u3);
RWStructuredBuffer<uint4> u_SharcVoxelDataBufferPrev : register(u4);

[numthreads(LINEAR_BLOCK_SIZE, 1, 1)]
void main(in uint2 did : SV_DispatchThreadID)
{
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

    SharcResolveParameters resolveParameters;
    resolveParameters.accumulationFrameNum = 8;
    resolveParameters.staleFrameNumMax = 16;
    resolveParameters.cameraPositionPrev = FrameBuffer::CameraPreviousPosAdjust[0].xyz;
    resolveParameters.enableAntiFireflyFilter = true;

    SharcResolveEntry(did.x, sharcParameters, resolveParameters);
}