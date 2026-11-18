#include "EngineFix.h"

#include "EngineFixes/LandscapeSeamsFix.h"
#include "EngineFixes/ShadowmapCascadeCullingFix.h"

const std::vector<EngineFix*>& EngineFix::GetOnPostPostLoadFixesList()
{
	static ShadowmapCascadeCullingFix shadowmapCascadeCullingFix;

	static std::vector<EngineFix*> fixes = {
		&LandscapeSeamsFix::GetInstance(),
		&shadowmapCascadeCullingFix
	};

	return fixes;
}

const std::vector<EngineFix*>& EngineFix::GetOnDataLoadedFixesList()
{
	static std::vector<EngineFix*> fixes = {};

	return fixes;
}

void EngineFix::InstallFixes(const std::vector<EngineFix*>& fixes)
{
	for (const auto fix : fixes) {
		fix->Install();
		logger::info("[Engine Fixes] Installed {}", fix->GetName());
	}
}

void EngineFix::InstallOnPostPostLoadFixes()
{
	InstallFixes(GetOnPostPostLoadFixesList());
}

void EngineFix::InstallOnDataLoadedFixes()
{
	InstallFixes(GetOnDataLoadedFixesList());
}

void EngineFix::GenerateAllShaderPermutations(RE::BSShader* a_shader)
{
	for (const auto fix : GetOnPostPostLoadFixesList())
		fix->GenerateShaderPermutations(a_shader);
	for (const auto fix : GetOnDataLoadedFixesList())
		fix->GenerateShaderPermutations(a_shader);
}
