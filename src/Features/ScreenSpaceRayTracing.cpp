#include "ScreenSpaceRayTracing.h"

#include <DDSTextureLoader.h>

#include "Deferred.h"
#include "JiayeStatement.h"
#include "Menu.h"
#include "State.h"
#include "ShaderCache.h"

#include "DynamicCubemaps.h"
#include "ScreenSpaceGI.h"
#include "Skylighting.h"

#include "Utils/DenoiserTimers.h"
#include "Utils/GpuTimers.h"

#ifdef ENABLE_SHARC
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    ScreenSpaceRayTracing::Settings,
    EnableSpecular,
    MaxSteps,
    MaxMips,
    Thickness,
    NormalBias,
    BRDFBias,
    UseDynamicCubemapsAsFallback,
    UseDynamicCubemapsAsFallbackSpecular,
    DiffuseSPP,
    DiffuseSamplingMode,
    EnableDiffuse,
    SpecularMult,
    DiffuseMult,
    AmbientMult,
    EnableAmbientReinjection,
    AmbientReinjectionStrength,
    CubemapFillBlend,
    LowResConfidenceFilter,
    TemporalAmbientConfidence,
    AmbientConfidenceMaxFrames,
    OcclusionStrength,
    CubemapNormalization,
    DenoiserMethod,
    ReblurFeedHitCoverageConfidence,
    ReblurDiffuse,
    ReblurSpecular,
    ReblurHitDistA,
    ReblurHitDistB,
    ReblurHitDistC,
    SpecularPrepassBlurRadius,
    UsePrepassOnlyForSpecularMotionEstimation,
    ReblurSkipSpecularPrepass,
    ReblurFoldUnpack,
    DistanceLimit,
    DistanceLimitMeters,
    EnablePreBlur,
    MaxAccumulatedFrames,
    AtrousIterations,
    ColorPhi,
    NormalPhi,
    HitRadiusStrength,
    AdaptiveFiltering,
    AdaptiveHistoryThreshold,
    AdaptiveVarianceEps,
    FireflyClamp,
    FireflyClampSigma,
    SpecularDenoiseRoughnessCutoff,
    SpecularMaxRoughness,
    HistoryClampSigma,
    UseBlueNoise,
    FreezeNoisePhase,
    DisableHistoryDepthTest,
    DisableHistoryNormalTest,
    ForceAcceptHistory,
    RotatedNormalGate,
    HistoryDebugView,
    EnableSharc
)
#else
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    ScreenSpaceRayTracing::Settings,
    EnableSpecular,
    MaxSteps,
    MaxMips,
    Thickness,
    NormalBias,
    BRDFBias,
    UseDynamicCubemapsAsFallback,
    UseDynamicCubemapsAsFallbackSpecular,
    DiffuseSPP,
    DiffuseSamplingMode,
    EnableDiffuse,
    SpecularMult,
    DiffuseMult,
    AmbientMult,
    EnableAmbientReinjection,
    AmbientReinjectionStrength,
    CubemapFillBlend,
    LowResConfidenceFilter,
    TemporalAmbientConfidence,
    AmbientConfidenceMaxFrames,
    OcclusionStrength,
    CubemapNormalization,
    DenoiserMethod,
    ReblurFeedHitCoverageConfidence,
    ReblurDiffuse,
    ReblurSpecular,
    ReblurHitDistA,
    ReblurHitDistB,
    ReblurHitDistC,
    SpecularPrepassBlurRadius,
    UsePrepassOnlyForSpecularMotionEstimation,
    ReblurSkipSpecularPrepass,
    ReblurFoldUnpack,
    DistanceLimit,
    DistanceLimitMeters,
    EnablePreBlur,
    MaxAccumulatedFrames,
    AtrousIterations,
    ColorPhi,
    NormalPhi,
    HitRadiusStrength,
    AdaptiveFiltering,
    AdaptiveHistoryThreshold,
    AdaptiveVarianceEps,
    FireflyClamp,
    FireflyClampSigma,
    SpecularDenoiseRoughnessCutoff,
    SpecularMaxRoughness,
    HistoryClampSigma,
    UseBlueNoise,
    FreezeNoisePhase,
    DisableHistoryDepthTest,
    DisableHistoryNormalTest,
    ForceAcceptHistory,
    RotatedNormalGate,
    HistoryDebugView
)
#endif

void ScreenSpaceRayTracing::DrawSettings()
{
    ImGui::Checkbox("Enable Specular", &settings.EnableSpecular);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Traced reflections that show what is actually on screen, instead of only the cubemap reflection.");
    ImGui::SameLine();
    ImGui::Checkbox("Enable Diffuse", &settings.EnableDiffuse);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Traced bounce light: light and color reflected from nearby surfaces onto others.");
    ImGui::SliderInt("Max Steps", (int*)&settings.MaxSteps, 1, 256);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("How far each ray can search for something to hit. Higher = finds more distant objects, but costs more GPU time.");
    // (audit P3) The traversal can load exactly mip SSRTCB::MaxMips, so the highest
    // legal setting is maxMips - 1; the old bound of maxMips let the ray sample a mip
    // that does not exist, and an out-of-range Load returns 0 == near plane, i.e. an
    // instant false hit.
    ImGui::SliderInt("Max Mip Level", (int*)&settings.MaxMips, 1, maxMips - 1, "%d", ImGuiSliderFlags_AlwaysClamp);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("How big a jump each ray step can take. Higher = rays reach farther for the same cost, but may skip over thin objects.");
    recompileFlag |= ImGui::SliderInt("Diffuse SPP", (int*)&settings.DiffuseSPP, 1, 16, "%d", ImGuiSliderFlags_AlwaysClamp);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Samples per pixel for diffuse component. Higher values reduce noise but impact performance.");

    // (batch 28b) Belongs with the ray march, not with a denoiser. It first went in beside
    // Specular Mirror Cutoff on the strength of the name, and that one lives under
    // SVGFSelected() -- so on REBLUR the slider was simply not drawn. This gate stops the march
    // itself and applies whatever the denoiser is, including none.
    if (settings.EnableSpecular) {
        ImGui::SliderFloat("Specular Max Roughness", &settings.SpecularMaxRoughness, 0.05f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Surfaces rougher than this skip traced reflections and keep the cheaper cubemap reflection. "
                "Lower = more FPS with little change on rough surfaces, but watch shiny floors, metal and water as you go lower; 1.00 = trace everything.");
    }

    // (batch 12) Sparse sampling. Placed directly under Diffuse SPP because the two are the same
    // axis read from opposite ends: SPP is rays per pixel, this is pixels per ray. Not gated on the
    // denoiser -- it covers the ray march, so it applies with the denoiser off as well -- and not a
    // recompile, because all three permutations are built at startup.
    {
        static const char* samplingModes[] = { "Full", "Half Resolution", "Checkerboard" };
        int sm = (int)std::min(settings.DiffuseSamplingMode, (uint)kSamplingCheckerboard);
        if (ImGui::Combo("Diffuse Sampling", &sm, samplingModes, 3))
            settings.DiffuseSamplingMode = (uint)sm;
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "How many pixels share one bounce-light ray. Full = best quality; Half Resolution = cheapest, but bounce light gets softer "
                "and very thin objects can lose it; Checkerboard = almost as cheap and keeps detail better, but may shimmer slightly in fast motion. "
                "Reflections are not affected."
#ifdef ENABLE_SHARC
                " Ignored while SHARC is enabled."
#endif
            );
#ifdef ENABLE_SHARC
        if (settings.DiffuseSamplingMode != kSamplingFull && settings.EnableSharc)
            ImGui::TextColored({ 1.0f, 0.7f, 0.2f, 1.0f }, "Sparse diffuse sampling is inactive: SHARC is enabled.");
        else
#endif
            if (settings.DiffuseSamplingMode != kSamplingFull && activeSamplingMode == kSamplingFull && settings.EnableDiffuse)
            ImGui::TextColored({ 1.0f, 0.7f, 0.2f, 1.0f }, "Sparse diffuse sampling failed to start, so Full is running. See the log.");
    }

    ImGui::SliderFloat("Specular Multiplier", &settings.SpecularMult, 0.0f, 5.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Brightness of the traced reflections. 1 = neutral.");
    ImGui::SliderFloat("Diffuse Multiplier", &settings.DiffuseMult, 0.01f, 5.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Brightness of the traced bounce light. 1 = neutral.");
    ImGui::SliderFloat("Occlusion Strength", &settings.OcclusionStrength, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "How much the rays darken ambient light in corners and behind objects. "
            "With Screen Space GI's Contact AO on, very close contacts are left to Contact AO, so nothing gets darkened twice.");
    ImGui::BeginDisabled(settings.EnableAmbientReinjection);
    ImGui::SliderFloat("Ambient Multiplier", &settings.AmbientMult, 0.0f, 1.0f, "%.2f");
    ImGui::EndDisabled();
    if (auto _tt = Util::HoverTooltipWrapper()) {
        if (settings.EnableAmbientReinjection)
            ImGui::Text("Locked to 1 while Ambient Reinjection is on. Turn Ambient Reinjection off to change it.");
        else
            ImGui::Text("Mix diffuse with vanilla ambient color. Not suggested if using dynamic cubemaps as fallback.");
    }

    ImGui::SeparatorText("Ambient Energy");

    ImGui::Checkbox("Ambient Reinjection", &settings.EnableAmbientReinjection);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Keeps the game's normal ambient light and replaces it with traced bounce light only where the rays actually found something. "
            "Steadier, less noisy lighting in open areas; while on, the diffuse cubemap fallback only works through Cubemap Fill Blend below.");

    if (settings.EnableAmbientReinjection) {
        ImGui::SliderFloat("Reinjection Strength", &settings.AmbientReinjectionStrength, 0.0f, 1.0f, "%.2f");
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "How much the traced light replaces the normal ambient light. 1 = full replacement (most accurate); "
                "lower keeps more of the normal ambient and adds traced light on top, so brighter but never darker than vanilla.");

        // (batch 8) beta. Disabled without the diffuse cubemap fallback switched on, because that
        // switch is what decides whether the cubemap estimate is built at all -- with it off there
        // is nothing for this slider to blend towards.
        ImGui::BeginDisabled(!settings.UseDynamicCubemapsAsFallback);
        ImGui::SliderFloat("Cubemap Fill Blend", &settings.CubemapFillBlend, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::EndDisabled();
        if (auto _tt = Util::HoverTooltipWrapper()) {
            if (!settings.UseDynamicCubemapsAsFallback)
                ImGui::Text(
                    "Needs \"Use Dynamic Cubemaps as Fallback for Diffuse\" (further down) turned on; "
                    "without it this slider does nothing.");
            else
                ImGui::Text(
                    "Where the light comes from in spots the rays could not reach: 0 = normal ambient light, 1 = dynamic cubemap, in between = a mix. "
                    "Overall brightness stays about the same; most visible in shadowed nooks facing you, such as under eaves and arches.");
        }

        // (batch 6) The zero-lag noise fix, and the default. Placed above the temporal
        // accumulator because it supersedes it.
        ImGui::Checkbox("Low-Resolution Confidence Filter", &settings.LowResConfidenceFilter);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Removes most of the grain Ambient Reinjection can add, using a wide blur at lower resolution. "
                "Cheap and never smears in motion; the edge between traced light and normal ambient gets slightly softer. Off = sharper but grainier.");

        // (batch 6) Forced off while the spatial filter runs: the two are competing answers to
        // one question, and the accumulator is the one with lag. Disabled rather than hidden so
        // the A/B is still discoverable.
        ImGui::BeginDisabled(settings.LowResConfidenceFilter);
        ImGui::Checkbox("Accumulate Confidence Over Time", &settings.TemporalAmbientConfidence);
        ImGui::EndDisabled();
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Alternative to the filter above that averages over several frames: removes grain, but leaves bands of wrong brightness trailing moving objects. "
                "Off by default; only available with the Low-Resolution Confidence Filter turned off.");

        if (settings.TemporalAmbientConfidence && !settings.LowResConfidenceFilter) {
            ImGui::SliderInt("Confidence Frames", (int*)&settings.AmbientConfidenceMaxFrames, 1, 60, "%d", ImGuiSliderFlags_AlwaysClamp);
            if (auto _tt = Util::HoverTooltipWrapper())
                ImGui::Text("How many frames are averaged. Higher = less grain but slower to catch up when the view changes; 30 is a good default.");
        }
    }

    ImGui::Separator();

    // (spec F1 / audit #3) Range recalibrated to the parameter's actual unit -- game
    // units of depth-buffer thickness, not the 0-50 window that only ever made sense
    // against the old mip-1 validation. Logarithmic so the useful 10-60 region is still
    // draggable at a 500-unit top end.
    ImGui::SliderFloat("Thickness", &settings.Thickness, 0.0f, 500.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "How thick objects are assumed to be when a ray passes behind them (game units, 1 unit ~ 1.4 cm). "
            "Too low = traced light goes missing on ground and slopes; too high = light leaks through thin objects and reflections smear behind edges.");
    ImGui::SliderFloat("Normal Bias", &settings.NormalBias, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Starts each ray slightly off its surface so it does not hit itself. "
            "Higher = fewer false dark speckles, but weaker contact shading around hair and foliage.");
    ImGui::SliderFloat("BRDF Bias", &settings.BRDFBias, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Specular only. Higher BRDF bias reduces noise but makes reflections more glossy.");
    // (batch 8) No longer disabled under Ambient Reinjection. It used to be, because in that mode
    // the flag could only have done harm; it now selects whether the Cubemap Fill Blend slider has
    // a source at all, so leaving it locked would make beta unreachable for anyone whose saved
    // value happens to be off -- that slider is itself disabled while this is off, so the pair
    // would deadlock. At beta = 0 this checkbox still changes nothing in the frame: DrawSSRTDiffuse
    // only lets it through when beta is non-zero.
    ImGui::Checkbox("Use Dynamic Cubemaps as Fallback for Diffuse", &settings.UseDynamicCubemapsAsFallback);
    if (auto _tt = Util::HoverTooltipWrapper()) {
        if (settings.EnableAmbientReinjection)
            ImGui::Text(
                "With Ambient Reinjection on, this only supplies the cubemap that \"Cubemap Fill Blend\" (under Ambient Energy) mixes in. "
                "Off, or Fill Blend at 0, costs nothing.");
        else
            ImGui::Text("When ray marching misses, use dynamic cubemaps for reflections.");
    }
    ImGui::Checkbox("Use Dynamic Cubemaps as Fallback for Specular", &settings.UseDynamicCubemapsAsFallbackSpecular);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("When ray marching misses, use dynamic cubemaps for reflections. Recommended for specular.");
    ImGui::SliderFloat("Cubemap Normalization", &settings.CubemapNormalization, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Matches cubemap luminance with ambient color.");

    ImGui::SeparatorText("Denoiser");

    // (batch C1) The A/B switch. Both chains stay resident and hot-switch; the one
    // not selected dispatches nothing at all.
    {
        static const char* denoiserModes[] = { "Off", "SVGF", "REBLUR (NRD)" };
        int dm = (int)std::min(settings.DenoiserMethod, (uint)kDenoiserREBLUR);
        if (ImGui::Combo("Denoiser", &dm, denoiserModes, 3)) {
            settings.DenoiserMethod = (uint)dm;
            // Both transitions latch their own reset through UpdateHistoryValidity
            // next frame; nothing else to do here.
        }
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "What cleans up the grainy traced light. REBLUR (NVIDIA) = default, stable with clean reflections, slightly softer; "
                "SVGF = the older built-in one, sharper in places but longer trails behind moving objects; Off = raw, noisy result.\n"
                "Takes about a second to settle after switching.");
    }

    // (S1.3) One line of truth about what is actually running, whatever was asked for.
    // The old page printed three separate "REBLUR cannot run and the signal stays
    // undenoised" notices, which described a behaviour that no longer exists: an
    // unavailable REBLUR falls back to SVGF now, not to raw noise.
    if (denoiserFallbackReason) {
        ImGui::TextWrapped("Running %s instead of the selection above: %s",
            effectiveDenoiserDiffuse == kDenoiserSVGF || effectiveDenoiserSpecular == kDenoiserSVGF ? "SVGF" : "no denoiser",
            denoiserFallbackReason);
    }

    if (ReblurSelected()) {
        auto& nrdSvc = globals::features::nrd;
        if (!nrdSvc.loaded)
            ImGui::TextWrapped(
                "The NRD feature is not installed or failed to load, so SVGF is running instead of REBLUR.");
        else if (!nrdSvc.settings.Enabled)
            ImGui::TextWrapped(
                "NRD is turned off on its own feature page, so SVGF is running instead of REBLUR.");
        if (REL::Module::IsVR())
            ImGui::TextWrapped("REBLUR is not available in VR; SVGF runs instead.");

        bool reblurChanged = false;

        // (batch 36f) Cost switches. Both are runtime switches; the master switch under
        // Advanced > Batch 36f overrides them.
        ImGui::Checkbox("Skip Reflection Pre-pass", &settings.ReblurSkipSpecularPrepass);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Drops REBLUR's reflection pre-pass (~0.3 ms at 1440p). It was only used to track how reflections move, and our reflections are already clean. "
                "Risk: a little more smearing in reflections during fast camera turns. Off = the 36e behaviour.");
        ImGui::Checkbox("Fold Unpack Into Composite", &settings.ReblurFoldUnpack);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Converts REBLUR's output inside the final composite instead of in two extra full-screen passes (~0.15 ms). "
                "The picture is identical either way; Off = the 36e separate passes.");
        if (!Batch36f::IsOn())
            ImGui::TextDisabled("Batch 36f master switch is off (Advanced > Batch 36f): both run as in 36e.");

        // (S1.1) The confidence input, off by default and labelled for what it is.
        if (ImGui::Checkbox("Feed Hit Coverage as History Confidence (experimental)", &settings.ReblurFeedHitCoverageConfidence))
            reblurChanged = true;
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Experimental, off by default. Makes REBLUR average fewer frames where the rays found little nearby geometry: "
                "trails behind moving objects may get shorter, but open ground gets noisier and blotchier. Does nothing while the diffuse cubemap fallback is on.");
        if (ImGui::TreeNode("REBLUR Hit Distance Normalization")) {
            reblurChanged |= ImGui::SliderFloat("Hit Dist A (game units)", &settings.ReblurHitDistA, 1.0f, 1000.0f, "%.0f");
            if (auto _tt = Util::HoverTooltipWrapper())
                ImGui::Text("Advanced. The distance REBLUR treats as 'far' when judging traced light, in game units (NVIDIA's default 210 = 3 m). Works together with B.");
            reblurChanged |= ImGui::SliderFloat("Hit Dist B (per unit viewZ)", &settings.ReblurHitDistB, 0.0f, 1.0f, "%.3f");
            reblurChanged |= ImGui::SliderFloat("Hit Dist C (roughness scale)", &settings.ReblurHitDistC, 1.0f, 40.0f, "%.1f");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("REBLUR Diffuse (advanced)")) {
            reblurChanged |= nrdSvc.DrawReblurSettings(settings.ReblurDiffuse, true, "ssrt_reblur_diffuse");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("REBLUR Specular (advanced)")) {
            reblurChanged |= nrdSvc.DrawReblurSettings(settings.ReblurSpecular, true, "ssrt_reblur_specular");
            reblurChanged |= ImGui::SliderFloat("Specular Pre-pass Radius", &settings.SpecularPrepassBlurRadius, 0.0f, 75.0f, "%.1f px");
            if (auto _tt = Util::HoverTooltipWrapper())
                ImGui::Text("Blur applied to reflections before REBLUR's frame averaging. Higher = less noisy but blurrier reflections; 0 = off.");
            reblurChanged |= ImGui::Checkbox("Use Pre-pass Only for Motion Estimation", &settings.UsePrepassOnlyForSpecularMotionEstimation);
            ImGui::TreePop();
        }
        if (reblurChanged) {
            resetReblurDiffuse = true;
            resetReblurSpecular = true;
        }
    }

    // (batch 36f, item 2) Any denoiser: it limits the tracing as well as REBLUR.
    ImGui::Checkbox("Distance Limit", &settings.DistanceLimit);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Stops ray tracing and denoising beyond the distance below; far away you get the game's own ambient light and cubemap reflections, "
            "blended in smoothly over the last 20%%. Saves time in open landscapes, nothing indoors. "
            "Bounce light is only limited while Ambient Reinjection is on.");
    if (settings.DistanceLimit) {
        ImGui::SliderFloat("Distance Limit (m)", &settings.DistanceLimitMeters, 20.0f, 1000.0f, "%.0f m", ImGuiSliderFlags_AlwaysClamp);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Lower = faster, but the hand-over to vanilla lighting comes closer and may become noticeable. "
                "150 m is about the edge of the fully loaded area around you.");
        if (!settings.EnableAmbientReinjection)
            ImGui::TextDisabled("Ambient Reinjection is off: only reflections are limited.");
    }
    if (!Batch36f::IsOn())
        ImGui::TextDisabled("Batch 36f master switch is off (Advanced > Batch 36f): no distance limit.");

    if (SVGFSelected()) {
        ImGui::Checkbox("Pre-Blur", &settings.EnablePreBlur);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Bounce light only. A light blur on the raw rays before frames are averaged, so the image settles faster and motion drags less noise. "
                "Best kept on together with Firefly Clamp.");
        ImGui::SliderInt("Max Accumulated Frames", (int*)&settings.MaxAccumulatedFrames, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text("How many past frames are averaged together. Higher = smoother and less noisy, but longer trails behind moving objects.");
        ImGui::SliderInt("À Trous Iterations", (int*)&settings.AtrousIterations, 1, 5, "%d", ImGuiSliderFlags_AlwaysClamp);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text("Number of À Trous wavelet filter iterations. More iterations yield smoother results but may blur details and have a higher computational cost.");
        ImGui::SliderFloat("Color Phi", &settings.ColorPhi, 0.01f, 32.0f, "%.2f");
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "How different in brightness neighbouring pixels can be and still be blurred together. "
                "Lower = keeps more detail but leaves more noise (below about 1 it barely denoises); higher = smoother but blurrier.");
        ImGui::SliderFloat("Normal Phi", &settings.NormalPhi, 1.0f, 1024.0f, "%.2f");
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "How closely surface directions must match for neighbouring pixels to be blurred together. "
                "Higher = keeps more surface detail but denoises less (above about 256 it barely denoises bumpy surfaces).");

        ImGui::SliderFloat("Hit Distance Kernel Strength", &settings.HitRadiusStrength, 0.0f, 8.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Bounce light only. Blurs less where the light came from something close, so contact shadows and dark corners stay crisp. "
                "Higher = crisper contact shading; 0 = off.");

        ImGui::Checkbox("Firefly Clamp", &settings.FireflyClamp);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text("Removes single overly bright pixels (fireflies) before they get smeared into slowly fading blobs. Recommended on.");
        if (settings.FireflyClamp) {
            ImGui::SliderFloat("Firefly Clamp Sigma", &settings.FireflyClampSigma, 1.0f, 8.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            if (auto _tt = Util::HoverTooltipWrapper())
                ImGui::Text(
                    "How much brighter than its neighbours a pixel must be to count as a firefly. "
                    "Lower = removes more, but below about 2.65 it starts removing real highlights.");
        }

        ImGui::SliderFloat("History Clamp Sigma", &settings.HistoryClampSigma, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Stops old frames from leaving trails of stale lighting behind moving objects. "
                "Lower = shorter trails but more noise (noticeable below about 0.75); 0 = off.");

        ImGui::Checkbox("Adaptive Filtering", &settings.AdaptiveFiltering);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text("Skips some blur work in areas that are already clean, for more FPS. Off = the classic SVGF behaviour.");
        if (settings.AdaptiveFiltering) {
            ImGui::SliderInt("Adaptive History Threshold", (int*)&settings.AdaptiveHistoryThreshold, 4, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
            if (auto _tt = Util::HoverTooltipWrapper())
                ImGui::Text("How many frames a pixel must have been averaged before it can count as clean. Matching Max Accumulated Frames is a good default.");
            ImGui::SliderFloat("Adaptive Variance Threshold (relative)", &settings.AdaptiveVarianceEps, 1e-4f, 1.0f, "%.5f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
            if (auto _tt = Util::HoverTooltipWrapper())
                ImGui::Text("How little noise a pixel may have to count as clean. Higher = skips more work for more FPS, but may leave visible noise.");
        }

        if (settings.EnableSpecular) {
            ImGui::SliderFloat("Specular Mirror Cutoff", &settings.SpecularDenoiseRoughnessCutoff, 0.0f, 0.25f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
            if (auto _tt = Util::HoverTooltipWrapper())
                ImGui::Text(
                    "Mirror-like surfaces smoother than this (water, glass, polished metal) skip the denoiser blur, "
                    "saving GPU time with no visible change. 0 = off.");
        }
    }
#ifdef ENABLE_SHARC
    ImGui::Checkbox("(Broken) Enable SHARC", &settings.EnableSharc);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("(Experimental) Enables Spatially Hashed Radiance Cache (SHARC) to improve diffuse quality. This requires more memory and might impact performance.");
#endif
    ImGui::SeparatorText("Sampling");

    // (S3.10) Default on. Free, and it is the noise the rest of the pipeline is built to remove.
    ImGui::Checkbox("Blue Noise Sampling", &settings.UseBlueNoise);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Turns leftover grain into a fine, even pattern instead of drifting blotches, which the denoiser and upscaler clean up much better. "
            "Free; keep it on.");

    ImGui::SeparatorText("Debug");

    ImGui::Checkbox("Freeze Noise Phase", &settings.FreezeNoisePhase);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Diagnostic, not for normal play. Uses the same ray directions every frame, to tell whether smearing comes from the upscaler (goes away) "
            "or the denoiser (stays). Leaving it on locks a fixed noise pattern onto the screen.");

    // (batch 11, item C3) The five temporal-history diagnostics, moved behind a collapsed node
    // and shown only while SVGF is the denoiser that reads them.
    //
    // They cost nothing to leave switched on -- the production permutation of ssrt_temporal.hlsl
    // has every one of them `#define`d to the literal 0u, so fxc folds the branches away and
    // there is no GPU price to reclaim here. What there is to reclaim is the confusion: under
    // REBLUR, which is the default, ssrt_temporal.hlsl is not dispatched at all, so all five
    // switches do nothing whatsoever while sitting in the top level of the page. Their tooltips
    // still said "Requires Enable SVGF", which was a checkbox before the denoiser became a
    // dropdown, so the one hint they gave named a control that no longer exists.
    //
    // AnyChainSVGF() rather than the diffuse chain alone: the first four steer ssrt_temporal.hlsl,
    // which both chains dispatch, so a specular-only SVGF configuration must still reach them.
    // History Debug View is diffuse-only for a different reason (the two chains share one debug
    // surface and the specular pass deliberately passes 0), which its own tooltip states.
    //
    // Freeze Noise Phase stays outside: it seeds the ray-direction noise in ssrt_raymarch.hlsl
    // and is denoiser-independent, so it is as useful under REBLUR as under SVGF.
    if (!AnyChainSVGF()) {
        ImGui::TextDisabled("SVGF history diagnostics: select the SVGF denoiser to reach them.");
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text("These switches only affect the SVGF denoiser, which is not running now.");
    } else if (ImGui::TreeNode("SVGF History Diagnostics")) {
        ImGui::Checkbox("Disable History Depth Test", &settings.DisableHistoryDepthTest);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Diagnostic, not for normal play. Turns off the depth check that stops SVGF reusing old frames across object edges (trails come back). "
                "Try it and History Clamp Sigma 0 one at a time to find which one stops the image from settling.");

        ImGui::Checkbox("Disable History Normal Test", &settings.DisableHistoryNormalTest);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Diagnostic, not for normal play. Turns off the check that a surface still faces the same way as last frame. "
                "History Debug View shows which check to try first.");

        ImGui::Checkbox("Force Accept History", &settings.ForceAcceptHistory);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Diagnostic, not for normal play. Reuses old frames with no checks at all, so expect heavy smearing. "
                "If the image still will not settle with this on, the checks are not the problem.");

        ImGui::Checkbox("Rotated Normal Gate", &settings.RotatedNormalGate);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Diagnostic. A more accurate facing check that allows for the camera turning, so fast turns keep more history. "
                "Off by default; turn it on with History Debug View, and no magenta pixels means it works.");

        ImGui::Checkbox("History Debug View", &settings.HistoryDebugView);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Diagnostic; needs the SVGF denoiser and Enable Diffuse. Shows what SVGF decided about each pixel's history in "
                "texDebugHistory under Buffer Viewer; the game picture is not changed.\n"
                "Grey = working (brighter = more settled). Red = rejected by the depth check, green = by the facing check, blue = off screen or empty. "
                "Yellow/orange = depth check skipped. Magenta = Rotated Normal Gate correction failed. Black = sky.\n"
                "A few colored pixels at edges is normal; one color over the whole screen means that check is the problem.");

        ImGui::TreePop();
    }

	if (ImGui::TreeNode("Buffer Viewer")) {
		static float debugRescale = .3f;
		ImGui::SliderFloat("View Resize", &debugRescale, 0.f, 1.f);

        // (batch 11, item C1) This tree no longer pulls the SVGF history surfaces into existence.
        //
        // It used to raise a flag that SvgfHistoryNeeded() and the allocation gate in
        // ResolveDenoisers both read, on the argument that "a debug view that silently freezes
        // the moment the default denoiser is selected would be worse than the waste". Three
        // things were wrong with that trade:
        //
        //   * The waste was not small. The ten surfaces are 68 bytes per output pixel -- 134.5
        //     MiB at 1080p and 537.9 MiB at a 4K allocation -- and ResetFrameState is the only
        //     thing that releases them, which runs on a resolution change. Expanding the tree once
        //     therefore held that memory for the rest of the session, with the tree closed and
        //     the menu shut.
        //   * The maintenance was not small either: three full-screen copies per frame while the
        //     tree was open, ~330 MB/frame at a 4K allocation.
        //   * And the freeze it was buying off happened anyway. Six of the ten panels are written
        //     only inside the SVGF dispatch blocks, which do not run under REBLUR whatever this
        //     flag says -- so under the default denoiser the flag bought ten allocations and
        //     three copies per frame in order to display six cleared black rectangles.
        //
        // The honest presentation is the one below: show the panels when the denoiser that fills
        // them is running, and say so plainly when it is not.

		BUFFER_VIEWER_NODE(texDepth, debugRescale)
        BUFFER_VIEWER_NODE(texColor, debugRescale)
        BUFFER_VIEWER_NODE(texSSRColor, debugRescale)
        BUFFER_VIEWER_NODE(texSSRTDiffuseColor, debugRescale)
        BUFFER_VIEWER_NODE(texSSRTDiffuseConfidence, debugRescale)
        BUFFER_VIEWER_NODE(texSSRTDiffuseConfidenceSmooth, debugRescale)
        // (batch 6) The spatial filter's quarter-resolution working set, null until the frame
        // after the filter first runs. texSSRTConfidenceLo is the finished low-resolution field
        // (compare it against the raw surface two nodes up to see what the chain removes);
        // texSSRTConfidenceLoBlur is the horizontal pass only, so a difference between the two
        // is the vertical pass's contribution. The depth and normal guides should look like
        // smaller copies of the G-buffer with no blur across silhouettes -- if a guide is smeared
        // the geometric tests in the chain are being fed the wrong thing.
        if (texSSRTConfidenceLo)
            BUFFER_VIEWER_NODE(texSSRTConfidenceLo, debugRescale)
        if (texSSRTConfidenceLoBlur)
            BUFFER_VIEWER_NODE(texSSRTConfidenceLoBlur, debugRescale)
        if (texSSRTConfidenceLoNormal)
            BUFFER_VIEWER_NODE(texSSRTConfidenceLoNormal, debugRescale)
        // (reinjection noise) The accumulator's history. .x is the accumulated coverage -- compare
        // it against texSSRTDiffuseConfidenceSmooth above with the toggle off to see how much
        // per-frame grain the temporal window is removing -- and .z is the per-pixel frame count,
        // which is the direct read on the disocclusion test: it should ramp to the window length
        // over open ground and drop back to 1 in a thin band along the trailing edge of anything
        // moving, not over whole regions.
        //
        // (batch 10) Guarded, like every other on-demand entry in this tree. The pair is
        // allocated only by EnsureAmbientConfidenceResources (:1345), reached from the one
        // call site at :2932 under `EnableAmbientReinjection && TemporalAmbientConfidence &&
        // !confidenceFilter`, and SetupResources drops both at :1102-1103. With the shipped
        // defaults -- TemporalAmbientConfidence false and LowResConfidenceFilter true
        // (ScreenSpaceRayTracing.h) -- that condition never holds, so this is null out of the
        // box and expanding the node dereferenced it inside BUFFER_VIEWER_NODE
        // (src/Utils/UI.h:19 touches ->srv and ->desc). The guard outside the macro also
        // hides the label entirely rather than offering an empty node.
        if (texSSRTConfidenceHistoryPrev)
            BUFFER_VIEWER_NODE(texSSRTConfidenceHistoryPrev, debugRescale)
        // (batch 9) The two G-buffer channels that decide whether the ambient re-add can reach a
        // pixel at all. These are RenderTargetData, not our own Texture2D -- they have .SRV and
        // no desc -- so BUFFER_VIEWER_NODE cannot take them and the size comes from the screen.
        //
        // Reading them together, on eyes and hair:
        //   Albedo black + NormalRoughness showing a normal sphere/strands => the albedo was
        //     crushed by Lighting.hlsl's `outputAlbedo *= 1 - reflectance`, so the one channel
        //     that puts ambient light back (ssrtDiffuse * albedo) has nothing to multiply. This
        //     also implies Masks.z is 0, since Masks.z is computed after that multiply.
        //   Both black => the geometry is not in the deferred window at all, and no amount of
        //     albedo work will help.
        // Both cases look identical on Albedo alone -- src/Deferred.cpp:375 clears the G-buffer
        // every frame -- which is exactly why both have to be on screen at once.
        //
        // Prediction worth checking after the batch 9 eye fix: eyes should stop being black
        // holes here. The old chrome-eye F0 (~1 from the cubemap average) drove reflectance to
        // near 1, so `1 - reflectance` zeroed the albedo; pinning eye F0 back to 0.027 leaves
        // most of the albedo intact.
        {
            auto gbufferNode = [&](const char* label, const RE::BSGraphics::RenderTargetData& rt) {
                if (!rt.SRV)
                    return;
                if (ImGui::TreeNode(label)) {
                    Util::BufferViewerImage(rt.SRV,
                        { globals::state->screenSize.x * debugRescale, globals::state->screenSize.y * debugRescale });
                    ImGui::TreePop();
                }
            };
            auto& renderTargets = globals::game::renderer->GetRuntimeData().renderTargets;
            gbufferNode("G-Buffer Albedo", renderTargets[ALBEDO]);
            gbufferNode("G-Buffer NormalRoughness", renderTargets[NORMALROUGHNESS]);
        }
        // (batch 1, item 2) Black = the rays hit something within a texel or two, so the kernel
        // collapses towards the centre and contact detail survives. White = they went further
        // than the kernel reaches, or missed, so the kernel runs at full width. A healthy
        // exterior reads mostly white with dark outlines around contacts, creases and foliage.
        BUFFER_VIEWER_NODE(texSSRTDiffuseHitDistance, debugRescale)

        // (batch 11, item C1) The SVGF-only surfaces, shown only while SVGF is the denoiser
        // that writes them. AnyChainSVGF() rather than a per-chain test because these are one
        // allocation group: EnsureSvgfResources brings all ten up together the moment either
        // chain resolves to SVGF, and none of them is written by any other code path.
        //
        // (batch 16, item 5) Ten allocated, nine shown. The allocation group is texHistory,
        // texHistoryDiffuse, texTemporal, texVariance, texMoments, texHistoryMoments,
        // texHistoryMomentsDiffuse (7 x RGBA16F), texHistoryNormals (R10G10B10A2),
        // texDebugHistory (RGBA8) and texHistoryDepth (R32) -- 68 bytes per output pixel, which
        // is the number the 537.9 MiB figure comes from. texHistoryNormals has no viewer entry,
        // which is where the old "nine" came from.
        //
        // Every entry keeps its own null guard as well. The predicate says "SVGF is running", not
        // "the allocation succeeded", and a failed EnsureSvgfResources is exactly the case where
        // an unguarded BUFFER_VIEWER_NODE would dereference null on expand (batch 10: the macro
        // touches ->srv and ->desc inside the expanded branch, so the crash only shows up when
        // someone clicks the label).
        if (AnyChainSVGF()) {
            if (texHistory)
                BUFFER_VIEWER_NODE(texHistory, debugRescale)
            if (texHistoryDiffuse)
                BUFFER_VIEWER_NODE(texHistoryDiffuse, debugRescale)
            if (texTemporal)
                BUFFER_VIEWER_NODE(texTemporal, debugRescale)
            // (S2.8) The three moment surfaces rotate through each other every frame now -- the
            // temporal pass writes the scratch one and it then *becomes* the history rather than
            // being copied into it -- so the variable names no longer say which part a physical
            // texture is playing. These labels do: the two "current" entries are what the temporal
            // pass wrote this frame (and what the next frame will read as history), and "scratch"
            // is the buffer waiting to be overwritten. Menu draw happens after the frame's
            // dispatches, so this is the ownership as of end of frame.
            auto momentNode = [&](const char* label, const eastl::unique_ptr<Texture2D>& tex) {
                if (!tex)
                    return;
                if (ImGui::TreeNode(label)) {
                    Util::BufferViewerImage(tex->srv.get(),
                        { tex->desc.Width * debugRescale, tex->desc.Height * debugRescale });
                    ImGui::TreePop();
                }
            };
            momentNode("moments (diffuse, current)", texHistoryMomentsDiffuse);
            momentNode("moments (specular, current)", texHistoryMoments);
            momentNode("moments (scratch)", texMoments);

            if (texVariance)
                BUFFER_VIEWER_NODE(texVariance, debugRescale)
            if (texHistoryDepth)
                BUFFER_VIEWER_NODE(texHistoryDepth, debugRescale)
            if (texDebugHistory)
                BUFFER_VIEWER_NODE(texDebugHistory, debugRescale)
        } else {
            ImGui::TextDisabled("SVGF history buffers: select the SVGF denoiser to see them.");
            if (auto _tt = Util::HoverTooltipWrapper())
                ImGui::Text("These buffers only exist while SVGF is running, to save VRAM (about 540 MB at 4K).");
        }

        // (batch C1) REBLUR-path scratch; null until REBLUR is first selected.
        // (S2.7) One shared pair, so what these hold is whichever chain ran last in the
        // frame -- specular when it is enabled, diffuse otherwise.
        // (batch 12) The compact sparse set; null unless a sparse Diffuse Sampling mode is active.
        // Guarded because BUFFER_VIEWER_NODE dereferences unconditionally.
        if (texSparseColor)
            BUFFER_VIEWER_NODE(texSparseColor, debugRescale)
        if (texSparseConfidence)
            BUFFER_VIEWER_NODE(texSparseConfidence, debugRescale)
        if (texSparseHitDistance)
            BUFFER_VIEWER_NODE(texSparseHitDistance, debugRescale)

        if (texNRDPackInput)
            BUFFER_VIEWER_NODE(texNRDPackInput, debugRescale)
        if (texNRDPackOutput)
            BUFFER_VIEWER_NODE(texNRDPackOutput, debugRescale)
        if (auto validation = settings.ReblurDiffuse.EnableValidation ? nrdReblurDiffuse.GetValidationSRV() : nullptr) {
            if (ImGui::TreeNode("NRD Validation (Diffuse)")) {
                ImGui::Image(validation, { nrdReblurDiffuse.GetWidth() * debugRescale, nrdReblurDiffuse.GetHeight() * debugRescale });
                ImGui::TreePop();
            }
        }
        if (auto validation = settings.ReblurSpecular.EnableValidation ? nrdReblurSpecular.GetValidationSRV() : nullptr) {
            if (ImGui::TreeNode("NRD Validation (Specular)")) {
                ImGui::Image(validation, { nrdReblurSpecular.GetWidth() * debugRescale, nrdReblurSpecular.GetHeight() * debugRescale });
                ImGui::TreePop();
            }
        }

		ImGui::TreePop();
	}

    JiayeStatement::GetSingleton()->DrawJSInfo();
}

