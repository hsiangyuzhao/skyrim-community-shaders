#include "Common/BRDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/Math.hlsli"
#include "Common/PBR.hlsli"
#include "Common/SharedData.hlsli"

#define WATER_ROUGHNESS 0.1f
#define WATER_F0 0.02f

namespace Skin
{
	float CalculateCurvature(float3 N)
	{
		const float3 dNdx = ddx(N);
		const float3 dNdy = ddy(N);
		return length(float2(dot(dNdx, dNdx), dot(dNdy, dNdy)));
	}
	
#if defined(PSHADER)
	cbuffer SkinPerGeometry : register(b7)
	{
		float4 skinPerGeometry;
		// (44a) Wet Skin, per character: x = swim wetness with the hold and the dry-off timer (New
		// model), y = deepest submersion since entering the water, z = 1 while the character is
		// in the water, w unused.
		float4 wetSkinPerGeometry;
		// (44a) Wet Skin settings (WetSkin.cpp, WetSkin::GPUParams).
		float4 wetSkinParams[6];
		// (44c) Character Wetness, per character / per geometry: x = seconds since leaving the
		// water (0 while in it), y = item class prior from the worn item's keywords (0 unknown,
		// 1 cloth, 2 leather, 3 metal, 4 fur), z = 1 while in the water, w unused.
		float4 wetCharPerGeometry;
		// (44c) Character Wetness: hair and gear settings (WetSkin::GPUParams::c, layout there).
		float4 wetCharParams[12];
		// (46a) Dynamic Snow, per character: x = own snow amount (< 0: none kept, use the global
		// amount), y = amount on the part that was under water, z = height of that part above the
		// feet (0 = none), w = the feet's world Z. Read only under DynamicSnow::FlagCharacterSnowState.
		float4 snowPerGeometry;
		// (46a) See-through clothes in water: x = water surface world Z, y = 1 when this draw is tested
		// against the depth from before the water (its part below x is tinted), z = tint strength.
		float4 waterPerGeometry;
	};

	/// (46a) This character's snow amount at a world height (absolute Z), or a_global when the
	/// character has none kept. The washed part (below the water line) has its own amount, with a
	/// 3-unit soft edge.
	float CharacterSnowAmount(float worldZ, float a_global)
	{
		const float4 s = snowPerGeometry;
		[branch] if (s.x < 0.0)
			return a_global;
		[branch] if (s.z > 0.0)
			return lerp(s.y, s.x, smoothstep(s.z - 3.0, s.z + 3.0, worldZ - s.w));
		return s.x;
	}

	// (44a) Wet Skin model: New (1) or Original (0). (47f) Physical (2) is a New model too.
	bool WetSkinNewModel()
	{
		return wetSkinParams[0].x > 0.5;
	}

	// (47f) Character Wetness "Physical": the CPU keeps each character's sweat, rain and swim water and hands over its
	// look in the same parameters (WetSkin::PhysicalLook, Skin::BSLightingShader_SetupGeometry):
	//   wetSkinParams[0].w = the rain level itself (not a weight), wetSkinParams[0].y = manual water (0 for oil),
	//   wetSkinPerGeometry.w = droplet driver, wetCharPerGeometry.w = rivulet driver, wetCharParams[0].xy = runoff /
	//   evaporation time, wetCharParams[11].z = the character's strongest source (droplet gate), [11].w = oil.
	bool WetPhysical()
	{
		return wetSkinParams[0].x > 1.5;
	}

	/// (47f) Physical: how much of the character's droplet / rivulet driver this pixel gets: its own wetness against
	/// the character's strongest source (below a water line, a sweatier forehead ...).
	float WetPhysicalGate(float pixelWetness)
	{
		return saturate(pixelWetness / max(wetCharParams[11].z, 0.02));
	}

	/// (47f) Physical, Manual Look "Oil": the oil on the skin, 0..1 (0 in every other model).
	float WetOil()
	{
		return WetPhysical() ? saturate(wetCharParams[11].w) : 0.0;
	}

	// (47c) "Wetness: Skip Invisible Work" (wetCharParams[11].x): leave out work whose result is
	// multiplied by 0 (faded noise octaves, sub-pixel rivulets, rain detail under a roof, dry hair's
	// clumps). The picture stays the same.
	bool WetSkipInvisible()
	{
		return wetCharParams[11].x > 0.5;
	}

	// (47c) "Wetness: Faster Noise & Distance LOD" (wetCharParams[11].y): the cheaper patch noise and
	// no droplets / rivulets on characters far away.
	bool WetFastNoiseLOD()
	{
		return wetCharParams[11].y > 0.5;
	}

	/// (47c) Distance LOD for droplets and rivulets: 1 up close, fading to 0 while one pixel grows
	/// from 0.8 to 1.2 game units (about 1.1 to 1.7 cm) on the character. pixelWorld: the world
	/// size of a pixel at this distance (view-ray derivatives, not foreshortened). Around 13 m and
	/// farther at 1440p; a third-person player (up to about 8.5 m) never reaches it. Off: 1.
	float WetDetailLOD(float pixelWorld)
	{
		[branch] if (!WetFastNoiseLOD())
			return 1.0;
		return 1.0 - smoothstep(0.8, 1.2, pixelWorld);
	}

	/// (44c) Twomey wet albedo, d = 0..2: d <= 1 moves toward one soaking step A / (1.77 - 0.77 A),
	/// above 1 a second step on top (the same formula again), so a "darker than soaked" slider
	/// stays a plausible colour.
	float3 WetDarkenAlbedo(float3 A, float d)
	{
		A = max(A, 0.0);
		const float3 a1 = lerp(A, A / (1.77 - 0.77 * saturate(A)), saturate(d));
		[branch] if (d > 1.0)
			return lerp(a1, a1 / (1.77 - 0.77 * saturate(a1)), saturate(d - 1.0));
		return a1;
	}

	/// (44c) Swimming wetness of one material class with its own drying multiplier (skin = 1,
	/// hair, cloth, leather, metal, fur from Character Wetness): full in the water, held for
	/// Hold x mult after leaving it, then a smooth dry-off over Dry x mult. progress: 0..1 how far
	/// the dry-off is.
	float CharSwimClass(float dryMult, out float progress)
	{
		progress = 0.0;
		if (wetCharPerGeometry.z > 0.5)
			return 1.0;
		// (47f) Physical: the runoff (45 % of a soaked skin's water, within seconds) and the evaporation of the rest, both
		// slower by this class's dry multiplier (WetSkin::PhysicalSwimLevel).
		[branch] if (WetPhysical())
		{
			const float tp = max(wetCharPerGeometry.x, 0.0) / max(dryMult, 0.25);
			const float evaporated = saturate(tp / max(wetCharParams[0].y, 1.0));
			progress = evaporated;
			return 0.45 * exp(-tp / max(wetCharParams[0].x, 0.1)) + 0.55 * (1.0 - evaporated);
		}
		const float hold = max(wetCharParams[0].x, 0.0) * dryMult;
		const float dry = max(wetCharParams[0].y * dryMult, 1.0);
		const float t = saturate((wetCharPerGeometry.x - hold) / dry);
		progress = t;
		return 1.0 - t * t * (3.0 - 2.0 * t);
	}

