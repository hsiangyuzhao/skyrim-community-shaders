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
    EnableDiffuse,
    SpecularMult,
    DiffuseMult,
    AmbientMult,
    EnableAmbientReinjection,
    AmbientReinjectionStrength,
    OcclusionStrength,
    CubemapNormalization,
    EnableSVGF,
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
    HistoryClampSigma,
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
    EnableDiffuse,
    SpecularMult,
    DiffuseMult,
    AmbientMult,
    EnableAmbientReinjection,
    AmbientReinjectionStrength,
    OcclusionStrength,
    CubemapNormalization,
    EnableSVGF,
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
    HistoryClampSigma,
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
    ImGui::SameLine();
    ImGui::Checkbox("Enable Diffuse", &settings.EnableDiffuse);
    ImGui::SliderInt("Max Steps", (int*)&settings.MaxSteps, 1, 256);
    // (audit P3) The traversal can load exactly mip SSRTCB::MaxMips, so the highest
    // legal setting is maxMips - 1; the old bound of maxMips let the ray sample a mip
    // that does not exist, and an out-of-range Load returns 0 == near plane, i.e. an
    // instant false hit.
    ImGui::SliderInt("Max Mip Level", (int*)&settings.MaxMips, 1, maxMips - 1, "%d", ImGuiSliderFlags_AlwaysClamp);
    recompileFlag |= ImGui::SliderInt("Diffuse SPP", (int*)&settings.DiffuseSPP, 1, 16, "%d", ImGuiSliderFlags_AlwaysClamp);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Samples per pixel for diffuse component. Higher values reduce noise but impact performance.");
    ImGui::SliderFloat("Specular Multiplier", &settings.SpecularMult, 0.0f, 5.0f, "%.2f");
    ImGui::SliderFloat("Diffuse Multiplier", &settings.DiffuseMult, 0.01f, 5.0f, "%.2f");
    ImGui::SliderFloat("Occlusion Strength", &settings.OcclusionStrength, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "How strongly a ray that ran into the back of geometry darkens the fallback "
            "ambient for that pixel. Rays that simply failed to trace no longer count "
            "towards this.\n\n"
            "While Screen Space GI's Contact AO is on, this only counts geometry further away "
            "than that setting's radius. Anything closer is handed to Contact AO instead, "
            "because two rays per pixel cannot decide how dark a tight contact is without the "
            "answer changing every frame. The two never darken the same geometry twice.\n\n"
            "With Screen Space GI or its Contact AO switched off, the rays go back to deciding "
            "the near field themselves -- flickery, but never missing.");
    ImGui::BeginDisabled(settings.EnableAmbientReinjection);
    ImGui::SliderFloat("Ambient Multiplier", &settings.AmbientMult, 0.0f, 1.0f, "%.2f");
    ImGui::EndDisabled();
    if (auto _tt = Util::HoverTooltipWrapper()) {
        if (settings.EnableAmbientReinjection)
            ImGui::Text(
                "Pinned to 1 while Ambient Reinjection is on: that mode needs the full vanilla "
                "ambient in the frame so the composite can take back exactly the part the rays "
                "resolved. Turn Ambient Reinjection off to edit this again.");
        else
            ImGui::Text("Mix diffuse with vanilla ambient color. Not suggested if using dynamic cubemaps as fallback.");
    }

    ImGui::SeparatorText("Ambient Energy");

    ImGui::Checkbox("Ambient Reinjection", &settings.EnableAmbientReinjection);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Keeps the vanilla ambient light in the frame and lets the traced result displace "
            "it in proportion to how much geometry the rays actually found, instead of zeroing "
            "the ambient and relying on the cubemap fallback to stand in for it. Each pixel "
            "ends up at a blend of vanilla ambient and traced radiance weighted by hit "
            "confidence.\n\n"
            "Why it matters. A screen-space ray can only bring back light from a surface that "
            "is on screen, inside the hemisphere and lit; sky, off-screen and unlit hits carry "
            "nothing. With the ambient zeroed there is nothing underneath that, which is why "
            "the frame collapses when the fallback is switched off. It also removes most of the "
            "unfiltered sampling noise reaching the upscaler, because the noisy term is now "
            "scaled by confidence and the low-confidence regions -- open ground, sky-facing "
            "surfaces -- rest on a perfectly stable ambient instead.\n\n"
            "Forces the diffuse cubemap fallback off, since the two are competing answers to "
            "the same question and running both would count the environment twice. The "
            "specular fallback is unaffected.\n\n"
            "Turn it off to get the previous behaviour back exactly, for an A/B comparison.");

    if (settings.EnableAmbientReinjection) {
        ImGui::SliderFloat("Reinjection Strength", &settings.AmbientReinjectionStrength, 0.0f, 1.0f, "%.2f");
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "How much of the hit confidence is allowed to displace the vanilla ambient. 1 "
                "conserves energy: a fully confident pixel is pure traced radiance. 0 keeps the "
                "whole ambient and adds the traced light on top, which is brighter than the "
                "truth but never darker than vanilla. Slightly below 1 is a reasonable hedge in "
                "scenes whose on-screen surfaces are not representative of the surrounding "
                "environment.");
    }

    ImGui::Separator();

    // (spec F1 / audit #3) Range recalibrated to the parameter's actual unit -- game
    // units of depth-buffer thickness, not the 0-50 window that only ever made sense
    // against the old mip-1 validation. Logarithmic so the useful 10-60 region is still
    // draggable at a 500-unit top end.
    ImGui::SliderFloat("Thickness", &settings.Thickness, 0.0f, 500.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "How far behind the validated surface, in game units (1 unit ~ 1.4 cm), a hit "
            "still counts. Too low and grazing ground loses all confidence and falls back "
            "to the cubemap; too high and light leaks through thin geometry and specular "
            "reflections stretch behind silhouettes. The shader adds roughness * 10.");
    ImGui::SliderFloat("Normal Bias", &settings.NormalBias, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Pushes the ray origin off its own surface to avoid false self-hits, scaled "
            "with distance and with the grazing angle. Raising it also makes rays miss "
            "genuinely nearby geometry, so contact shading around hair and foliage gets "
            "weaker as this goes up.");
    ImGui::SliderFloat("BRDF Bias", &settings.BRDFBias, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Specular only. Higher BRDF bias reduces noise but makes reflections more glossy.");
    ImGui::BeginDisabled(settings.EnableAmbientReinjection);
    ImGui::Checkbox("Use Dynamic Cubemaps as Fallback for Diffuse", &settings.UseDynamicCubemapsAsFallback);
    ImGui::EndDisabled();
    if (auto _tt = Util::HoverTooltipWrapper()) {
        if (settings.EnableAmbientReinjection)
            ImGui::Text(
                "Forced off while Ambient Reinjection is on: the vanilla ambient is what fills "
                "the missed directions in that mode, and letting the cubemap fill them as well "
                "would count the same environment light twice. The saved value is kept and "
                "comes back when Ambient Reinjection is turned off.");
        else
            ImGui::Text("When ray marching misses, use dynamic cubemaps for reflections.");
    }
    ImGui::Checkbox("Use Dynamic Cubemaps as Fallback for Specular", &settings.UseDynamicCubemapsAsFallbackSpecular);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("When ray marching misses, use dynamic cubemaps for reflections. Recommended for specular.");
    ImGui::SliderFloat("Cubemap Normalization", &settings.CubemapNormalization, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Matches cubemap luminance with ambient color.");

    ImGui::Separator();

    ImGui::Checkbox("Enable Spatiotemporal Variance-Guided Filtering", &settings.EnableSVGF);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("SVGF denoiser. This may introduce some blurriness and temporal artifacts but significantly reduces noise.");
    if (settings.EnableSVGF) {
        ImGui::Checkbox("Pre-Blur", &settings.EnablePreBlur);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Diffuse only. Runs one small, very gentle smoothing step on the raw rays "
                "*before* the frame-to-frame averaging looks at them, instead of only "
                "afterwards.\n\n"
                "Why it matters: the frame-to-frame average writes whatever it is given into a "
                "buffer that the next frame reads back and averages again. Hand it a noisy "
                "picture and the noise goes into that buffer, and every later decision -- how "
                "much of the old frame to trust, how hard to smooth -- has to be made through "
                "it. Taking the worst of the noise off first makes all of those decisions "
                "better at once. Every commercial denoiser does this; ours was the odd one "
                "out.\n\n"
                "It is deliberately weak -- about a third of the blur one A Trous pass applies "
                "-- because its job is to cut the extreme pixels, not to make things look "
                "smooth. Expect a stationary shot to settle faster and cleaner, and moving "
                "shots to stop dragging noise into the following seconds. Contact shadows and "
                "creases should not soften; if they do, turn this off and say so.\n\n"
                "The single-bright-pixel cleanup (Firefly Clamp) moves into this step while it "
                "is on, so it still happens exactly once. With Firefly Clamp off, this step has "
                "nothing stopping it from smearing a stray bright pixel across nine, so the two "
                "are best left on together.");
        ImGui::SliderInt("Max Accumulated Frames", (int*)&settings.MaxAccumulatedFrames, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderInt("À Trous Iterations", (int*)&settings.AtrousIterations, 1, 5, "%d", ImGuiSliderFlags_AlwaysClamp);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text("Number of À Trous wavelet filter iterations. More iterations yield smoother results but may blur details and have a higher computational cost.");
        ImGui::SliderFloat("Color Phi", &settings.ColorPhi, 0.01f, 32.0f, "%.2f");
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "How many standard deviations of luminance difference a neighbouring pixel "
                "may have before the A Trous filter rejects it. A tap about 1 sigma away "
                "differs by noise and should be averaged in; one 4 sigma away is a real "
                "edge and should be rejected. At the default 2.0 those keep 61%% and 13%% of "
                "their weight. Lower preserves more detail but retains noise -- below about "
                "1.0 the filter starts treating its own noise as detail and stops averaging "
                "at all. The SVGF paper uses 4.0, which is too loose for this pipeline's "
                "shorter kernel chain.");
        ImGui::SliderFloat("Normal Phi", &settings.NormalPhi, 1.0f, 1024.0f, "%.2f");
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "How closely a neighbouring pixel's normal must match before the A Trous "
                "filter will average it in. The weight is dot(n, nP) raised to this power, "
                "so it is an angle: at the default 128 a tap keeps 1/e of its weight at "
                "7.2 degrees and 2%% at 14. Raising it preserves detail the indirect light "
                "does not actually carry, and above about 256 the filter stops averaging on "
                "any normal-mapped surface -- which is most of Skyrim -- so extra iterations "
                "buy nothing.");

        ImGui::SliderFloat("Hit Distance Kernel Strength", &settings.HitRadiusStrength, 0.0f, 8.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Diffuse only. Makes the A Trous filter smooth less where the rays hit "
                "something close by, and keep smoothing at full width where they went far or "
                "missed entirely.\n\n"
                "Why: light bouncing off a wall a foot away changes over a foot. Two pixels "
                "further apart than that are lit by different things, so averaging them is a "
                "blur, not a denoise -- that is contact shading and corner darkening getting "
                "washed out. Light that came from the sky or from far away changes over "
                "hundreds of feet, so there the widest possible average is both safe and "
                "exactly where the leftover noise is. Until now the filter used the same width "
                "for both.\n\n"
                "At the default 4 a contact pixel gets about 60%% of the filter width and a "
                "distant or missed one gets 100%% of it, unchanged to the last bit. It can only "
                "ever narrow, never widen, and it never narrows to nothing -- the nearest ring "
                "of neighbours always keeps at least a third of its weight, because contact "
                "pixels are just as noisy as everything else. Raise it if contact shading still "
                "looks washed out; 0 turns the whole thing off.");

        ImGui::Checkbox("Firefly Clamp", &settings.FireflyClamp);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Clamps single-pixel radiance outliers against their 3x3 neighbourhood "
                "before the temporal accumulation sees them. Fireflies are the one artefact "
                "the A Trous filter makes worse rather than better -- it spreads them into "
                "slowly fading blobs -- so this is what keeps a low iteration count safe. "
                "Turn off for a bit-exact classic SVGF temporal pass.");
        if (settings.FireflyClamp) {
            ImGui::SliderFloat("Firefly Clamp Sigma", &settings.FireflyClampSigma, 1.0f, 8.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            if (auto _tt = Util::HoverTooltipWrapper())
                ImGui::Text(
                    "Standard deviations above the neighbourhood mean a pixel may reach before "
                    "it counts as a firefly. Below 2.65 the clamp starts reaching values a "
                    "neighbour also produced, i.e. real signal; higher values only catch the "
                    "most extreme spikes.");
        }

        ImGui::SliderFloat("History Clamp Sigma", &settings.HistoryClampSigma, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "How far, in standard deviations, the reprojected history may sit outside "
                "what this frame's 3x3 neighbourhood says the radiance can be. This is what "
                "stops a moving object dragging stale lighting behind it: without it the "
                "accumulation happily blends in history that is geometrically plausible but "
                "radiometrically wrong, and the streak then takes Max Accumulated Frames to "
                "fade.\n\n"
                "At the default 1.0 a converged still image is untouched -- the clamp engages "
                "on well under 1%% of pixels per frame -- while a ghost, whose error is of the "
                "order of the local brightness itself, is cut on its first frame. Lower "
                "shortens trails further but starts pulling the history back towards a "
                "nine-sample mean and feeding that mean's noise into it, so below about 0.75 "
                "you are trading convergence for motion. 0 disables the clamp.\n\n"
                "0 is also the diagnostic bypass for this mechanism: the shader tests the "
                "value itself, so 0 skips the clamp and its neighbourhood prefetch entirely. "
                "Pair it with Disable History Depth Test under Debug to isolate the two "
                "temporal mechanisms one at a time.");

        ImGui::Checkbox("Adaptive Filtering", &settings.AdaptiveFiltering);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Lets an 8x8 tile whose pixels have all converged skip an À Trous iteration, "
                "spending the filter only where the temporal accumulation is still noisy "
                "(motion, disocclusion). Turn off for a bit-exact classic SVGF.");
        if (settings.AdaptiveFiltering) {
            ImGui::SliderInt("Adaptive History Threshold", (int*)&settings.AdaptiveHistoryThreshold, 4, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
            if (auto _tt = Util::HoverTooltipWrapper())
                ImGui::Text("Accumulated frames a pixel needs before it may count as converged. Matching Max Accumulated Frames is a good default.");
            ImGui::SliderFloat("Adaptive Variance Threshold (relative)", &settings.AdaptiveVarianceEps, 1e-4f, 1.0f, "%.5f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
            if (auto _tt = Util::HoverTooltipWrapper())
                ImGui::Text(
                    "Relative luminance variance below which a pixel counts as converged, "
                    "measured against the pixel's own brightness (a squared coefficient of "
                    "variation). The default 0.013 is a per-frame noise level of about 11%% "
                    "of local brightness, which the temporal accumulation reduces to about "
                    "2%% in the image -- the point where it stops being visible. Higher "
                    "values skip more tiles at the cost of residual noise. Measuring this "
                    "relative to brightness rather than absolutely is what keeps it a "
                    "convergence test instead of a \"is this pixel dark\" test.");
        }

        if (settings.EnableSpecular) {
            ImGui::SliderFloat("Specular Mirror Cutoff", &settings.SpecularDenoiseRoughnessCutoff, 0.0f, 0.25f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
            if (auto _tt = Util::HoverTooltipWrapper())
                ImGui::Text(
                    "Roughness at or below which an 8x8 tile of specular pixels skips the A Trous "
                    "kernel entirely. On a near-mirror the filter already discards every neighbour "
                    "-- that is what the roughness scaling of Color Phi and Normal Phi is for -- so "
                    "it computes the pixel it was handed. Skipping it removes the cost of water, "
                    "glass and polished metal without changing what they look like. 0 disables.");
        }
    }
#ifdef ENABLE_SHARC
    ImGui::Checkbox("(Broken) Enable SHARC", &settings.EnableSharc);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("(Experimental) Enables Spatially Hashed Radiance Cache (SHARC) to improve diffuse quality. This requires more memory and might impact performance.");
#endif
    ImGui::SeparatorText("Debug");

    ImGui::Checkbox("Freeze Noise Phase", &settings.FreezeNoisePhase);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Diagnostic. Not for normal play.\n\n"
            "Freezes the per-frame phase of the ray-direction noise, so every frame traces "
            "the same sample directions instead of a fresh set. Use it to tell two causes of "
            "directional smearing apart: smearing produced by the upscaler clamping a "
            "changing stochastic signal along motion disappears when the phase is frozen, "
            "while smearing produced by the denoiser's own temporal reprojection survives "
            "unchanged.\n\n"
            "Leaving this on locks the sampling noise into a fixed screen-space pattern that "
            "no amount of accumulation can average away.");

    ImGui::Checkbox("Disable History Depth Test", &settings.DisableHistoryDepthTest);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Diagnostic. Not for normal play. Requires Enable SVGF.\n\n"
            "Switches off the geometric disocclusion test the temporal pass applies to every "
            "history candidate -- the check that the surface point which occupied that history "
            "texel last frame still lies in the plane of the surface being shaded now. "
            "Everything else stays on: screen bounds, the 30 degree normal agreement and the "
            "non-finite rejection, so the accumulation behaves exactly as it did before that "
            "test existed.\n\n"
            "Use it together with History Clamp Sigma 0 (which is the off switch for the "
            "neighbourhood history clamp) to isolate the two mechanisms one at a time. Both "
            "produce the same complaint -- \"the denoiser is not denoising, and there is no "
            "ghosting either\" -- because both end with the pixel taking this frame's sample "
            "whole, so only turning them off separately says which one is responsible.\n\n"
            "With this off the accumulation will read history across depth discontinuities "
            "again, i.e. the ghosting it was added to remove comes back. With the test on and "
            "working, turning it off should now change very little: that is the check that the "
            "plane criterion is accepting history instead of rejecting all of it.");

    ImGui::Checkbox("Disable History Normal Test", &settings.DisableHistoryNormalTest);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Diagnostic. Not for normal play. Requires Enable SVGF.\n\n"
            "Switches off the other geometric check the temporal pass applies to a history "
            "candidate: that the surface facing this way last frame is still facing roughly "
            "the same way now, within 30 degrees. Everything else stays on.\n\n"
            "This is the partner of the switch above. Both checks produce the same complaint "
            "when they go wrong -- the picture stays noisy and nothing accumulates -- so the "
            "only way to tell them apart is to turn them off one at a time. Use History Debug "
            "View to see which one to reach for first.");

    ImGui::Checkbox("Force Accept History", &settings.ForceAcceptHistory);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Diagnostic. Not for normal play. Requires Enable SVGF.\n\n"
            "Takes whatever the motion vector points at, with no geometric checking at all. "
            "Only three things can still turn a candidate away: it is off screen, it contains "
            "a corrupt number, or it has nothing accumulated in it yet.\n\n"
            "This is the last-resort test. The two switches above can each show that one check "
            "is the thing blocking accumulation, but neither can show that the checks are the "
            "*only* thing blocking it. If the picture still refuses to settle down with this "
            "on, the problem is somewhere else entirely and the checks were never the "
            "culprit.\n\n"
            "Expect heavy smearing while it is on -- that is the point. Nothing is stopping the "
            "filter from dragging lighting off a wall onto whatever walks in front of it.");

    ImGui::Checkbox("Rotated Normal Gate", &settings.RotatedNormalGate);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Diagnostic. Requires Enable SVGF.\n\n"
            "Changes which version of the 30 degree facing check runs.\n\n"
            "Off (the default) is the version that has been shipping for months. It compares "
            "surface directions without correcting for the camera having turned between the two "
            "frames, so it is slightly too strict during fast turns and fine the rest of the "
            "time. Well understood, mild when it misbehaves.\n\n"
            "On is the mathematically correct version, which corrects for that camera turn. It "
            "should be strictly better -- but only if an assumption about the game's own camera "
            "matrices holds, and that cannot be checked outside the game.\n\n"
            "That assumption is now checked at runtime, every frame, per pixel: the correction has "
            "to leave a surface direction the same length it started, which nothing but a genuine "
            "camera rotation does. Where the check fails the uncorrected version runs instead and "
            "History Debug View paints the pixel magenta, so this switch can no longer take the "
            "screen down with it -- the worst it can do now is quietly do nothing.\n\n"
            "So it is off by default and this switch is how it gets proven. Turn it on with "
            "History Debug View also on: no magenta means the correction is sound and safe to "
            "adopt as the default; magenta everywhere means the assumption is wrong and off is "
            "right.");

    ImGui::Checkbox("History Debug View", &settings.HistoryDebugView);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Diagnostic. Requires Enable SVGF and Enable Diffuse.\n\n"
            "Paints a picture of what the denoiser decided about every pixel's history this "
            "frame, into texDebugHistory under Buffer Viewer below. It costs nothing while it "
            "is off and it does not change what you see on screen either way.\n\n"
            "Reading it:\n"
            "  Grey, getting brighter over a second or two -- working. Brightness is how many "
            "frames have been averaged together; white means fully settled.\n"
            "  Red -- history thrown away by the depth/plane check.\n"
            "  Green -- history thrown away by the 30 degree facing check.\n"
            "  Blue -- history thrown away for being off screen, corrupt, or empty.\n"
            "  Yellow or orange -- the plane check could not be set up for that pixel, so it was "
            "skipped and the history was judged on facing and bounds alone. Any yellow means "
            "\"test not run\", not \"history rejected\". The four shades say why: pale yellow -- "
            "the point has no place in last frame's view at all; orange -- last frame's camera "
            "cannot see it; dark orange -- the surface is exactly edge-on; lemon -- the tolerance "
            "came out nonsense.\n"
            "  Magenta -- only possible with Rotated Normal Gate on: the camera-turn correction "
            "failed its own sanity check, so the uncorrected facing check ran instead.\n"
            "  Black -- sky, or nothing to shade.\n\n"
            "A few coloured pixels along edges and around moving things is normal and correct. "
            "One flat colour covering the whole screen is the fault: it means that one check is "
            "rejecting everything, everywhere, which leaves the denoiser doing nothing at all. "
            "The colour tells you which switch above to reach for.\n\n"
            "Shows the diffuse pass only. Specular shares the same buffer and deliberately "
            "leaves it alone.");

	if (ImGui::TreeNode("Buffer Viewer")) {
		static float debugRescale = .3f;
		ImGui::SliderFloat("View Resize", &debugRescale, 0.f, 1.f);

		BUFFER_VIEWER_NODE(texDepth, debugRescale)
        BUFFER_VIEWER_NODE(texColor, debugRescale)
        BUFFER_VIEWER_NODE(texSSRColor, debugRescale)
        BUFFER_VIEWER_NODE(texSSRTDiffuseColor, debugRescale)
        BUFFER_VIEWER_NODE(texSSRTDiffuseConfidence, debugRescale)
        BUFFER_VIEWER_NODE(texSSRTDiffuseConfidenceSmooth, debugRescale)
        // (batch 1, item 2) Black = the rays hit something within a texel or two, so the kernel
        // collapses towards the centre and contact detail survives. White = they went further
        // than the kernel reaches, or missed, so the kernel runs at full width. A healthy
        // exterior reads mostly white with dark outlines around contacts, creases and foliage.
        BUFFER_VIEWER_NODE(texSSRTDiffuseHitDistance, debugRescale)
        BUFFER_VIEWER_NODE(texHistory, debugRescale)
        BUFFER_VIEWER_NODE(texHistoryDiffuse, debugRescale)
        BUFFER_VIEWER_NODE(texTemporal, debugRescale)
        BUFFER_VIEWER_NODE(texMoments, debugRescale)
        BUFFER_VIEWER_NODE(texHistoryMoments, debugRescale)
        BUFFER_VIEWER_NODE(texHistoryMomentsDiffuse, debugRescale)
        BUFFER_VIEWER_NODE(texVariance, debugRescale)
        BUFFER_VIEWER_NODE(texHistoryDepth, debugRescale)
        BUFFER_VIEWER_NODE(texDebugHistory, debugRescale)

		ImGui::TreePop();
	}

    JiayeStatement::GetSingleton()->DrawJSInfo();
}