void ScreenSpaceRayTracing::RestoreDefaultSettings()
{
    settings = {};
}

// (S1.4) One clamp site for every setting whose out-of-range value is a hazard rather than
// merely a bad look. The UI is not the entry point that needs this -- its sliders carry
// ImGuiSliderFlags_AlwaysClamp -- LoadSettings is: a hand-edited json, a config written by a
// future build, or a partially-migrated one all arrive here unfiltered.
//
// Three classes are represented, and the reason they share a function is that they used to be
// scattered across the call sites that happened to notice them:
//   * dispatch geometry (DiffuseSPP, MaxMips, AtrousIterations) -- a value outside range makes
//     a dispatch or a shader permutation illegal rather than ugly;
//   * numerical guards (the phi and sigma family, AdaptiveVarianceEps) -- zero or negative
//     turns an edge-stopping weight into a divide by zero or an exp() of a positive number;
//   * REBLUR's hit-distance constants, which the pack shader and nrd::ReblurSettings must
//     agree on: A <= 0 makes the normalisation curve non-monotonic.
void ScreenSpaceRayTracing::SanitizeSettings()
{
    // (batch C1) An out-of-range value must not fall through the dispatch gates as
    // "neither SVGF nor REBLUR but not Off either".
    settings.DenoiserMethod = std::min(settings.DenoiserMethod, (uint)kDenoiserREBLUR);

    // DIFFUSE_SPP is a compile-time macro and the ray march's sample loop divides by it.
    // 0 is a divide by zero in the estimator; the 16 ceiling is the Hammersley table's.
    settings.DiffuseSPP = std::clamp(settings.DiffuseSPP, 1u, 16u);
    // (batch 12) An out-of-range value must not fall through ResolveSamplingMode's switch as
    // "sparse, but neither of the two sparse modes", which would leave the compact set allocated
    // and no shader selected to write it.
    settings.DiffuseSamplingMode = std::min(settings.DiffuseSamplingMode, (uint)kSamplingCheckerboard);
    // (audit P3) The traversal can load exactly mip MaxMips, and mip maxMips-1 is the last
    // one allocated; an out-of-range Load returns 0 == near plane, i.e. an instant false hit.
    settings.MaxMips = std::clamp(settings.MaxMips, 1u, maxMips - 1u);
    settings.MaxSteps = std::clamp(settings.MaxSteps, 1u, 256u);
    // The a-trous loop's ping-pong parity and its history feed both assume >= 1.
    settings.AtrousIterations = std::clamp(settings.AtrousIterations, 1u, 5u);
    settings.MaxAccumulatedFrames = std::clamp(settings.MaxAccumulatedFrames, 1u, 64u);
    settings.AdaptiveHistoryThreshold = std::clamp(settings.AdaptiveHistoryThreshold, 4u, 64u);

    settings.Thickness = std::clamp(settings.Thickness, 0.0f, 500.0f);
    settings.NormalBias = std::clamp(settings.NormalBias, 0.0f, 1.0f);
    settings.BRDFBias = std::clamp(settings.BRDFBias, 0.0f, 1.0f);
    settings.SpecularMult = std::clamp(settings.SpecularMult, 0.0f, 5.0f);
    settings.DiffuseMult = std::clamp(settings.DiffuseMult, 0.01f, 5.0f);
    settings.AmbientMult = std::clamp(settings.AmbientMult, 0.0f, 1.0f);
    settings.AmbientReinjectionStrength = std::clamp(settings.AmbientReinjectionStrength, 0.0f, 1.0f);
    // (batch 8) beta is a blend fraction. Above 1 it would report a confidence over 1 -- which the
    // shader's saturate() would eat, leaving the fill radiance over-weighted relative to the
    // ambient the composite then removes, i.e. net energy gain. Below 0 it would subtract cubemap
    // light from the frame.
    settings.CubemapFillBlend = std::clamp(settings.CubemapFillBlend, 0.0f, 1.0f);
    // (reinjection noise) 1 is "no accumulation", i.e. alpha == 1 and the spatial mean straight
    // through, so the low end is inert rather than degenerate; the 60 ceiling matches the slider
    // and keeps the fp16 frame counter in .z far inside its exact range.
    settings.AmbientConfidenceMaxFrames = std::clamp(settings.AmbientConfidenceMaxFrames, 1u, 60u);
    settings.OcclusionStrength = std::clamp(settings.OcclusionStrength, 0.0f, 1.0f);
    settings.CubemapNormalization = std::clamp(settings.CubemapNormalization, 0.0f, 1.0f);

    // phiLuminance = ColorPhi * sqrt(variance) and the a-trous weight is exp(-k / ColorPhi);
    // 0 is a divide by zero and negative inverts the edge-stop into an edge-*seeker*.
    settings.ColorPhi = std::clamp(settings.ColorPhi, 0.01f, 32.0f);
    settings.NormalPhi = std::clamp(settings.NormalPhi, 1.0f, 1024.0f);
    settings.HitRadiusStrength = std::clamp(settings.HitRadiusStrength, 0.0f, 8.0f);
    settings.FireflyClampSigma = std::clamp(settings.FireflyClampSigma, 1.0f, 8.0f);
    settings.HistoryClampSigma = std::clamp(settings.HistoryClampSigma, 0.0f, 4.0f);
    settings.AdaptiveVarianceEps = std::clamp(settings.AdaptiveVarianceEps, 1e-4f, 1.0f);
    settings.SpecularDenoiseRoughnessCutoff = std::clamp(settings.SpecularDenoiseRoughnessCutoff, 0.0f, 0.25f);
    settings.SpecularMaxRoughness = std::clamp(settings.SpecularMaxRoughness, 0.05f, 1.0f);

    settings.ReblurHitDistA = std::clamp(settings.ReblurHitDistA, 1.0f, 1000.0f);
    settings.ReblurHitDistB = std::clamp(settings.ReblurHitDistB, 0.0f, 1.0f);
    settings.ReblurHitDistC = std::clamp(settings.ReblurHitDistC, 1.0f, 40.0f);
    settings.SpecularPrepassBlurRadius = std::clamp(settings.SpecularPrepassBlurRadius, 0.0f, 75.0f);
    // (batch 36f) Same range as the slider. Far below 20 m the limit would cut SSRT off at
    // arm's length; the 1000 m ceiling is far past where screen-space rays find anything.
    settings.DistanceLimitMeters = std::clamp(settings.DistanceLimitMeters, 20.0f, 1000.0f);
}

void ScreenSpaceRayTracing::LoadSettings(json& o_json)
{
    settings = o_json;
    SanitizeSettings();
    // (S1.4) DiffuseSPP is a compile-time macro. The UI path set recompileFlag; this one did
    // not, so a loaded value the compiled permutation does not implement was traced silently.
    // Prepass now compares settings.DiffuseSPP against compiledDiffuseSPP directly, which
    // covers *every* path that can change it -- including this one -- with no flag to forget.
}

void ScreenSpaceRayTracing::SaveSettings(json& o_json)
{
    o_json = settings;
}