	/// (44c) CharDryWetness for hair and gear: their own swimming wetness (CharSwimClass), the
	/// water line raised / lowered by levelOffset (cloth wicking, receding while drying), and
	/// their share of the sweat.
	float CharDryWetnessClass(float worldZ, float sweat, float swimClass, float levelOffset, float sweatShare, out float wSwim, out float wManual, out float wSweat)
	{
		const float waterLevel = skinPerGeometry.z + wetSkinPerGeometry.y + levelOffset;
		const float swim = wetSkinPerGeometry.y > 0.0 ? swimClass * (1.0 - smoothstep(waterLevel - 2.5, waterLevel + 2.5, worldZ)) : 0.0;
		wSwim = saturate(swim * wetSkinParams[0].z);
		wManual = saturate(wetSkinParams[0].y);
		wSweat = saturate(sweat * wetSkinParams[1].x * sweatShare);
		return 1.0 - (1.0 - wSwim) * (1.0 - wManual) * (1.0 - wSweat);
	}

	// ---- (44c) Gear: soft material classes ------------------------------------------------------
	//
	// wetCharParams (WetSkin::GPUParams::c):
	//   [0] x hold (s), y dry-off (s), z hair dry-time multiplier, w hair sweat share
	//   [1] x wet hair on, y hair darkening, z highlight sharpness, w highlight strength
	//   [2] x secondary highlight loss, y scatter loss, z clumping, w clump count
	//   [3] x clump streakiness, y gap darkening, z debug class tint, w unused
	//   [4] x gear mode (0 off, 1 New, 2 42c), y overall sheen, z patch contrast, w cloth wicking
	//   [5] x gear sweat share, y item keyword trust (0 = pixels only), z leather droplets, w leather rivulets
	//   [6] x metal droplets, y metal rivulets, z unused, w unused
	//   [7..10] cloth, leather, metal, fur: x darkening, y sheen, z roughness floor, w dry-time multiplier
	//   [11] unused

	/// Class weights: x cloth, y leather, z metal (x + y + z = 1), w fur / other (blends the result
	/// toward the fur parameters). m0: metal evidence (TruePBR metallic, env mask); g0: gloss;
	/// l0: leather-vs-cloth from the gloss. The item keyword prior (44f) nudges l0 / m0 without
	/// overriding the pixel: a buckle on a shirt still reads as metal.
	float4 GearClassWeights(float m0, float g0, float l0, bool fur)
	{
		const float prior = wetCharPerGeometry.y;
		const float trust = saturate(wetCharParams[5].y);
		float f = fur ? 1.0 : 0.0;
		[branch] if (trust > 0.0 && prior > 0.5)
		{
			if (prior < 1.5)
				l0 *= 1.0 - trust;  // cloth
			else if (prior < 2.5)
				l0 = max(l0, trust);  // leather
			else if (prior < 3.5)
				m0 = max(m0, trust * smoothstep(0.25, 0.6, g0));  // metal without an env mask
			else
				f = max(f, trust);  // fur (animals)
		}
		const float m = saturate(m0);
		l0 = saturate(l0);
		return float4((1.0 - m) * (1.0 - l0), (1.0 - m) * l0, m, f);
	}

	float4 GearClassParams(float4 w)
	{
		return lerp(w.x * wetCharParams[7] + w.y * wetCharParams[8] + w.z * wetCharParams[9], wetCharParams[10], w.w);
	}

	/// G-buffer gloss cap per class (cloth 0.55, leather 0.70, metal 0.75, fur 0.50).
	float GearClassGlossCap(float4 w)
	{
		return lerp(w.x * 0.55 + w.y * 0.70 + w.z * 0.75, 0.50, w.w);
	}

	/// (44b) Wet Skin New model: one character's wetness without the rain, the same for skin, hair
	/// and gear: swimming (in the water, then held, then drying off; below the deepest water line
	/// only), the manual slider and sweat, each weighted, combined as independent layers. Rain is
	/// added by the caller, where the sky occlusion is known: only the rain is sky-occluded.
	/// sweat: Advanced Skin's sweat for this pixel (Skin::GetWetness().x).
	float CharDryWetness(float worldZ, float sweat, out float wSwim, out float wManual, out float wSweat)
	{
		const float waterLevel = skinPerGeometry.z + wetSkinPerGeometry.y;
		const float swim = wetSkinPerGeometry.x * (1.0 - smoothstep(waterLevel - 2.5, waterLevel + 2.5, worldZ));
		wSwim = saturate(swim * wetSkinParams[0].z);
		wManual = saturate(wetSkinParams[0].y);
		wSweat = saturate(sweat * wetSkinParams[1].x);
		return 1.0 - (1.0 - wSwim) * (1.0 - wManual) * (1.0 - wSweat);
	}
#endif
#if defined(SKIN)
	Texture2D<float4> TexSkinDetailNormal : register(t72);

	struct SkinSurfaceProperties
	{
		float RoughnessPrimary;
		float RoughnessSecondary;
		float3 F0;
		float SecondarySpecIntensity;
		float Curvature;
		float3 Albedo;
		float Thickness;
		float3 SubsurfaceColor;
		float AO;
		float FuzzRoughness;
		float3 FuzzColor;
		float FuzzWeight;
		float Wetness;
		float WetnessFilmStrength;
		// (42c, Wetness Effects "Wet Film Highlight") Direct-light water film override.
		// WetFilmEnergy < 0 keeps the original film (WATER_ROUGHNESS, energy from Wetness and
		// WetnessFilmStrength). The indirect lobe never reads these two.
		float WetFilmRoughness;
		float WetFilmEnergy;
		// (44a) Wet Skin "New" model: droplet beads (mask 0..1, their own low roughness on the
		// direct film lobe only) and the film used by the indirect lobe (roughness >= 0.2, energy
		// <= 1). WetIndirectEnergy < 0 keeps the original indirect film.
		float WetDropletMask;
		float WetDropletRoughness;
		float WetIndirectRoughness;
		float WetIndirectEnergy;
		// (47f) The film's F0 (water 0.02; a Physical oil film a little more), direct lobe and the New indirect lobe.
		float WetF0;
	};

