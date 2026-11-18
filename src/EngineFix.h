#pragma once

struct EngineFix
{
	virtual ~EngineFix() = default;

	virtual std::string GetName() = 0;

	virtual void Install() {}

	/** @brief Requests shader permutations this fix selects at draw time, so they precompile with the rest (#2917). */
	virtual void GenerateShaderPermutations(RE::BSShader*) {}

	static void InstallOnPostPostLoadFixes();
	static void InstallOnDataLoadedFixes();
	/** @brief Generates the shader permutations of every fix (BSShader::LoadShaders, disk cache on). */
	static void GenerateAllShaderPermutations(RE::BSShader* a_shader);

private:
	static const std::vector<EngineFix*>& GetOnPostPostLoadFixesList();
	static const std::vector<EngineFix*>& GetOnDataLoadedFixesList();
	static void InstallFixes(const std::vector<EngineFix*>& fixes);
};