void ScreenSpaceRayTracing::SetupResources()
{
    // (S4.15) This is not only the boot path — BSShaderRenderTargets_Create re-runs
    // State::Setup() whenever the game recreates its render targets, i.e. on a resolution
    // change. Everything below is about to be reallocated, so every latch that describes the
    // old allocation has to go with it, in one place, before the first of them is rebuilt.
    ResetFrameState();

    auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	logger::debug("Creating buffers...");
	{
        ssrtCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<SSRTCB>());
        denoiserCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<DenoiserCB>());
        compositeCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<CompositeCB>());
        // (batch 11, item A) nrdPackCB is gone with ssrt_nrd_pack.hlsl: its three constants ride
        // on row 3 of SSRTCB, which the ray march already binds.
    }

    logger::debug("Creating textures...");
    {
        auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
        D3D11_TEXTURE2D_DESC texDesc = {};
        mainTex.texture->GetDesc(&texDesc);
        // (audit P9) None of these textures is ever bound as a render target or passed
        // to GenerateMips -- the only GenerateMips call in this feature is commented out
        // in Prepass, and no RTV is ever created. D3D11_RESOURCE_MISC_GENERATE_MIPS also
        // *requires* BIND_RENDER_TARGET, so the two go together; dropping both lets the
        // driver pick a layout without RT compression metadata for ~14 full-screen
        // surfaces (plus the whole depth pyramid).
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        // Explicitly 0 rather than leaving whatever kMAIN carried: MiscFlags used to be
        // OR-ed into, and any inherited GENERATE_MIPS would now fail creation because it
        // requires BIND_RENDER_TARGET.
        texDesc.MiscFlags = 0;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = texDesc.MipLevels }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

        texColor = eastl::make_unique<Texture2D>(texDesc);
        texColor->CreateSRV(srvDesc);
        texColor->CreateUAV(uavDesc);
        texSSRColor = eastl::make_unique<Texture2D>(texDesc);
        texSSRColor->CreateSRV(srvDesc);
        texSSRColor->CreateUAV(uavDesc);
        texSSRTDiffuseColor = eastl::make_unique<Texture2D>(texDesc);
        texSSRTDiffuseColor->CreateSRV(srvDesc);
        texSSRTDiffuseColor->CreateUAV(uavDesc);

        // (S2.5) texHistory / texHistoryDiffuse / texTemporal / texVariance, the moment trio,
        // texHistoryNormals, texHistoryDepth and texDebugHistory have moved to
        // EnsureSvgfResources: they are read by ssrt_temporal.hlsl and the passes around it and
        // by nothing else, so under REBLUR or Off they were 68 bytes per output pixel of
        // allocation that no dispatch touched.

        // (defect D5) The moment pair stays at R16G16B16A16_FLOAT -- i.e. it keeps the format
        // set above rather than dropping to R11G11B10_FLOAT as it used to.
        //
        // This is the storage the whole variance channel is built on, and 11-bit floats cannot
        // carry it. R11 and G11 have a 6-bit mantissa, so round-to-nearest costs up to
        // 2^-7 = 0.78% of the value; B10 has a 5-bit one. Three separate failures follow, and
        // together they are what made MaxAccumulatedFrames inert:
        //
        //   * Cancellation. ssrt_temporal.hlsl publishes variance = mu2 - mu1^2, and for a
        //     pixel whose per-frame relative sigma is s those two operands differ by only
        //     s^2 / (1 + s^2) of their own magnitude. Quantising mu1 alone already costs
        //     2 * 0.78% = 1.56% of mean^2 in mu1^2, which *exceeds the entire variance* below
        //     s = 0.125 and is still 69% of it at s = 0.15. That is not an exotic corner: with
        //     ambient reinjection off and the cubemap fallback carrying the ray misses, much of
        //     the screen is a largely deterministic signal with exactly that little per-frame
        //     spread. The published variance there is quantisation error, not variance -- and
        //     because the mean is spatially smooth, neighbouring texels round the same way, so
        //     the error is *region shaped* rather than per-pixel. The a-trous luminance
        //     edge-stop it steers therefore flips whole patches of the image between "blur" and
        //     "copy", and the patch boundaries move as the accumulated mean drifts by one
        //     quantum. That is the low-frequency temporal flicker the denoiser was reported to
        //     *add* rather than remove.
        //
        //   * An EMA dead zone that gets *worse* with a longer window, which is the direct
        //     mechanism behind "MaxAccumulatedFrames changes nothing". The moment update is
        //     lerp(prev, cur, alpha) with alpha = 1 / (MaxAccumulatedFrames + 1), so the step
        //     it asks the storage to take is alpha * |cur - prev|. At s = 0.15 the second
        //     moment's own frame-to-frame spread is |cur - prev| ~ 2s * mean^2 = 0.3 * mean^2,
        //     so the step is 0.018 * mean^2 at alpha = 1/17 -- above the 0.0078 * mean^2
        //     rounding threshold -- and 0.0046 * mean^2 at alpha = 1/65, *below* it. Past
        //     roughly MaxAccumulatedFrames 32 the write simply rounds back to the value already
        //     there and the estimate freezes. Raising the slider did not lengthen the
        //     accumulation; it stopped the moments updating at all.
        //
        //   * The frame counter in .z. A 5-bit mantissa represents every integer up to 64 and
        //     no odd one above it, so accumFrames + 1 rounds back to 64 forever -- the count
        //     latched at exactly the top of the slider's own range.
        //
        // R16G16B16A16_FLOAT costs 8 bytes per texel instead of 4 (three full-screen surfaces,
        // so ~+24 MB at 1080p and ~+100 MB at a 4K allocation, against the seven RGBA16F
        // surfaces this feature already holds) and buys a 10-bit mantissa, i.e. a 16x smaller
        // rounding step: the mu1^2 cancellation error falls to 4.4% of the variance at
        // s = 0.15 (and only reaches 100% below s = 0.031), the alpha = 1/65 update clears the
        // rounding threshold by 9x, denormals resolve to 6e-8 instead of 9.5e-7, and the frame
        // count is exact past 2000. The exponent range is identical -- both formats carry 5
        // exponent bits -- so nothing about the representable magnitude changes.
        //
        // SSRT_MOMENT_LUMINANCE_MAX in ssrt_temporal.hlsl needs no change: fp16's largest
        // finite value is 65504, so the 250 ceiling (250^2 = 62500) still keeps the second
        // moment representable, and it was already derived against a very similar bound.
        // (S2.5) The moment trio and texHistoryNormals live in EnsureSvgfResources now; the
        // derivation above still governs their format, which that function repeats verbatim.

        // (ambient reinjection) The confidence pair. R8_UNORM because the quantity is a
        // coverage fraction in [0,1]: 1/255 quantisation is an order of magnitude below the
        // ~0.07 residual noise of the 7x7 spatial mean that produces the second surface, and a
        // UNORM read cannot be non-finite, so no consumer needs its own guard. One byte per
        // texel is 7.9 MiB each at a 4K allocation, against 63.3 MiB for each of the three
        // unconditional RGBA16F surfaces above (texColor, texSSRColor, texSSRTDiffuseColor).
        //
        // (batch 16, item 5) Both numbers in that sentence used to be wrong: "~33 MB each" was
        // half the real figure -- R16G16B16A16_FLOAT is 8 bytes per pixel, so a full-screen
        // surface is 8 * 3840 * 2160 = 63.3 MiB, not 33 -- and "eight" was a miscount of the
        // three allocated above this line.
        texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R8_UNORM;
        texSSRTDiffuseConfidence = eastl::make_unique<Texture2D>(texDesc);
        texSSRTDiffuseConfidence->CreateSRV(srvDesc);
        texSSRTDiffuseConfidence->CreateUAV(uavDesc);
        texSSRTDiffuseConfidenceSmooth = eastl::make_unique<Texture2D>(texDesc);
        texSSRTDiffuseConfidenceSmooth->CreateSRV(srvDesc);
        texSSRTDiffuseConfidenceSmooth->CreateUAV(uavDesc);

        // (reinjection noise) Release the confidence accumulator rather than allocating it here.
        // It is created on demand by EnsureAmbientConfidenceResources, but this function runs
        // again on a resolution change and every surface around it is rebuilt at the new extent --
        // so a pair left resident would keep the *old* extent, and the accumulator would sample
        // its history through a mismatched reprojection. Dropping them makes the next frame that
        // needs them allocate at the current size.
        texSSRTConfidenceHistory.reset();
        texSSRTConfidenceHistoryPrev.reset();

        // (batch 6) The spatial filter's quarter-resolution set, released for exactly the reason
        // above: their extent is derived from this function's texDesc, so a set left resident
        // across a resolution change would be addressed with a low-resolution grid the surfaces do
        // not have -- and unlike the accumulator that is not a subtly wrong sample, it is a write
        // outside the texture. EnsureConfidenceFilterResources rebuilds them at the new size on
        // the next frame that needs them.
        texSSRTConfidenceLo.reset();
        texSSRTConfidenceLoBlur.reset();
        texSSRTConfidenceLoDepth.reset();
        texSSRTConfidenceLoNormal.reset();

        // (batch 1, item 2) The diffuse hit-distance surface. Same R8_UNORM as the confidence
        // pair above and for the same three reasons: the payload is a [0,1] fraction, 1/255 is
        // an order of magnitude below the granularity of what it steers (a smooth per-pixel
        // weight over one-texel tap offsets), and a UNORM read cannot be non-finite so no
        // consumer needs a guard of its own. 7.9 MiB of a 4K allocation, against 63.3 MiB for
        // each of the three unconditional RGBA16F surfaces this feature already holds.
        // (batch 16, item 5) Corrected: see the note on texSSRTDiffuseConfidence above.
        //
        // No entry in ClearDenoiserHistory: this surface is rewritten in full by every diffuse
        // ray-march dispatch before anything reads it, so it carries no state across frames and
        // has nothing to reset.
        texSSRTDiffuseHitDistance = eastl::make_unique<Texture2D>(texDesc);
        texSSRTDiffuseHitDistance->CreateSRV(srvDesc);
        texSSRTDiffuseHitDistance->CreateUAV(uavDesc);

        // (S2.5) texDebugHistory moved to EnsureSvgfResources.

        texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R32_FLOAT;

        // (audit #20) Was a bare `new Texture2D` and therefore leaked. It is *not*
        // dead: Upscaling.cpp copies it into specHitDistanceShared12 as the DLSS-RR
        // specular hit-distance guide whenever Ray Reconstruction is enabled.
        texHitDistance = eastl::make_unique<Texture2D>(texDesc);
        texHitDistance->CreateSRV(srvDesc);
        texHitDistance->CreateUAV(uavDesc);

        // (S2.5) texHistoryDepth moved to EnsureSvgfResources.

        texDesc.MipLevels = maxMips;
        srvDesc.Texture2D.MipLevels = texDesc.MipLevels;
        texDepth = eastl::make_unique<Texture2D>(texDesc);
        texDepth->CreateSRV(srvDesc);
        texDepth->CreateUAV(uavDesc);

        for (uint i = 0; i < maxMips; i++) {
			D3D11_SHADER_RESOURCE_VIEW_DESC mipSrvDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MostDetailedMip = i, .MipLevels = 1 }
			};
			DX::ThrowIfFailed(device->CreateShaderResourceView(texDepth->resource.get(), &mipSrvDesc, depthSRVs[i].put()));

			D3D11_UNORDERED_ACCESS_VIEW_DESC mipUavDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = i }
			};
			DX::ThrowIfFailed(device->CreateUnorderedAccessView(texDepth->resource.get(), &mipUavDesc, depthUAVs[i].put()));
		}
    }

    logger::debug("Creating samplers...");
	{
		D3D11_SAMPLER_DESC samplerDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
			.MaxAnisotropy = 1,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearSampler.put()));
	}

    logger::debug("Loading noise texture...");
    {
        DirectX::CreateDDSTextureFromFile(device, globals::d3d::context, L"Data\\Shaders\\ScreenSpaceRayTracing\\noise.dds",
            nullptr, noiseSRV.put());
    }

	CompileComputeShaders();
}

#ifdef ENABLE_SHARC
// (audit P6) The four SHARC buffers are 44 MB that used to be allocated at feature
// setup even though SHARC is experimental, marked "(Broken)" in the UI and off by
// default. Create them the first time SHARC is actually switched on instead. Called
// from DrawSSRTDiffuse before the first dispatch that binds them, so a runtime enable
// never dispatches against null UAVs; disabling again binds nullptr rather than
// releasing, so there is nothing to dangle.
void ScreenSpaceRayTracing::EnsureSharcResources()
{
    if (sharcHashEntries && sharcHashCopyOffsets && sharcVoxelData && sharcVoxelDataPrev)
        return;

    logger::debug("Creating SHARC buffers...");
	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DEFAULT;
		sbDesc.CPUAccessFlags = 0;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.FirstElement = 0;
		uavDesc.Buffer.Flags = 0;

        std::uint32_t numEntries = sharcNumEntries;

        // Hash entries buffer - structured buffer with 64-bits entries to store the hashes
        // Voxel data buffer - structured buffer with 128-bit entries which stores accumulated radiance and sample count. Two instances are used to store current and previous frame data

        sbDesc.StructureByteStride = sizeof(std::uint64_t);
        sbDesc.ByteWidth = sbDesc.StructureByteStride * numEntries;
        sharcHashEntries = eastl::make_unique<Buffer>(sbDesc);
        srvDesc.Buffer.NumElements = numEntries;
        uavDesc.Buffer.NumElements = numEntries;
        sharcHashEntries->CreateSRV(srvDesc);
        sharcHashEntries->CreateUAV(uavDesc);

        sbDesc.StructureByteStride = sizeof(std::uint32_t);
        sbDesc.ByteWidth = sbDesc.StructureByteStride * numEntries;
        sharcHashCopyOffsets = eastl::make_unique<Buffer>(sbDesc);
        srvDesc.Buffer.NumElements = numEntries;
        uavDesc.Buffer.NumElements = numEntries;
        sharcHashCopyOffsets->CreateSRV(srvDesc);
        sharcHashCopyOffsets->CreateUAV(uavDesc);

        sbDesc.StructureByteStride = 4 * sizeof(uint32_t);
        sbDesc.ByteWidth = numEntries * sizeof(float4);
        sharcVoxelData = eastl::make_unique<Buffer>(sbDesc);
        srvDesc.Buffer.NumElements = numEntries;
        uavDesc.Buffer.NumElements = numEntries;
        sharcVoxelData->CreateSRV(srvDesc);
        sharcVoxelData->CreateUAV(uavDesc);
        sharcVoxelDataPrev = eastl::make_unique<Buffer>(sbDesc);
        sharcVoxelDataPrev->CreateSRV(srvDesc);
        sharcVoxelDataPrev->CreateUAV(uavDesc);
	}
}
#endif

// (S2.5) The SVGF-only surfaces, allocated on first need rather than at boot -- the same
// argument EnsureSharcResources and EnsureNRDResources make, applied to the chain that is no
// longer the default.
//
// Ten textures, 68 bytes per output pixel between them: 134.5 MiB at 1080p, 537.9 MiB at a 4K
// allocation. ssrt_temporal.hlsl and the three passes around it are their only readers (plus
// the Buffer Viewer), and none of those is dispatched under REBLUR or Off. The t0
// declarations in ssrt_raymarch / ssrt_variance / ssrt_spatial name the colour histories but
// never reference them, so fxc strips the bindings and those passes do not count as readers.
//
// Called from ResolveDenoisers on every path that has concluded SVGF will run -- including
// the REBLUR-unavailable fallback -- so no consumer can reach a null surface. (batch 11, item
// C1: the Buffer Viewer used to be a third path into here and no longer is.)
// Switching away afterwards keeps them resident, which is what makes
// the A/B toggle instant in both directions; only a resolution change releases them.
void ScreenSpaceRayTracing::EnsureSvgfResources()
{
    if (texHistoryDiffuse && texHistory && texTemporal && texVariance && texMoments &&
        texHistoryMoments && texHistoryMomentsDiffuse && texHistoryNormals && texHistoryDepth &&
        texDebugHistory)
        return;

    logger::debug("Creating SSRT SVGF resources...");

    auto renderer = globals::game::renderer;
    auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
    D3D11_TEXTURE2D_DESC texDesc{};
    mainTex.texture->GetDesc(&texDesc);
    // (audit P9) No RTV is ever created for any of these and none is passed to GenerateMips,
    // so the render-target bind flag and its compression metadata are pure cost. MiscFlags is
    // set rather than OR-ed because GENERATE_MIPS requires BIND_RENDER_TARGET.
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    texDesc.MiscFlags = 0;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
        .Format = texDesc.Format,
        .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
        .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
    };
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
        .Format = texDesc.Format,
        .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
        .Texture2D = { .MipSlice = 0 }
    };

    auto makeTex = [&](eastl::unique_ptr<Texture2D>& tex, const char* name) {
        if (!tex) {
            tex = eastl::make_unique<Texture2D>(texDesc);
            tex->CreateSRV(srvDesc);
            tex->CreateUAV(uavDesc);
            Util::SetResourceName(tex->resource.get(), name);
        }
    };

    makeTex(texHistory, "SSRT::HistorySpecular");
    makeTex(texHistoryDiffuse, "SSRT::HistoryDiffuse");
    makeTex(texTemporal, "SSRT::Temporal");
    makeTex(texVariance, "SSRT::Variance");
    // (defect D5) The moment trio stays R16G16B16A16_FLOAT. See the derivation in
    // SetupResources: at R11G11B10 the mu2 - mu1^2 cancellation, the EMA dead zone and the
    // integer frame counter in .z all fail, which is what made MaxAccumulatedFrames inert.
    makeTex(texMoments, "SSRT::Moments");
    makeTex(texHistoryMoments, "SSRT::HistoryMomentsSpecular");
    makeTex(texHistoryMomentsDiffuse, "SSRT::HistoryMomentsDiffuse");

    texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    makeTex(texHistoryNormals, "SSRT::HistoryNormals");

    // (diagnostic H) Three display colours plus an alpha the Buffer Viewer ignores, so a byte
    // per channel is exactly enough; being UNORM, no read of it can be non-finite.
    texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    makeTex(texDebugHistory, "SSRT::DebugHistory");

    // (defect D3) Previous frame's raw depth, same format and extent as mip 0 of the Hi-Z
    // pyramid that CopyHistoryGeometry snapshots it from. The UAV exists only so
    // ClearDenoiserHistory can reset it to the far plane.
    texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
    makeTex(texHistoryDepth, "SSRT::HistoryDepth");

    // Freshly allocated, so their contents are undefined -- which for RGBA16F includes every
    // NaN and Inf bit pattern. Guard G8's flag is the mechanism that makes that safe.
    historyClearPending = true;
}

// (reinjection noise) The confidence accumulator's ping-pong pair, allocated on first need on
// the same argument EnsureSvgfResources makes: 63.3 MiB each, 126.6 MiB for the pair, at a 4K
// allocation (R16G16B16A16_FLOAT, 8 bytes per pixel). (batch 16, item 5) This line used to say
// "~66 MB between them", which is the figure for one surface, not two. And
// nothing reads either surface unless ambient reinjection and TemporalAmbientConfidence are
// both on. Unlike the SVGF set these are *not* released when the toggle goes off, so the A/B is
// instant in both directions; only a resolution change releases them.
bool ScreenSpaceRayTracing::EnsureAmbientConfidenceResources()
{
    if (texSSRTConfidenceHistory && texSSRTConfidenceHistoryPrev)
        return true;

    logger::debug("Creating SSRT ambient confidence accumulator...");

    auto renderer = globals::game::renderer;
    if (!renderer)
        return false;

    auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
    if (!mainTex.texture)
        return false;

    D3D11_TEXTURE2D_DESC texDesc{};
    mainTex.texture->GetDesc(&texDesc);
    // Same reasoning as every other surface in this feature: no RTV is created for either and
    // neither is passed to GenerateMips, so the render-target bind flag and its compression
    // metadata are pure cost. MiscFlags set rather than OR-ed, because an inherited
    // GENERATE_MIPS would fail creation once BIND_RENDER_TARGET is gone.
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    texDesc.MiscFlags = 0;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
        .Format = texDesc.Format,
        .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
        .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
    };
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
        .Format = texDesc.Format,
        .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
        .Texture2D = { .MipSlice = 0 }
    };

    auto makeTex = [&](eastl::unique_ptr<Texture2D>& tex, const char* name) {
        if (!tex) {
            tex = eastl::make_unique<Texture2D>(texDesc);
            tex->CreateSRV(srvDesc);
            tex->CreateUAV(uavDesc);
            Util::SetResourceName(tex->resource.get(), name);
        }
    };

    makeTex(texSSRTConfidenceHistory, "SSRT::ConfidenceHistory");
    makeTex(texSSRTConfidenceHistoryPrev, "SSRT::ConfidenceHistoryPrev");

    if (!texSSRTConfidenceHistory || !texSSRTConfidenceHistoryPrev)
        return false;

    // Freshly allocated contents are undefined, and for RGBA16F that includes every NaN and Inf
    // bit pattern. The accumulator's own finiteness test rejects those, but a NaN that survived
    // into .z would read as "history exists" under a naive comparison, so the surfaces are zeroed
    // here as well: accumFrames 0 is the same "no history" state a disocclusion produces, which
    // the pass already handles.
    if (auto context = globals::d3d::context) {
        const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        context->ClearUnorderedAccessViewFloat(texSSRTConfidenceHistory->uav.get(), zero);
        context->ClearUnorderedAccessViewFloat(texSSRTConfidenceHistoryPrev->uav.get(), zero);
    }

    return true;
}

// (batch 6) The spatial confidence filter's quarter-resolution working set, allocated on first
// need on the same argument EnsureAmbientConfidenceResources makes -- and released only by a
// resolution change, so the A/B toggle is instant in both directions.
//
// Half width, half height, rounded up, so that an odd render extent still has a low-resolution
// texel covering its last column and row. Derived from the kMAIN allocation rather than from the
// current dynamic-resolution sub-rect, because the sub-rect changes per frame while the
// allocation does not; every pass computes the *used* extent itself from BufferDim and
// DynamicResolutionParams1, exactly as the rest of this feature does.
bool ScreenSpaceRayTracing::EnsureConfidenceFilterResources()
{
    if (texSSRTConfidenceLo && texSSRTConfidenceLoBlur && texSSRTConfidenceLoDepth && texSSRTConfidenceLoNormal)
        return true;

    logger::debug("Creating SSRT confidence filter resources...");

    auto renderer = globals::game::renderer;
    if (!renderer)
        return false;

    auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
    if (!mainTex.texture)
        return false;

    D3D11_TEXTURE2D_DESC texDesc{};
    mainTex.texture->GetDesc(&texDesc);
    // Same reasoning as every other surface in this feature: no RTV is created for any of these
    // and none is passed to GenerateMips, so the render-target bind flag and its compression
    // metadata are pure cost. MiscFlags set rather than OR-ed, because an inherited GENERATE_MIPS
    // would fail creation once BIND_RENDER_TARGET is gone.
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    texDesc.MiscFlags = 0;
    texDesc.MipLevels = 1;
    texDesc.Width = std::max(1u, (texDesc.Width + 1u) / 2u);
    texDesc.Height = std::max(1u, (texDesc.Height + 1u) / 2u);

    auto makeTex = [&](eastl::unique_ptr<Texture2D>& tex, DXGI_FORMAT format, const char* name) {
        if (tex)
            return;
        texDesc.Format = format;
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
            .Format = format,
            .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
        };
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
            .Format = format,
            .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MipSlice = 0 }
        };
        tex = eastl::make_unique<Texture2D>(texDesc);
        tex->CreateSRV(srvDesc);
        tex->CreateUAV(uavDesc);
        Util::SetResourceName(tex->resource.get(), name);
    };

    // R8_UNORM for the two confidence surfaces, for the reason the full-resolution pair carries
    // it: the payload is a coverage fraction in [0,1] and 1/255 sits well below the residual noise
    // of even the finished field, so the storage is not what limits the result. A UNORM read also
    // cannot be non-finite, so no consumer needs a guard.
    makeTex(texSSRTConfidenceLo, DXGI_FORMAT_R8_UNORM, "SSRT::ConfidenceLo");
    makeTex(texSSRTConfidenceLoBlur, DXGI_FORMAT_R8_UNORM, "SSRT::ConfidenceLoBlur");
    // R32_FLOAT for the depth guide, and this one is load bearing rather than conservative: the
    // blur's plane test differences two near-equal reciprocals of it to recover the surface slope,
    // and fp16's 11-bit mantissa would put ~1e-3 of relative noise straight into a gradient whose
    // legitimate magnitude is 2e-3 face-on. Distant LOD terrain also runs past fp16's 65504
    // ceiling outright.
    makeTex(texSSRTConfidenceLoDepth, DXGI_FORMAT_R32_FLOAT, "SSRT::ConfidenceLoDepth");
    // R8G8B8A8_SNORM for the normal guide: the components are a unit view-space vector, so a
    // signed normalised format is the exact shape of the data, and 8 bits per axis is ~0.4 degrees
    // of angular error against the 45 degree gate that reads it -- two orders of magnitude of
    // headroom. The unused .w is written 0.
    makeTex(texSSRTConfidenceLoNormal, DXGI_FORMAT_R8G8B8A8_SNORM, "SSRT::ConfidenceLoNormal");

    // No clear here, unlike the accumulator: every one of these surfaces is fully rewritten by its
    // own dispatch before anything reads it, within the same frame and before the composite, so
    // none of them carries state and none can present undefined memory to a consumer.
    return texSSRTConfidenceLo && texSSRTConfidenceLoBlur && texSSRTConfidenceLoDepth && texSSRTConfidenceLoNormal;
}

bool ScreenSpaceRayTracing::EnsureSparseResources()
{
    if (texSparseColor && texSparseConfidence && texSparseHitDistance)
        return true;

    logger::debug("Creating SSRT sparse sampling resources...");

    auto renderer = globals::game::renderer;
    auto context = globals::d3d::context;
    if (!renderer || !context)
        return false;

    auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
    if (!mainTex.texture)
        return false;

    D3D11_TEXTURE2D_DESC texDesc{};
    mainTex.texture->GetDesc(&texDesc);
    // Same reasoning as every other surface in this feature (audit P9): no RTV is created for any
    // of these and none is passed to GenerateMips, so the render-target bind flag and its
    // compression metadata are pure cost. MiscFlags set rather than OR-ed, because an inherited
    // GENERATE_MIPS would fail creation once BIND_RENDER_TARGET is gone.
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    texDesc.MiscFlags = 0;
    texDesc.MipLevels = 1;
    // Half the width, full height. That is the checkerboard grid exactly -- one compact texel per
    // horizontal pair of render pixels, every row -- and a superset of the half-resolution grid,
    // which uses the top-left quadrant. Rounded *up* rather than floor, deliberately and in the
    // opposite direction from SSRT_GetSparseExtent's floor: the extent every shader clamps against
    // is derived from the render sub-rect, and this is the allocation that has to contain it for
    // every dynamic-resolution ratio the sub-rect can take, including 1.0 with an odd full width.
    texDesc.Width = std::max(1u, (texDesc.Width + 1u) / 2u);

    const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    auto makeTex = [&](eastl::unique_ptr<Texture2D>& tex, DXGI_FORMAT format, const char* name) {
        if (tex)
            return;
        texDesc.Format = format;
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
            .Format = format,
            .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
        };
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
            .Format = format,
            .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
            .Texture2D = { .MipSlice = 0 }
        };
        tex = eastl::make_unique<Texture2D>(texDesc);
        tex->CreateSRV(srvDesc);
        tex->CreateUAV(uavDesc);
        Util::SetResourceName(tex->resource.get(), name);
        // Cleared, unlike the confidence filter's set. Those surfaces are each rewritten in full
        // by their own dispatch before anything reads them; these are written over the *compact*
        // sub-rect only, so the region between it and the allocation is never written -- and the
        // resolve's tap clamp keeps it out of reach, but a fresh texture's undefined contents are
        // not something to leave sitting behind a clamp on the strength of an argument.
        context->ClearUnorderedAccessViewFloat(tex->uav.get(), zero);
    };

    // Formats mirror the three full-resolution surfaces these stand in for, exactly. That is what
    // lets ssrt_raymarch.hlsl's output block stay untouched: the stores are the same stores to the
    // same register slots with the same types, and only the bound resource differs.
    makeTex(texSparseColor, DXGI_FORMAT_R16G16B16A16_FLOAT, "SSRT::SparseColor");
    makeTex(texSparseConfidence, DXGI_FORMAT_R8_UNORM, "SSRT::SparseConfidence");
    makeTex(texSparseHitDistance, DXGI_FORMAT_R8_UNORM, "SSRT::SparseHitDistance");

    return texSparseColor && texSparseConfidence && texSparseHitDistance;
}