	SkinSurfaceProperties InitSkinSurfaceProperties()
	{
		SkinSurfaceProperties skin;
		skin.RoughnessPrimary = 0.55;
		skin.RoughnessSecondary = 0.35;
		skin.F0 = float3(0.0278, 0.0278, 0.0278);
		skin.SecondarySpecIntensity = 0.15;
		skin.Curvature = 0.0;
		skin.Albedo = float3(0.8, 0.6, 0.5);
		skin.Thickness = 0.15;
		skin.SubsurfaceColor = float3(0.6, 0.3, 0.2);
		skin.AO = 0.0;
		skin.FuzzRoughness = 0.35;
		skin.FuzzColor = float3(0.045, 0.045, 0.045);
		skin.FuzzWeight = 0.0;
		skin.Wetness = 0.0;
		skin.WetnessFilmStrength = 1.0;
		skin.WetFilmRoughness = WATER_ROUGHNESS;
		skin.WetFilmEnergy = -1.0;
		skin.WetDropletMask = 0.0;
		skin.WetDropletRoughness = 0.1;
		skin.WetIndirectRoughness = 0.2;
		skin.WetIndirectEnergy = -1.0;
		skin.WetF0 = WATER_F0;
		return skin;
	}

	// A thin water film becomes visually apparent well before geometric
	// coverage reaches one. Keep the response continuous for anti-aliasing,
	// while retaining the strong wet appearance of the original binary path.
	float GetWetnessResponse(float wetness)
	{
		return sqrt(sqrt(saturate(wetness)));
	}

	// Specular reflection and the loss of dry-surface transmission become
	// visible faster than the normal perturbation of a thin film.  Boost only
	// that optical response so anti-aliased mask coverage stays stable without
	// making low and medium wetness look nearly dry.
	float GetWetnessOpticalResponse(float wetness)
	{
		return saturate(GetWetnessResponse(wetness) * 1.6f);
	}

	// [Jorge Jimenez, Diego Gutierrez 2015, "Separable Subsurface Scattering"]
	// https://www.iryoku.com/separable-sss/
	float3 SSSSTransmittance(float translucency, float sssWidth, float3 worldNormal, float3 light, float d)
	{
		/**
		* Calculate the scale of the effect.
		*/
		float scale = 8.25 * (1.0 - translucency) / sssWidth;

		/**
		* First we shrink the position inwards the surface to avoid artifacts:
		* (Note that this can be done once for all the lights)
		*/
		// float4 shrinkedPos = float4(worldPosition - 0.005 * worldNormal, 1.0);

		/**
		* Now we calculate the thickness from the light point of view:
		*/
		// float4 shadowPosition = mul(shrinkedPos, lightViewProjection);
		// float d1 = SSSSSampleShadowmap(shadowPosition.xy / shadowPosition.w).r; // 'd1' has a range of 0..1
		// float d2 = shadowPosition.z; // 'd2' has a range of 0..'lightFarPlane'
		// d1 *= lightFarPlane; // So we scale 'd1' accordingly:
		// float d = scale * abs(d1 - d2);
		d = scale * abs(d);  // Use the passed 'd' value instead of calculating it here.

		/**
		* Armed with the thickness, we can now calculate the color by means of the
		* precalculated transmittance profile.
		* (It can be precomputed into a texture, for maximum performance):
		*/
		float dd = -d * d;
		float3 profile = float3(0.233, 0.455, 0.649) * exp(dd / 0.0064) +
		                 float3(0.1, 0.336, 0.344) * exp(dd / 0.0484) +
		                 float3(0.118, 0.198, 0.0) * exp(dd / 0.187) +
		                 float3(0.113, 0.007, 0.007) * exp(dd / 0.567) +
		                 float3(0.358, 0.004, 0.0) * exp(dd / 1.99) +
		                 float3(0.078, 0.0, 0.0) * exp(dd / 7.41);

		/** 
		* Using the profile, we finally approximate the transmitted lighting from
		* the back of the object:
		*/
		return profile * saturate(0.3 + dot(light, -worldNormal));
	}

	float3 GetDualSpecularGGX(float AverageRoughness, float Lobe0Roughness, float Lobe1Roughness, float LobeMix, float3 SpecularColor, float NdotL, float NdotV, float NdotH, float VdotH, out float3 F)
	{
		float D = lerp(BRDF::D_GGX(Lobe0Roughness, NdotH), BRDF::D_GGX(Lobe1Roughness, NdotH), LobeMix);
		float G = BRDF::Vis_SmithJointApprox(AverageRoughness, NdotV, NdotL);
		F = BRDF::F_Schlick(SpecularColor, VdotH);

		return D * G * F;
	}

	// a contact shadow approximation, totally not physically correct; a riff on "Chan 2018, "Material Advances in Call of Duty: WWII" and "The Technical Art of Uncharted 4" http://advances.realtimerendering.com/other/2016/naughty_dog/NaughtyDog_TechArt_Final.pdf (microshadowing)"
	float ApproximateDirectOcculusion(float aoVisibility, float NdotL)
	{
		float aperture = rsqrt(1.0000001 - aoVisibility);
		NdotL += 0.1;  // when using bent normals, avoids overshadowing - bent normals are just approximation anyhow
		return saturate(NdotL * aperture);
	}

