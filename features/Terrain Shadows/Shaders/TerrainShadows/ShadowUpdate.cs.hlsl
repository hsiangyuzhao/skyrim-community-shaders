Texture2D<float> TexHeight : register(t0);
RWTexture2D<float2> RWTexShadowHeights : register(u0);

cbuffer ShadowUpdateCB : register(b0)
{
	float2 LightPxDir : packoffset(c0.x);   // direction on which light descends, from one pixel to next via dda
	float2 LightDeltaZ : packoffset(c0.z);  // per lightUVDir, normalised, [upper, lower] penumbra, should be negative
	uint StartPxCoord : packoffset(c1.x);
	float2 PxSize : packoffset(c1.y);
	float BlendWeight : packoffset(c1.w);  // (batch 40) share of this update; 0.5 = the old fixed blend, 1 = full refresh
	float2 PosRange : packoffset(c2.x);
	float2 ZRange : packoffset(c2.z);
}

// (batch 40) Two update kernels, chosen by Terrain Shadows > "Stable Soft Edges":
// - LEGACY_UPDATE: the kernel this fork shipped up to batch 39.
// - default: upstream jiayev #2729 (652521d42 + df687ca41, 2026-09-23). Reads the heightmap texel
//   the ray stands on instead of interpolating across the ray (which made the penumbra crawl as the
//   sun moved), clamps instead of dropping edge texels, wraps the minor axis by modulo, and fixes a
//   race in the parallel scan (a thread could read a neighbour's slot in the same step it was being
//   written; the combine now happens after a barrier).

#if defined(LEGACY_UPDATE)

float GetInterpolatedHeight(float2 pxCoord, bool isVertical)
{
	uint2 dims;
	TexHeight.GetDimensions(dims.x, dims.y);

	// oob is fine
	int2 lerpPxCoordA = int2(pxCoord - .5 * float2(isVertical, !isVertical));
	int2 lerpPxCoordB = int2(pxCoord + .5 * float2(isVertical, !isVertical));
	float heightA = TexHeight[lerpPxCoordA];
	float heightB = TexHeight[lerpPxCoordB];

	// normalize
	heightA = lerp(PosRange.x, PosRange.y, heightA);
	heightB = lerp(PosRange.x, PosRange.y, heightB);
	heightA = (heightA - ZRange.x) / (ZRange.y - ZRange.x);
	heightB = (heightB - ZRange.x) / (ZRange.y - ZRange.x);

	bool inBoundA = all(lerpPxCoordA > 0);
	bool inBoundB = all(lerpPxCoordB < int2(dims));
	if (inBoundA && inBoundB)
		return lerp(heightA, heightB, frac((isVertical ? pxCoord.x : pxCoord.y) - .5));
	else if (!inBoundA)
		return heightB;
	else
		return heightA;
}

float2 GetInterpolatedHeightRW(float2 pxCoord, bool isVertical)
{
	uint2 dims;
	RWTexShadowHeights.GetDimensions(dims.x, dims.y);

	int2 lerpPxCoordA = int2(pxCoord - .5 * float2(isVertical, !isVertical));
	int2 lerpPxCoordB = int2(pxCoord + .5 * float2(isVertical, !isVertical));
	float2 heightA = RWTexShadowHeights[lerpPxCoordA];
	float2 heightB = RWTexShadowHeights[lerpPxCoordB];

	bool inBoundA = all(lerpPxCoordA > 0);
	bool inBoundB = all(lerpPxCoordB < int2(dims));
	if (inBoundA && inBoundB)
		return lerp(heightA, heightB, frac((isVertical ? pxCoord.x : pxCoord.y) - .5));
	else if (!inBoundA)
		return heightB;
	else
		return heightA;
}

#define NTHREADS 128
groupshared float2 g_shadowHeight[NTHREADS];

[numthreads(NTHREADS, 1, 1)] void main(const uint gtid
									   : SV_GroupThreadID, const uint gid
									   : SV_GroupID) {
	uint2 dims;
	TexHeight.GetDimensions(dims.x, dims.y);

	bool isVertical = abs(LightPxDir.y) > abs(LightPxDir.x);
	float2 lightUVDir = LightPxDir * PxSize;

	uint2 rayStartPxCoord = isVertical ? uint2(gid, StartPxCoord) : uint2(StartPxCoord, gid);
	float2 rayStartUV = (rayStartPxCoord + .5) * PxSize;
	float2 rawThreadUV = rayStartUV + gtid * lightUVDir;

	bool2 isUVinRange = (rawThreadUV > 0) && (rawThreadUV < 1);
	bool isValid = isVertical ? isUVinRange.y : isUVinRange.x;

	float2 threadUV = rawThreadUV - floor(rawThreadUV);  // wraparound
	float2 threadPxCoord = threadUV * dims;

float2 pastHeights = 0.0f.xx;
	if (isValid) {
		pastHeights = RWTexShadowHeights[uint2(threadPxCoord)];

		// bifilter
		float2 heights = GetInterpolatedHeight(threadPxCoord, isVertical).xx;

		// fetch last dispatch
		if (gtid == 0 && all(floor(rawThreadUV - lightUVDir) == floor(rawThreadUV))) {
			float2 sampleHeights = GetInterpolatedHeightRW(threadPxCoord - LightPxDir, isVertical) + LightDeltaZ;
			heights = heights.x > sampleHeights.x ? heights : sampleHeights;
		}

		g_shadowHeight[gtid] = heights;
	}

	GroupMemoryBarrierWithGroupSync();

	// simple parallel scan
	[unroll] for (uint offset = 1; offset < NTHREADS; offset <<= 1)
	{
		if (isValid && gtid >= offset) {
			if (all(floor(rawThreadUV - lightUVDir * offset) == floor(rawThreadUV)))  // no wraparound happened
			{
				float2 currentHeights = g_shadowHeight[gtid];
				float2 sampleHeights = g_shadowHeight[gtid - offset] + LightDeltaZ * offset;
				g_shadowHeight[gtid] = currentHeights.x > sampleHeights.x ? currentHeights : sampleHeights;
			}
		}
		GroupMemoryBarrierWithGroupSync();
	}

	// save
	if (isValid) {
		RWTexShadowHeights[uint2(threadPxCoord)] = lerp(pastHeights, g_shadowHeight[gtid], BlendWeight);
	}
}
#else