void ScreenSpaceRayTracing::ReleaseSparseResources()
{
    if (!texSparseColor && !texSparseConfidence && !texSparseHitDistance)
        return;

    logger::debug("Releasing SSRT sparse sampling resources...");

    // The D3D11 runtime holds its own reference to a resource for as long as a submitted command
    // list can still reference it, so dropping ours here does not free anything the GPU is
    // reading. Nothing this frame has been bound yet either: the only caller is
    // ResolveSamplingMode, which runs at the top of DrawSSRTDiffuse before the first binding.
    texSparseColor = nullptr;
    texSparseConfidence = nullptr;
    texSparseHitDistance = nullptr;
}

uint ScreenSpaceRayTracing::ResolveSamplingMode()
{
    uint mode = std::min(settings.DiffuseSamplingMode, (uint)kSamplingCheckerboard);

    // Nothing to sample sparsely if the diffuse chain is not running at all, and this is the path
    // that gives the compact set back instead of stranding ~41 MB of a 4K allocation.
    if (!settings.EnableDiffuse)
        mode = kSamplingFull;

#ifdef ENABLE_SHARC
    // SHARC traces into a world-space hash grid whose update pass is tuned for a full-resolution
    // dispatch, and it is experimental and marked "(Broken)" in the UI; rather than add two more
    // ray-march permutations for it, sparse sampling yields. The UI says so when both are set.
    if (settings.EnableSharc)
        mode = kSamplingFull;
#endif

    // A failed compile falls back to full density rather than to a null CSSetShader, which in
    // D3D11 is a silent no-op: the ray march would write nothing, the resolve would read an
    // unwritten compact surface, and the frame would look plausible and be wrong. Same argument as
    // the whole-chain readiness guard at the top of DrawSSRTDiffuse.
    if (mode == kSamplingHalfRes && (!raymarchDiffuseHalfResCS || !sparseResolveHalfResCS))
        mode = kSamplingFull;
    if (mode == kSamplingCheckerboard && (!raymarchDiffuseCheckerCS || !sparseResolveCheckerCS))
        mode = kSamplingFull;

    if (mode != kSamplingFull && !EnsureSparseResources())
        mode = kSamplingFull;

    if (mode == kSamplingFull)
        ReleaseSparseResources();

    activeSamplingMode = mode;
    return mode;
}

// (batch C1) REBLUR-path resources, allocated on first selection rather than at boot,
// on the same argument EnsureSharcResources makes: a user who stays on SVGF (or Off)
// never pays for the packed scratch surfaces or the two instances' permanent and
// transient pools. Switching REBLUR off afterwards keeps them resident, which is what
// makes the A/B toggle instant in both directions.
//
// (S2.7) One shared scratch pair instead of one per chain. The two chains run strictly
// serially inside Deferred::DeferredPasses and neither surface carries anything across the
// RunReblur call, so the duplication bought nothing and cost 16 bytes per output pixel
// (~127 MiB at a 4K allocation). The two nrd::Instances stay separate: their permanent pools
// *are* the per-signal history and are not scratch.
void ScreenSpaceRayTracing::EnsureNRDResources()
{
    if (texNRDPackInput && texNRDPackOutput && nrdReblurDiffuse.IsValid() && nrdReblurSpecular.IsValid())
        return;

    logger::debug("Creating SSRT NRD resources...");

    auto renderer = globals::game::renderer;
    auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
    D3D11_TEXTURE2D_DESC mainDesc;
    mainTex.texture->GetDesc(&mainDesc);

    D3D11_TEXTURE2D_DESC texDesc{
        .Width = mainDesc.Width,
        .Height = mainDesc.Height,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
        .SampleDesc = { 1, 0 },
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
    };
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
        .Format = texDesc.Format,
        .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
        .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
    };
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
        .Format = texDesc.Format,
        .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
        .Texture2D = { .MipSlice = 0 }
    };

    auto makeTex = [&](eastl::unique_ptr<Texture2D>& tex, const char* name) {
        if (!tex) {
            tex = eastl::make_unique<Texture2D>(texDesc);
            tex->CreateSRV(srvDesc);
            tex->CreateUAV(uavDesc);
            Util::SetResourceName(tex->resource.get(), name);
        }
    };
    makeTex(texNRDPackInput, "SSRT::NRDPackInput");
    makeTex(texNRDPackOutput, "SSRT::NRDPackOutput");

    // Instances are sized at the *output* resolution, like every history surface in
    // this feature; the per-frame dynamic-resolution sub-rect travels through
    // nrd::CommonSettings::rectSize, which the NRD service fills.
    if (!nrdReblurDiffuse.IsValid())
        nrdReblurDiffuse.Init(mainDesc.Width, mainDesc.Height, nrd::Denoiser::REBLUR_DIFFUSE, 0);
    if (!nrdReblurSpecular.IsValid())
        nrdReblurSpecular.Init(mainDesc.Width, mainDesc.Height, nrd::Denoiser::REBLUR_SPECULAR, 1);

    resetReblurDiffuse = true;
    resetReblurSpecular = true;
}

// (S1.3) The static half of the REBLUR readiness question: everything that is knowable
// before SSRT has allocated anything of its own. Deliberately does *not* consult
// AreGuidesReady(), because every feature's Prepass() runs ahead of NRD::PrepareGuides()
// and the answer there would be last frame's. CanPrepareGuides() asks only about
// configuration -- NRD switched on and its guide shader compiled -- which is the question with
// a stable answer at that point. (batch 11, item C2: it also no longer asks whether the guide
// textures exist, because those are allocated on the strength of this very answer.)
bool ScreenSpaceRayTracing::ReblurStaticallyAvailable(bool a_specular) const
{
    // Mono guide surfaces; the NRD feature does not load in VR, but developer mode
    // can force-load it there, so the gate is explicit.
    if (REL::Module::IsVR())
        return false;

    auto& nrdSvc = globals::features::nrd;
    if (!nrdSvc.loaded || !nrdSvc.CanPrepareGuides())
        return false;

    // (batch 11, item A) The front-end pack shaders are gone -- the ray march produces that
    // layout itself, and its own availability is already a precondition of the draw pass that
    // can reach this path at all -- so the back-end unpack is the whole shader requirement now.
    // It is chain-independent, hence no a_specular branch.
    (void)a_specular;
    return (bool)nrdUnpackCS;
}

// (S1.3) Everything except this frame's guides. This is the predicate ResolveDenoisers uses,
// and it must not ask about the guides: NRD::PrepareGuides is gated on a consumer having
// selected REBLUR (S2.6), and the answer to *that* is what this function decides. Asking
// AreGuidesReady() here would read last frame's value, and on the first frame both sides would
// answer "no" forever — REBLUR would never start because no guides had been published, and no
// guides would be published because REBLUR had not started.
bool ScreenSpaceRayTracing::ReblurResourcesReady(bool a_specular) const
{
    if (!ReblurStaticallyAvailable(a_specular))
        return false;

    if (a_specular)
        return texNRDPackInput && texNRDPackOutput && nrdReblurSpecular.IsValid();
    return texNRDPackInput && texNRDPackOutput && nrdReblurDiffuse.IsValid();
}

bool ScreenSpaceRayTracing::ReblurReady(bool a_specular) const
{
    // The per-frame half, asked at dispatch time when PrepareGuides has already run.
    return ReblurResourcesReady(a_specular) && globals::features::nrd.AreGuidesReady();
}

// (S1.3 / S1.4) Whole-chain readiness for SVGF, one signal at a time.
//
// Atomic on purpose. Before this, a failed compile of any single pass left the chain running
// with a hole in it: the temporal pass would still write texTemporal, the variance pass would
// still be dispatched against a null shader (a silent no-op in D3D11), and the a-trous loop
// would then filter and publish whatever texVariance last held -- an arbitrarily old frame.
// Consuming a stale texture is worse than not denoising, because it is not visibly wrong.
bool ScreenSpaceRayTracing::SvgfChainReady(bool a_specular) const
{
    if (!temporalCS || !varianceCS || !denoiserCB)
        return false;
    if (!texTemporal || !texMoments || !texVariance || !texHistoryNormals || !texHistoryDepth || !texDebugHistory)
        return false;
    if (a_specular)
        return spatialSpecularCS && texSSRColor && texHistory && texHistoryMoments;
    // The pre-blur is intentionally absent: it degrades on its own (the temporal pass reads
    // the raw surface and keeps the real firefly clamp), which is a documented path.
    return spatialCS && texSSRTDiffuseColor && texHistoryDiffuse && texHistoryMomentsDiffuse &&
           texSSRTDiffuseHitDistance;
}

// (S1.3) Decide, once per frame and before anything allocates or dispatches, which denoiser
// each signal gets — and bring up whatever that answer needs.
//
// The rule this implements is the project's, not NRD's: any reasonable combination of
// settings has to work on its own. REBLUR being unavailable is a reasonable combination (VR,
// NRD not installed, NRD switched off in its own page, a failed pack-shader compile) and the
// previous behaviour in every one of those cases was raw 2-spp Monte-Carlo noise on screen,
// because the readiness test ran inside the dispatch gate and its false branch was "do
// nothing". Falling back to SVGF is the only answer consistent with the rule; falling back to
// Off would just relabel the same failure.
void ScreenSpaceRayTracing::ResolveDenoisers()
{
    denoiserFallbackReason = nullptr;

    // If REBLUR is wanted and could work for either chain, bring its resources up *now* —
    // before the readiness question is asked and long before the dispatch gate. The old
    // order asked "is the instance valid?" and only allocated inside the branch that had
    // already been taken on the answer.
    if (ReblurSelected() && (ReblurStaticallyAvailable(false) || ReblurStaticallyAvailable(true)))
        EnsureNRDResources();

    // (S2.5) Bring the SVGF surfaces up only when this frame is actually going to need them:
    // the user selected SVGF, or REBLUR was selected and cannot run for one of the chains (the
    // fallback). The REBLUR-working case -- the default -- allocates none of them.
    //
    // (batch 11, item C1) The `|| bufferViewerActive` term is gone. Having the Buffer Viewer tree
    // expanded once was enough to allocate all ten surfaces -- 68 B/px, 537.9 MiB at a 4K
    // allocation -- and nothing but a resolution change ever released them again, so the memory
    // stayed committed for the rest of the session with the tree closed and the menu shut. See
    // SvgfHistoryNeeded for why the debug view it was protecting was not actually being served.
    if (SVGFSelected() ||
        (ReblurSelected() && (!ReblurResourcesReady(false) || !ReblurResourcesReady(true))))
        EnsureSvgfResources();

    const bool svgfDiffuse = SvgfChainReady(false);
    const bool svgfSpecular = SvgfChainReady(true);

    auto resolve = [&](bool a_specular, bool a_svgfOk) -> uint {
        switch (settings.DenoiserMethod) {
        case kDenoiserOff:
            return kDenoiserOff;
        case kDenoiserSVGF:
            if (a_svgfOk)
                return kDenoiserSVGF;
            denoiserFallbackReason = "SVGF failed to load (see the log).";
            return kDenoiserOff;
        case kDenoiserREBLUR:
        default:
            if (ReblurResourcesReady(a_specular))
                return kDenoiserREBLUR;
            if (a_svgfOk) {
                if (!denoiserFallbackReason) {
                    if (REL::Module::IsVR())
                        denoiserFallbackReason = "REBLUR is not available in VR.";
                    else if (!globals::features::nrd.loaded)
                        denoiserFallbackReason = "the NRD feature is not loaded.";
                    else if (!globals::features::nrd.settings.Enabled)
                        denoiserFallbackReason = "NRD is switched off on its own feature page.";
                    else
                        denoiserFallbackReason = "REBLUR failed to start (see the log).";
                }
                return kDenoiserSVGF;
            }
            denoiserFallbackReason = "neither REBLUR nor SVGF could be loaded (see the log).";
            return kDenoiserOff;
        }
    };

    effectiveDenoiserDiffuse = resolve(false, svgfDiffuse);
    effectiveDenoiserSpecular = resolve(true, svgfSpecular);
}

// (S4.15) Extents, history validity and the lazily built REBLUR path are one state machine.
void ScreenSpaceRayTracing::ResetFrameState()
{
    lastDepthExtentX = 0;
    lastDepthExtentY = 0;
    lastHistoryExtentX = 0;
    lastHistoryExtentY = 0;
    historyClearPending = true;

    lastEffectiveDenoiserDiffuse = kDenoiserOff;
    lastEffectiveDenoiserSpecular = kDenoiserOff;
    effectiveDenoiserDiffuse = kDenoiserOff;
    effectiveDenoiserSpecular = kDenoiserOff;
    lastEnableDiffuse = false;
    lastEnableSpecular = false;
    lastCell = nullptr;
    denoiserFallbackReason = nullptr;

    resetReblurDiffuse = true;
    resetReblurSpecular = true;

    // The REBLUR surfaces and instances are sized at the output resolution and are allocated
    // lazily, so a rebuild at a *different* resolution left them at the old dimensions
    // indefinitely — EnsureNRDResources' early-out only asks whether they exist. Drop them
    // and let the next selection rebuild them against the new extent.
    texNRDPackInput = nullptr;
    texNRDPackOutput = nullptr;
    nrdReblurDiffuse.Shutdown();
    nrdReblurSpecular.Shutdown();

    // (S2.5) Same argument for the SVGF surfaces, which are lazily allocated for the same
    // reason. SetupResources reallocates everything else at the new extent; these would
    // otherwise keep the old one until the process exited.
    texHistory = nullptr;
    texHistoryDiffuse = nullptr;
    texTemporal = nullptr;
    texVariance = nullptr;
    texMoments = nullptr;
    texHistoryMoments = nullptr;
    texHistoryMomentsDiffuse = nullptr;
    texHistoryNormals = nullptr;
    texHistoryDepth = nullptr;
    texDebugHistory = nullptr;

    // (batch 12) And the compact sparse set, for exactly the same reason: its extent is derived
    // from kMAIN's, so a set left resident across a resolution change would be addressed with a
    // compact grid the surfaces do not have -- which unlike a subtly wrong sample is a write
    // outside the texture. ResolveSamplingMode rebuilds it at the new size on the next frame that
    // asks for a sparse mode.
    ReleaseSparseResources();
    activeSamplingMode = kSamplingFull;
}

void ScreenSpaceRayTracing::ClearShaderCache()
{
    static const std::vector<winrt::com_ptr<ID3D11ComputeShader>*> shaderPtrs = {
        &raymarchSpecularCS, &raymarchDiffuseCS, &prepareColorCS, &preprocessDepthCS, &depthDownsampleCS, &diffuseCompositeCS, &preblurCS, &temporalCS, &temporalDiagCS, &varianceCS, &spatialCS, &spatialSpecularCS,
        &nrdUnpackCS,
        // (batch 36f) folded-unpack composite twins
        &diffuseCompositePackedCS, &diffuseCompositeExternalConfPackedCS,
        // (batch 12) sparse sampling: the two ray-march permutations and their two resolve passes
        &raymarchDiffuseHalfResCS, &raymarchDiffuseCheckerCS, &sparseResolveHalfResCS, &sparseResolveCheckerCS,
#ifdef ENABLE_SHARC
        &raymarchDiffuseSharcCS, &sharcUpdateRaymarchCS, &sharcResolveCS
#endif
    };

    for (auto shader : shaderPtrs)
        *shader = nullptr;

    CompileComputeShaders();
}

void ScreenSpaceRayTracing::CompileComputeShaders()
{
    struct ShaderCompileInfo
    {
        winrt::com_ptr<ID3D11ComputeShader>* programPtr;
        std::string_view filename;
        std::vector<std::pair<const char*, const char*>> defines;
    };

    std::vector<std::pair<const char*, const char*>> defines;

    if (globals::features::dynamicCubemaps.loaded)
		defines.push_back({ "DYNAMIC_CUBEMAPS", nullptr });

    if (globals::features::screenSpaceGI.loaded)
		defines.push_back({ "SSGI", nullptr });

    if (globals::features::skylighting.loaded)
		defines.push_back({ "SKYLIGHTING", nullptr });

    // (contact AO) ENV_AMBIENT used to be defined here so that ssrt_raymarch.hlsl could include
    // EnvAmbient.hlsli for its contact-occlusion kernel. The kernel now lives in Screen Space GI
    // and reaches the ray march through the SSGI AO texture, so this feature no longer depends on
    // the Environment Ambient shader folder at all.

    const std::string DiffuseSPPStr = std::to_string(settings.DiffuseSPP);

    defines.push_back({ "DIFFUSE_SPP", DiffuseSPPStr.c_str() });

#ifdef ENABLE_SHARC
    auto definesSharcUpdate = defines;
    definesSharcUpdate.push_back({ "SHARC_UPDATE", "1" });

    auto definesSharc = defines;
    definesSharc.push_back({ "SHARC_RENDER", "1" });
#endif

    auto definesSpecular = defines;
    definesSpecular.push_back({ "SSRT_SPECULAR", nullptr });

    // (batch 12) The two sparse ray-march permutations. Built on `defines` -- unlike the
    // denoiser-side lists below -- because they are the *same shader* as the full-density diffuse
    // permutation and therefore read every one of its axes: DYNAMIC_CUBEMAPS, SSGI, SKYLIGHTING
    // and DIFFUSE_SPP all still apply.
    auto definesSparseHalfRes = defines;
    definesSparseHalfRes.push_back({ "SSRT_SPARSE_HALFRES", "1" });
    auto definesSparseChecker = defines;
    definesSparseChecker.push_back({ "SSRT_SPARSE_CHECKERBOARD", "1" });
    // The resolve pass, whose only axis is which sparse layout it reconstructs from. Deliberately
    // not built on `defines`, like definesWideKernel below: ssrt_sparse_resolve.hlsl reads none of
    // DYNAMIC_CUBEMAPS / SSGI / SKYLIGHTING / DIFFUSE_SPP, and giving it four permutation axes
    // that change nothing would only multiply the compile time.
    const std::vector<std::pair<const char*, const char*>> definesResolveHalfRes = {
        { "SSRT_SPARSE_HALFRES", "1" }
    };
    const std::vector<std::pair<const char*, const char*>> definesResolveChecker = {
        { "SSRT_SPARSE_CHECKERBOARD", "1" }
    };

    // (defect D6) The diffuse a-trous permutation runs the 5-tap B3 spline kernel; the
    // specular one keeps the 3-tap binomial. Deliberately *not* built on `defines`: this
    // shader has never received DYNAMIC_CUBEMAPS / SSGI / SKYLIGHTING / DIFFUSE_SPP and does
    // not read any of them, so the list stays a single entry rather than acquiring four
    // permutation axes that would change nothing. See the derivation at SSRT_SVGF_KERNEL_5X5
    // in ssrt_spatial.hlsl -- in short, spec S2's 3x3 chain was accepted on a build whose
    // variance channel was still broken, and its 1.58 px second-moment sigma is an order of
    // magnitude short of what a 2-spp signal needs.
    const std::vector<std::pair<const char*, const char*>> definesWideKernel = {
        { "SSRT_SVGF_KERNEL_5X5", "1" }
    };

    // (perf 1) The temporal pass ships as two permutations. The production one folds the five
    // diagnostic gates to literal zero; this one wires them to their constant-buffer fields.
    //
    // fxc /Ges /O3 puts the pair at 1764 and 2048 instruction slots and at dcl_temps 18 and 20,
    // and the diagnostic permutation's generated code is byte-identical to the single shader that
    // preceded the split -- so the split costs a diagnostic dispatch nothing and saves a shipping
    // one 284 slots, two temporaries and the u2 declaration. Like definesWideKernel this list is
    // deliberately not built on `defines`: none of DYNAMIC_CUBEMAPS / SSGI / SKYLIGHTING /
    // DIFFUSE_SPP is read by this file, so the production permutation above passes {} and this one
    // has to match it entry for entry or the two would stop being the same shader.
    const std::vector<std::pair<const char*, const char*>> definesDenoiserDiag = {
        { "SSRT_DENOISER_DIAG", "1" }
    };

    std::vector<ShaderCompileInfo>
        shaderInfos = {
            { &raymarchDiffuseCS, "ssrt_raymarch.hlsl", defines },
            { &raymarchSpecularCS, "ssrt_raymarch.hlsl", definesSpecular },
            { &prepareColorCS, "ssrt_prepare_color.hlsl", {} },
            { &preprocessDepthCS, "ssrt_preprocess_depth.hlsl", {} },
            { &depthDownsampleCS, "ssrt_depth_downsample.hlsl", {} },
            { &diffuseCompositeCS, "ssrt_diffuse_composite.hlsl", {} },
            // (batch 6) The composite's second permutation, with the confidence smoothing
            // compiled out because the three passes below publish that surface instead. Like
            // definesWideKernel this define list is deliberately not built on `defines`: this file
            // reads none of DYNAMIC_CUBEMAPS / SSGI / SKYLIGHTING / DIFFUSE_SPP, so the entry
            // above passes {} and this one has to match it entry for entry or the two would stop
            // being the same shader.
            { &diffuseCompositeExternalConfCS, "ssrt_diffuse_composite.hlsl", { { "SSRT_CONF_EXTERNAL_FILTER", "1" } } },
            // (batch 36f) Settings::ReblurFoldUnpack: the same two composites reading REBLUR's packed
            // output at t0 and decoding it in registers, in place of ssrt_nrd_unpack.hlsl.
            { &diffuseCompositePackedCS, "ssrt_diffuse_composite.hlsl", { { "SSRT_DIFFUSE_PACKED_INPUT", "1" } } },
            { &diffuseCompositeExternalConfPackedCS, "ssrt_diffuse_composite.hlsl", { { "SSRT_CONF_EXTERNAL_FILTER", "1" }, { "SSRT_DIFFUSE_PACKED_INPUT", "1" } } },
            // (batch 6) The spatial confidence filter. None of the three files reads any of the
            // permutation axes above either; the blur's only axis is which way it runs.
            { &confDownsampleCS, "ssrt_conf_downsample.hlsl", {} },
            { &confBlurHorizontalCS, "ssrt_conf_blur.hlsl", {} },
            { &confBlurVerticalCS, "ssrt_conf_blur.hlsl", { { "SSRT_CONF_BLUR_VERTICAL", "1" } } },
            { &confUpsampleCS, "ssrt_conf_upsample.hlsl", {} },
            { &preblurCS, "ssrt_preblur.hlsl", {} },
            { &temporalCS, "ssrt_temporal.hlsl", {} },
            { &temporalDiagCS, "ssrt_temporal.hlsl", definesDenoiserDiag },
            { &varianceCS, "ssrt_variance.hlsl", {} },
            { &spatialCS, "ssrt_spatial.hlsl", definesWideKernel },
            { &spatialSpecularCS, "ssrt_spatial.hlsl", definesSpecular },
            // (batch C1) REBLUR back-end unpack. Deliberately not built on `defines`: the file
            // reads none of DYNAMIC_CUBEMAPS / SSGI / SKYLIGHTING / DIFFUSE_SPP, and both chains
            // share it, so it has no permutation axis at all.
            // (batch 11, item A) The two front-end pack permutations that used to sit here are
            // gone; ssrt_raymarch.hlsl produces that layout itself.
            { &nrdUnpackCS, "ssrt_nrd_unpack.hlsl", {} },
            // (batch 12) Sparse sampling. Both variants of both passes are always compiled, so
            // Settings::DiffuseSamplingMode is a hot switch and never a recompile.
            { &raymarchDiffuseHalfResCS, "ssrt_raymarch.hlsl", definesSparseHalfRes },
            { &raymarchDiffuseCheckerCS, "ssrt_raymarch.hlsl", definesSparseChecker },
            { &sparseResolveHalfResCS, "ssrt_sparse_resolve.hlsl", definesResolveHalfRes },
            { &sparseResolveCheckerCS, "ssrt_sparse_resolve.hlsl", definesResolveChecker },
#ifdef ENABLE_SHARC
            { &raymarchDiffuseSharcCS, "ssrt_raymarch.hlsl", definesSharc },
            { &sharcUpdateRaymarchCS, "ssrt_raymarch.hlsl", definesSharcUpdate },
            { &sharcResolveCS, "sharc_resolve.hlsl", {} }
#endif
        };

    for (auto& info : shaderInfos) {
        auto path = std::filesystem::path("Data\\Shaders\\ScreenSpaceRayTracing") / info.filename;
        if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0")))
            info.programPtr->attach(rawPtr);
    }

    // (S1.4) Record what DIFFUSE_SPP the permutations above were actually built with, so
    // Prepass can notice a setting change on any path -- UI, config load, RestoreDefaults --
    // rather than only on the one that remembered to raise recompileFlag.
    compiledDiffuseSPP = settings.DiffuseSPP;
}