	void SkinDirectLightInput(
		out float3 diffuse,
		out float3 transmission,
		out float3 specular,
		PBR::LightProperties light,
		SkinSurfaceProperties skin,
		float3 N, float3 V, float3 L, float3 WN)
	{
		diffuse = 0;
		transmission = 0;
		specular = 0;

		light.LightColor *= Color::PBRLightingCompensation;

		float3 H = normalize(V + L);
		const float oNdotL = dot(N, L);
		float NdotL = clamp(oNdotL, 1e-5, 1.0);
		float NdotV = saturate(abs(dot(N, V)) + 1e-5);
		float NdotH = saturate(dot(N, H));
		float VdotH = saturate(dot(V, H));
		float VdotL = dot(V, L);
		float oVdotH = VdotH;

		light.LightColor *= ApproximateDirectOcculusion(skin.AO, NdotL);

		float averageRoughness = lerp(skin.RoughnessPrimary, skin.RoughnessSecondary, skin.SecondarySpecIntensity);

		diffuse += light.LightColor * NdotL * BRDF::Diffuse_Burley(averageRoughness, NdotV, NdotL, VdotH);

		float3 F;
		float3 F0 = skin.F0 * saturate(1 - skin.Curvature);

		specular += GetDualSpecularGGX(averageRoughness, skin.RoughnessPrimary, skin.RoughnessSecondary, skin.SecondarySpecIntensity, F0, NdotL, NdotV, NdotH, VdotH, F) * light.LightColor * NdotL;

		float2 specularBRDF = BRDF::EnvBRDF(averageRoughness, NdotV);
		specular *= 1 + F0 * (1 / (specularBRDF.x + specularBRDF.y) - 1);

		if (skin.FuzzWeight > 0.0) {
			float3 FuzzF0 = skin.FuzzColor * saturate(1 - skin.Curvature);
			float3 fuzzSpecular = PBR::GetSpecularDirectLightMultiplierMicroflakes(skin.FuzzRoughness, FuzzF0, NdotL, NdotV, NdotH, VdotH) * light.LightColor * NdotL;
			float2 fuzzSpecularBRDF = BRDF::EnvBRDFApproxLazarov(skin.FuzzRoughness, NdotV);
			fuzzSpecular *= 1 + skin.FuzzColor * (1 / (fuzzSpecularBRDF.x + fuzzSpecularBRDF.y) - 1);

			specular += fuzzSpecular * skin.FuzzWeight;
		}

		// (42c) WetFilmEnergy >= 0: the Wetness Effects film (rain and swimming merged with
		// max, its own roughness) or (44a) the Wet Skin "New" film; otherwise the original
		// Advanced Skin film (41b), its energy capped at 2 (only reached with Film Strength > 2).
		const bool wetFilmOverride = skin.WetFilmEnergy >= 0.0;
		if (wetFilmOverride ? (skin.WetFilmEnergy > 0.0) : (skin.Wetness > 0.0)) {
			const float wetnessEnergy = wetFilmOverride ? min(skin.WetFilmEnergy, 2.5) : min(GetWetnessOpticalResponse(skin.Wetness) * max(skin.WetnessFilmStrength, 0.0f), 2.0);
			// (44a) Droplet beads: their own low roughness (0.1), inside the bead only.
			const float wetRoughness = wetFilmOverride ? lerp(max(skin.WetFilmRoughness, 0.2), skin.WetDropletRoughness, saturate(skin.WetDropletMask)) : WATER_ROUGHNESS;
			float3 wetnessF;
			float WNdotL = clamp(dot(WN, L), 1e-5, 1.0);
			float WNdotV = saturate(abs(dot(WN, V)) + 1e-5);
			float WNdotH = saturate(dot(WN, H));

			float3 wetSpecular = PBR::GetSpecularDirectLightMultiplierMicrofacet(wetRoughness, skin.WetF0, WNdotL, WNdotV, WNdotH, oVdotH, wetnessF) * light.LightColor * WNdotL;
			float2 wetSpecularBRDF = BRDF::EnvBRDFApproxLazarov(wetRoughness, WNdotV);
			wetSpecular *= 1 + skin.WetF0 * (1 / (wetSpecularBRDF.x + wetSpecularBRDF.y) - 1);
			const float waterTransmission = lerp(1.0, 1.0 - wetnessF.x, saturate(wetnessEnergy));
			specular *= waterTransmission;
			specular += wetSpecular * wetnessEnergy;
			diffuse *= waterTransmission;
		}
	}

	void SkinIndirectLobeWeights(
		out float3 diffuseWeight,
		out float3 specularWeight,
		SkinSurfaceProperties skin,
		float3 N, float3 V, float3 VN, float3 WN)
	{
		float NdotV = saturate(dot(N, V));
		specularWeight = 0;

		float averageRoughness = lerp(skin.RoughnessPrimary, skin.RoughnessSecondary, skin.SecondarySpecIntensity);

		float2 specularBRDF = BRDF::EnvBRDF(averageRoughness, NdotV);

		specularWeight = skin.F0 * specularBRDF.x + specularBRDF.y;

		float waterTransmission = 1.f;
		float3 wetSpecular = 0.f;

		if (skin.WetIndirectEnergy >= 0.0) {
			// (44a) Wet Skin "New": the film as the reflections see it. Roughness >= 0.2 and
			// energy <= 1, so wet skin never turns into a mirror (the G-buffer gloss is capped to
			// match in Lighting.hlsl).
			if (skin.WetIndirectEnergy > 0.0) {
				const float wetnessEnergy = saturate(skin.WetIndirectEnergy);
				const float wetRoughness = max(skin.WetIndirectRoughness, 0.2);
				float WNdotV = saturate(dot(WN, V));
				float2 wetSpecularBRDF = BRDF::EnvBRDF(wetRoughness, WNdotV);
				wetSpecular += skin.WetF0 * wetSpecularBRDF.x + wetSpecularBRDF.y;
				wetSpecular *= 1 + skin.WetF0 * (1 / (wetSpecularBRDF.x + wetSpecularBRDF.y) - 1);
				wetSpecular *= wetnessEnergy;
				waterTransmission = lerp(1.0, 1.0 - (skin.WetF0 * wetSpecularBRDF.x + wetSpecularBRDF.y), wetnessEnergy);
			}
		} else if (skin.Wetness > 0.0) {
			const float wetnessAmount = GetWetnessOpticalResponse(skin.Wetness);
			// (44a) Capped at 2 (only reached with Film Strength > 2).
			const float wetnessEnergy = min(wetnessAmount * max(skin.WetnessFilmStrength, 0.0f), 2.0);
			float WNdotV = saturate(dot(WN, V));
			float2 wetSpecularBRDF = BRDF::EnvBRDF(WATER_ROUGHNESS, WNdotV);
			wetSpecular += WATER_F0 * wetSpecularBRDF.x + wetSpecularBRDF.y;
			wetSpecular *= 1 + WATER_F0 * (1 / (wetSpecularBRDF.x + wetSpecularBRDF.y) - 1);
			wetSpecular *= wetnessEnergy;
			waterTransmission = lerp(1.0, 1.0 - (WATER_F0 * wetSpecularBRDF.x + wetSpecularBRDF.y), saturate(wetnessEnergy));
		}

		diffuseWeight = skin.Albedo * (1.0 - specularWeight.x - specularWeight.y) * waterTransmission;
		specularWeight *= waterTransmission;
		specularWeight *= 1 + skin.F0 * (1 / (specularBRDF.x + specularBRDF.y) - 1);
		specularWeight += wetSpecular;

		float3 R = reflect(-V, N);
		float horizon = min(1.0 + dot(R, VN), 1.0);
		horizon *= horizon;
		specularWeight *= horizon;

		float3 diffuseAO = skin.AO;
		float3 specularAO = Color::SpecularAOLagarde(NdotV, skin.AO, averageRoughness);

		diffuseAO = Color::MultiBounceAO(skin.Albedo, diffuseAO.x).y;
		specularAO = Color::MultiBounceAO(skin.F0, specularAO.x).y;

		diffuseWeight *= diffuseAO;
		specularWeight *= specularAO;

		specularWeight *= saturate(1 - skin.Curvature);
	}

	// https://blog.selfshadow.com/publications/blending-in-detail/
	// geometric normal s, a base normal t and a secondary (or detail) normal u
	float3 ReorientNormal(float3 u, float3 t, float3 s)
	{
		// Build the shortest-arc quaternion
		float4 q = float4(cross(s, t), dot(s, t) + 1) / sqrt(2 * (dot(s, t) + 1));

		// Rotate the normal
		return u * (q.w * q.w - dot(q.xyz, q.xyz)) + 2 * q.xyz * dot(q.xyz, u) + 2 * q.w * cross(q.xyz, u);
	}