void ScreenSpaceRayTracing::RestoreDefaultSettings()
{
    settings = {};
}

void ScreenSpaceRayTracing::LoadSettings(json& o_json)
{
    settings = o_json;
}

void ScreenSpaceRayTracing::SaveSettings(json& o_json)
{
    o_json = settings;
}

void ScreenSpaceRayTracing::SetupResources()
{
    auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	logger::debug("Creating buffers...");
	{
        ssrtCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<SSRTCB>());
        denoiserCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<DenoiserCB>());
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
        texHistory = eastl::make_unique<Texture2D>(texDesc);
        texHistory->CreateSRV(srvDesc);
        texHistory->CreateUAV(uavDesc);
        texHistoryDiffuse = eastl::make_unique<Texture2D>(texDesc);
        texHistoryDiffuse->CreateSRV(srvDesc);
        texHistoryDiffuse->CreateUAV(uavDesc);
        texTemporal = eastl::make_unique<Texture2D>(texDesc);
        texTemporal->CreateSRV(srvDesc);
        texTemporal->CreateUAV(uavDesc);
        texVariance = eastl::make_unique<Texture2D>(texDesc);
        texVariance->CreateSRV(srvDesc);
        texVariance->CreateUAV(uavDesc);

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
        texMoments = eastl::make_unique<Texture2D>(texDesc);
        texMoments->CreateSRV(srvDesc);
        texMoments->CreateUAV(uavDesc);
        texHistoryMoments = eastl::make_unique<Texture2D>(texDesc);
        texHistoryMoments->CreateSRV(srvDesc);
        texHistoryMoments->CreateUAV(uavDesc);
        texHistoryMomentsDiffuse = eastl::make_unique<Texture2D>(texDesc);
        texHistoryMomentsDiffuse->CreateSRV(srvDesc);
        texHistoryMomentsDiffuse->CreateUAV(uavDesc);

        texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        texHistoryNormals = eastl::make_unique<Texture2D>(texDesc);
        texHistoryNormals->CreateSRV(srvDesc);
        texHistoryNormals->CreateUAV(uavDesc);

        // (ambient reinjection) The confidence pair. R8_UNORM because the quantity is a
        // coverage fraction in [0,1]: 1/255 quantisation is an order of magnitude below the
        // ~0.07 residual noise of the 7x7 spatial mean that produces the second surface, and a
        // UNORM read cannot be non-finite, so no consumer needs its own guard. One byte per
        // texel is ~8 MB each at a 4K allocation, against ~33 MB for each of the eight
        // RGBA16F surfaces above.
        texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R8_UNORM;
        texSSRTDiffuseConfidence = eastl::make_unique<Texture2D>(texDesc);
        texSSRTDiffuseConfidence->CreateSRV(srvDesc);
        texSSRTDiffuseConfidence->CreateUAV(uavDesc);
        texSSRTDiffuseConfidenceSmooth = eastl::make_unique<Texture2D>(texDesc);
        texSSRTDiffuseConfidenceSmooth->CreateSRV(srvDesc);
        texSSRTDiffuseConfidenceSmooth->CreateUAV(uavDesc);

        // (batch 1, item 2) The diffuse hit-distance surface. Same R8_UNORM as the confidence
        // pair above and for the same three reasons: the payload is a [0,1] fraction, 1/255 is
        // an order of magnitude below the granularity of what it steers (a smooth per-pixel
        // weight over one-texel tap offsets), and a UNORM read cannot be non-finite so no
        // consumer needs a guard of its own. ~8 MB of a 4K allocation, against ~33 MB for each
        // of the eight RGBA16F surfaces this feature already holds.
        //
        // No entry in ClearDenoiserHistory: this surface is rewritten in full by every diffuse
        // ray-march dispatch before anything reads it, so it carries no state across frames and
        // has nothing to reset.
        texSSRTDiffuseHitDistance = eastl::make_unique<Texture2D>(texDesc);
        texSSRTDiffuseHitDistance->CreateSRV(srvDesc);
        texSSRTDiffuseHitDistance->CreateUAV(uavDesc);

        // (diagnostic H) The history-acceptance picture. R8G8B8A8_UNORM: the payload is three
        // display colours plus an alpha the Buffer Viewer ignores (it draws with blending
        // disabled), so a byte per channel is exactly enough and, being UNORM, no read of it
        // can be non-finite. A quarter the footprint of the RGBA16F surfaces above.
        //
        // Allocated at the same extent as every other surface here and addressed by pixel, so
        // the dispatch writes the dynamic-resolution sub-rect and the rest stays at whatever
        // ClearDenoiserHistory last left it -- which is why that clear covers this texture too
        // rather than leaving a border of stale colour to be misread.
        texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDebugHistory = eastl::make_unique<Texture2D>(texDesc);
        texDebugHistory->CreateSRV(srvDesc);
        texDebugHistory->CreateUAV(uavDesc);

        texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R32_FLOAT;

        // (audit #20) Was a bare `new Texture2D` and therefore leaked. It is *not*
        // dead: Upscaling.cpp copies it into specHitDistanceShared12 as the DLSS-RR
        // specular hit-distance guide whenever Ray Reconstruction is enabled.
        texHitDistance = eastl::make_unique<Texture2D>(texDesc);
        texHitDistance->CreateSRV(srvDesc);
        texHitDistance->CreateUAV(uavDesc);

        // (defect D3) The previous frame's raw depth, so ssrt_temporal.hlsl can tell a
        // reprojection that landed on the same surface from one that landed on a different
        // surface at a different distance. Same format and extent as mip 0 of the Hi-Z
        // pyramid, which is what CopyHistoryGeometry snapshots it from; the UAV exists only
        // so ClearDenoiserHistory can reset it to the far plane.
        texHistoryDepth = eastl::make_unique<Texture2D>(texDesc);
        texHistoryDepth->CreateSRV(srvDesc);
        texHistoryDepth->CreateUAV(uavDesc);

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