// (guard G8) The SVGF history is the only state in this feature that outlives a frame, and
// nothing ever reset it. texHistoryDiffuse / texHistoryMomentsDiffuse (and the specular
// pair) are read by ssrt_temporal.hlsl and rewritten from its own output, so their contents
// survive a save load, a cell transition and a resolution change; before G4 a single
// non-finite texel therefore survived until the process exited.
//
// Note on Feature::Reset(): it is *not* the hook for this, despite the name. State::Reset()
// calls it from IDXGISwapChain_Present (Hooks.cpp) on **every frame** -- the same function
// increments frameCount and advances the timer -- so clearing the history there would wipe
// it 60 times a second and silently reduce SVGF to a passthrough. Hence the pending-flag
// design below, latched at the earliest point in the frame that still precedes every reader.
void ScreenSpaceRayTracing::ClearDenoiserHistory()
{
    auto context = globals::d3d::context;
    if (!context)
        return;

    // (reinjection noise) The confidence accumulator, cleared ahead of the SVGF guard below
    // because it is not an SVGF surface: it is allocated on demand and its pass runs under
    // REBLUR and Off as well, so gating it on the SVGF set being resident would skip it on the
    // default denoiser -- which is where the reinjection noise was reported.
    //
    // Zero for the same reason the colour and moment pairs are: accumFrames 0 in .z makes
    // alpha = max(1 / (0 + 1), invMax) = 1, so every pixel takes this frame's spatial mean whole,
    // indistinguishable from a disocclusion. A cell reload is exactly when the stored reference
    // depths stop describing the world in front of the camera, so leaving them would let a frame
    // of wrong coverage survive the transition.
    const float zeroConfidence[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (texSSRTConfidenceHistory)
        context->ClearUnorderedAccessViewFloat(texSSRTConfidenceHistory->uav.get(), zeroConfidence);
    if (texSSRTConfidenceHistoryPrev)
        context->ClearUnorderedAccessViewFloat(texSSRTConfidenceHistoryPrev->uav.get(), zeroConfidence);

    if (!texHistoryDiffuse || !texHistoryMomentsDiffuse || !texHistory || !texHistoryMoments)
        return;

    // Zero, not "some safe colour": ssrt_temporal.hlsl derives its blend weight from the
    // accumulated frame count in the moments texture's .z, and
    // alpha = max(1 / (0 + 1), invMaxAccumulatedFrames) = 1 means the pixel takes this
    // frame's sample whole. A zeroed history is therefore indistinguishable from a
    // disocclusion, which is a path the temporal and variance passes already handle.
    const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->ClearUnorderedAccessViewFloat(texHistoryDiffuse->uav.get(), zero);
    context->ClearUnorderedAccessViewFloat(texHistoryMomentsDiffuse->uav.get(), zero);
    // The specular pair has the identical failure mode and the identical fix; clearing all
    // four costs four ClearUAV calls on an event that happens once per load or transition.
    context->ClearUnorderedAccessViewFloat(texHistory->uav.get(), zero);
    context->ClearUnorderedAccessViewFloat(texHistoryMoments->uav.get(), zero);

    // (defect D3) texHistoryDepth is cleared to the *far plane*, not to zero. It is the
    // observed side of a comparison, not an accumulator: zero is the near plane, which a
    // genuinely near surface could match, whereas 1.0 unprojects to a point on the far plane
    // itself -- somewhere no on-screen surface can be, so ssrt_temporal.hlsl's plane-distance
    // test measures an enormous distance to it and rejects it for every candidate. A cleared
    // frame therefore reseeds every pixel, which is the same behaviour the zeroed
    // colour/moment pair produces.
    if (texHistoryDepth) {
        const float farPlane[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        context->ClearUnorderedAccessViewFloat(texHistoryDepth->uav.get(), farPlane);
    }

    // (diagnostic H) Not history, but it shares the problem: the temporal dispatch only writes
    // the dynamic-resolution sub-rect, so at any scale below 1.0 the border of this texture
    // holds whatever was last written at a larger extent. Black is the same value the sky and
    // far plane write, so a cleared border reads as "no history question here" rather than as a
    // rejection colour that never updates.
    if (texDebugHistory)
        context->ClearUnorderedAccessViewFloat(texDebugHistory->uav.get(), zero);

    // texHistoryNormals is deliberately not in the list: CopyHistoryGeometry overwrites it
    // in full every frame from the live G-buffer, so it is never stale, and its
    // R10G10B10A2_UNORM storage cannot represent a NaN in the first place.
}

void ScreenSpaceRayTracing::UpdateHistoryValidity()
{
    // A false -> true transition on any of these means the previous frame did not produce
    // the history this frame is about to read.
    //
    // EnableSVGF is the case the audit called out, and it is specifically the *moments* that
    // go stale: texHistoryDiffuse / texHistory are re-copied by every draw pass whether or
    // not SVGF is on -- from the first a-trous iteration when it is and from the raw pass
    // output when it is not (defect D4) -- so the colour is always one frame old, but
    // texHistoryMomentsDiffuse / texHistoryMoments are only written inside the EnableSVGF
    // block. Flipping it back on therefore resumes from the moment pair -- and the
    // accumulated frame count -- left by the last time it was on, which can be an entire
    // session earlier. The count is the damaging half: a stale MaxAccumulatedFrames drives
    // alpha straight to its floor on a history that no longer describes the scene, and any
    // Inf that pair had accumulated comes back with it.
    //
    // EnableDiffuse / EnableSpecular are the same hazard by the same mechanism: with a pass
    // switched off, neither its history colour nor its moments are updated at all.
    // (batch C1) The SVGF history clear latches on a transition *into* SVGF, exactly
    // as it used to latch on EnableSVGF flipping on; the two REBLUR instances have the
    // same class of hazard (their permanent pools survive while another denoiser is
    // selected) and latch their own reset flags on a transition into REBLUR. Both
    // resets also ride the pass-enable transitions below, because with a pass off
    // neither denoiser's history is updated at all.
    // (S1.3) Keyed on the *effective* denoiser per chain, not on Settings::DenoiserMethod.
    // A fallback from REBLUR to SVGF -- because NRD was switched off, or because this is VR --
    // is precisely the transition the SVGF history clear exists for, and the requested-method
    // form of this test would have missed every one of them: the setting does not move.
    const bool diffuseRising = settings.EnableDiffuse && !lastEnableDiffuse;
    const bool specularRising = settings.EnableSpecular && !lastEnableSpecular;

    const bool svgfRising =
        (effectiveDenoiserDiffuse == kDenoiserSVGF && lastEffectiveDenoiserDiffuse != kDenoiserSVGF) ||
        (effectiveDenoiserSpecular == kDenoiserSVGF && lastEffectiveDenoiserSpecular != kDenoiserSVGF);
    const bool reblurRising =
        (effectiveDenoiserDiffuse == kDenoiserREBLUR && lastEffectiveDenoiserDiffuse != kDenoiserREBLUR) ||
        (effectiveDenoiserSpecular == kDenoiserREBLUR && lastEffectiveDenoiserSpecular != kDenoiserREBLUR);

    if (svgfRising || diffuseRising || specularRising)
        historyClearPending = true;

    if (reblurRising || diffuseRising || specularRising) {
        resetReblurDiffuse = true;
        resetReblurSpecular = true;
    }

    lastEffectiveDenoiserDiffuse = effectiveDenoiserDiffuse;
    lastEffectiveDenoiserSpecular = effectiveDenoiserSpecular;
    lastEnableDiffuse = settings.EnableDiffuse;
    lastEnableSpecular = settings.EnableSpecular;

    // Cell identity, compared by pointer exactly as SkySync::Update does. Covers the cases
    // the audit named as the ones that let a poisoned history outlive its cause: loading a
    // save, changing cell, fast travel. The history is screen-space, so none of it means
    // anything once the view is somewhere else.
    if (auto player = RE::PlayerCharacter::GetSingleton()) {
        auto cell = player->GetParentCell();
        if (cell != lastCell) {
            lastCell = cell;
            historyClearPending = true;
            // (batch C1) REBLUR's accumulated history is just as screen-space as
            // SVGF's; a load, fast travel or door transition invalidates both.
            resetReblurDiffuse = true;
            resetReblurSpecular = true;
        }
        // (audit P9) Same lookup, so the interior test rides along instead of repeating it:
        // both draw passes read this member and neither may pay for the cell walk again.
        inInterior = cell ? cell->IsInteriorCell() : true;
    } else {
        inInterior = true;
    }
}

void ScreenSpaceRayTracing::Prepass()
{
    // (S1.4) compiledDiffuseSPP closes the hole recompileFlag left: DIFFUSE_SPP is a
    // compile-time macro, and any path that changes the setting without raising the flag
    // (LoadSettings, RestoreDefaultSettings) used to leave the ray march tracing a sample
    // count the configuration no longer asks for.
    if (recompileFlag || compiledDiffuseSPP != settings.DiffuseSPP) {
        recompileFlag = false;
        CompileComputeShaders();
    }

    // (batch 11, item C1) The debug view's claim on the SVGF surfaces was latched here. It is
    // gone: the Buffer Viewer no longer makes one. See SvgfHistoryNeeded.

    // (S1.3) Before UpdateHistoryValidity, because the history latches key on the effective
    // denoiser and a fallback is one of the transitions they have to catch; before the
    // enable gate below, because a frame in which neither pass renders must still record
    // where the state machine got to. This is also where the REBLUR path is brought up, so
    // no dispatch gate is ever the first thing to ask whether its resources exist.
    ResolveDenoisers();

    // (guard G8) Before the enable gate below, so a transition is never missed just because
    // both passes happened to be off on the frame it occurred; the flag latches until a
    // frame that actually renders services it. PrepassPasses runs before DeferredPasses, so
    // this is the earliest point in the frame and precedes every reader of the history.
    UpdateHistoryValidity();

    // (audit P8) The Hi-Z pyramid this pass builds is only ever read by the two SSRT
    // raymarch passes, so with both switched off it was 1 copy + 8 downsample
    // dispatches of pure waste every frame.
    if (!settings.EnableDiffuse && !settings.EnableSpecular)
        return;

    // (audit P9) inInterior is cached once per frame by UpdateHistoryValidity above, which
    // already has to walk to the player's cell for the G8 discontinuity test. It runs
    // before the gate, so the value is fresh whether or not either pass is enabled, and
    // Prepass still precedes both draws.

    auto renderer = globals::game::renderer;
    auto context = globals::d3d::context;
    auto state = globals::state;

    auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

    float2 size = Util::ConvertToDynamic(state->screenSize);
    float2 dispatchCount = { (size.x + 7) / 8, (size.y + 7) / 8 };

    std::array<ID3D11ShaderResourceView*, 5> srvs = { nullptr };
	std::array<ID3D11UnorderedAccessView*, 1> uavs = { nullptr };

    auto resetViews = [&]() {
		srvs.fill(nullptr);
		uavs.fill(nullptr);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	};

    std::array<ID3D11SamplerState*, 1> samplers = { linearSampler.get() };
    context->CSSetSamplers(0, 1, samplers.data());

    state->BeginPerfEvent("SSRT Prepass");
    Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::SSRTDepthPyramid);

    // (audit #8) texDepth is allocated at full resolution but only the dynamic-resolution
    // sub-rect is ever written, and it was never cleared. The region beyond the dispatch
    // therefore held undefined data that the downsample's min() drags into the coarse
    // mips; a value near 0 (= near plane) makes any ray landing in such a tile report an
    // instant hit. Clearing every mip to the far plane makes the pyramid well defined:
    // combined with the out-of-sub-rect writes in ssrt_preprocess_depth.hlsl, min() with
    // 1.0 is a no-op so each level inherits "1.0 outside the valid area" by induction.
    // Only needed when the extent changes -- with dynamic resolution off that is once,
    // and even with it on it is one clear per resolution change, not per frame.
    //
    // Compared as whole texels. `size` is screenSize scaled by a dynamic-resolution ratio, so
    // an exact float comparison reports a change for any ratio wobble at all, and the pyramid
    // only cares about which texels exist.
    const uint depthExtentX = (uint)size.x;
    const uint depthExtentY = (uint)size.y;
    if (depthExtentX != lastDepthExtentX || depthExtentY != lastDepthExtentY) {
        lastDepthExtentX = depthExtentX;
        lastDepthExtentY = depthExtentY;
        const float farPlane[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        for (uint i = 0; i < maxMips; ++i)
            context->ClearUnorderedAccessViewFloat(depthUAVs[i].get(), farPlane);
    }

    // (guard G8, repaired) The history clear is latched on the *output* extent, which is the
    // extent the history textures are allocated at and addressed in, and it is deliberately no
    // longer chained to the depth-pyramid latch above.
    //
    // The pyramid's latch is about which texels of a full-resolution surface hold real depth,
    // so it must follow the dynamic-resolution sub-rect. The history's is not: a sub-rect
    // change leaves the history textures exactly as valid as they were, because
    // ssrt_temporal.hlsl already validates every tap against the *previous* frame's sub-rect
    // (audit #16) and then against the previous frame's depth (defect D3). Sharing one latch
    // therefore bought nothing and cost everything: any source of per-frame ratio movement --
    // the engine's own adaptive dynamic resolution, or the sub-rect being read on a frame where
    // dynamicResolutionLock happens to be clear -- wiped all four history textures and
    // texHistoryDepth 60 times a second. Every pixel then reseeds with accumFrames = 1, alpha
    // is exactly 1, and SVGF becomes a passthrough that also, for the same reason, cannot
    // ghost.
    //
    // This is a P0-era mechanism, which is why no earlier build showed it: before guard G8
    // nothing ever cleared the history at all, so a wobbling ratio was harmless.
    const uint historyExtentX = (uint)state->screenSize.x;
    const uint historyExtentY = (uint)state->screenSize.y;
    if (historyExtentX != lastHistoryExtentX || historyExtentY != lastHistoryExtentY) {
        lastHistoryExtentX = historyExtentX;
        lastHistoryExtentY = historyExtentY;
        historyClearPending = true;
    }

    if (historyClearPending) {
        historyClearPending = false;
        ClearDenoiserHistory();
    }

    // preprocess depth
    {
        uavs.at(0) = texDepth->uav.get();
        srvs.at(4) = depth.depthSRV;

        context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
        context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
        context->CSSetShader(preprocessDepthCS.get(), nullptr, 0);

        context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);

        // context->GenerateMips(texDepth->srv.get());

        resetViews();
    }

    // downsample depth
    {
        state->BeginPerfEvent("Downsample Depth - HiZ Buffer");
        // Build only the levels the traversal can reach. ssrt_raymarch.hlsl blocks the climb at
        // `current_mip >= SSRT_DEPTH_HIERARCHY_MAX_MIP` (= SSRTCB::MaxMips) *after* loading the
        // level it is on, so the coarsest level ever read is exactly MaxMips -- levels 0..MaxMips
        // must be valid and nothing above them is ever sampled. The loop used to run a fixed
        // maxMips - 1 times, so at the default MaxMips of 6 it was already exact but at any lower
        // setting it downsampled levels no ray could reach: at MaxMips 4 that is two dispatches
        // and two pass boundaries per frame spent filling mips 5 and 6 for nobody.
        //
        // texDepth keeps all maxMips levels allocated, because MaxMips is a live slider; only the
        // downsample work is skipped. The bound below is read fresh every frame from the same
        // clamped setting the traversal uses, so raising the slider refills the newly reachable
        // levels on this very pass -- and hiZTopMipBuilt then clamps SSRTCB::MaxMips so the
        // traversal cannot outrun the builder even for one frame.
        const uint hiZTopMip = std::min(settings.MaxMips, maxMips - 1u);
        for (uint i = 0; i < hiZTopMip; ++i) {
            uavs.at(0) = depthUAVs[i + 1].get();
            srvs.at(0) = depthSRVs[i].get();

            context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
            context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
            context->CSSetShader(depthDownsampleCS.get(), nullptr, 0);

            // (audit P3) One thread per *destination* pixel, i.e. per pixel of mip i+1.
            // The old `dispatchCount >> i` sized the dispatch for mip i and launched 4x
            // the threads actually needed at every level. Note this cannot be written
            // as `dispatchCount >> (i + 1)`: that is floor(ceil(size/8) / 2^(i+1)) and
            // under-covers whenever the group count is odd (e.g. 1080 -> 135 groups ->
            // 67 instead of the 68 needed for mip 1, leaving the bottom rows unwritten).
            //
            // (S4.12) VERIFIED AND FIXED: the shift was still a floor, and the covering
            // count is a ceiling.
            //
            // Destination texel j of mip k reads source texels 2j and 2j+1 of mip k-1, so
            // the active region of mip k is every j with 2j < activeWidth(k-1), i.e.
            // ceil(activeWidth(k-1) / 2) texels. Iterated from mip 0, the active width at
            // level L is ceil(size.x / 2^L) -- not floor. At a dynamic-resolution width of
            // 1921 that is 961 columns at mip 1 while `1921 >> 1` dispatches 960: column
            // 960, whose source pair is texels 1920 and 1921, is never written. It keeps
            // whatever the far-plane clear left there, and because every coarser level takes
            // a min() over its children, that far-plane value propagates all the way up the
            // pyramid -- one column of the traversal reporting "nothing here" at every mip
            // above 0. Odd extents are the normal case under dynamic resolution, so this was
            // live whenever DRS moved off an even width.
            //
            // Clamped to the mip's own allocated dimension as well, because D3D11 mip sizes
            // *are* floors of the allocation: at an odd *output* width the ceiling can ask
            // for a texel the resource does not have, and that write would be discarded
            // silently.
            const uint level = (uint)(i + 1);
            const uint allocW = std::max(1u, (uint)texDepth->desc.Width >> level);
            const uint allocH = std::max(1u, (uint)texDepth->desc.Height >> level);
            const uint roundUp = 1u << level;
            const uint mipWidth = std::min(allocW, std::max(1u, ((uint)size.x + roundUp - 1u) >> level));
            const uint mipHeight = std::min(allocH, std::max(1u, ((uint)size.y + roundUp - 1u) >> level));
            context->Dispatch((mipWidth + 7) / 8, (mipHeight + 7) / 8, 1);
            resetViews();
        }
        hiZTopMipBuilt = hiZTopMip;
        state->EndPerfEvent();
    }

    Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::SSRTDepthPyramid);
    state->EndPerfEvent();

    auto view = texDepth->srv.get();
    context->PSSetShaderResources(99, 1, &view);
}

ID3D11ShaderResourceView* ScreenSpaceRayTracing::GetSpecularCompositeSRV() const
{
    if (specularCompositeSRV)
        return specularCompositeSRV;
    return texSSRColor ? texSSRColor->srv.get() : nullptr;
}

void ScreenSpaceRayTracing::BindCompositeConstants()
{
    if (!compositeCB)
        return;
    CompositeCB data;
    // Only meaningful while the specular chain ran this frame; DeferredCompositeCS reads t16 only
    // under SharedData's EnableSpecular, which is the same setting.
    data.SpecularPacked = (settings.EnableSpecular && specularCompositePacked) ? 1u : 0u;
    if (settings.EnableSpecular && DistanceLimitOnChain(true)) {
        data.SpecularCap = 1u;
        data.CapEnd = DistanceLimitUnits();
        data.CapStart = data.CapEnd * 0.8f;
    }
    compositeCB->Update(data);
    auto buffer = compositeCB->CB();
    globals::d3d::context->CSSetConstantBuffers(1, 1, &buffer);
}

