cbuffer AutoExposureCB : register(b1)
{
	float2 AdaptArea;
	float2 AdaptationRange;
	float AdaptLerp;
	float ExposureCompensation;
	float PurkinjeStartEV;
	float PurkinjeMaxEV;
	float PurkinjeStrength;

#if defined(EYE_MODE)
	// (50a) Eye Model (physical lighting redo, stage 1): the eye variants only (EYE_MODE), so the classic shaders compile from the same text as
	// before. R5 (49i f3416d4dd): a float3 pad (the old float pad[3] took three registers), the eye fields from offset 48 (C++ asserts).
	float3 pad;
	float EyeLogLo;          // log2 of the user's Adaptation Range minimum (scene luminance, x 0.125 as Classic)
	float EyeLogHi;          // ... maximum
	float EyeNightDarkness;  // 1 = the scene's own darkness below the range kept (as Classic), 0 = fully adapted
	float EyeMinVisibility;  // soft floor of the shown average, as a fraction of middle grey (0 = off)
	float EyeTauBright;      // adaptation time constants (s) toward brighter / darker scenes
	float EyeTauDark;
	float EyeMaxRate;        // largest change of the adapted level (EV per second; 0 = no limit)
	float EyeDeadband;       // changes of the measured level smaller than this (EV) are ignored
	float EyeWobble;         // Scene Scale Wobble self-test: the frame's scale factor this frame (1 = off)
	float EyeMesopic;        // per-pixel mesopic shift strength (0 = off: the Classic Purkinje settings apply)
	float EyeDt;             // real seconds since the last frame
	uint EyeFlags;           // kEyeSnap / kEyeGlobalFloor / kEyeInstant
#else
	float pad[3];
#endif
};

#if defined(EYE_MODE)
static const uint kEyeSnap = 1u;         // a loading screen / time skip / mode switch: the adapted level jumps to the measured one
static const uint kEyeGlobalFloor = 2u;  // Minimum Visibility on the global level (Local Exposure is not in the chain)
static const uint kEyeInstant = 4u;      // adaptation forced instant (the self-test)
// The eye's histogram: a wider range than Classic's (-8 .. +13 log2): -20 .. +16 log2 of scene luminance, 254 bins of 0.142 EV.
static const float kEyeMinLogLum = -20.0;
static const float kEyeLogLumRange = 36.0;
#endif