void ScreenSpaceRayTracing::ClearShaderCache()
{
    static const std::vector<winrt::com_ptr<ID3D11ComputeShader>*> shaderPtrs = {
        &raymarchSpecularCS, &raymarchDiffuseCS, &prepareColorCS, &preprocessDepthCS, &depthDownsampleCS, &diffuseCompositeCS, &preblurCS, &temporalCS, &temporalDiagCS, &varianceCS, &spatialCS, &spatialSpecularCS,
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
            { &preblurCS, "ssrt_preblur.hlsl", {} },
            { &temporalCS, "ssrt_temporal.hlsl", {} },
            { &temporalDiagCS, "ssrt_temporal.hlsl", definesDenoiserDiag },
            { &varianceCS, "ssrt_variance.hlsl", {} },
            { &spatialCS, "ssrt_spatial.hlsl", definesWideKernel },
            { &spatialSpecularCS, "ssrt_spatial.hlsl", definesSpecular },
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
    if (!context || !texHistoryDiffuse || !texHistoryMomentsDiffuse || !texHistory || !texHistoryMoments)
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
    if ((settings.EnableSVGF && !lastEnableSVGF) ||
        (settings.EnableDiffuse && !lastEnableDiffuse) ||
        (settings.EnableSpecular && !lastEnableSpecular))
        historyClearPending = true;

    lastEnableSVGF = settings.EnableSVGF;
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
    if (recompileFlag) {
        recompileFlag = false;
        CompileComputeShaders();
    }

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
        for (int i = 0; i < maxMips - 1; ++i) {
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
            const uint mipWidth = std::max(1u, (uint)size.x >> (i + 1));
            const uint mipHeight = std::max(1u, (uint)size.y >> (i + 1));
            context->Dispatch((mipWidth + 7) / 8, (mipHeight + 7) / 8, 1);
            resetViews();
        }
        state->EndPerfEvent();
    }

    state->EndPerfEvent();

    auto view = texDepth->srv.get();
    context->PSSetShaderResources(99, 1, &view);
}