void ScreenSpaceRayTracing::DrawSSRTSpecular()
{
    // (batch 36f) What DeferredCompositeCS reads at t16 unless the REBLUR block below folds the
    // unpack: the linear surface, as before. Reset every frame before any early-out.
    specularCompositeSRV = texSSRColor ? texSSRColor->srv.get() : nullptr;
    specularCompositePacked = false;

    if (!settings.EnableSpecular)
        return;

    // (S1.4) See the matching guard in DrawSSRTDiffuse. Neutral here is a zeroed texSSRColor
    // and a zeroed hit distance: the deferred composite binds texSSRColor directly and adds
    // it, so zero is "no reflection contribution", and Upscaling's DLSS-RR guide reads the
    // hit distance, which must not be a stale or uninitialised depth.
    if (!prepareColorCS || !raymarchSpecularCS || !texColor || !texSSRColor || !texHitDistance) {
        auto ctx = globals::d3d::context;
        const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        if (texSSRColor)
            ctx->ClearUnorderedAccessViewFloat(texSSRColor->uav.get(), zero);
        if (texHitDistance)
            ctx->ClearUnorderedAccessViewFloat(texHitDistance->uav.get(), zero);
        // The once-per-frame history snapshot is specular's responsibility when it runs; with
        // the chain down, diffuse's own `if (!settings.EnableSpecular)` branch will not have
        // taken it either, so take it here rather than leaving the SVGF history geometry a
        // frame stale.
        CopyHistoryGeometry();
        return;
    }

    auto renderer = globals::game::renderer;
    auto context = globals::d3d::context;
    auto state = globals::state;

    state->BeginPerfEvent("SSRT Compute");
    Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::SSRTTraceSpecular);

    auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
    auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
    auto specular = renderer->GetRuntimeData().renderTargets[SPECULAR];
    auto normal = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
    auto motion = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

    auto& dynamicCubemaps = globals::features::dynamicCubemaps;
    auto& ssgi = globals::features::screenSpaceGI;
    auto& skylighting = globals::features::skylighting;

    float2 size = Util::ConvertToDynamic(state->screenSize);
    float2 dispatchCount = { (size.x + 7) / 8, (size.y + 7) / 8 };

    // (batch 11, item A) This frame's denoiser, resolved *before* the ray march instead of after
    // it. It has to move up because the ray march now decides its own output layout on the
    // answer, and it can move up safely because nothing in the answer depends on the ray march:
    // EffectiveDenoiser was resolved in Prepass, and ReblurReady / SvgfChainReady ask only about
    // this frame's guides and this feature's own allocations.
    //
    // (S1.3) The one thing still worth re-testing at this point is whether this frame's guides
    // arrived, since Prepass runs ahead of NRD::PrepareGuides.
    uint denoiser = EffectiveDenoiser(true);
    if (denoiser == kDenoiserREBLUR && !ReblurReady(true)) {
        denoiser = SvgfChainReady(true) ? kDenoiserSVGF : kDenoiserOff;
        // The SVGF history was not maintained while REBLUR owned the signal, so reseed it
        // next frame rather than accumulate onto whatever it last held.
        historyClearPending = true;
    }
    // (batch 11, item A) Whether the ray march writes REBLUR's front-end layout into
    // texNRDPackInput at u0 instead of its own linear layout into texSSRColor. Derived from the
    // same `denoiser` the RunReblur call below is gated on, so the two cannot disagree; and
    // denoiser == kDenoiserREBLUR implies ReblurReady, which implies texNRDPackInput exists.
    const bool nrdFrontEndPack = (denoiser == kDenoiserREBLUR);

    SSRTCB ssrCBData;
    {
        ssrCBData.MaxSteps = settings.MaxSteps;
        // (audit P3) Clamp against the allocated mip count: a config saved by an older
        // build may hold a value above maxMips - 1, and loading a mip that does not
        // exist returns 0 == near plane, i.e. an immediate false hit.
        //
        // Also clamped to hiZTopMipBuilt, the coarsest level Prepass actually downsampled.
        // Prepass builds up to this same setting, so the two agree; the clamp only matters if
        // the setting were raised between Prepass and here, where an unbuilt level would read
        // the far-plane clear -- "nothing here" at every tile, i.e. a leak, not a crash.
        ssrCBData.MaxMips = std::min({ settings.MaxMips, maxMips - 1u, hiZTopMipBuilt });
        ssrCBData.Thickness = settings.Thickness;
        ssrCBData.NormalBias = settings.NormalBias;
        ssrCBData.BRDFBias = settings.BRDFBias;
        ssrCBData.UseDynamicCubemapsAsFallback = (uint)settings.UseDynamicCubemapsAsFallbackSpecular && dynamicCubemaps.loaded;
        ssrCBData.OcclusionStrength = settings.OcclusionStrength;
        ssrCBData.CubemapNormalization = settings.CubemapNormalization;
        // (diagnostic T2) Applies to both passes: the specular and diffuse permutations
        // share SampleRandomVector2DBaked, so freezing the phase has to freeze both or the
        // experiment is confounded by whichever one is still animating.
        ssrCBData.FreezeNoisePhase = settings.FreezeNoisePhase ? 1u : 0u;
        ssrCBData.UseBlueNoise = (settings.UseBlueNoise && noiseSRV) ? 1u : 0u;  // (S3.10)
        // (reinjection noise) Diffuse-only mechanism, and the specular chain has no composite
        // pass that reads it. Zeroed rather than left uninitialised so the buffer is fully
        // written on both paths.
        ssrCBData.TemporalAmbientConfidence = 0u;
        ssrCBData.AmbientConfidenceInvMaxFrames = 1.0f;
        // (batch 8) The specular permutation compiles the beta fill out entirely -- ambient
        // reinjection is a diffuse-only energy model -- so this is 0 for the same reason the two
        // fields above are: the buffer is shared and every field has to be written, but nothing
        // in this dispatch reads it.
        ssrCBData.CubemapFillBlend = 0.0f;
        // (batch 11, item A) REBLUR's front-end packing, folded in from the retired pack pass.
        // The three constants must be the same values RunReblur hands
        // nrd::ReblurSettings::hitDistanceParameters -- see the derivation at
        // Settings::ReblurHitDistA -- and they are written unconditionally so the buffer is fully
        // defined on both paths; the flag is what decides whether the shader reads them.
        ssrCBData.NRDHitDistA = settings.ReblurHitDistA;
        ssrCBData.NRDHitDistB = settings.ReblurHitDistB;
        ssrCBData.NRDHitDistC = settings.ReblurHitDistC;
        ssrCBData.NRDFrontEndPack = nrdFrontEndPack ? 1u : 0u;
        ssrCBData.SpecularMaxRoughness = settings.SpecularMaxRoughness;
        // (batch 36f, item 2) Distance limit, specular chain: not traced past the limit; the
        // fade happens in DeferredCompositeCS (lerp towards the cubemap), not here.
        if (DistanceLimitOnChain(true)) {
            ssrCBData.DistanceCapEnd = DistanceLimitUnits();
            ssrCBData.DistanceCapStart = ssrCBData.DistanceCapEnd * 0.8f;
        }
    }
    ssrtCB->Update(ssrCBData);
    auto buffer = ssrtCB->CB();
    context->CSSetConstantBuffers(1, 1, &buffer);

    // (audit P6) Specular raymarch UAV slots: u0 radiance/confidence,
	// u1 hit distance (was u2; u1 came free when texHitPDF was dropped). The SVGF
	// temporal pass below reuses u0/u1 for its own two outputs.
    // (diagnostic H) Three slots now, because ssrt_temporal.hlsl declares texDebugHistory at
    // u2. The specular chain never writes it -- it passes historyDebugView 0 -- but the slot is
    // bound anyway so that the declared UAV is never left dangling from another pass.
    std::array<ID3D11ShaderResourceView*, 12> srvs = { nullptr };
	std::array<ID3D11UnorderedAccessView*, 3> uavs = { nullptr };

    auto resetViews = [&]() {
		srvs.fill(nullptr);
		uavs.fill(nullptr);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	};

    std::array<ID3D11SamplerState*, 1> samplers = { linearSampler.get() };

    context->CSSetSamplers(0, 1, samplers.data());

    // prepare color
    srvs.at(0) = main.SRV;
    srvs.at(1) = specular.SRV;
    srvs.at(2) = normal.SRV;
    uavs.at(0) = texColor->uav.get();

    context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
    context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
    context->CSSetShader(prepareColorCS.get(), nullptr, 0);

    context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);

    resetViews();

    const auto envTexture = dynamicCubemaps.loaded ? dynamicCubemaps.envTexture->srv.get() : nullptr;
	const auto envReflectionsTexture = dynamicCubemaps.loaded ? dynamicCubemaps.envReflectionsTexture->srv.get() : nullptr;

    auto [ssgi_ao, ssgi_y, ssgi_cocg, ssgi_gi_spec] = ssgi.GetOutputTextures();

    // raymarch
    state->BeginPerfEvent("Raymarch");
    
    // (batch 11, item A) u0 is the packed NRD front-end surface under REBLUR and the chain's own
    // radiance surface otherwise. Same slot, same format (both RGBA16F), same dispatch: only the
    // meaning of the four channels changes, and SSRTCB::NRDFrontEndPack is what tells the shader
    // which meaning to write. Under REBLUR the back-end unpack is what fills texSSRColor, which
    // is where the deferred composite reads it from either way.
    uavs.at(0) = nrdFrontEndPack ? texNRDPackInput->uav.get() : texSSRColor->uav.get();
    // Unchanged on both paths: this R32_FLOAT surface is Upscaling.cpp's DLSS-RR specular
    // hit-distance guide, not private pack-pass input, so it cannot be traded away.
    uavs.at(1) = texHitDistance->uav.get();  // (audit P6) was u2; u1 freed by dropping texHitPDF

    // (S2.5) t0 stays unbound. ssrt_raymarch.hlsl declares HistoryTexture at t0 and never
    // references it, so fxc strips the binding entirely -- and texHistory is allocated lazily
    // now, so dereferencing it here would be a null read on the default denoiser.
    srvs.at(0) = nullptr;
    srvs.at(1) = motion.SRV;
    srvs.at(2) = normal.SRV;
    srvs.at(3) = texColor->srv.get();
    srvs.at(4) = depth.depthSRV;
    srvs.at(5) = texDepth->srv.get();
    srvs.at(6) = noiseSRV.get();
    srvs.at(7) = envTexture;
    srvs.at(8) = inInterior ? envTexture : envReflectionsTexture;
    srvs.at(9) = ssgi_ao;
    srvs.at(10) = dynamicCubemaps.loaded && skylighting.loaded ? skylighting.texProbeArray->srv.get() : nullptr;
    srvs.at(11) = dynamicCubemaps.loaded && skylighting.loaded ? skylighting.stbn_vec3_2Dx1D_128x128x64.get() : nullptr;

    context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
    context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
    context->CSSetShader(raymarchSpecularCS.get(), nullptr, 0);
    context->CSSetConstantBuffers(1, 1, &buffer);

    context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);

    state->EndPerfEvent();
    Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::SSRTTraceSpecular);

    resetViews();

    // (defect D4) The temporal history must be fed from the *first* a-trous iteration, not
    // from the end of the chain.
    //
    // What the end-of-chain feed did. texHistory received the output of the whole filter, so
    // the accumulation read back an image that had already been blurred by every iteration,
    // filtered it again, and fed that back -- a recursion with no fixed point short of full
    // spatial convergence. Two things follow, and both are what the audit's symptom-1
    // measurements show. The blur compounds geometrically: at AtrousIterations 2 the chain's
    // second-moment sigma is 1.58 px per pass (spec S2), and re-filtering an
    // already-filtered history makes the effective kernel grow with the accumulation window
    // rather than staying fixed. And any reprojection error is *spread* before it is
    // re-accumulated, so a ghost does not merely persist for MaxAccumulatedFrames frames, it
    // widens across the screen while it does -- which is why the streaks read as directional
    // smears rather than as crisp displaced copies.
    //
    // Feeding the first iteration instead is what SVGF prescribes (Schied et al. 2017 take
    // the temporal feedback after wavelet iteration one, and Falcor's SVGF pass does the
    // same). The reason is precise: the accumulation buffer's job is to hold an estimate of
    // the *radiance*, and one iteration of guided spatial filtering is the largest amount of
    // neighbourhood support that can be added without the estimate starting to describe its
    // own output. The later, wider iterations exist to make the current frame presentable and
    // must not be allowed to become an input.
    //
    // The composite is unaffected: it still reads the full chain output. Only what the next
    // frame accumulates from changes. Note also that at AtrousIterations 1 the two are the
    // same texture contents, so this is a no-op at that setting and only takes effect from 2
    // upwards -- the default being 2.
    //
    // The specular chain gets the identical treatment because it has the identical defect:
    // same shader, same ping-pong, same end-of-chain CopyResource. Its near-mirror tiles are
    // already immune (spec S3 makes them skip the kernel outright, so chain output equals
    // chain input there), but the glossy band in between is where the recursion was live and
    // most damaging, since an over-blurred reflection is more visible than an over-blurred
    // diffuse bounce.
    bool historyFed = false;

    // (batch 11, item A) `denoiser` is resolved above the ray march now, because the ray march's
    // own output layout depends on it. Nothing else about the sequence changed.

    if (denoiser == kDenoiserSVGF) {
        Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::SSRTSvgf);
        DenoiserCB denoiserCBData = GetDenoiserCBData(false);
        denoiserCB->Update(denoiserCBData);
        auto denoiserBuffer = denoiserCB->CB();
        context->CSSetConstantBuffers(2, 1, &denoiserBuffer);

        // temporal filter
        uavs.at(0) = texTemporal->uav.get();
        uavs.at(1) = texMoments->uav.get();
        // (diagnostic H) Bound but never written by this chain: GetDenoiserCBData is called
        // with the specular flag below, which forces historyDebugView to 0. The picture belongs
        // to the diffuse pass, and specular running afterwards must not overwrite it.
        uavs.at(2) = texDebugHistory->uav.get();
        srvs.at(0) = texHistory->srv.get();
        srvs.at(1) = motion.SRV;
        srvs.at(2) = normal.SRV;
        srvs.at(3) = texSSRColor->srv.get();
        srvs.at(4) = depth.depthSRV;
        srvs.at(5) = texHistoryMoments->srv.get();
        srvs.at(6) = texHistoryNormals->srv.get();
        srvs.at(7) = texHistoryDepth->srv.get();  // (defect D3)
        // (batch 1, item 1) The specular chain has no pre-blur, so its raw surface *is* its
        // temporal input and both slots get the same texture. The binding is not optional: the
        // shader is a single permutation and declares t8 either way, and an unbound SRV reads as
        // zero -- which would collapse the defect D1 reference box to the single point 0 and
        // clamp every specular history towards black.
        srvs.at(8) = texSSRColor->srv.get();

        context->CSSetShaderResources(0, 9, srvs.data());
        context->CSSetUnorderedAccessViews(0, 3, uavs.data(), nullptr);
        // (perf 1) The production permutation unless a diagnostic switch is set; see
        // SelectTemporalShader. u2 stays bound either way -- the production permutation does not
        // declare it, and binding a UAV a shader does not use is free.
        context->CSSetShader(SelectTemporalShader(), nullptr, 0);

        context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);
        resetViews();

        // (S2.8) The moments copy is gone: the two surfaces swap owners instead. See the
        // derivation at the matching site in DrawSSRTDiffuse; the swap happens once this
        // frame's readers -- the variance pass and the a-trous loop, both of which read
        // texMoments -- are done with it.

        // variance filter
        uavs.at(0) = texVariance->uav.get();
        // (S2.8) t0 stays unbound. ssrt_variance.hlsl declares HistoryTexture there and never
        // references it, so fxc strips the binding; leaving it bound would additionally alias
        // the surface the a-trous loop is about to write.
        srvs.at(0) = nullptr;
        srvs.at(1) = texMoments->srv.get();
        srvs.at(2) = normal.SRV;
        srvs.at(3) = texTemporal->srv.get();
        srvs.at(4) = depth.depthSRV;

        context->CSSetShaderResources(0, 5, srvs.data());
        context->CSSetUnorderedAccessViews(0, 1, uavs.data(), nullptr);
        context->CSSetShader(varianceCS.get(), nullptr, 0);

        context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);
        resetViews();

        // (S2.8) A-trous routing by ownership. See the block comment at the diffuse twin.
        const int atrousN = (int)settings.AtrousIterations;
        ID3D11ShaderResourceView* atrousSrc = texVariance->srv.get();

        // spatial filter
        for (int i = 0; i < atrousN; ++i)
        {
            denoiserCBData.atrousIterations = i;
            denoiserCB->Update(denoiserCBData);
            denoiserBuffer = denoiserCB->CB();
            context->CSSetConstantBuffers(2, 1, &denoiserBuffer);

            Texture2D* dst = nullptr;
            if (i == 0)
                dst = (atrousN == 1) ? texSSRColor.get() : texHistory.get();
            else
                dst = (((atrousN - 1 - i) % 2) == 0) ? texSSRColor.get() : texVariance.get();

            uavs.at(0) = dst->uav.get();
            // (S2.8) t0 stays unbound: ssrt_spatial.hlsl declares HistoryTexture there and
            // never references it, and at i == 0 that surface *is* the destination.
            srvs.at(0) = nullptr;
            // (spec A1) t1 carries the moments texture, whose .z is the accumulated frame
            // count the adaptive early-out votes on. It used to receive the motion-vector
            // target, which ssrt_spatial.hlsl never declared.
            srvs.at(1) = texMoments->srv.get();
            srvs.at(2) = normal.SRV;
            srvs.at(3) = atrousSrc;
            srvs.at(4) = depth.depthSRV;

            context->CSSetShaderResources(0, 5, srvs.data());
            context->CSSetUnorderedAccessViews(0, 1, uavs.data(), nullptr);
            context->CSSetShader(spatialSpecularCS.get(), nullptr, 0);

            context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);

            resetViews();

            atrousSrc = dst->srv.get();

            // (defect D4) The history is the first iteration's output, and at atrousN >= 2 it
            // was written straight into texHistory, so there is nothing to copy.
            if (i == 0 && atrousN >= 2)
                historyFed = true;
        }

        // (S2.8) At atrousN == 1 the single iteration wrote texSSRColor, which is both the
        // final result and the first-iteration output, so one copy is still owed.
        if (atrousN == 1 && SvgfHistoryNeeded(true) && texHistory) {
            CopyDynamicRegion(texHistory->resource.get(), texSSRColor->resource.get());
            historyFed = true;
        }

        // (S2.8) Moment ownership swap in place of the copy above. texHistoryMoments must end
        // the frame holding what the temporal pass just wrote; swapping the two pointers says
        // exactly that and moves no bytes.
        std::swap(texMoments, texHistoryMoments);

        Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::SSRTSvgf);
    } else if (denoiser == kDenoiserREBLUR) {
        // (batch C1) REBLUR_SPECULAR: pack texSSRColor + the world-space hit distance
        // into NRD's front-end layout, dispatch the instance, unpack the result back
        // into texSSRColor for the deferred composite. From here the specular signal
        // no longer shares the diffuse chain's surface-motion temporal reprojection —
        // REBLUR's virtual-position mechanism owns the reflected content's motion.
        // historyFed stays false, so the unconditional copy below publishes the
        // denoised result as next frame's raymarch history, same as the SVGF-off path.
        //
        // (S1.2) A false return means the dispatch did not complete: texSSRColor still
        // holds this frame's raw ray march, which is the correct thing to publish and to
        // hand the composite. The reset stays pending inside RunReblur.
        // (batch 36f, item 4) Folded unpack: DeferredCompositeCS decodes the packed surface.
        const bool foldUnpack = FoldUnpackActive();
        const bool dispatched = RunReblur(true, foldUnpack);
        if (foldUnpack) {
            specularCompositeSRV = (dispatched ? texNRDPackOutput : texNRDPackInput)->srv.get();
            specularCompositePacked = true;
        }
    }

    // output
    // (audit P6) texOutput was a byte-identical copy of texSSRColor whose only reader
    // was the deferred composite's SRV; that now binds texSSRColor->srv directly
    // (Deferred.cpp), saving a full-screen R16G16B16A16 CopyResource per frame plus the
    // texture itself.
    // (audit #13) Specular runs after diffuse, so it owns the once-per-frame snapshot.
    CopyHistoryGeometry();
    // (S2.5) ssrt_temporal.hlsl at t0 is the only reader of texHistory (plus the Buffer
    // Viewer). The specular ray march declares HistoryTexture at t0 and never references it,
    // so fxc strips the binding; ssrt_variance and ssrt_spatial do the same. Under REBLUR or
    // Off this copy was therefore a full-screen RGBA16F move -- 16 bytes per pixel of
    // read+write -- with no consumer at all.
    if (!historyFed && SvgfHistoryNeeded(true) && texHistory)
        CopyDynamicRegion(texHistory->resource.get(), texSSRColor->resource.get());

    context->CSSetShader(nullptr, nullptr, 0);

    state->EndPerfEvent();
}

// (audit #13) texHistoryNormals is the previous frame's normal-roughness buffer that
// ssrt_temporal.hlsl validates its reprojected history against. The copy used to live
// only at the end of DrawSSRTSpecular, so with EnableSpecular off and EnableSVGF on the
// texture stayed at its cleared contents forever, IsValidHistory() rejected every
// candidate and the diffuse temporal filter never accumulated -- exactly the
// configuration the diffuse+fallback setup runs in.
//
// It must happen after every temporal pass of the frame has read it, and exactly once.
// Deferred::DeferredPasses calls DrawSSRTDiffuse then DrawSSRTSpecular, so specular
// takes it when enabled and diffuse takes it otherwise.
//
// (defect D3) The depth snapshot rides on the same contract for the same reason, which is
// why it lives here and not in either draw pass: the two temporal dispatches share one
// history-geometry pair, and the second of them must still see the *previous* frame's
// values when it runs. Taking it from mip 0 of the Hi-Z pyramid rather than from the
// depth-stencil is what makes it a plain same-format copy -- the pyramid is already an
// R32_FLOAT restatement of kPOST_ZPREPASS_COPY, written this frame in Prepass and untouched
// since, with the far plane outside the dynamic-resolution sub-rect (audit #8) which is
// exactly the value that must reject there.
// (perf 3) The dynamic-resolution sub-rectangle, as a copy box. See the contract at the
// declaration for why every reader in this feature is insensitive to what lies outside it.
void ScreenSpaceRayTracing::CopyDynamicRegion(ID3D11Resource* a_dst, ID3D11Resource* a_src) const
{
    auto context = globals::d3d::context;
    const float2 size = Util::ConvertToDynamic(globals::state->screenSize);
    const auto full = globals::state->screenSize;

    // One texel of margin against a float-rounding disagreement with the shader's own
    // `BufferDim.xy * DynamicResolutionParams1.xy`, then clamped to the allocation. A dynamic
    // ratio of 1.0 therefore degenerates to the whole surface, i.e. to what CopyResource did.
    const UINT w = std::min((UINT)std::max(full.x, 1.0f), (UINT)std::max(size.x, 1.0f) + 1u);
    const UINT h = std::min((UINT)std::max(full.y, 1.0f), (UINT)std::max(size.y, 1.0f) + 1u);

    const D3D11_BOX box{ 0, 0, 0, w, h, 1 };
    context->CopySubresourceRegion(a_dst, 0, 0, 0, 0, a_src, 0, &box);
}

void ScreenSpaceRayTracing::CopyHistoryGeometry()
{
    // (S2.5) ssrt_temporal.hlsl at t6 is the only reader of texHistoryNormals, so under
    // REBLUR or Off this was a full-screen R10G10B10A2 copy -- 8 bytes per pixel of read+write
    // traffic, ~66 MB/frame at a 4K allocation -- feeding nothing. It is not in the Buffer
    // Viewer either, so SVGF is its whole consumer set.
    if (!(SvgfHistoryNeeded(false) || SvgfHistoryNeeded(true)) || !texHistoryNormals)
        return;

    auto normal = globals::game::renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
    CopyDynamicRegion(texHistoryNormals->resource.get(), normal.texture);

    // Only the denoiser reads it, so an SVGF-off frame must not pay for it. Flipping SVGF
    // back on latches historyClearPending (guard G8), which resets this to the far plane, so
    // the stale content left behind while it was off can never be consumed.
    //
    // (perf 3) Restricted to the dynamic-resolution sub-rect, which is where the copy's whole
    // purpose lies. The region outside it now keeps the far plane ClearDenoiserHistory wrote
    // instead of inheriting texDepth's own untouched content -- the same value, arrived at more
    // directly, and the one audit #8's convention asks for there.
    if (texHistoryDepth)
        CopyDynamicRegion(texHistoryDepth->resource.get(), texDepth->resource.get());
}

// (batch C1) One REBLUR leg, shared by both chains. The NRD instance runs its own pipeline
// against the guide surfaces the NRD feature published this frame, and the unpack pass writes
// the denoised radiance back over the chain's own surface — which is exactly where the SVGF
// chain would have left its result, so everything downstream (composites, history feeds,
// DLSS-RR's hit-distance guide) is untouched by the choice of denoiser.
//
// (batch 11, item A) Two dispatches, not three. The front-end pack that used to open this
// function is now part of the ray march: texNRDPackInput arrives already carrying this frame's
// radiance in NRD's IN_*_RADIANCE_HITDIST layout, because the caller bound it at u0 and set
// SSRTCB::NRDFrontEndPack on the same condition that calls this function.
bool ScreenSpaceRayTracing::RunReblur(bool a_specular, bool a_skipUnpack)
{
    auto context = globals::d3d::context;
    auto state = globals::state;
    auto& nrdSvc = globals::features::nrd;

    const float2 size = Util::ConvertToDynamic(state->screenSize);
    const float2 dispatchCount = { (size.x + 7) / 8, (size.y + 7) / 8 };

    state->BeginPerfEvent(a_specular ? "SSRT REBLUR Specular" : "SSRT REBLUR Diffuse");
    Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::SSRTReblur);

    auto& integration = a_specular ? nrdReblurSpecular : nrdReblurDiffuse;
    auto& reblurUI = a_specular ? settings.ReblurSpecular : settings.ReblurDiffuse;
    auto& reblurNative = a_specular ? reblurSpecularSettings : reblurDiffuseSettings;
    bool& resetFlag = a_specular ? resetReblurSpecular : resetReblurDiffuse;
    auto& texInput = texNRDPackInput;
    auto& texOutput = texNRDPackOutput;
    auto& texRadiance = a_specular ? texSSRColor : texSSRTDiffuseColor;

    std::array<ID3D11ShaderResourceView*, 4> srvs = { nullptr };
    std::array<ID3D11UnorderedAccessView*, 1> uavs = { nullptr };
    auto resetViews = [&]() {
        srvs.fill(nullptr);
        uavs.fill(nullptr);
        context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
        context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
    };

    // (S1.2) Set by the NRD dispatch below; gates the unpack and the reset-flag clear.
    bool dispatched = false;

    // (batch 11, item A) The front-end pack pass that used to stand here is gone. texInput
    // already holds this frame's radiance in REBLUR's IN_*_RADIANCE_HITDIST layout, written
    // straight out of the ray march: DrawSSRT{Diffuse,Specular} bound it at u0 and set
    // SSRTCB::NRDFrontEndPack, on exactly the condition that leads here. Nothing between that
    // dispatch and this one touches the surface.
    //
    // (depth and normal are still read below for the SVGF sibling's sake in the *caller*; this
    // function itself no longer needs either, which is why the two locals are only used by the
    // recovery path at the bottom.)

    // ---- REBLUR dispatch ----
    {
        auto commonSettings = nrdSvc.GetCommonSettings();
        commonSettings.splitScreen = reblurUI.SplitScreen;
        commonSettings.enableValidation = reblurUI.EnableValidation;
        // (S1.1) Off by default. Batch C1 wired the ray march's R8 hit-coverage surface to
        // IN_DIFF_CONFIDENCE on the strength of a shared word; NRD's own header calls that
        // input "user-provided history confidence ... i.e. antilag" and requires it to be
        // "computed for the previous frame in the current frame". Coverage is neither a
        // history statement nor a previous-frame quantity, and where it varies it is
        // anti-correlated with what the formula wants. Kept as an opt-in A/B switch, with
        // the derivation at Settings::ReblurFeedHitCoverageConfidence.
        const bool feedConfidence =
            !a_specular && settings.ReblurFeedHitCoverageConfidence && texSSRTDiffuseConfidence;
        commonSettings.isHistoryConfidenceAvailable = feedConfidence;
        // (batch 36f, item 2) Distance limit: NRD skips every pixel whose viewZ is past
        // denoisingRange (16x16 tiles that are entirely past it are classified as sky and skip
        // every pass). Set a hair above the shader-side limit, so any pixel the composites treat
        // as inside the limit is certainly one NRD denoised; the sliver in between was not traced
        // and carries zero radiance, which NRD handles like any other input. Per instance: the
        // diffuse chain is limited only under ambient reinjection (DistanceLimitOnChain).
        if (DistanceLimitOnChain(a_specular))
            commonSettings.denoisingRange = DistanceLimitUnits() * 1.001f;
        if (resetFlag)
            commonSettings.accumulationMode = nrd::AccumulationMode::CLEAR_AND_RESTART;
        integration.SetCommonSettings(commonSettings);

        // Antilag stays off and checkerboard stays OFF for the whole first
        // integration (NRD bring-up guidance); ApplyReblurSettings enforces the
        // former for every consumer.
        nrdSvc.ApplyReblurSettings(reblurNative, reblurUI, nrd::CheckerboardMode::OFF);
        reblurNative.hitDistanceParameters.A = settings.ReblurHitDistA;
        reblurNative.hitDistanceParameters.B = settings.ReblurHitDistB;
        reblurNative.hitDistanceParameters.C = settings.ReblurHitDistC;
        if (a_specular) {
            // (batch 36f, item 1) Radius 0 makes NRD drop the PrePass dispatch (Reblur.cpp
            // skipPrePass: diffuse radius is always 0 here and checkerboard is OFF). Off, or with
            // the Batch 36f master switch off: the batch 34 / 36e values.
            if (SkipSpecularPrepassActive()) {
                reblurNative.specularPrepassBlurRadius = 0.0f;
                reblurNative.usePrepassOnlyForSpecularMotionEstimation = false;
            } else {
                reblurNative.specularPrepassBlurRadius = std::max(settings.SpecularPrepassBlurRadius, 0.0f);
                reblurNative.usePrepassOnlyForSpecularMotionEstimation = settings.UsePrepassOnlyForSpecularMotionEstimation;
            }
        }
        integration.SetDenoiserSettings(&reblurNative);

        integration.SetNamedSRV(nrd::ResourceType::IN_MV, nrdSvc.GetMotionVectorSRV());
        integration.SetNamedUAV(nrd::ResourceType::IN_MV, nrdSvc.GetMotionVectorUAV());
        integration.SetNamedSRV(nrd::ResourceType::IN_NORMAL_ROUGHNESS, nrdSvc.GetNormalRoughnessSRV());
        integration.SetNamedSRV(nrd::ResourceType::IN_VIEWZ, nrdSvc.GetViewZSRV());
        if (a_specular) {
            integration.SetNamedSRV(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST, texInput->srv.get());
            integration.SetNamedSRV(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, texOutput->srv.get());
            integration.SetNamedUAV(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, texOutput->uav.get());
        } else {
            if (feedConfidence)
                integration.SetNamedSRV(nrd::ResourceType::IN_DIFF_CONFIDENCE, texSSRTDiffuseConfidence->srv.get());
            integration.SetNamedSRV(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, texInput->srv.get());
            integration.SetNamedSRV(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, texOutput->srv.get());
            integration.SetNamedUAV(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, texOutput->uav.get());
        }

        integration.SetTimingGroup(a_specular ? "NRD specular" : "NRD diffuse");
        dispatched = integration.Dispatch();
        // (S1.2) The reset is only consumed if the dispatch that was supposed to consume it
        // actually ran. Clearing it on a skipped dispatch is what let a stale or never-written
        // NRD history come back as if it had been restarted.
        if (dispatched)
            resetFlag = false;
    }

    // ---- back-end unpack ----
    // (S1.2) The source depends on whether the dispatch completed. texOutput would otherwise hold
    // either the previous frame's denoised result or, on the first frame, uninitialised RGBA16F --
    // which includes every NaN and Inf bit pattern.
    //
    // (batch 11, item A) The unpack now runs on *both* paths, and that is what preserves S1.2's
    // guarantee. Before this batch the failure path could simply skip the pass, because
    // texRadiance still held the raw ray march; now the ray march wrote texInput instead, so
    // skipping would hand the composites last frame's contents. Unpacking texInput is the honest
    // recovery: it is this frame's own radiance, undenoised, and the YCoCg transform is its own
    // exact inverse (Y = .25r+.5g+.25b, Co = .5(r-b), Cg = .5g-.25(r+b) reconstructs r, g, b
    // algebraically), so what lands in texRadiance is the ray march's output to within fp
    // rounding. .w carries the normalized hit distance rather than the confidence -- which is
    // already true on the success path, and no consumer reads .w of either surface.
    if (!dispatched)
        logger::warn("SSRT: REBLUR {} dispatch did not complete; publishing this frame's undenoised radiance",
            a_specular ? "specular" : "diffuse");

    // (batch 36f, item 4) Folded unpack: the caller's composite decodes the packed surface itself
    // (texOutput on success, texInput on failure -- the same choice as below).
    if (!a_skipUnpack) {
        srvs.at(0) = (dispatched ? texOutput : texInput)->srv.get();
        uavs.at(0) = texRadiance->uav.get();

        context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
        context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
        context->CSSetShader(nrdUnpackCS.get(), nullptr, 0);
        {
            Util::DenoiserTimerScope timing("SSRT", a_specular ? "Unpack specular" : "Unpack diffuse",
                (uint)dispatchCount.x, (uint)dispatchCount.y, (uint)dispatchCount.x * 8u, (uint)dispatchCount.y * 8u);
            context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);
        }
        resetViews();
    }

    // The NRD dispatch owns s0/s1 and b1 while it runs; restore what the surrounding
    // draw pass set up so anything dispatched after this (the diffuse composite, or a
    // caller-side pass added later) sees the bindings it expects.
    {
        std::array<ID3D11SamplerState*, 1> samplers = { linearSampler.get() };
        context->CSSetSamplers(0, 1, samplers.data());
        auto ssrtBuffer = ssrtCB->CB();
        context->CSSetConstantBuffers(1, 1, &ssrtBuffer);
    }

    Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::SSRTReblur);
    state->EndPerfEvent();

    return dispatched;
}