	// for when s = (0,0,1)
	float3 ReorientNormal(float3 n1, float3 n2)
	{
		n1 += float3(0, 0, 1);
		n2 *= float3(-1, -1, 1);

		return n1 * dot(n1, n2) / n1.z - n2;
	}

	float3x3 ReconstructTBN(float3 worldPos, float3 worldNormal, float2 uv)
	{
		float3 dFdx = ddx(worldPos);
		float3 dFdy = ddy(worldPos);
		float2 dUVdx = ddx(uv);
		float2 dUVdy = ddy(uv);
		float3 tangent = normalize(dFdx * dUVdy.y - dFdy * dUVdx.y);
		float3 bitangent = normalize(dFdy * dUVdx.x - dFdx * dUVdy.x);
		tangent = normalize(tangent - worldNormal * dot(worldNormal, tangent));
		bitangent = normalize(bitangent - worldNormal * dot(worldNormal, bitangent));
		
		return float3x3(tangent, bitangent, normalize(worldNormal));
	}

	float3 CalculateNormalFromHeight(float height, float heightScale, float2 uv)
	{
		float dHdx = ddx(height);
		float dHdy = ddy(height);
		float2 dUVdx = ddx(uv);
		float2 dUVdy = ddy(uv);

		float det = dUVdx.x * dUVdy.y - dUVdx.y * dUVdy.x;
		if (det == 0.0f) {
			return float3(0, 0, 1); // Avoid division by zero
		}

		float dHdx_Tex = (dHdx * dUVdy.y - dHdy * dUVdx.y) / det;
		float dHdy_Tex = (dHdy * dUVdx.x - dHdx * dUVdy.x) / det;
		float3 normal = float3(-dHdx_Tex, -dHdy_Tex, 0);
		return normal * heightScale + float3(0, 0, 1);
	}

#endif  // SKIN
#if defined(PSHADER)
	// (44c) uv_pixel_footprint passed in: callers in divergent control flow take it where the
	// derivatives are defined.
	float FBMFootprint(float2 uv, float uv_pixel_footprint, float base_scale, int octaves, float lacunarity, float persistence, float z_offset_multiplier)
	{
		// (47c) Skip Invisible Work: an octave faded out completely (octave_filter == 0) adds
		// lerp(0.5, n, 0) = 0.5 exactly, so its noise is not evaluated. With Lacunarity < 1 the
		// first octave is the finest, so the skipped ones are not at the end of the loop.
		const bool skipFaded = WetSkipInvisible();
		float total = 0.0;
		float frequency = base_scale;
		float amplitude = 1.0;
		float max_amplitude = 0.0;
		for (int i = 0; i < octaves; i++)
		{
			const float2 octave_uv = uv * frequency;
			const float pixel_footprint = uv_pixel_footprint * abs(frequency);
			// Fade only this octave as its grid approaches the pixel Nyquist
			// limit. An unresolved Perlin octave contributes its expected mean
			// instead of a temporally unstable point sample. Scale, lacunarity and
			// persistence still control the same frequency/amplitude progression.
			const float octave_filter = 1.0f - smoothstep(0.25f, 0.5f, pixel_footprint);
			float octave_value = 0.5f;
			[branch] if (!skipFaded || octave_filter > 0.0f)
			{
				const float octave_noise = (Random::perlinNoise(float3(octave_uv, (float)i * z_offset_multiplier)) + 1.0f) * 0.5f;
				octave_value = lerp(0.5f, octave_noise, octave_filter);
			}
			total += amplitude * octave_value;

			max_amplitude += amplitude;
			amplitude *= persistence;
			frequency *= lacunarity;
		}
		if (max_amplitude > 0.0) {
			return total / max_amplitude;
		}
		return 0.0;
	}

	// ---- (47c) Faster Noise: a 2D gradient noise on a cheap integer hash -------------------------
	//
	// Replaces the 5-octave 3D murmur3 Perlin of the wet patches (New model, skin and gear) when
	// "Wetness: Faster Noise & Distance LOD" is on. Same uv (mesh uv: stays put on a moving
	// character), same Scale / Lacunarity / Persistence / footprint fade; each octave has its own
	// seed (the old one had its own z slice). Per-octave gain: the old octave's spread (std of the
	// 0..1 value, 0.124-0.134, measured over 400k samples) over this noise's (0.086), so the patch
	// statistics match: the wet fraction at a given wetness is within about 0.01 of the old one
	// (user's 256 / 0.6 / 2, and the defaults). The patches are in other places.
	// Cost (fxc /O3, all 5 octaves resolved): 221 instruction slots, about 400 scalar ops, instead of
	// 657 slots, about 1010 scalar ops (8 murmur3 hashes, 8 gradient lookups, a trilinear blend per octave).

	/// One xorshift-multiply-xorshift round (the first half of Wellons' lowbias32): enough for
	/// gradients (neighbour and seed correlation of the gradient below 0.005 over 200k cells).
	uint WetHashMix(uint h)
	{
		h ^= h >> 16;
		h *= 0x7feb352du;
		h ^= h >> 15;
		return h;
	}

	/// The corner's gradient (two signed 16-bit halves of the hash, -1..1 each) dotted with d.
	float WetGrad2(uint h, float2 d)
	{
		const float2 g = float2(float(int(h << 16) >> 16) * (1.0 / 32768.0), float(int(h)) * (1.0 / 2147483648.0));
		return dot(g, d);
	}

	/// 2D gradient noise, about -0.7..0.7 (std 0.17), lattice of 1.
	float WetGradientNoise2(float2 p, uint seed)
	{
		const float2 cell = floor(p);
		const float2 f = p - cell;
		const uint2 i = asuint(int2(cell));
		const uint hx0 = i.x * 0x8da6b343u;
		const uint hx1 = hx0 + 0x8da6b343u;
		const uint hy0 = i.y * 0xd8163841u + seed * 0xcb1ab31fu;
		const uint hy1 = hy0 + 0xd8163841u;
		const float v00 = WetGrad2(WetHashMix(hx0 + hy0), f);
		const float v10 = WetGrad2(WetHashMix(hx1 + hy0), f - float2(1.0, 0.0));
		const float v01 = WetGrad2(WetHashMix(hx0 + hy1), f - float2(0.0, 1.0));
		const float v11 = WetGrad2(WetHashMix(hx1 + hy1), f - float2(1.0, 1.0));
		const float2 u = f * f * (3.0 - 2.0 * f);
		return lerp(lerp(v00, v10, u.x), lerp(v01, v11, u.x), u.y);
	}

