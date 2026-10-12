#include "HDROutputFeature.h"

#include "Features/Upscaling/HDROutput.h"

void HDROutputFeature::DrawSettings()
{
	HDROutput::DrawSettings();
}

void HDROutputFeature::LoadSettings(json& o_json)
{
	HDROutput::GetSettings() = o_json;
	// Own settings exist: the 43b ones (if any) are not used.
	HDROutput::Settings ignored;
	HDROutput::TakeLegacySettings(ignored);
}

void HDROutputFeature::SaveSettings(json& o_json)
{
	o_json = HDROutput::GetSettings();
}

void HDROutputFeature::RestoreDefaultSettings()
{
	HDROutput::Settings settings{};
	// First start after 43b: no "HDR Output" settings yet, take Upscaling > hdrOutput.
	HDROutput::TakeLegacySettings(settings);
	HDROutput::GetSettings() = settings;
}

void HDROutputFeature::DataLoaded()
{
	// Startup is over: anything not taken by now (no 43b settings, or already migrated) is dropped,
	// so a later "Restore defaults" really restores the defaults.
	HDROutput::ClearLegacy();
}