void ScreenSpaceRayTracing::DrawSSRTSpecular()
{
    if (!settings.EnableSpecular)
        return;

    auto renderer = globals::game::renderer;
    auto context = globals::d3d::context;
    auto state = globals::state;

    state->BeginPerfEvent("SSRT Compute");

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
    
    SSRTCB ssrCBData;
    {
        ssrCBData.MaxSteps = settings.MaxSteps;
        // (audit P3) Clamp against the allocated mip count: a config saved by an older
        // build may hold a value above maxMips - 1, and loading a mip that does not
        // exist returns 0 == near plane, i.e. an immediate false hit.
        ssrCBData.MaxMips = std::min(settings.MaxMips, maxMips - 1);
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
        ssrCBData.pad0[0] = ssrCBData.pad0[1] = ssrCBData.pad0[2] = 0.0f;
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

    auto [ssgi_ao, ssgi_y, ssgi_cocg, ssgi_gi_spec, ssgi_bent_normal_unused] = ssgi.GetOutputTextures();
    (void)ssgi_bent_normal_unused;  // (directional env) composite-only consumer

    // raymarch
    state->BeginPerfEvent("Raymarch");
    
    uavs.at(0) = texSSRColor->uav.get();
    uavs.at(1) = texHitDistance->uav.get();  // (audit P6) was u2; u1 freed by dropping texHitPDF

    srvs.at(0) = texHistory->srv.get();
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

    if (settings.EnableSVGF) {
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

        CopyDynamicRegion(texHistoryMoments->resource.get(), texMoments->resource.get());

        // variance filter
        uavs.at(0) = texVariance->uav.get();
        srvs.at(0) = texHistory->srv.get();
        srvs.at(1) = texMoments->srv.get();
        srvs.at(2) = normal.SRV;
        srvs.at(3) = texTemporal->srv.get();
        srvs.at(4) = depth.depthSRV;

        context->CSSetShaderResources(0, 5, srvs.data());
        context->CSSetUnorderedAccessViews(0, 1, uavs.data(), nullptr);
        context->CSSetShader(varianceCS.get(), nullptr, 0);

        context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);
        resetViews();

        // spatial filter
        for (int i = 0; i < (int)settings.AtrousIterations; ++i)
        {
            denoiserCBData.atrousIterations = i;
            denoiserCB->Update(denoiserCBData);
            denoiserBuffer = denoiserCB->CB();
            context->CSSetConstantBuffers(2, 1, &denoiserBuffer);
            uavs.at(0) = (i % 2 == 0) ? texSSRColor->uav.get() : texVariance->uav.get();
            srvs.at(0) = texHistory->srv.get();
            // (spec A1) t1 carries the moments texture, whose .z is the accumulated frame
            // count the adaptive early-out votes on. It used to receive the motion-vector
            // target, which ssrt_spatial.hlsl never declared.
            srvs.at(1) = texMoments->srv.get();
            srvs.at(2) = normal.SRV;
            srvs.at(3) = (i % 2 == 0) ? texVariance->srv.get() : texSSRColor->srv.get();
            srvs.at(4) = depth.depthSRV;

            context->CSSetShaderResources(0, 5, srvs.data());
            context->CSSetUnorderedAccessViews(0, 1, uavs.data(), nullptr);
            context->CSSetShader(spatialSpecularCS.get(), nullptr, 0);

            context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);

            resetViews();

            // (defect D4) i == 0 is the even leg of the ping-pong, so the first iteration's
            // output sits in texSSRColor and the next iteration is about to overwrite it.
            if (i == 0) {
                CopyDynamicRegion(texHistory->resource.get(), texSSRColor->resource.get());
                historyFed = true;
            }
        }

        if (settings.AtrousIterations % 2 == 0) {
            CopyDynamicRegion(texSSRColor->resource.get(), texVariance->resource.get());
        }
    }

    // output
    // (audit P6) texOutput was a byte-identical copy of texSSRColor whose only reader
    // was the deferred composite's SRV; that now binds texSSRColor->srv directly
    // (Deferred.cpp), saving a full-screen R16G16B16A16 CopyResource per frame plus the
    // texture itself.
    // (audit #13) Specular runs after diffuse, so it owns the once-per-frame snapshot.
    CopyHistoryGeometry();
    if (!historyFed)
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
    if (settings.EnableSVGF && texHistoryDepth)
        CopyDynamicRegion(texHistoryDepth->resource.get(), texDepth->resource.get());
}