float2 GetInterpolatedHeightRW(float2 pxCoord, bool isVertical)
{
	uint2 dims;
	RWTexShadowHeights.GetDimensions(dims.x, dims.y);

	int2 lerpPxCoordA = int2(floor(pxCoord - .5 * float2(isVertical, !isVertical)));
	int2 lerpPxCoordB = lerpPxCoordA + int2(isVertical, !isVertical);
	lerpPxCoordA = clamp(lerpPxCoordA, int2(0, 0), int2(dims) - 1);
	lerpPxCoordB = clamp(lerpPxCoordB, int2(0, 0), int2(dims) - 1);
	float2 heightA = RWTexShadowHeights[lerpPxCoordA];
	float2 heightB = RWTexShadowHeights[lerpPxCoordB];

	return lerp(heightA, heightB, frac((isVertical ? pxCoord.x : pxCoord.y) - .5));
}

#define NTHREADS 128
groupshared float2 g_shadowHeight[NTHREADS];

// Offsets can span more than one dimension on small heightmaps, so wrap by modulo rather than a single step.
uint GetWrappedCoord(int coord, uint dimension)
{
	uint magnitude = uint(abs(coord)) % dimension;
	return coord < 0 ? (dimension - magnitude) % dimension : magnitude;
}

[numthreads(NTHREADS, 1, 1)] void main(const uint gtid : SV_GroupThreadID, const uint gid : SV_GroupID) {
	uint2 dims;
	TexHeight.GetDimensions(dims.x, dims.y);

	bool isVertical = abs(LightPxDir.y) > abs(LightPxDir.x);
	int majorStep = (isVertical ? LightPxDir.y : LightPxDir.x) > 0.0 ? 1 : -1;
	int majorPxCoord = int(StartPxCoord) + int(gtid) * majorStep;
	uint majorDimension = isVertical ? dims.y : dims.x;
	uint minorDimension = isVertical ? dims.x : dims.y;
	bool isValid = majorPxCoord >= 0 && majorPxCoord < int(majorDimension);
	float minorDirection = isVertical ? LightPxDir.x : LightPxDir.y;
	float minorOffset = 0.5 + gtid * minorDirection;
	int rawMinorPxCoord = int(gid) + int(floor(minorOffset));
	uint minorPxCoord = GetWrappedCoord(rawMinorPxCoord, minorDimension);
	uint2 outputPxCoord = isVertical ? uint2(minorPxCoord, majorPxCoord) : uint2(majorPxCoord, minorPxCoord);
	float rayWrap = floor(float(rawMinorPxCoord) / minorDimension);

	float2 pastHeights = 0.0;
	if (isValid) {
		if (BlendWeight < 1.0)
			pastHeights = RWTexShadowHeights[outputPxCoord];

		float terrainHeight = lerp(PosRange.x, PosRange.y, TexHeight[outputPxCoord]);
		float2 heights = ((terrainHeight - ZRange.x) / (ZRange.y - ZRange.x)).xx;

		// fetch last dispatch
		int previousMajorCoord = majorPxCoord - majorStep;
		float previousMinorCoord = gid + 0.5 - minorDirection;
		if (gtid == 0 && previousMajorCoord >= 0 && previousMajorCoord < int(majorDimension) && previousMinorCoord >= 0.0 && previousMinorCoord < float(minorDimension)) {
			float2 previousPxCoord = isVertical ? float2(previousMinorCoord, previousMajorCoord + 0.5) : float2(previousMajorCoord + 0.5, previousMinorCoord);
			float2 sampleHeights = GetInterpolatedHeightRW(previousPxCoord, isVertical) + LightDeltaZ;
			heights = max(heights, sampleHeights);
		}

		g_shadowHeight[gtid] = heights;
	}

	GroupMemoryBarrierWithGroupSync();

	// simple parallel scan
	[unroll] for (uint offset = 1; offset < NTHREADS; offset <<= 1)
	{
		bool combineHeights = false;
		float2 currentHeights = 0.0;
		float2 sampleHeights = 0.0;
		if (isValid && gtid >= offset) {
			int previousMinorPxCoord = int(gid) + int(floor(0.5 + (gtid - offset) * minorDirection));
			if (floor(float(previousMinorPxCoord) / minorDimension) == rayWrap) {
				combineHeights = true;
				currentHeights = g_shadowHeight[gtid];
				sampleHeights = g_shadowHeight[gtid - offset] + LightDeltaZ * offset;
			}
		}
		GroupMemoryBarrierWithGroupSync();
		if (combineHeights) {
			g_shadowHeight[gtid] = max(currentHeights, sampleHeights);
		}
		GroupMemoryBarrierWithGroupSync();
	}

	// save
	if (isValid) {
		RWTexShadowHeights[outputPxCoord] = lerp(pastHeights, g_shadowHeight[gtid], BlendWeight);
	}
}

#endif  // LEGACY_UPDATE