void ScreenSpaceRayTracing::DrawSSRTDiffuse()
{
    // (batch 12) Above the early-out on purpose, so switching the diffuse component off hands the
    // compact sparse set back instead of stranding it; and above every binding in this function,
    // so the resolution this frame traces at is fixed before the first one. The frame reads this
    // local and never the setting again -- see ResolveSamplingMode.
    const uint samplingMode = ResolveSamplingMode();
    const bool sparseSampling = samplingMode != kSamplingFull;

    if (!settings.EnableDiffuse)
        return;

    // (S1.4) Whole-chain readiness before the first dispatch, and a neutral surface if it
    // fails. Without this, a failed compile of any tracing pass left a null CSSetShader --
    // a silent no-op in D3D11 -- and every pass after it consumed whatever the surface last
    // held: at best the previous frame's radiance, at worst uninitialised RGBA16F. The
    // denoiser then filtered it, the history accumulated it, and the composite added it, so
    // one missing shader produced a plausible-looking wrong image instead of a visible
    // failure.
    //
    // Neutral is confidence 0 with zero radiance, not merely zero radiance. Confidence 0 says
    // "the rays resolved nothing", which is exactly what happened, and under ambient
    // reinjection DeferredCompositeCS then keeps the whole vanilla ambient -- the frame looks
    // like SSRT diffuse is switched off, which is the honest presentation of a chain that
    // cannot run.
    if (!prepareColorCS || !diffuseCompositeCS || !texSSRTDiffuseColor ||
        !texSSRTDiffuseConfidence || !texSSRTDiffuseConfidenceSmooth || !texSSRTDiffuseHitDistance ||
#ifdef ENABLE_SHARC
        (settings.EnableSharc ? (!raymarchDiffuseSharcCS || !sharcUpdateRaymarchCS || !sharcResolveCS) : !raymarchDiffuseCS)
#else
        !raymarchDiffuseCS
#endif
    ) {
        auto ctx = globals::d3d::context;
        const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        if (texSSRTDiffuseColor)
            ctx->ClearUnorderedAccessViewFloat(texSSRTDiffuseColor->uav.get(), zero);
        if (texSSRTDiffuseConfidence)
            ctx->ClearUnorderedAccessViewFloat(texSSRTDiffuseConfidence->uav.get(), zero);
        if (texSSRTDiffuseConfidenceSmooth)
            ctx->ClearUnorderedAccessViewFloat(texSSRTDiffuseConfidenceSmooth->uav.get(), zero);
        return;
    }

    auto renderer = globals::game::renderer;
    auto context = globals::d3d::context;
    auto state = globals::state;

    state->BeginPerfEvent("SSRT Diffuse Compute");
    Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::SSRTTraceDiffuse);

    auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
    auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
    auto specular = renderer->GetRuntimeData().renderTargets[SPECULAR];
    auto normal = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
    auto albedo = renderer->GetRuntimeData().renderTargets[ALBEDO];
    auto motion = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

    auto& dynamicCubemaps = globals::features::dynamicCubemaps;
    auto& ssgi = globals::features::screenSpaceGI;
    auto& skylighting = globals::features::skylighting;

    float2 size = Util::ConvertToDynamic(state->screenSize);
    float2 dispatchCount = { (size.x + 7) / 8, (size.y + 7) / 8 };

    // (batch 12) The extent the ray march itself covers, which is the compact grid under a sparse
    // mode and the render sub-rect otherwise. Every other dispatch in this function keeps
    // dispatchCount: the resolve pass restores full resolution before any of them runs.
    //
    // Integer halving with *floor*, matching SSRT_GetSparseExtent and -- for half resolution --
    // mip 1 of the Hi-Z pyramid, whose downsample dispatch is sized `max(1, size >> 1)`. Ceil
    // would leave the last compact column of an odd render extent backed by a mip 1 texel nobody
    // wrote, which by the audit #8 convention reads as sky. The full-resolution column that floor
    // leaves without a compact sample of its own is reconstructed by the resolve pass from its
    // clamped neighbour, so nothing is left stale at the edge.
    const uint traceExtentX = sparseSampling ? std::max(1u, (uint)size.x / 2u) : (uint)size.x;
    const uint traceExtentY = samplingMode == kSamplingHalfRes ? std::max(1u, (uint)size.y / 2u) : (uint)size.y;
    const uint traceDispatchX = (traceExtentX + 7u) / 8u;
    const uint traceDispatchY = (traceExtentY + 7u) / 8u;

    // (reinjection noise) Whether the confidence accumulator runs this frame. Decided once, in
    // the constant-buffer block below, and read again by the composite dispatch so the CPU-side
    // bindings and the shader-side branch cannot disagree.
    bool confidenceTemporal = false;

    // (batch 6) Whether the three-pass spatial confidence filter runs this frame. Decided once,
    // here, because three separate sites depend on the same answer -- the accumulator gate below,
    // the chain's own dispatches, and which permutation of the composite is bound -- and any
    // disagreement between them would leave texSSRTDiffuseConfidenceSmooth written twice or not at
    // all. The allocation is attempted as part of the predicate, before any dispatch binds it, so a
    // failure simply selects the full-resolution 7x7 path, which is the toggle's off behaviour.
    const bool confidenceFilter =
        settings.EnableAmbientReinjection && settings.LowResConfidenceFilter &&
        confDownsampleCS && confBlurHorizontalCS && confBlurVerticalCS && confUpsampleCS &&
        diffuseCompositeExternalConfCS && EnsureConfidenceFilterResources();

    // (batch 11, item A) This frame's denoiser, resolved before the ray march rather than after
    // it: the ray march now chooses its own output layout on the answer. See the matching site
    // and derivation in DrawSSRTSpecular.
    //
    // (S1.3) Resolved in Prepass; re-tested here only for this frame's guides, and it falls back
    // to SVGF rather than to raw noise.
    uint denoiser = EffectiveDenoiser(false);
    if (denoiser == kDenoiserREBLUR && !ReblurReady(false)) {
        denoiser = SvgfChainReady(false) ? kDenoiserSVGF : kDenoiserOff;
        historyClearPending = true;
    }
    const bool nrdFrontEndPack = (denoiser == kDenoiserREBLUR);

    SSRTCB ssrCBData;
    {
        ssrCBData.MaxSteps = settings.MaxSteps;
        // (audit P3) See DrawSSRTSpecular: clamp to the allocated mip count, and to the level
        // count the pyramid build actually reached this frame.
        ssrCBData.MaxMips = std::min({ settings.MaxMips, maxMips - 1u, hiZTopMipBuilt });
        ssrCBData.Thickness = settings.Thickness;
        ssrCBData.NormalBias = settings.NormalBias;
        ssrCBData.BRDFBias = settings.BRDFBias;
        // (ambient reinjection) The vanilla ambient and the cubemap fallback are two answers to
        // the same question -- what light arrives from the directions the rays could not resolve
        // -- and running both counts the environment twice. Worse, the fallback path reports
        // `confidence = 1` for every pixel it touches, so the composite would remove *all* of
        // the vanilla ambient and then add the cubemap's estimate of the same light back on top.
        // Force it off rather than trusting the user to keep the two consistent; the saved
        // setting is untouched and returns the moment reinjection comes off.
        //
        // (batch 8) ...unless beta asks for it. Cubemap Fill Blend turns the either/or above into
        // a dial, and the block this flag gates is where the cubemap estimate is built, so a
        // non-zero beta has to let it run. What stops the double-count the comment above warns
        // about is not this gate any more -- it is that the block no longer reports
        // `confidence = 1` in this mode. It hands the estimate to the reinjection block as a
        // weighted (radiance, coverage) contribution instead, and the weights still sum to the
        // unresolved fraction. See ssrt_raymarch.hlsl.
        //
        // beta is gated on reinjection here rather than in the shader, so the fallback path can
        // never see a non-zero value however the settings file was hand-edited, and so the shader
        // needs one comparison instead of two.
        const bool cubemapFill =
            settings.EnableAmbientReinjection && settings.UseDynamicCubemapsAsFallback &&
            settings.CubemapFillBlend > 0.0f && dynamicCubemaps.loaded;
        const bool legacyFallback =
            settings.UseDynamicCubemapsAsFallback && !settings.EnableAmbientReinjection && dynamicCubemaps.loaded;
        ssrCBData.UseDynamicCubemapsAsFallback = (legacyFallback || cubemapFill) ? 1u : 0u;
        ssrCBData.CubemapFillBlend = cubemapFill ? settings.CubemapFillBlend : 0.0f;
        ssrCBData.OcclusionStrength = settings.OcclusionStrength;
        ssrCBData.CubemapNormalization = settings.CubemapNormalization;
        ssrCBData.FreezeNoisePhase = settings.FreezeNoisePhase ? 1u : 0u;  // (diagnostic T2)
        ssrCBData.UseBlueNoise = (settings.UseBlueNoise && noiseSRV) ? 1u : 0u;  // (S3.10)
        // (reinjection noise) Gated on reinjection as well as on its own switch: with reinjection
        // off nothing reads the confidence surface, so accumulating it would be pure cost. The
        // allocation is attempted here, before any dispatch binds it, and a failure clears the
        // flag -- the composite pass then publishes the spatial mean alone, which is exactly the
        // pre-existing behaviour.
        //
        // (batch 6) And gated on the spatial filter being off. The two are competing answers to
        // one question and the accumulator is the one that lags, so the spatial filter wins --
        // exactly as ambient reinjection wins over the diffuse cubemap fallback above. It is also
        // a hard requirement rather than a preference: with the filter on, the composite runs the
        // permutation that has the whole confidence block compiled out, so there is no spatial
        // mean there for an accumulator to blend with.
        confidenceTemporal = settings.EnableAmbientReinjection && settings.TemporalAmbientConfidence &&
                             !confidenceFilter && EnsureAmbientConfidenceResources();
        ssrCBData.TemporalAmbientConfidence = confidenceTemporal ? 1u : 0u;
        ssrCBData.AmbientConfidenceInvMaxFrames = 1.0f / (settings.AmbientConfidenceMaxFrames + 1.0f);
        // (batch 11, item A) See the matching block in DrawSSRTSpecular. Written unconditionally
        // so the buffer is fully defined on both paths; the flag decides whether they are read.
        ssrCBData.NRDHitDistA = settings.ReblurHitDistA;
        ssrCBData.NRDHitDistB = settings.ReblurHitDistB;
        ssrCBData.NRDHitDistC = settings.ReblurHitDistC;
        ssrCBData.NRDFrontEndPack = nrdFrontEndPack ? 1u : 0u;
        ssrCBData.SpecularMaxRoughness = settings.SpecularMaxRoughness;
        // (batch 36f, item 2) Distance limit, diffuse chain (only under ambient reinjection):
        // not traced past the limit, and radiance + confidence fade together over the last 20%.
        // The composite below reads the same two fields to ignore whatever the denoiser left
        // past the limit.
        if (DistanceLimitOnChain(false)) {
            ssrCBData.DistanceCapEnd = DistanceLimitUnits();
            ssrCBData.DistanceCapStart = ssrCBData.DistanceCapEnd * 0.8f;
        }
    }
    ssrtCB->Update(ssrCBData);
    auto buffer = ssrtCB->CB();
    context->CSSetConstantBuffers(1, 1, &buffer);

    // (batch 36f, item 4) Set by the REBLUR block when the unpack is folded into the composite:
    // the packed surface to read at t0 instead of texSSRTDiffuseColor.
    ID3D11ShaderResourceView* compositePackedSRV = nullptr;

    // (audit P6) Raymarch UAV slots: u0 radiance/confidence, u1..u4 SHARC (bound only
	// while SHARC is enabled, and only declared by the SHARC shader permutations),
	// u5 the raw confidence copy (ambient reinjection), u6 the hit-distance surface
	// (batch 1, item 2). Keep this in lockstep with the register map at the top of
	// ssrt_raymarch.hlsl and sharc_resolve.hlsl. The SVGF temporal pass below reuses slots
	// u0/u1 for its own two outputs, and the diffuse composite reuses u0/u1 for kMAIN and the
	// smoothed confidence, so the array is never smaller than 2. Seven entries against the
	// eight UAVs a cs_5_0 dispatch may bind.
    std::array<ID3D11ShaderResourceView*, 13> srvs = { nullptr };
	std::array<ID3D11UnorderedAccessView*, 7> uavs = { nullptr };

    auto resetViews = [&]() {
		srvs.fill(nullptr);
		uavs.fill(nullptr);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	};

    std::array<ID3D11SamplerState*, 1> samplers = { linearSampler.get() };

    context->CSSetSamplers(0, 1, samplers.data());

    const auto envTexture = dynamicCubemaps.loaded ? dynamicCubemaps.envTexture->srv.get() : nullptr;
	const auto envReflectionsTexture = dynamicCubemaps.loaded ? dynamicCubemaps.envReflectionsTexture->srv.get() : nullptr;

    auto [ssgi_ao, ssgi_y, ssgi_cocg, ssgi_gi_spec] = ssgi.GetOutputTextures();

    // (batch 11, item A) u0 is the packed NRD front-end surface under REBLUR and the chain's own
    // radiance surface otherwise -- same slot, same RGBA16F format, only the meaning of the four
    // channels changes. Under REBLUR the back-end unpack is what fills texSSRTDiffuseColor, which
    // is where the diffuse composite reads it from either way. See DrawSSRTSpecular.
    //
    // (batch 12) ...and it is the *compact* surface under a sparse mode, whatever the layout. The
    // ray march's stores are unchanged either way -- it writes u0/u5/u6 at its own dispatch
    // coordinate, which the sparse permutations know is a compact grid coordinate -- so the choice
    // of surface is made here and nowhere else. The resolve pass a few dispatches below is what
    // puts the result back on the full-resolution grid the whole rest of this function reads.
    uavs.at(0) = sparseSampling ? texSparseColor->uav.get() :
                                  (nrdFrontEndPack ? texNRDPackInput->uav.get() : texSSRTDiffuseColor->uav.get());
    // (ambient reinjection) Always bound, not gated on the setting: the shader writes it
    // unconditionally so that the surface stays deterministic for every texel the dispatch
    // covers, and an unbound UAV would make that write a silent no-op. The consumer side is
    // what the setting gates.
    uavs.at(5) = sparseSampling ? texSparseConfidence->uav.get() : texSSRTDiffuseConfidence->uav.get();
    // (batch 1, item 2) Always bound, for the same reason the confidence surface is: the shader
    // writes it unconditionally so the surface stays deterministic for every texel the dispatch
    // covers -- including the 1.0 "as distant as the encoding can say" a far-plane lane resolves
    // to -- and an unbound UAV would turn that write into a silent no-op, leaving the a-trous
    // pass reading a stale or cleared surface. HitRadiusStrength 0 is what makes the mechanism
    // inert, on the consumer side.
    uavs.at(6) = sparseSampling ? texSparseHitDistance->uav.get() : texSSRTDiffuseHitDistance->uav.get();
#ifdef ENABLE_SHARC
    if (settings.EnableSharc) {
        EnsureSharcResources();  // (audit P6) allocate on first enable, before any dispatch binds them
        uavs.at(1) = sharcHashEntries->uav.get();
        uavs.at(2) = sharcHashCopyOffsets->uav.get();
        uavs.at(3) = sharcVoxelData->uav.get();
        uavs.at(4) = sharcVoxelDataPrev->uav.get();
    }
#endif

    // (S2.5) t0 stays unbound; see the matching site in DrawSSRTSpecular.
    srvs.at(0) = nullptr;
    srvs.at(1) = motion.SRV;
    srvs.at(2) = normal.SRV;
    srvs.at(3) = main.SRV;
    srvs.at(4) = depth.depthSRV;
    srvs.at(5) = texDepth->srv.get();
    srvs.at(6) = noiseSRV.get();
    srvs.at(7) = envTexture;
    srvs.at(8) = inInterior ? envTexture : envReflectionsTexture;
    srvs.at(9) = ssgi_ao;
    srvs.at(10) = dynamicCubemaps.loaded && skylighting.loaded ? skylighting.texProbeArray->srv.get() : nullptr;
    srvs.at(11) = dynamicCubemaps.loaded && skylighting.loaded ? skylighting.stbn_vec3_2Dx1D_128x128x64.get() : nullptr;
    srvs.at(12) = albedo.SRV;

    context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
    context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
    context->CSSetConstantBuffers(1, 1, &buffer);
#ifdef ENABLE_SHARC
    if (settings.EnableSharc) {
        state->BeginPerfEvent("SHARC Update");
        context->CSSetShader(sharcUpdateRaymarchCS.get(), nullptr, 0);

        context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);
        state->EndPerfEvent();

        state->BeginPerfEvent("SHARC Resolve");
        context->CSSetShader(sharcResolveCS.get(), nullptr, 0);

        context->Dispatch(sharcNumEntries / 256u, 1, 1);
        state->EndPerfEvent();
    }

    // (batch 12) samplingMode is kSamplingFull whenever SHARC is on -- ResolveSamplingMode forces
    // it -- so the two selectors below cannot both fire.
    context->CSSetShader(settings.EnableSharc ? raymarchDiffuseSharcCS.get() : SelectDiffuseRaymarchShader(samplingMode), nullptr, 0);
#else
    context->CSSetShader(SelectDiffuseRaymarchShader(samplingMode), nullptr, 0);
#endif
    context->Dispatch(traceDispatchX, traceDispatchY, 1);
    resetViews();

    Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::SSRTTraceDiffuse);

    // (batch 12) THE SPARSE RESOLVE. Half a ray per render pixel goes in, three full-resolution
    // surfaces come out, and everything after this point is the code that ran before this batch --
    // the denoiser (either one), the confidence filter, the composite -- reading the same three
    // surfaces at the same extent with the same meanings.
    //
    // Placed here rather than folded into the denoiser: it has to precede *both* denoiser paths
    // and the no-denoiser path, and it has to precede the confidence filter, whose input is the
    // raw ray-march confidence. Its own timing bucket rather than the trace bucket, so the user can
    // confirm that what the sparse trace saves is not being handed straight back -- which is the
    // whole question this batch exists to answer.
    if (sparseSampling) {
        state->BeginPerfEvent("SSRT Sparse Resolve");
        Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::SSRTSparseResolve);

        // u0 is the destination the ray march would have written at full density, in whichever
        // layout SSRTCB::NRDFrontEndPack selected -- the resolve is channel-agnostic, so the same
        // shader carries REBLUR's packed layout and the chain's linear one.
        uavs.at(0) = nrdFrontEndPack ? texNRDPackInput->uav.get() : texSSRTDiffuseColor->uav.get();
        uavs.at(1) = texSSRTDiffuseConfidence->uav.get();
        uavs.at(2) = texSSRTDiffuseHitDistance->uav.get();

        srvs.at(0) = texSparseColor->srv.get();
        srvs.at(1) = texSparseConfidence->srv.get();
        srvs.at(2) = normal.SRV;
        srvs.at(3) = texSparseHitDistance->srv.get();
        srvs.at(4) = depth.depthSRV;
        // t5 is mip 1 of the Hi-Z pyramid, i.e. the 2x2 minimum, which is exactly the depth of the
        // subpixel each half-resolution ray was traced from. Declared only by the half-resolution
        // permutation: the checkerboard one reads full-resolution depth at t4 for its taps,
        // because every one of its compact texels corresponds to a real full-resolution pixel.
        srvs.at(5) = samplingMode == kSamplingHalfRes ? depthSRVs[1].get() : nullptr;

        context->CSSetShaderResources(0, 6, srvs.data());
        context->CSSetUnorderedAccessViews(0, 3, uavs.data(), nullptr);
        context->CSSetShader(samplingMode == kSamplingHalfRes ? sparseResolveHalfResCS.get() : sparseResolveCheckerCS.get(), nullptr, 0);

        {
            Util::DenoiserTimerScope timing("SSRT", "Sparse Resolve",
                (uint)dispatchCount.x, (uint)dispatchCount.y, (uint)dispatchCount.x * 8u, (uint)dispatchCount.y * 8u);
            context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);
        }
        resetViews();

        Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::SSRTSparseResolve);
        state->EndPerfEvent();
    }

#ifdef ENABLE_SHARC
    if (settings.EnableSharc) {
        std::swap(sharcVoxelData, sharcVoxelDataPrev);
    }