void ScreenSpaceRayTracing::DrawSSRTDiffuse()
{
    if (!settings.EnableDiffuse)
        return;

    auto renderer = globals::game::renderer;
    auto context = globals::d3d::context;
    auto state = globals::state;

    state->BeginPerfEvent("SSRT Diffuse Compute");

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
    
    SSRTCB ssrCBData;
    {
        ssrCBData.MaxSteps = settings.MaxSteps;
        // (audit P3) See DrawSSRTSpecular: clamp to the allocated mip count.
        ssrCBData.MaxMips = std::min(settings.MaxMips, maxMips - 1);
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
        ssrCBData.UseDynamicCubemapsAsFallback =
            (uint)(settings.UseDynamicCubemapsAsFallback && !settings.EnableAmbientReinjection) && dynamicCubemaps.loaded;
        ssrCBData.OcclusionStrength = settings.OcclusionStrength;
        ssrCBData.CubemapNormalization = settings.CubemapNormalization;
        ssrCBData.FreezeNoisePhase = settings.FreezeNoisePhase ? 1u : 0u;  // (diagnostic T2)
        ssrCBData.pad0[0] = ssrCBData.pad0[1] = ssrCBData.pad0[2] = 0.0f;
    }
    ssrtCB->Update(ssrCBData);
    auto buffer = ssrtCB->CB();
    context->CSSetConstantBuffers(1, 1, &buffer);

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

    auto [ssgi_ao, ssgi_y, ssgi_cocg, ssgi_gi_spec, ssgi_bent_normal_unused] = ssgi.GetOutputTextures();
    (void)ssgi_bent_normal_unused;  // (directional env) composite-only consumer

    uavs.at(0) = texSSRTDiffuseColor->uav.get();
    // (ambient reinjection) Always bound, not gated on the setting: the shader writes it
    // unconditionally so that the surface stays deterministic for every texel the dispatch
    // covers, and an unbound UAV would make that write a silent no-op. The consumer side is
    // what the setting gates.
    uavs.at(5) = texSSRTDiffuseConfidence->uav.get();
    // (batch 1, item 2) Always bound, for the same reason the confidence surface is: the shader
    // writes it unconditionally so the surface stays deterministic for every texel the dispatch
    // covers -- including the 1.0 "as distant as the encoding can say" a far-plane lane resolves
    // to -- and an unbound UAV would turn that write into a silent no-op, leaving the a-trous
    // pass reading a stale or cleared surface. HitRadiusStrength 0 is what makes the mechanism
    // inert, on the consumer side.
    uavs.at(6) = texSSRTDiffuseHitDistance->uav.get();
#ifdef ENABLE_SHARC
    if (settings.EnableSharc) {
        EnsureSharcResources();  // (audit P6) allocate on first enable, before any dispatch binds them
        uavs.at(1) = sharcHashEntries->uav.get();
        uavs.at(2) = sharcHashCopyOffsets->uav.get();
        uavs.at(3) = sharcVoxelData->uav.get();
        uavs.at(4) = sharcVoxelDataPrev->uav.get();
    }
#endif

    srvs.at(0) = texHistoryDiffuse->srv.get();
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

    context->CSSetShader(settings.EnableSharc ? raymarchDiffuseSharcCS.get() : raymarchDiffuseCS.get(), nullptr, 0);
#else
    context->CSSetShader(raymarchDiffuseCS.get(), nullptr, 0);
#endif
    context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);
    resetViews();

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

    if (settings.EnableSVGF) {
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
        // nothing where a dedicated surface would cost a full-screen RGBA16F (~33 MB at a 4K
        // allocation, against the eight this feature already holds).
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

        CopyDynamicRegion(texHistoryMomentsDiffuse->resource.get(), texMoments->resource.get());

        // variance filter
        uavs.at(0) = texVariance->uav.get();
        srvs.at(0) = texHistoryDiffuse->srv.get();
        srvs.at(1) = texMoments->srv.get();
        srvs.at(2) = normal.SRV;
        srvs.at(3) = texTemporal->srv.get();
        srvs.at(4) = depth.depthSRV;

        context->CSSetShaderResources(0, 5, srvs.data());
        context->CSSetUnorderedAccessViews(0, 1, uavs.data(), nullptr);
        context->CSSetShader(varianceCS.get(), nullptr, 0);

        context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);
        resetViews();

        // spatial filter
        for (int i = 0; i < (int)settings.AtrousIterations; ++i)
        {
            denoiserCBData.atrousIterations = i;
            denoiserCB->Update(denoiserCBData);
            denoiserBuffer = denoiserCB->CB();
            context->CSSetConstantBuffers(2, 1, &denoiserBuffer);
            uavs.at(0) = (i % 2 == 0) ? texSSRTDiffuseColor->uav.get() : texVariance->uav.get();
            srvs.at(0) = texHistoryDiffuse->srv.get();
            // (spec A1) t1 = moments; see the matching binding in DrawSSRTSpecular.
            srvs.at(1) = texMoments->srv.get();
            srvs.at(2) = normal.SRV;
            srvs.at(3) = (i % 2 == 0) ? texVariance->srv.get() : texSSRTDiffuseColor->srv.get();
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

            // (defect D4) The history is the *first* iteration's output. i == 0 is the even
            // leg of the ping-pong, so that output is in texSSRTDiffuseColor right now and
            // the following iteration is about to overwrite it -- this is the only point in
            // the frame where it can be taken.
            if (i == 0) {
                CopyDynamicRegion(texHistoryDiffuse->resource.get(), texSSRTDiffuseColor->resource.get());
                historyFed = true;
            }
        }

        if (settings.AtrousIterations % 2 == 0) {
            CopyDynamicRegion(texSSRTDiffuseColor->resource.get(), texVariance->resource.get());
        }
    }

    if (!historyFed)
        CopyDynamicRegion(texHistoryDiffuse->resource.get(), texSSRTDiffuseColor->resource.get());

    // composite
    {
        uavs.at(0) = main.UAV;
        // (ambient reinjection) The pass doubles as the confidence smoothing filter: it reads
        // the raw surface at t3 and publishes the depth-aware 7x7 mean at u1 for
        // DeferredCompositeCS, which Deferred::DeferredPasses dispatches after this one. Folded
        // in here rather than given its own dispatch because this pass is three texture reads
        // of otherwise idle ALU, and because it keeps the change out of the pass schedule.
        uavs.at(1) = texSSRTDiffuseConfidenceSmooth->uav.get();
        srvs.at(0) = texSSRTDiffuseColor->srv.get();
        srvs.at(1) = albedo.SRV;
        srvs.at(3) = texSSRTDiffuseConfidence->srv.get();
        srvs.at(4) = depth.depthSRV;

        context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
        context->CSSetUnorderedAccessViews(0, 2, uavs.data(), nullptr);
        context->CSSetShader(diffuseCompositeCS.get(), nullptr, 0);

        context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);

        resetViews();
    }

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