	/// FBMFootprint with WetGradientNoise2 (5 octaves, the wet patches' z offset is not needed).
	float FBMFootprintFast(float2 uv, float uv_pixel_footprint, float base_scale, float lacunarity, float persistence)
	{
		static const float kOctaveGain[5] = { 1.427, 1.514, 1.465, 1.431, 1.544 };
		const bool skipFaded = WetSkipInvisible();
		float total = 0.0;
		float frequency = base_scale;
		float amplitude = 1.0;
		float max_amplitude = 0.0;
		[unroll] for (int i = 0; i < 5; i++)
		{
			const float pixel_footprint = uv_pixel_footprint * abs(frequency);
			const float octave_filter = 1.0f - smoothstep(0.25f, 0.5f, pixel_footprint);
			float octave_value = 0.5f;
			[branch] if (!skipFaded || octave_filter > 0.0f)
			{
				const float octave_noise = saturate(0.5f + 0.5f * kOctaveGain[i] * WetGradientNoise2(uv * frequency, (uint)i));
				octave_value = lerp(0.5f, octave_noise, octave_filter);
			}
			total += amplitude * octave_value;
			max_amplitude += amplitude;
			amplitude *= persistence;
			frequency *= lacunarity;
		}
		if (max_amplitude > 0.0) {
			return total / max_amplitude;
		}
		return 0.0;
	}

	float FBM(float2 uv, float base_scale, int octaves, float lacunarity, float persistence, float z_offset_multiplier)
	{
		return FBMFootprint(uv, max(length(ddx(uv)), length(ddy(uv))), base_scale, octaves, lacunarity, persistence, z_offset_multiplier);
	}

	float PerlinNoise(float2 uv, float scale, float lacunarity, float persistence, float strength)
	{
		if (strength <= 0.001f)
		{
			return 0.0f;
		}
		if (strength >= 0.999f)
		{
			return 1.0f;
		}
		int octaves = 5;
		float z_offset_multiplier = 7.375f;

		float noise_value = FBM(uv, scale, octaves, lacunarity, persistence, z_offset_multiplier);

		float dynamic_threshold = 1.0f - strength;
		// Band-limiting the FBM octaves is not sufficient on its own: the wet/dry
		// threshold below creates a new high-frequency edge, and the final 0.1
		// power strongly magnifies tiny samples around that edge. Analytically
		// box-filter the positive ramp over one pixel, preserving the exact old
		// response outside the transition footprint.
		const float threshold_distance = noise_value - dynamic_threshold;
		const float threshold_half_width = max(fwidth(noise_value) * 0.5f, 1e-5f);
		float filtered_positive;
		if (threshold_distance <= -threshold_half_width) {
			filtered_positive = 0.0f;
		} else if (threshold_distance >= threshold_half_width) {
			filtered_positive = threshold_distance;
		} else {
			const float covered_ramp = threshold_distance + threshold_half_width;
			filtered_positive = covered_ramp * covered_ramp / (4.0f * threshold_half_width);
		}

		float sweat_intensity = saturate(filtered_positive / strength);
		sweat_intensity = pow(sweat_intensity, 1.5f);

		if (strength > 0.8f) {
			sweat_intensity = sweat_intensity * saturate(0.99f - (strength - 0.8f) * 5.0f) + (strength - 0.8f) * 5.0f;
		}

		return pow(sweat_intensity, 0.1f);
	}

	// ---- (44a) Wet Skin "New" model ------------------------------------------------------------
	//
	// wetSkinParams (WetSkin::GPUParams):
	//   [0] x model (1 = New), y manual wetness, z swim weight, w rain weight
	//   [1] x sweat weight, y film roughness when slightly wet, z film roughness at full wetness, w full-wetness strength
	//   [2] x patch softness, y patch contrast, z droplet amount, w droplet density
	//   [3] x droplet size, y size randomness, z face droplets, w droplet rim
	//   [4] x droplet trails, y streak amount, z streak length, w streak width
	//   [5] x streak contrast, y darkening, z droplet normal strength, w rain droplets

	float WetHash12(float2 p)
	{
		float3 p3 = frac(p.xyx * 0.1031);
		p3 += dot(p3, p3.yzx + 33.33);
		return frac((p3.x + p3.y) * p3.z);
	}

	float2 WetHash22(float2 p)
	{
		float3 p3 = frac(p.xyx * float3(0.1031, 0.1030, 0.0973));
		p3 += dot(p3, p3.yzx + 33.33);
		return frac((p3.xx + p3.yz) * p3.zy);
	}

	float WetValueNoise(float2 p)
	{
		const float2 i = floor(p);
		const float2 f = p - i;
		const float2 u = f * f * (3.0 - 2.0 * f);
		return lerp(lerp(WetHash12(i), WetHash12(i + float2(1, 0)), u.x), lerp(WetHash12(i + float2(0, 1)), WetHash12(i + float2(1, 1)), u.x), u.y);
	}

	/// (44c) Wet hair clumps: l along the strands, s across them in clumps (both from uv). x: 0..1
	/// across the clump; r: the clump's random highlight offset, -0.5..0.5. (47c) A function so Skip
	/// Invisible Work can run it only where the hair is wet.
	void WetHairClump(float l, float s, out float x, out float r)
	{
		s += 0.7 * (WetValueNoise(float2(l * 3.0, s * 0.1)) - 0.5);
		const float id = floor(s);
		x = s - id;
		r = WetHash12(float2(id, 7.31)) - 0.5;
	}

	/// Soft wet patches: the Perlin noise (Scale / Lacunarity / Persistence) against a threshold that
	/// falls as the wetness rises, through a wide smoothstep. Patches grow and merge; at full wetness
	/// the coverage is 1 everywhere. Contrast blends between an even film (0) and the patches (1).
	float WetCoverageEx(float2 uv, float uvFootprint, float wetness, float scale, float lacunarity, float persistence, float softness, float contrast)
	{
		softness = clamp(softness, 0.05, 0.6);
		contrast = saturate(contrast);
		if (wetness <= 0.0)
			return 0.0;
		if (contrast <= 0.0 || wetness >= 1.0)
			return wetness;
		// (47c) Faster Noise: the 2D gradient FBM (same parameters, matched statistics).
		float fbm;
		[branch] if (WetFastNoiseLOD())
			fbm = FBMFootprintFast(uv, uvFootprint, scale, lacunarity, persistence);
		else
			fbm = FBMFootprint(uv, uvFootprint, scale, 5, lacunarity, persistence, 7.375f);
		const float noise = saturate((fbm - 0.5) * 3.0 + 0.5);
		const float threshold = lerp(1.0 + softness, -softness, wetness);
		const float patches = smoothstep(threshold - softness, threshold + softness, noise);
		return wetness * lerp(1.0, patches, contrast);
	}

	float WetCoverage(float2 uv, float wetness, float scale, float lacunarity, float persistence)
	{
		const float uvFootprint = max(length(ddx(uv)), length(ddy(uv)));
		return WetCoverageEx(uv, uvFootprint, wetness, scale, lacunarity, persistence, wetSkinParams[2].x, wetSkinParams[2].y);
	}

	struct WetSkinDetail
	{
		float dropMask;    // 0..1, bead coverage
		float2 dropSlope;  // bead surface slope in uv cells (dome normal before projection)
		float dropRim;     // 0..1, the bead's edge
		float trail;       // 0..1, a short trail below a bead
		float streak;      // 0..1, rivulets along gravity
	};