#endif

    // (defect D4) See the derivation at the matching site in DrawSSRTSpecular. False until
    // the a-trous loop has published its first iteration's output as next frame's history;
    // if it never runs -- SVGF off, or a config carrying AtrousIterations 0 -- the
    // unconditional copy at the bottom stands in unchanged.
    bool historyFed = false;

    // (batch 11, item A) `denoiser` is resolved above the ray march now, because the ray march's
    // own output layout depends on it. Nothing else about the sequence changed.

    if (denoiser == kDenoiserSVGF) {
        Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::SSRTSvgf);
        DenoiserCB denoiserCBData = GetDenoiserCBData(true);

        // (batch 1, item 1) The pre-blur, and the one thing about it that needs explaining on
        // this side: where its output goes.
        //
        // It writes texVariance. That surface is not free-floating scratch by accident -- it is
        // provably dead at this point in the frame. Its only writer inside DrawSSRTDiffuse is
        // the variance pass three dispatches below, its only readers are that pass's own output
        // consumers (the a-trous ping-pong), and DrawSSRTSpecular does not run until this whole
        // function has returned. So the lifetime is: pre-blur writes it, the temporal pass reads
        // it, the variance pass overwrites it, the a-trous chain ping-pongs it. Reusing it costs
        // nothing where a dedicated surface would cost a full-screen RGBA16F (63.3 MiB at a 4K
        // allocation, against the three unconditional ones this feature already holds).
        //
        // The one thing that would break: inserting a pass between the pre-blur and the variance
        // pass that reads texVariance expecting last frame's content. Nothing does today, and
        // this comment is the tripwire if something ever wants to.
        const bool preBlurActive = settings.EnablePreBlur && preblurCS;
        ID3D11ShaderResourceView* temporalInput = texSSRTDiffuseColor->srv.get();

        auto denoiserBuffer = denoiserCB->CB();

        if (preBlurActive) {
            // The pre-blur is handed the real FireflyClampSigma; the temporal pass below is
            // handed 0. See the "why the firefly clamp moved in here" block in
            // ssrt_preblur.hlsl: the clamp cannot survive an unconditional spatial filter in
            // front of it, so it moves rather than being defeated, and it must not then run
            // twice.
            denoiserCB->Update(denoiserCBData);
            denoiserBuffer = denoiserCB->CB();
            context->CSSetConstantBuffers(2, 1, &denoiserBuffer);

            uavs.at(0) = texVariance->uav.get();
            srvs.at(2) = normal.SRV;
            srvs.at(3) = texSSRTDiffuseColor->srv.get();
            srvs.at(4) = depth.depthSRV;

            context->CSSetShaderResources(0, 5, srvs.data());
            context->CSSetUnorderedAccessViews(0, 1, uavs.data(), nullptr);
            context->CSSetShader(preblurCS.get(), nullptr, 0);

            context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);
            resetViews();

            temporalInput = texVariance->srv.get();
            denoiserCBData.fireflyClampSigma = 0.0f;
        }

        denoiserCB->Update(denoiserCBData);
        denoiserBuffer = denoiserCB->CB();
        context->CSSetConstantBuffers(2, 1, &denoiserBuffer);
        // temporal filter
        uavs.at(0) = texTemporal->uav.get();
        uavs.at(1) = texMoments->uav.get();
        // (diagnostic H) The one dispatch that writes the picture; see GetDenoiserCBData.
        uavs.at(2) = texDebugHistory->uav.get();
        srvs.at(0) = texHistoryDiffuse->srv.get();
        srvs.at(1) = motion.SRV;
        srvs.at(2) = normal.SRV;
        srvs.at(3) = temporalInput;
        srvs.at(4) = depth.depthSRV;
        srvs.at(5) = texHistoryMomentsDiffuse->srv.get();
        srvs.at(6) = texHistoryNormals->srv.get();
        srvs.at(7) = texHistoryDepth->srv.get();  // (defect D3)
        // (batch 1, item 1) The raw ray-march surface, for the defect D1 history clamp's
        // reference neighbourhood only. Identical to t3 whenever the pre-blur is off, which is
        // what makes that configuration bit-identical to the previous build; see the declaration
        // of RawColorTexture in ssrt_temporal.hlsl for why the box must not be built from the
        // pre-blurred surface.
        srvs.at(8) = texSSRTDiffuseColor->srv.get();

        context->CSSetShaderResources(0, 9, srvs.data());
        context->CSSetUnorderedAccessViews(0, 3, uavs.data(), nullptr);
        // (perf 1) The production permutation unless a diagnostic switch is set; see
        // SelectTemporalShader. u2 stays bound either way -- the production permutation does not
        // declare it, and binding a UAV a shader does not use is free.
        context->CSSetShader(SelectTemporalShader(), nullptr, 0);

        context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);
        resetViews();

        // (S2.8) THE COPY CHAIN, AND WHY SIX OF ITS EIGHT COPIES WERE UNNECESSARY.
        //
        // What was here. Per chain, three full-screen RGBA16F CopySubresourceRegions: the
        // moments into the moment history, the first a-trous iteration's output into the
        // colour history, and -- at an even iteration count -- the ping-pong's final leg back
        // into the surface the rest of the pipeline reads. Two chains, so six; with the normal
        // and depth snapshots that is the eight the audit counted. 16 bytes per pixel of
        // read+write each, 96 bytes per pixel in total, ~800 MB/frame at a 4K allocation.
        //
        // Why none of the three is needed. Every one of them exists to put a surface's
        // *contents* where a name expects them, and in each case the name could simply have
        // been pointed at the surface instead:
        //
        //   * The moment history. The temporal pass reads the history and writes the current
        //     moments; the copy then makes the current moments the history. Swapping the two
        //     pointers once this frame's readers of the current moments are done says the same
        //     thing and moves nothing. (The three moment surfaces -- one scratch, two
        //     histories -- rotate through each other over a frame; they are the same format
        //     and extent, so which physical texture plays which part is immaterial.)
        //
        //   * The colour history. Defect D4 requires the history to be the *first* a-trous
        //     iteration's output, and the copy took it because iteration 0's output was in the
        //     surface iteration 1 was about to overwrite. Writing iteration 0 straight into the
        //     history surface is the same bytes, one dispatch earlier, with no copy at all --
        //     and the surface is free at that moment, because the temporal pass read it two
        //     dispatches ago and nothing else reads it this frame.
        //
        //   * The final relocation. The ping-pong alternated between the result surface and the
        //     variance scratch starting from the result surface, so at an even iteration count
        //     it finished in the scratch. Choosing the destinations by *parity from the end*
        //     instead of from the start lands the last iteration in the result surface for any
        //     count, which is what the relocation was for.
        //
        // The routing, for N iterations (N >= 2), with V = variance scratch, H = colour
        // history, R = the result surface the composite reads:
        //     variance pass -> V
        //     iteration 0   : V -> H          (H is now defect D4's first-iteration history)
        //     iteration i>=1: previous -> ((N-1-i) even ? R : V)
        // so the last iteration always writes R and each reads what the one before wrote:
        // N=2 gives V->H->R, N=3 V->H->V->R, N=4 V->H->R->V->R. At N=1 the single iteration is
        // both the final result and the first-iteration output, so it writes R and one copy to
        // H is still owed -- that is the one case the old code already handled minimally.
        //
        // This changes no shader source and no shader arithmetic: it is entirely a question of
        // which resource view is bound to which slot. The fxc output is expected to be
        // byte-identical, and the closeout notes record the baseline diff that proves it.
        //
        // One binding has to go with it. ssrt_spatial.hlsl and ssrt_variance.hlsl both declare
        // HistoryTexture at t0 and neither references it, so fxc strips it -- but at iteration
        // 0 that surface is now the dispatch's own destination, and binding a resource as SRV
        // and UAV in one dispatch is a state conflict D3D11 resolves by silently dropping one.
        // t0 is left unbound in both passes instead, which is what the shaders already assume.

        // variance filter
        uavs.at(0) = texVariance->uav.get();
        srvs.at(0) = nullptr;  // (S2.8) declared at t0, never referenced
        srvs.at(1) = texMoments->srv.get();
        srvs.at(2) = normal.SRV;
        srvs.at(3) = texTemporal->srv.get();
        srvs.at(4) = depth.depthSRV;

        context->CSSetShaderResources(0, 5, srvs.data());
        context->CSSetUnorderedAccessViews(0, 1, uavs.data(), nullptr);
        context->CSSetShader(varianceCS.get(), nullptr, 0);

        context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);
        resetViews();

        const int atrousN = (int)settings.AtrousIterations;
        ID3D11ShaderResourceView* atrousSrc = texVariance->srv.get();

        // spatial filter
        for (int i = 0; i < atrousN; ++i)
        {
            denoiserCBData.atrousIterations = i;
            denoiserCB->Update(denoiserCBData);
            denoiserBuffer = denoiserCB->CB();
            context->CSSetConstantBuffers(2, 1, &denoiserBuffer);

            Texture2D* dst = nullptr;
            if (i == 0)
                dst = (atrousN == 1) ? texSSRTDiffuseColor.get() : texHistoryDiffuse.get();
            else
                dst = (((atrousN - 1 - i) % 2) == 0) ? texSSRTDiffuseColor.get() : texVariance.get();

            uavs.at(0) = dst->uav.get();
            srvs.at(0) = nullptr;  // (S2.8) declared at t0, never referenced; and at i == 0 it is dst
            // (spec A1) t1 = moments; see the matching binding in DrawSSRTSpecular.
            srvs.at(1) = texMoments->srv.get();
            srvs.at(2) = normal.SRV;
            srvs.at(3) = atrousSrc;
            srvs.at(4) = depth.depthSRV;
            // (batch 1, item 2) The hit-distance surface, which the diffuse permutation declares
            // at t5 and the specular one does not declare at all. It is the ray march's own
            // output and is not part of the ping-pong, so the same binding serves every
            // iteration -- what changes per iteration is the hard radius it is judged against,
            // which comes from atrousIterations in the constant buffer.
            srvs.at(5) = texSSRTDiffuseHitDistance->srv.get();

            context->CSSetShaderResources(0, 6, srvs.data());
            context->CSSetUnorderedAccessViews(0, 1, uavs.data(), nullptr);
            context->CSSetShader(spatialCS.get(), nullptr, 0);

            context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);

            resetViews();

            atrousSrc = dst->srv.get();

            // (defect D4) The history is the *first* iteration's output, written straight into
            // texHistoryDiffuse at atrousN >= 2, so there is nothing left to copy.
            if (i == 0 && atrousN >= 2)
                historyFed = true;
        }

        // (S2.8) atrousN == 1 is the one case where the first iteration's output and the final
        // result are the same surface, so one copy is still owed.
        if (atrousN == 1 && SvgfHistoryNeeded(false) && texHistoryDiffuse) {
            CopyDynamicRegion(texHistoryDiffuse->resource.get(), texSSRTDiffuseColor->resource.get());
            historyFed = true;
        }

        // (S2.8) Moment ownership swap in place of the copy the temporal pass used to need.
        // Safe here and not earlier: the variance pass and every a-trous iteration read
        // texMoments, and all of them have now run.
        std::swap(texMoments, texHistoryMomentsDiffuse);

        Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::SSRTSvgf);
    } else if (denoiser == kDenoiserREBLUR) {
        // (batch C1) REBLUR_DIFFUSE: pack the raw 2-spp radiance plus the decoded
        // hit distance into NRD's front-end layout, dispatch the instance, unpack back
        // into texSSRTDiffuseColor for the diffuse composite below. historyFed stays
        // false, so the unconditional copy right after publishes the denoised radiance
        // as next frame's raymarch history.
        //
        // (S1.1) IN_DIFF_CONFIDENCE is no longer wired unconditionally — see
        // Settings::ReblurFeedHitCoverageConfidence.
        // (S1.2) A false return leaves texSSRTDiffuseColor holding the raw ray march.
        // (batch 36f, item 4) Folded unpack: the packed-input composite twin decodes it, and
        // only if that twin compiled (otherwise the separate unpack pass runs as before).
        const bool foldUnpack = FoldUnpackActive() && diffuseCompositePackedCS && diffuseCompositeExternalConfPackedCS;
        const bool dispatched = RunReblur(false, foldUnpack);
        if (foldUnpack)
            compositePackedSRV = (dispatched ? texNRDPackOutput : texNRDPackInput)->srv.get();
    }

    // (S2.5) Same as the specular twin: ssrt_temporal.hlsl at t0 plus the Buffer Viewer are
    // the whole consumer set for texHistoryDiffuse.
    if (!historyFed && SvgfHistoryNeeded(false) && texHistoryDiffuse)
        CopyDynamicRegion(texHistoryDiffuse->resource.get(), texSSRTDiffuseColor->resource.get());

    // (batch 6) The spatial confidence filter, in its own timing bucket so the user can confirm
    // that the wider kernel is genuinely cheaper than the 7x7 window it replaces rather than
    // taking that on trust. Placed here, after the denoiser and before the composite, for one
    // reason: its input is the raw ray-march confidence (which the denoiser never touches) and its
    // output is read by DeferredCompositeCS, which Deferred::DeferredPasses dispatches after this
    // whole function. Nothing between those two points reads a previous frame.
    if (confidenceFilter) {
        state->BeginPerfEvent("SSRT Confidence Filter");
        Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::SSRTConfidenceFilter);

        // The quarter-resolution grid, rounded up exactly as every shader in the chain rounds it,
        // and the dispatch that covers it.
        const uint loWidth = std::max(1u, ((uint)size.x + 1u) / 2u);
        const uint loHeight = std::max(1u, ((uint)size.y + 1u) / 2u);
        const uint loDispatchX = (loWidth + 7u) / 8u;
        const uint loDispatchY = (loHeight + 7u) / 8u;

        // Stage 1: 2x2 depth- and normal-aware average of the raw confidence, publishing the
        // low-resolution depth and normal guides the two stages after it are steered by.
        srvs.at(0) = texSSRTDiffuseConfidence->srv.get();
        srvs.at(1) = depth.depthSRV;
        srvs.at(2) = normal.SRV;
        uavs.at(0) = texSSRTConfidenceLo->uav.get();
        uavs.at(1) = texSSRTConfidenceLoDepth->uav.get();
        uavs.at(2) = texSSRTConfidenceLoNormal->uav.get();
        context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
        context->CSSetUnorderedAccessViews(0, 3, uavs.data(), nullptr);
        context->CSSetShader(confDownsampleCS.get(), nullptr, 0);
        {
            Util::DenoiserTimerScope timing("Confidence", "Downsample", loDispatchX, loDispatchY, loDispatchX * 8u, loDispatchY * 8u);
            context->Dispatch(loDispatchX, loDispatchY, 1);
        }
        resetViews();

        // Stage 2: the separable pair. Horizontal reads the downsample's output and writes the
        // scratch surface; vertical reads that back and returns to the first, so the finished field
        // ends up in texSSRTConfidenceLo whichever way round the pair is inspected. A separable
        // pass cannot run in place, which is the only reason there are two surfaces.
        const auto blurPass = [&](ID3D11ComputeShader* a_shader, Texture2D* a_source, Texture2D* a_target, const char* a_timingName) {
            srvs.at(0) = a_source->srv.get();
            srvs.at(1) = texSSRTConfidenceLoDepth->srv.get();
            srvs.at(3) = texSSRTConfidenceLoNormal->srv.get();
            uavs.at(0) = a_target->uav.get();
            context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
            context->CSSetUnorderedAccessViews(0, 1, uavs.data(), nullptr);
            context->CSSetShader(a_shader, nullptr, 0);
            {
                Util::DenoiserTimerScope timing("Confidence", a_timingName, loDispatchX, loDispatchY, loDispatchX * 8u, loDispatchY * 8u);
                context->Dispatch(loDispatchX, loDispatchY, 1);
            }
            resetViews();
        };
        blurPass(confBlurHorizontalCS.get(), texSSRTConfidenceLo.get(), texSSRTConfidenceLoBlur.get(), "Blur horizontal");
        blurPass(confBlurVerticalCS.get(), texSSRTConfidenceLoBlur.get(), texSSRTConfidenceLo.get(), "Blur vertical");

        // Stage 3: joint bilateral upsample onto texSSRTDiffuseConfidenceSmooth -- the same surface
        // the 7x7 window used to write, with the same semantics and the same written region, so
        // DeferredCompositeCS needs no change. The raw confidence at t0 is the fallback a pixel
        // with no geometrically valid low-resolution neighbour publishes; see the degeneracy rule
        // in ssrt_conf_upsample.hlsl.
        srvs.at(0) = texSSRTDiffuseConfidence->srv.get();
        srvs.at(1) = depth.depthSRV;
        srvs.at(2) = normal.SRV;
        srvs.at(3) = texSSRTConfidenceLo->srv.get();
        srvs.at(4) = texSSRTConfidenceLoDepth->srv.get();
        srvs.at(5) = texSSRTConfidenceLoNormal->srv.get();
        uavs.at(0) = texSSRTDiffuseConfidenceSmooth->uav.get();
        context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
        context->CSSetUnorderedAccessViews(0, 1, uavs.data(), nullptr);
        context->CSSetShader(confUpsampleCS.get(), nullptr, 0);
        {
            Util::DenoiserTimerScope timing("Confidence", "Upsample",
                (uint)dispatchCount.x, (uint)dispatchCount.y, (uint)dispatchCount.x * 8u, (uint)dispatchCount.y * 8u);
            context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);
        }
        resetViews();

        Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::SSRTConfidenceFilter);
        state->EndPerfEvent();
    }

    // composite
    Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::SSRTComposite);
    {
        uavs.at(0) = main.UAV;
        srvs.at(0) = compositePackedSRV ? compositePackedSRV : texSSRTDiffuseColor->srv.get();
        srvs.at(1) = albedo.SRV;
        // (batch 36f) Depth at t4 on every permutation: the distance limit reads it in the
        // external-filter one too (the other permutation already had it here for its smoothing).
        srvs.at(4) = depth.depthSRV;

        uint uavCount = 1;
        if (!confidenceFilter) {
            // (ambient reinjection) The pass doubles as the confidence smoothing filter: it reads
            // the raw surface at t3 and publishes the depth-aware 7x7 mean at u1 for
            // DeferredCompositeCS, which Deferred::DeferredPasses dispatches after this one. Folded
            // in here rather than given its own dispatch because this pass is three texture reads
            // of otherwise idle ALU, and because it keeps the change out of the pass schedule.
            //
            // (batch 6) ...and none of that is true any more once the filter above runs, which is
            // why this whole half is now conditional. The SSRT_CONF_EXTERNAL_FILTER permutation has
            // the block, its LDS tile and its barrier compiled out, so these bindings would have no
            // reader; leaving u1 bound in particular would be actively misleading, since the
            // surface it points at was written moments ago by stage 3.
            uavs.at(1) = texSSRTDiffuseConfidenceSmooth->uav.get();
            srvs.at(3) = texSSRTDiffuseConfidence->srv.get();
            srvs.at(4) = depth.depthSRV;
            uavCount = 2;

            // (reinjection noise) The confidence accumulator's extra ends. Bound only when it runs,
            // so with it off the compiled binding table is reached exactly as it was before: the
            // shader's branch is a constant-buffer test, and an unbound SRV reads zero while an
            // unbound UAV write is a no-op, neither of which the spatial-only path performs.
            if (confidenceTemporal) {
                srvs.at(5) = texSSRTConfidenceHistoryPrev->srv.get();
                srvs.at(6) = motion.SRV;
                uavs.at(2) = texSSRTConfidenceHistory->uav.get();
                uavCount = 3;
            }
        }

        // b1 is set at the top of this function and only b2 is written after it, so this rebind
        // is belt and braces rather than a fix -- but the accumulator now reads b1 from a pass
        // that never used to, and a future pass inserted between the two would break silently.
        context->CSSetConstantBuffers(1, 1, &buffer);
        context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
        context->CSSetUnorderedAccessViews(0, uavCount, uavs.data(), nullptr);
        ID3D11ComputeShader* compositeShader = confidenceFilter ? diffuseCompositeExternalConfCS.get() : diffuseCompositeCS.get();
        if (compositePackedSRV)
            compositeShader = confidenceFilter ? diffuseCompositeExternalConfPackedCS.get() : diffuseCompositePackedCS.get();
        context->CSSetShader(compositeShader, nullptr, 0);

        context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);

        resetViews();

        // (reinjection noise) This frame's accumulator becomes next frame's history. A pointer
        // swap, matching the moment pair at the end of the SVGF block, so no copy is issued.
        if (confidenceTemporal)
            std::swap(texSSRTConfidenceHistory, texSSRTConfidenceHistoryPrev);
    }
    Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::SSRTComposite);

    // (audit #13) Only when specular will not run afterwards, so the snapshot still
    // happens exactly once per frame and after every temporal pass has read it.
    if (!settings.EnableSpecular)
        CopyHistoryGeometry();

    state->EndPerfEvent();

    context->CSSetShader(nullptr, nullptr, 0);
}

ScreenSpaceRayTracing::DenoiserCB ScreenSpaceRayTracing::GetDenoiserCBData(bool a_isDiffuseChain) const
{
    DenoiserCB data;
    data.invMaxAccumulatedFrames = 1.0f / (settings.MaxAccumulatedFrames + 1.0f);
    data.atrousIterations = settings.AtrousIterations;
    data.colorPhi = settings.ColorPhi;
    data.normalPhi = settings.NormalPhi;
    data.adaptiveFiltering = settings.AdaptiveFiltering ? 1u : 0u;
    data.adaptiveHistoryThreshold = (float)settings.AdaptiveHistoryThreshold;
    data.adaptiveVarianceEps = settings.AdaptiveVarianceEps;
    // (spec S1) The bool collapses into the strength: 0 sigmas is the off state the
    // shader tests, so ssrt_temporal.hlsl needs a single group-uniform predicate rather
    // than two.
    data.fireflyClampSigma = settings.FireflyClamp ? settings.FireflyClampSigma : 0.0f;
    // (spec S3) Only the SSRT_SPECULAR permutation reads this, so DrawSSRTDiffuse simply
    // passes a value nothing looks at.
    data.specularRoughnessCutoff = settings.SpecularDenoiseRoughnessCutoff;
    // (defect D1) Shared by both chains: ssrt_temporal.hlsl is a single permutation, so the
    // diffuse and specular temporal passes necessarily see the same width. That is the right
    // default -- the box is built from each pass's own input, so it self-scales, and on a
    // near-mirror the neighbourhood's spatial sigma is large enough that the clamp is
    // effectively inert without needing to be switched off.
    data.historyClampSigma = settings.HistoryClampSigma;
    // (diagnostic D3) Shared by both chains for the same reason historyClampSigma is: one
    // permutation, one buffer, and isolating the mechanism means isolating it everywhere.
    data.disableHistoryDepthTest = settings.DisableHistoryDepthTest ? 1u : 0u;
    // (diagnostic H) The remaining three gate switches are shared by both chains for the same
    // reason the two above are: one permutation, one buffer, and isolating a mechanism means
    // isolating it everywhere. Anything else would leave the specular chain accumulating under
    // a different predicate than the one being measured.
    data.disableHistoryNormalTest = settings.DisableHistoryNormalTest ? 1u : 0u;
    data.forceAcceptHistory = settings.ForceAcceptHistory ? 1u : 0u;
    data.rotatedNormalGate = settings.RotatedNormalGate ? 1u : 0u;
    // (diagnostic H) The one field that is *not* shared. Both chains run the same shader and
    // both have texDebugHistory bound, so without this the specular pass -- which runs after
    // diffuse -- would overwrite the diffuse picture with its own every frame, and the view
    // would silently show whichever chain happened to be enabled last.
    data.historyDebugView = (a_isDiffuseChain && settings.HistoryDebugView) ? 1u : 0u;
    // (batch 1, item 2) Read only by the diffuse permutation of ssrt_spatial.hlsl -- the
    // specular one does not declare the field's surface and compiles the window away -- so the
    // value is passed unconditionally and the specular chain simply ignores it, exactly as it
    // does with specularRoughnessCutoff in reverse.
    data.hitRadiusStrength = settings.HitRadiusStrength;
    return data;
}

ScreenSpaceRayTracing::SharedData ScreenSpaceRayTracing::GetCommonBufferData()
{
    SharedData data;
    data.EnableSpecular = settings.EnableSpecular;
    data.SpecularMult = settings.SpecularMult;
    data.DiffuseMult = settings.EnableDiffuse ? settings.DiffuseMult : 0.0f;
    // (ambient reinjection) Gated on EnableDiffuse for the same reason DiffuseMult is: every
    // consumer keys off `DiffuseMult > 0`, and a reinjection flag left set while the diffuse
    // pass is not running would have DeferredCompositeCS reading a stale confidence surface.
    data.AmbientReinjection = (settings.EnableDiffuse && settings.EnableAmbientReinjection) ? 1u : 0u;
    data.AmbientReinjectionStrength = settings.AmbientReinjectionStrength;
    // (ambient reinjection) The forward ambient has to be present at full strength for the
    // composite's reconstruction to be faithful -- it corrects only the LUMINANCE of its
    // estimate from Masks.z, and AmbientMult scales exactly that half, so any value other than
    // 1 leaves the chroma over-saturated relative to the luminance and the removal deposits a
    // tint. Pinned here rather than by changing the default, so an existing configuration that
    // saved AmbientMult = 0 still reproduces the old look bit for bit the moment the
    // reinjection toggle comes off.
    data.AmbientMult = data.AmbientReinjection != 0u ? 1.0f : settings.AmbientMult;
    // (contact AO) Screen Space GI's contact pass, described for the ray march's benefit. This is
    // the only thing SSRT needs to know about it: ssrt_raymarch.hlsl suppresses the near-field
    // per-ray occlusion vote only while a deterministic term is standing in for it, and the term
    // reaches the march through the SSGI AO texture, not through this buffer.
    //
    // Every condition under which the term does not reach the frame has to clear this, or the ray
    // march suppresses its own near-field darkening and nothing replaces it -- which would be
    // strictly worse than the flicker the suppression exists to remove. So: the package present,
    // the feature on, its contact pass actually dispatching, *and* its shaders compiled --
    // ShadersOK() is the same test DrawSSGI uses before it decides to clear the AO output instead
    // of writing it.
    //
    // (P2.4 follow-up) contactAoActive rather than settings.EnableContactAo, because the setting
    // is the request and this is what the pass is doing. It is the same predicate DrawSSGI
    // dispatches on, so this flag and the texture's contents cannot disagree.
    auto& ssgi = globals::features::screenSpaceGI;
    const bool ssgiContactLive =
        ssgi.loaded && ssgi.settings.Enabled && ssgi.contactAoActive && ssgi.ShadersOK();
    data.SsgiContactAoActive = ssgiContactLive ? 1u : 0u;
    data.SsgiContactRadius = ssgi.settings.ContactRadius;
    return data;
}
