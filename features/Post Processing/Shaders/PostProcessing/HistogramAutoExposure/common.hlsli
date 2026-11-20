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

	// (49j) "Physical Brightness" mode (HistogramAutoExposure::AutoExposureCB; replaces 49h's Physical mode). Mode 0 = Classic: none of these is
	// read and the histogram / average run the code from before, bit for bit.
	float PhysMinLogLum;      // log2 cd/m^2 of the histogram's bin 1 (the histogram counts luminance / PhysPreExposure)
	float PhysLogLumRange;    // log2 range of bins 1..255
	uint PhysMode;            // 0 Classic, 1 Physical Brightness
	float PhysDeltaTime;      // real seconds since the last frame
	float PhysTrimLow;        // fraction of the samples ignored at the dark end
	float PhysWinsorEV;       // samples brighter than the median + this count as the median + this (design 2.2.1)
	float PhysCentreWeight;   // 0 = even, 1 = the centre counts 3x (Gaussian, sigma 0.3 of the half-diagonal)
	float PhysDeadband;       // 1 = hold changes below 0.1-0.35 EV
	float PhysTauDark;        // seconds, adapting into darker scenes
	float PhysTauBright;      // seconds, adapting into brighter scenes
	float PhysSlewDark;       // EV per second, at most
	float PhysSlewBright;
	float PhysPreExposure;    // P of this frame: every light was written x P (PhysicalBrightness)
	float PhysNightDarkness;  // t: display key = (1 - t) x the vanilla offset + t x the physiological one (design 2.2.2)
	float PhysSlope;          // s
	float PhysLogFullAdapt;   // log2 L_full
	float PhysBrightBoost;    // beta
	float PhysLogBright;      // log2 L_bright
	float PhysKeyVanilla;     // the vanilla reference's night offset, EV (C++)
	uint PhysFlags;           // 1 snap to the target, 2 hold (menus), 4 Minimum Visibility as a global floor (no Local Exposure)
	float PhysCompensation;   // EV
	float PhysFloorLog;       // log2 of Minimum Visibility (very negative = off)
	float PhysAdaptMinLog;    // adapted luminance clamp, log2 cd/m^2
	float PhysAdaptMaxLog;
};