	/// Droplets and streaks for one pixel. p: uv in droplet cells; footprint: uv cells per pixel;
	/// gravityUV: unit direction of "down" in uv (the tangent plane), alignment: how much the
	/// surface is not facing straight up / down (0..1). Beads come from a jittered cell grid,
	/// 3x3 neighbourhood; each cell holds at most one bead, so they never overlap into blobs.
	WetSkinDetail WetSkinDroplets(float2 p, float footprint, float2 gravityUV, float alignment, float presence, float streakDriver)
	{
		WetSkinDetail o;
		o.dropMask = 0.0;
		o.dropSlope = 0.0;
		o.dropRim = 0.0;
		o.trail = 0.0;
		o.streak = 0.0;

		const float sizeRandom = saturate(wetSkinParams[3].y);
		const float trails = saturate(wetSkinParams[4].x);
		const float2 across = float2(-gravityUV.y, gravityUV.x);

		[branch] if (presence > 0.0)
		{
			const float2 cell = floor(p);
			const float2 f = p - cell;
			float best = 0.0;
			[unroll] for (int j = -1; j <= 1; ++j)
			{
				[unroll] for (int i = -1; i <= 1; ++i)
				{
					const float2 c = cell + float2(i, j);
					if (WetHash12(c) > presence)
						continue;
					const float2 rnd = WetHash22(c + 17.0);
					const float radius = 0.42 * lerp(1.0, 0.35, rnd.x * sizeRandom);
					const float2 center = float2(i, j) + 0.5 + (rnd.yx - 0.5) * (1.0 - 2.0 * radius);
					const float2 d = f - center;
					const float r = length(d) / radius;
					// Anti-aliased edge: one pixel wide in bead radii.
					const float aa = max(footprint / radius, 0.05);
					const float m = 1.0 - smoothstep(1.0 - aa, 1.0 + aa, r);
					if (m > best) {
						best = m;
						o.dropSlope = d / radius;
						o.dropRim = saturate(1.0 - abs(r - 0.85) / max(0.2, aa));
					}
					// A short trail below the bead, along gravity, tapering away.
					[branch] if (trails > 0.0 && alignment > 0.2)
					{
						const float along = dot(d, gravityUV);
						const float length_ = radius * (1.5 + 5.0 * rnd.y) * trails;
						if (along > 0.0 && along < length_) {
							const float t = along / length_;
							const float width = radius * 0.45 * (1.0 - t) + footprint;
							const float side = abs(dot(d, across));
							o.trail = max(o.trail, (1.0 - smoothstep(width * 0.5, width, side)) * (1.0 - t) * alignment);
						}
					}
				}
			}
			o.dropMask = best;
		}

		// (44b) Rivulets: WetSkinRivulets (their own scale, not the droplet cells).
		return o;
	}

	/// (44b) One family of rivulets running along l, spaced across a; both in rivulet columns (one
	/// column holds at most one rivulet). aFoot: columns per pixel across. Each column gets a
	/// randomly placed, slightly meandering line of random width, broken into segments along l.
	/// The line is widened to at least about a pixel and dimmed by the same factor (its average is
	/// kept, nothing thinner than a pixel is drawn), so it never sparkles.
	float WetRivuletField(float a, float l, float aFoot, float lengthScale, float threshold, float seed)
	{
		const float s = a + 0.7 * (WetValueNoise(float2(l * 0.12 / lengthScale, a * 0.07 + seed)) - 0.5);
		const float id = floor(s);
		const float x = frac(s) - 0.5;
		const float2 h = WetHash22(float2(id, seed));
		const float center = (h.x - 0.5) * 0.4;
		const float halfWidth = 0.13 * lerp(0.7, 1.3, h.y);
		const float w = max(halfWidth, 0.6 * aFoot);
		const float profile = (1.0 - smoothstep(0.35 * w, w, abs(x - center))) * (halfWidth / w);
		const float segment = WetValueNoise(float2(l * 0.25 / lengthScale + h.y * 13.0, id * 1.37 + seed));
		return profile * smoothstep(threshold, threshold + 0.12, segment) * lerp(0.6, 1.0, h.x);
	}

	/// (44b) Rivulets for one pixel, 0..1. uv: the skin's wet uv; columnsPerUV: rivulet columns per uv
	/// (about 3 cm on the body at width 1); uvFootprint: uv per pixel; gravityUV: unit "down" in uv;
	/// alignment: how far the surface is from facing straight up or down; driver: 0..1 how much
	/// rivulet-making water there is. Two families (along u, along v) are blended by how gravity runs
	/// in uv, so the pattern stays put on the mesh and still runs downhill. They fade only when a
	/// rivulet would be narrower than about a pixel.
	/// (47c) lod: the distance LOD (WetDetailLOD, 1 when off), multiplied into the result.
	float WetSkinRivulets(float2 uv, float columnsPerUV, float uvFootprint, float2 gravityUV, float alignment, float driver, float lod)
	{
		const float amount = saturate(wetSkinParams[4].y) * driver * alignment;
		float streak = 0.0;
		[branch] if (amount > 0.0)
		{
			const float lengthScale = clamp(wetSkinParams[4].z, 0.1, 4.0);
			const float width = clamp(wetSkinParams[4].w, 0.1, 3.0);
			const float2 p = uv * (columnsPerUV / width);
			const float aFoot = max(uvFootprint * columnsPerUV / width, 1e-6);
			// A rivulet is about 0.26 column wide: fade below about a pixel.
			const float linePixels = 0.26 / aFoot;
			const float lineFade = smoothstep(0.5, 1.2, linePixels);
			// (47c) Skip Invisible Work: narrower than half a pixel (lineFade == 0) or past the
			// distance LOD, the product below is exactly 0 (the fields are finite): not evaluated.
			[branch] if (!WetSkipInvisible() || (lineFade > 0.0 && lod > 0.0))
			{
				// (44e) Contrast (RivuletContrast2): higher = more of each trail runs unbroken, so the trails
				// read more clearly against the skin. 44b-44d raised the segment threshold with Contrast
				// (fewer, broken trails), which is why a high value looked weaker. Same range, reversed:
				// new = 1 - old gives exactly the old threshold. The film between the trails is a
				// separate setting now (WetSkinFilm, Between-Trail Dulling).
				const float threshold = lerp(0.68, 0.48, saturate(wetSkinParams[5].x));
				const float2 g2 = gravityUV * gravityUV;
				const float wU = g2.x / max(g2.x + g2.y, 1e-4);
				float lines = 0.0;
				[branch] if (wU < 0.999)
					lines += (1.0 - wU) * WetRivuletField(p.x, p.y, aFoot, lengthScale, threshold, 31.7);
				[branch] if (wU > 0.001)
					lines += wU * WetRivuletField(p.y, p.x, aFoot, lengthScale, threshold, 7.3);
				streak = saturate(lines) * amount * lineFade * lod;
			}
		}
		return streak;
	}

#endif  // PSHADER (44c: the wet helpers are shared by skin, hair and gear)
#if defined(SKIN)
	/// (47c) Everything the skin's droplets and rivulets need, gathered where the screen derivatives
	/// are known, so the work itself can run later: after the sky occlusion (Lighting.hlsl), where
	/// a skin under a roof with only rain on it can leave it out.
	struct SkinWetDetailSetup
	{
		float2 wetUV;
		float2 cellUV;
		float footprint;      // droplet cells per pixel
		float uvFootprint;    // wet uv per pixel
		float2 gravityUV;
		float alignment;
		float presence;
		float dropVisibility;
		float streakDriver;   // droplet trails
		float rivuletColumnsPerUV;
		float rivuletDriver;  // streak driver x Rivulets on the Face
		float lod;            // distance LOD (1 when off)
		float3 baseN;
		float3 tu;
		float3 tv;
		float domeStrength;
	};

