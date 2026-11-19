cbuffer AutoExposureCB : register(b1)
{
	float2 AdaptArea;
	float2 AdaptationRange;
	float AdaptLerp;
	float ExposureCompensation;
	float PurkinjeStartEV;
	float PurkinjeMaxEV;
	float PurkinjeStrength;

	float pad[3];

	// (49h stage 2) "Exposure Mode: Physical" (HistogramAutoExposure::AutoExposureCB). Mode 0 = Classic: none of these is read and the
	// histogram / average run the code from before, bit for bit.
	float PhysMinLogLum;    // log2 luminance of the histogram's bin 1
	float PhysLogLumRange;  // log2 range of bins 1..255
	uint PhysMode;          // 0 Classic, 1 Physical
	float PhysDeltaTime;    // real seconds since the last frame
	float PhysTrimLow;      // fraction of the samples ignored at the dark end
	float PhysTrimHigh;     // ... and at the bright end
	float PhysCentreWeight; // 0 = even, 1 = the centre counts 3x (Gaussian, sigma 0.3 of the half-diagonal)
	float PhysDeadband;     // 1 = hold changes below 0.1-0.35 EV
	float PhysTauDark;      // seconds, adapting into darker scenes
	float PhysTauBright;    // seconds, adapting into brighter scenes
	float PhysSlewDark;     // EV per second, at most
	float PhysSlewBright;
	float PhysAlpha;        // compensation curve: -alpha softplus(EVref - ev) + beta softplus(ev - EVhi)
	float PhysEVRef;
	float PhysEVMin;        // the adaptation stops here (below it the scene darkens 1:1)
	float PhysBeta;
	float PhysEVHi;
	float PhysAdaptMinEV;  // adapted luminance clamp, EV
	float PhysAdaptMaxEV;
	uint PhysFlags;        // 1 snap to the target, 2 hold (menus)
};