	/// (47c) The 44a-44e skin droplets and rivulets, unchanged, from a SkinWetDetailSetup.
	void SkinWetDetail(SkinWetDetailSetup s, out float dropMask, out float dropRim, out float streak, out float3 wetN)
	{
		const WetSkinDetail detail = WetSkinDroplets(s.cellUV, s.footprint, s.gravityUV, s.alignment, s.presence, s.streakDriver);
		dropMask = detail.dropMask * s.dropVisibility;
		dropRim = detail.dropRim * dropMask;
		// (44b) Rivulets on their own scale (they used the droplet cells, about 1/800 uv wide, and
		// faded out below a pixel at any normal viewing distance).
		const float rivulets = WetSkinRivulets(s.wetUV, s.rivuletColumnsPerUV, s.uvFootprint, s.gravityUV, s.alignment, s.rivuletDriver, s.lod);
		streak = saturate(max(rivulets, detail.trail * 0.8 * s.dropVisibility));
		wetN = normalize(s.baseN + (detail.dropSlope.x * s.tu + detail.dropSlope.y * s.tv) * s.domeStrength * dropMask);
	}

	/// How far the skin darkens toward its soaked colour (0..1): follows the soft coverage, plus
	/// (44b) clearly along rivulets (more water standing in the skin's micro-relief) and at the
	/// droplets' edges (their refractive rim).
	float WetSkinDarkening(float wetness, float streak, float rim)
	{
		const float darkening = clamp(wetSkinParams[5].y, 0.0, 2.0);
		// (47f) Oil fills the skin's fine relief: a deeper, more saturated colour (0 outside Physical: unchanged).
		wetness = max(saturate(wetness), 0.55 * WetOil());
		return saturate(darkening * pow(saturate(wetness), 0.8) + 0.8 * streak * saturate(wetness) + saturate(wetSkinParams[3].w) * rim);
	}

	/// (47f) Physical, Manual Look "Oil": an even oily film over the skin (with whatever water there is): no beads or
	/// trails of its own, no noise, F0 0.02 -> 0.035, a crisp highlight (the beads' low roughness over the whole film) and
	/// a little more sheen than water; the reflections see it at roughness 0.2 (anti-mirror). oil = 0: unchanged.
	void WetSkinOil(inout SkinSurfaceProperties skin, float oil)
	{
		[branch] if (oil > 0.0)
		{
			const float k = saturate(oil);
			skin.WetFilmEnergy = max(max(skin.WetFilmEnergy, 0.0), 1.2 * k);
			skin.WetFilmRoughness = lerp(skin.WetFilmRoughness, 0.2, k);
			if (skin.WetDropletMask < 0.85 * k)
				skin.WetDropletRoughness = 0.12;
			skin.WetDropletMask = max(skin.WetDropletMask, 0.85 * k);
			skin.WetF0 = lerp(WATER_F0, 0.035, k);
			skin.WetIndirectEnergy = max(max(skin.WetIndirectEnergy, 0.0), 0.75 * k);
			skin.WetIndirectRoughness = lerp(skin.WetIndirectRoughness, 0.2, k);
		}
	}

	/// Fills the film fields of SkinSurfaceProperties for the New model: a broad sheen whose
	/// roughness goes from "slightly wet" to "full wetness" (never below 0.2) and whose strength
	/// grows faster than the wetness (subtle when damp, clearly glossy when soaked); rivulets a bit
	/// shinier, the film between them a bit less; beads a small bright point (roughness 0.1, bead
	/// only). The reflections see the film without the beads, energy at most 1.
	void WetSkinFilm(inout SkinSurfaceProperties skin, float streak, float streakDriver, float dropMask)
	{
		const float wetness = saturate(skin.Wetness);
		const float response = pow(wetness, 1.3);
		const float roughness = max(lerp(wetSkinParams[1].y, wetSkinParams[1].z, response) - 0.04 * streak, 0.2);
		// (44e) Between-Trail Dulling (wetCharParams[6].w): how much less wet the film between the
		// trails looks. Was the old Contrast value itself; migrated 1:1.
		const float between = 1.0 - 0.35 * saturate(wetCharParams[6].w) * saturate(wetSkinParams[4].y) * streakDriver;
		const float strength = clamp(wetSkinParams[1].w, 0.0, 2.5);
		// (44b) Rivulets: a clearly stronger, tighter highlight on the trails (their own roughness on
		// the direct lobe only, like the beads) and a little more reflection; G-buffer gloss unchanged.
		float energy = strength * response * (between + 1.5 * streak);
		energy += 1.5 * dropMask * saturate(wetness * 2.0);
		skin.WetFilmRoughness = roughness;
		skin.WetFilmEnergy = max(energy, 0.0);
		const float trailMask = 0.6 * saturate(streak) * response;
		skin.WetDropletMask = max(dropMask, trailMask);
		skin.WetDropletRoughness = dropMask >= trailMask ? 0.1 : 0.15;
		skin.WetIndirectRoughness = roughness;
		skin.WetIndirectEnergy = saturate(response * strength * 0.6 * between + 0.5 * streak * response);
		WetSkinOil(skin, WetOil());
	}
#endif

	float2 GetWetness(float z, float3 modelNormal)
	{
		if (skinPerGeometry.x == 0.f && skinPerGeometry.y == 0.f)
			return 0.f;

		float waterWet = 0.0f;
		float waterLevel = skinPerGeometry.z + skinPerGeometry.w;
		
		waterWet = skinPerGeometry.y * (1 - smoothstep(waterLevel - 2.5f, waterLevel + 2.5f, z));

		float sweatWet = skinPerGeometry.x;
#if !defined(SKIN)
		sweatWet *= 1.0f - saturate(dot(modelNormal, float3(0, 0, 1)));
#endif
		return float2(sweatWet, waterWet);
	}
}
