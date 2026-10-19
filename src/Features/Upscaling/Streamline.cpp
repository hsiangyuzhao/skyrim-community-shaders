#include "Streamline.h"

#include <dxgi.h>
#include <dxgi1_3.h>

#include "../../Deferred.h"
#include "../../Hooks.h"
#include "../../State.h"
#include "../../Util.h"
#include "Utils/FrameTrace.h"
#include "../RenderDoc.h"
#include "../Upscaling.h"
#include "DX12SwapChain.h"

#include <Psapi.h>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <vector>

namespace
{
using DLSSModelPreset = Upscaling::DLSSModelPreset;
using DLSSDModelPreset = Upscaling::DLSSDModelPreset;

constexpr uint kDLSSModelPresetCount = static_cast<uint>(DLSSModelPreset::kCount);
constexpr uint kUnreportedPreset = std::numeric_limits<uint>::max();

const char* GetDLSSModelPresetName(DLSSModelPreset a_preset)
{
	switch (a_preset) {
	case DLSSModelPreset::kF:
		return "F";
	case DLSSModelPreset::kJ:
		return "J";
	case DLSSModelPreset::kK:
		return "K";
	case DLSSModelPreset::kL:
		return "L";
	case DLSSModelPreset::kM:
		return "M";
	case DLSSModelPreset::kSDKDocumentedMapping:
		return "Streamline 2.12 documented mapping (K/K/K/M/L)";
	default:
		return "K (safe fallback)";
	}
}

void SetAllDLSSPresets(sl::DLSSOptions& a_options, sl::DLSSPreset a_preset)
{
	a_options.dlaaPreset = a_preset;
	a_options.qualityPreset = a_preset;
	a_options.balancedPreset = a_preset;
	a_options.performancePreset = a_preset;
	a_options.ultraPerformancePreset = a_preset;
}

void SetSDKDocumentedDLSSPresets(sl::DLSSOptions& a_options)
{
	a_options.dlaaPreset = sl::DLSSPreset::ePresetK;
	a_options.qualityPreset = sl::DLSSPreset::ePresetK;
	a_options.balancedPreset = sl::DLSSPreset::ePresetK;
	a_options.performancePreset = sl::DLSSPreset::ePresetM;
	a_options.ultraPerformancePreset = sl::DLSSPreset::ePresetL;
}

sl::DLSSPreset GetForcedDLSSPreset(DLSSModelPreset a_preset)
{
	switch (a_preset) {
	case DLSSModelPreset::kF:
		return sl::DLSSPreset::ePresetF;
	case DLSSModelPreset::kJ:
		return sl::DLSSPreset::ePresetJ;
	case DLSSModelPreset::kL:
		return sl::DLSSPreset::ePresetL;
	case DLSSModelPreset::kM:
		return sl::DLSSPreset::ePresetM;
	case DLSSModelPreset::kK:
	default:
		return sl::DLSSPreset::ePresetK;
	}
}

const char* GetDLSSDModelPresetName(DLSSDModelPreset a_preset)
{
	// Only kD and kF survive LoadSettings, so everything else lands on the D arm.
	switch (a_preset) {
	case DLSSDModelPreset::kF:
		return "F (DLSS 4.5, NVIDIA's current default)";
	case DLSSDModelPreset::kD:
	default:
		return "D (previous default)";
	}
}

sl::DLSSDPreset GetForcedDLSSDPreset(DLSSDModelPreset a_preset)
{
	switch (a_preset) {
	case DLSSDModelPreset::kDefault:
		return sl::DLSSDPreset::eDefault;
	case DLSSDModelPreset::kE:
		return sl::DLSSDPreset::ePresetE;
	case DLSSDModelPreset::kF:
		return sl::DLSSDPreset::ePresetF;
	case DLSSDModelPreset::kD:
	default:
		return sl::DLSSDPreset::ePresetD;
	}
}

void SetAllDLSSDPresets(sl::DLSSDOptions& a_options, sl::DLSSDPreset a_preset)
{
	a_options.dlaaPreset = a_preset;
	a_options.qualityPreset = a_preset;
	a_options.balancedPreset = a_preset;
	a_options.performancePreset = a_preset;
	a_options.ultraPerformancePreset = a_preset;
}

template <class T>
bool LoadFeatureFunction(PFun_slGetFeatureFunction* a_getter, sl::Feature a_feature, const char* a_name, T*& a_target, sl::Result& a_result)
{
	void* function = nullptr;
	a_result = a_getter(a_feature, a_name, function);
	if (a_result != sl::Result::eOk || function == nullptr) {
		a_target = nullptr;
		return false;
	}

	a_target = reinterpret_cast<T*>(function);
	return true;
}
}

void LoggingCallback(sl::LogType type, const char* msg)
{
	// Remove trailing newlines from the raw message
	std::string rawMsg(msg);
	while (!rawMsg.empty() && (rawMsg.back() == '\n' || rawMsg.back() == '\r'))
		rawMsg.pop_back();

	// Remove leading bracketed metadata
	const char* p = msg;
	while (*p == '[') {
		const char* close = strchr(p, ']');
		if (!close)
			break;
		p = close + 1;
		// Skip whitespace after each bracketed section
		while (*p == ' ' || *p == '\t') ++p;
	}
	// Now p points to the first non-bracketed section (file/line info or message)
	std::string cleanMsg(p);
	// Trim leading/trailing whitespace and newlines
	size_t start = cleanMsg.find_first_not_of(" \t\r\n");
	size_t end = cleanMsg.find_last_not_of(" \t\r\n");
	if (start != std::string::npos && end != std::string::npos)
		cleanMsg = cleanMsg.substr(start, end - start + 1);
	else
		cleanMsg.clear();

	// If the cleaned message is empty or only bracketed tokens, log the raw message
	bool onlyBrackets = true;
	for (char c : cleanMsg) {
		if (c != '[' && c != ']' && c != ' ' && c != '\t') {
			onlyBrackets = false;
			break;
		}
	}
	if (cleanMsg.empty() || onlyBrackets) {
		logger::info("[StreamlineSDK:RAW] {}", rawMsg);
		return;
	}

	// Use a clear prefix
	const char* prefix = "[StreamlineSDK]";
	switch (type) {
	case sl::LogType::eInfo:
		logger::info("{} {}", prefix, cleanMsg);
		break;
	case sl::LogType::eWarn:
		logger::warn("{} {}", prefix, cleanMsg);
		break;
	case sl::LogType::eError:
		logger::error("{} {}", prefix, cleanMsg);
		break;
	}
}

std::vector<std::pair<std::string, std::string>> Streamline::dllVersions = {};

// ---- (46w) Read-only frame-generation diagnostics ----
namespace
{
constexpr double kFGDiagIntervalSeconds = 3.0;
constexpr const wchar_t* kNvidiaDlssgVersionsDir = L"\\ngx\\models\\dlssg\\versions\\";
constexpr const wchar_t* kLocalStreamlineDir = L"\\shaders\\upscaling\\streamline\\";
constexpr const wchar_t* kUnlockerStatusFile = L"RTXMFG-Universal.status.json";

std::mutex g_fgDiagMutex;
Streamline::FGDllInfo g_fgDll;
Streamline::FGUnlockerInfo g_fgUnlocker;
double g_fgDllLastProbe = -1.0e9;
double g_fgUnlockerLastRead = -1.0e9;
std::string g_fgDllLoggedPath;
bool g_fgUnlockerLogged = false;
bool g_fgPresetCWarned = false;

double FGDiagNowSeconds()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string FGWideToUtf8(const std::wstring& a_text)
{
	if (a_text.empty())
		return {};
	const int size = WideCharToMultiByte(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), nullptr, 0, nullptr, nullptr);
	if (size <= 0)
		return {};
	std::string result(static_cast<size_t>(size), '\0');
	WideCharToMultiByte(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), result.data(), size, nullptr, nullptr);
	return result;
}

std::wstring FGLower(std::wstring a_text)
{
	for (auto& c : a_text)
		c = static_cast<wchar_t>(std::towlower(c));
	return a_text;
}

std::wstring FGModulePath(HMODULE a_module)
{
	std::wstring path(1024, L'\0');
	const DWORD length = GetModuleFileNameW(a_module, path.data(), static_cast<DWORD>(path.size()));
	if (length == 0 || length >= path.size())
		return {};
	path.resize(length);
	return path;
}

/// The driver's download sits in ...\versions\<encoded version>\files\<name>.bin, the folder name
/// being (major << 16) | (minor << 8) | build (20318480 = 310.9.16). Used when the file carries no version resource.
std::optional<REL::Version> FGVersionFromDownloadFolder(const std::wstring& a_lowerPath)
{
	const auto start = a_lowerPath.find(kNvidiaDlssgVersionsDir);
	if (start == std::wstring::npos)
		return std::nullopt;
	const size_t begin = start + std::wcslen(kNvidiaDlssgVersionsDir);
	const size_t end = a_lowerPath.find(L'\\', begin);
	if (end == std::wstring::npos || end == begin || end - begin > 10)
		return std::nullopt;
	uint64_t value = 0;
	for (size_t i = begin; i < end; ++i) {
		if (a_lowerPath[i] < L'0' || a_lowerPath[i] > L'9')
			return std::nullopt;
		value = value * 10 + static_cast<uint64_t>(a_lowerPath[i] - L'0');
	}
	if (value == 0 || value > 0xFFFFFFFFull)
		return std::nullopt;
	return REL::Version(static_cast<std::uint16_t>((value >> 16) & 0xFFFF), static_cast<std::uint16_t>((value >> 8) & 0xFF),
		static_cast<std::uint16_t>(value & 0xFF), 0);
}

Streamline::FGDllInfo::Source FGClassify(const std::wstring& a_lowerPath)
{
	using Source = Streamline::FGDllInfo::Source;
	if (a_lowerPath.find(kNvidiaDlssgVersionsDir) != std::wstring::npos)
		return Source::kNvidiaDownload;
	if (a_lowerPath.find(kLocalStreamlineDir) != std::wstring::npos)
		return Source::kLocal;
	return Source::kOther;
}

/// Caller holds g_fgDiagMutex.
void FGProbeDllLocked()
{
	// Every frame-generation candidate loaded in the process: the local nvngx_dlssg.dll (by name) and
	// the driver's download (by folder; NVIDIA loads it as e.g. 160_E658700.bin). When both are loaded,
	// the download is the one NGX chose (it only loads it to use it).
	std::wstring localPath;
	if (HMODULE local = GetModuleHandleW(L"nvngx_dlssg.dll"))
		localPath = FGModulePath(local);

	std::wstring downloadPath;
	std::wstring otherByName;
	{
		std::vector<HMODULE> modules(1024);
		DWORD needed = 0;
		if (K32EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed)) {
			const size_t count = std::min<size_t>(modules.size(), needed / sizeof(HMODULE));
			for (size_t i = 0; i < count; ++i) {
				const std::wstring path = FGModulePath(modules[i]);
				if (path.empty())
					continue;
				const std::wstring lower = FGLower(path);
				if (lower.find(kNvidiaDlssgVersionsDir) != std::wstring::npos) {
					if (downloadPath.empty())
						downloadPath = path;
				} else if (localPath.empty() && lower.ends_with(L"\\nvngx_dlssg.dll")) {
					otherByName = path;
				}
			}
		}
	}
	if (localPath.empty())
		localPath = otherByName;

	const std::wstring& chosen = !downloadPath.empty() ? downloadPath : localPath;
	const std::wstring& second = !downloadPath.empty() ? localPath : downloadPath;

	Streamline::FGDllInfo info{};
	if (!chosen.empty()) {
		const std::wstring lower = FGLower(chosen);
		info.found = true;
		info.path = FGWideToUtf8(chosen);
		info.otherPath = FGWideToUtf8(second);
		info.source = FGClassify(lower);
		auto version = Util::GetDllVersion(chosen);
		if (!version && info.source == Streamline::FGDllInfo::Source::kNvidiaDownload)
			version = FGVersionFromDownloadFolder(lower);
		if (version) {
			info.hasVersion = true;
			info.version = *version;
			info.versionText = Util::GetFormattedVersion(*version);
		}
	}
	// Final once seen while DLSS-G runs and reports what it shows: NGX has created and evaluated the
	// feature by then, so the module in use will not change for the session.
	const auto& upscaling = globals::features::upscaling;
	info.settled = info.found && upscaling.streamline.IsDLSSGActive() && upscaling.GetFrameGenerationPresentMultiplier() > 1.01f;
	g_fgDll = std::move(info);

	if (g_fgDll.found && g_fgDll.path != g_fgDllLoggedPath) {
		g_fgDllLoggedPath = g_fgDll.path;
		logger::info("[Streamline] (46w) Frame generation DLL in use: {} version {} ({}){}", g_fgDll.path, g_fgDll.versionText, g_fgDll.SourceName(),
			g_fgDll.otherPath.empty() ? std::string() : std::format("; also loaded: {}", g_fgDll.otherPath));
	}
}

int FGJsonInt(const json& a_json, const char* a_key, int a_default)
{
	const auto it = a_json.find(a_key);
	if (it == a_json.end())
		return a_default;
	if (it->is_boolean())
		return it->get<bool>() ? 1 : 0;
	if (it->is_number_integer() || it->is_number_unsigned()) {
		const auto value = it->get<int64_t>();
		return static_cast<int>(std::clamp<int64_t>(value, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
	}
	if (it->is_number_float())
		return static_cast<int>(it->get<double>());
	return a_default;
}

std::string FGJsonString(const json& a_json, const char* a_key)
{
	const auto it = a_json.find(a_key);
	if (it == a_json.end())
		return {};
	if (it->is_string())
		return it->get<std::string>();
	if (it->is_number())
		return it->dump();
	return {};
}

/// Caller holds g_fgDiagMutex.
void FGReadUnlockerLocked()
{
	std::filesystem::path statusPath;
	{
		const std::wstring exe = FGModulePath(nullptr);
		if (exe.empty())
			return;
		statusPath = std::filesystem::path(exe).parent_path() / kUnlockerStatusFile;
	}

	std::error_code ec;
	if (!std::filesystem::exists(statusPath, ec)) {
		// Keep nothing from an earlier read: without the file the game / driver default applies.
		g_fgUnlocker = {};
		return;
	}
	g_fgUnlocker.fileFound = true;

	std::string text;
	{
		std::ifstream file(statusPath, std::ios::binary);
		if (!file) {
			g_fgUnlocker.lastReadFailed = true;
			return;
		}
		text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	}
	// The unlocker rewrites the file while the game runs; a half-written read is simply skipped.
	const json status = json::parse(text, nullptr, false);
	if (status.is_discarded() || !status.is_object()) {
		g_fgUnlocker.lastReadFailed = true;
		return;
	}

	Streamline::FGUnlockerInfo info{};
	info.fileFound = true;
	info.parsed = true;
	info.statusFormat = FGJsonInt(status, "version", -1);
	const int pid = FGJsonInt(status, "pid", -1);
	info.stale = pid > 0 && static_cast<DWORD>(pid) != GetCurrentProcessId();
	info.presetRequested = FGJsonInt(status, "dlssgPresetRequested", -1);
	info.presetLatched = FGJsonInt(status, "dlssgPresetLatched", -1);
	info.presetObserved = FGJsonInt(status, "dlssgPresetObserved", -1);
	info.presetObservedValid = FGJsonInt(status, "dlssgPresetObservedValid", -1);
	info.restartRequired = FGJsonInt(status, "dlssgPresetRestartRequired", 0) != 0;
	const int major = FGJsonInt(status, "activeProviderVersionMajor", -1);
	if (major > 0) {
		const auto part = [&](const char* a_key) { return static_cast<std::uint16_t>(std::clamp(FGJsonInt(status, a_key, 0), 0, 0xFFFF)); };
		info.hasProviderVersion = true;
		info.providerVersion = REL::Version(static_cast<std::uint16_t>(std::min(major, 0xFFFF)), part("activeProviderVersionMinor"),
			part("activeProviderVersionBuild"), part("activeProviderVersionPrivate"));
		info.providerVersionText = Util::GetFormattedVersion(info.providerVersion);
	}
	info.providerPath = FGJsonString(status, "activeProviderPath");
	for (const char* key : { "unlockerVersion", "unlockerBuild", "productVersion", "buildVersion", "build" }) {
		info.unlockerBuild = FGJsonString(status, key);
		if (!info.unlockerBuild.empty())
			break;
	}
	g_fgUnlocker = std::move(info);

	if (!g_fgUnlockerLogged) {
		g_fgUnlockerLogged = true;
		logger::info("[Streamline] (46w) MFG Unlocker status: {}preset requested {}, latched {}, observed {}{}, restart required {}, provider {}",
			g_fgUnlocker.stale ? "(from another game session) " : "", Streamline::FGPresetName(g_fgUnlocker.presetRequested),
			Streamline::FGPresetName(g_fgUnlocker.presetLatched), Streamline::FGPresetName(g_fgUnlocker.presetObserved),
			g_fgUnlocker.presetObservedValid == 0 ? " (not seen yet)" : "", g_fgUnlocker.NeedsRestart() ? "yes" : "no",
			g_fgUnlocker.hasProviderVersion ? g_fgUnlocker.providerVersionText : std::string("-"));
	}
}

/// Caller holds g_fgDiagMutex.
void FGCheckWarningsLocked()
{
	if (!g_fgPresetCWarned && Streamline::FGPresetCNeedsNewerDll(g_fgDll, g_fgUnlocker)) {
		g_fgPresetCWarned = true;
		logger::warn("[Streamline] (46w) MFG Unlocker asks for frame generation Preset C, but the frame generation DLL in use is {} (Preset C needs 310.9.16 or newer)",
			g_fgDll.hasVersion ? g_fgDll.versionText : g_fgUnlocker.providerVersionText);
	}
}
}  // namespace

const char* Streamline::FGDllInfo::SourceName() const
{
	switch (source) {
	case Source::kLocal:
		return "local copy (Shaders\\Upscaling\\Streamline, AIO or MO2 override)";
	case Source::kNvidiaDownload:
		return "NVIDIA download";
	case Source::kOther:
		return "other location";
	default:
		return "not loaded";
	}
}

int Streamline::FGUnlockerInfo::EffectivePreset() const
{
	if (presetObserved >= 0 && presetObservedValid != 0)
		return presetObserved;
	if (presetLatched >= 0)
		return presetLatched;
	return presetRequested;
}

bool Streamline::FGUnlockerInfo::NeedsRestart() const
{
	return restartRequired || (presetRequested >= 0 && presetLatched >= 0 && presetRequested != presetLatched);
}

const char* Streamline::FGPresetName(int a_preset)
{
	switch (a_preset) {
	case 0:
		return "default";
	case 1:
		return "A";
	case 2:
		return "B";
	case 3:
		return "C";
	case -1:
		return "-";
	default:
		return "?";
	}
}

Streamline::FGDllInfo Streamline::GetFGDll(bool a_force)
{
	std::lock_guard lock(g_fgDiagMutex);
	try {
		const double now = FGDiagNowSeconds();
		// Nothing to look for when DLSS-G is not the startup backend; once final, the module does not change.
		if (globals::features::upscaling.streamline.IsDLSSGBackendSelectedAtBoot() && !g_fgDll.settled &&
			(a_force || now - g_fgDllLastProbe >= kFGDiagIntervalSeconds)) {
			g_fgDllLastProbe = now;
			Util::FrameTrace::NoteJob(Util::FrameTrace::Job::FgStatus);  // (47p)
			FGProbeDllLocked();
			FGCheckWarningsLocked();
		}
	} catch (const std::exception& e) {
		logger::warn("[Streamline] (46w) Frame generation DLL probe failed: {}", e.what());
	}
	return g_fgDll;
}

Streamline::FGUnlockerInfo Streamline::GetFGUnlocker(bool a_force)
{
	std::lock_guard lock(g_fgDiagMutex);
	try {
		const double now = FGDiagNowSeconds();
		if (a_force || now - g_fgUnlockerLastRead >= kFGDiagIntervalSeconds) {
			g_fgUnlockerLastRead = now;
			Util::FrameTrace::NoteJob(Util::FrameTrace::Job::FgStatus);  // (47p)
			FGReadUnlockerLocked();
			FGCheckWarningsLocked();
		}
	} catch (const std::exception& e) {
		g_fgUnlocker.lastReadFailed = true;
		logger::warn("[Streamline] (46w) Could not read the MFG Unlocker status: {}", e.what());
	}
	return g_fgUnlocker;
}

bool Streamline::FGPresetCNeedsNewerDll(const FGDllInfo& a_dll, const FGUnlockerInfo& a_unlocker)
{
	if (!a_unlocker.Active() || a_unlocker.presetRequested != 3)
		return false;
	static const REL::Version kPresetCMinimum(310, 9, 16, 0);
	if (a_dll.hasVersion)
		return a_dll.version < kPresetCMinimum;
	if (a_unlocker.hasProviderVersion)
		return a_unlocker.providerVersion < kPresetCMinimum;
	return false;
}

std::string Streamline::DescribeFGForTrace()
{
	const FGDllInfo dll = GetFGDll(true);
	const FGUnlockerInfo unlocker = GetFGUnlocker(true);
	std::string text;
	if (!globals::features::upscaling.streamline.IsDLSSGBackendSelectedAtBoot())
		text = "DLSS-G not the startup backend (no FG DLL expected)";
	else if (dll.found)
		text = std::format("DLL {} ({}: {}){}", dll.versionText, dll.SourceName(), dll.path,
			dll.otherPath.empty() ? std::string() : std::format(", also loaded {}", dll.otherPath));
	else
		text = "DLL not loaded yet";
	if (!unlocker.fileFound) {
		text += "; MFG Unlocker: no status file (game/driver default preset)";
	} else if (!unlocker.parsed) {
		text += "; MFG Unlocker: status file unreadable";
	} else {
		text += std::format("; MFG Unlocker{}: preset requested {}, latched {}, observed {}{}, restart required {}, provider {} ({}), build {}, status format {}",
			unlocker.stale ? " (status from another game session, not in effect)" : "", FGPresetName(unlocker.presetRequested),
			FGPresetName(unlocker.presetLatched), FGPresetName(unlocker.presetObserved), unlocker.presetObservedValid == 0 ? " (not seen yet)" : "",
			unlocker.NeedsRestart() ? "yes" : "no", unlocker.hasProviderVersion ? unlocker.providerVersionText : std::string("-"),
			unlocker.providerPath.empty() ? std::string("-") : unlocker.providerPath, unlocker.unlockerBuild.empty() ? std::string("-") : unlocker.unlockerBuild,
			unlocker.statusFormat);
	}
	if (FGPresetCNeedsNewerDll(dll, unlocker))
		text += "; WARNING: Preset C requested but the DLL is older than 310.9.16";
	return text;
}

json Streamline::DescribeFGJson()
{
	const FGDllInfo dll = GetFGDll(true);
	const FGUnlockerInfo unlocker = GetFGUnlocker(true);
	json result;
	result["dll"] = {
		{ "found", dll.found },
		{ "version", dll.versionText },
		{ "path", dll.path },
		{ "source", dll.SourceName() },
		{ "also_loaded", dll.otherPath },
		{ "settled", dll.settled },
	};
	result["unlocker"] = {
		{ "status_file", unlocker.fileFound },
		{ "parsed", unlocker.parsed },
		{ "stale", unlocker.stale },
		{ "last_read_failed", unlocker.lastReadFailed },
		{ "preset_requested", FGPresetName(unlocker.presetRequested) },
		{ "preset_latched", FGPresetName(unlocker.presetLatched) },
		{ "preset_observed", FGPresetName(unlocker.presetObserved) },
		{ "preset_observed_valid", unlocker.presetObservedValid },
		{ "preset_effective", unlocker.Active() ? FGPresetName(unlocker.EffectivePreset()) : "game/driver default" },
		{ "restart_required", unlocker.NeedsRestart() },
		{ "provider_version", unlocker.providerVersionText },
		{ "provider_path", unlocker.providerPath },
		{ "unlocker_build", unlocker.unlockerBuild },
		{ "status_format", unlocker.statusFormat },
	};
	result["preset_c_needs_newer_dll"] = FGPresetCNeedsNewerDll(dll, unlocker);
	return result;
}

void Streamline::SelectDLSSGBackendAtBoot(bool a_selected)
{
	if (triedInitialization) {
		logger::warn("[Streamline] DLSS-G backend selection is startup-only; ignoring a late change to {}", a_selected);
		return;
	}

	if (a_selected && REL::Module::IsVR()) {
		logger::warn("[Streamline] DLSS-G is unavailable in VR; keeping the backend disabled");
		dlssGBackendSelectedAtBoot = false;
		return;
	}

	dlssGBackendSelectedAtBoot = a_selected;
}

void Streamline::LoadInterposer()
{
	if (triedInitialization)
		return;

	// RenderDoc and sl.interposer both hook device creation; loading both crashes the game
	// at startup before anything can log. When a RenderDoc capture session is active, leave
	// Streamline unloaded entirely -- every downstream feature flag already handles the
	// interposer being absent (missing-DLL path below), so upscaling simply reports
	// unavailable for the session. FidelityFX.cpp documents the same interference at the
	// FSR dispatch level; this stops it one step earlier, where it is fatal.
	//
	// Gated on the persisted *setting*, not on IsAvailable(): Upscaling loads before
	// RenderDoc in the feature list, so at this point the RenderDoc API is never up yet,
	// while the setting was read by State::Load before any feature's Load() ran.
	if (RenderDoc::GetSingleton()->IsCaptureEnabled()) {
		logger::info("[Streamline] RenderDoc capture is active, skipping interposer load for this session");
		triedInitialization = true;
		initialized = false;
		return;
	}

	triedInitialization = true;
	initialized = false;
	deviceRegistered = false;
	featureDLSS_G = false;
	featureReflex = false;
	featurePCL = false;
	dlssGRuntimeFaulted = false;
	dlssGStateFallbackApplied = false;
	dlssGActive = false;
	dlssGOptionsInitialized = false;
	dlssGFunctionsReady = false;
	reflexFunctionsReady = false;
	pclFunctionsReady = false;
	reflexSleepFailureLogged = false;
	pclMarkerFailureLogged = false;
	dlssGFramesToGenerateMax = 0;
	dlssGAppliedFramesToGenerate = 1;
	dlssGRejectedFramesToGenerate = 0;
	dlssGCapabilityQueryCooldown = 0;

	std::wstring interposerPath = std::wstring(Streamline::PluginDir) + L"\\sl.interposer.dll";
	interposer = LoadLibraryW(interposerPath.c_str());
	if (interposer == nullptr) {
		DWORD errorCode = GetLastError();
		logger::info("[Streamline] Failed to load interposer: Error Code {0:x}", errorCode);
		return;
	} else {
		logger::info("[Streamline] Interposer loaded at address: {0:p}", static_cast<void*>(interposer));
	}

	// Dynamically log all DLL versions in the Streamline plugin directory
	std::filesystem::path pluginDir = std::filesystem::path(Streamline::PluginDir);
	Streamline::dllVersions.clear();
	for (const auto& entry : std::filesystem::directory_iterator(pluginDir)) {
		if (entry.is_regular_file() && entry.path().extension() == L".dll") {
			const auto& path = entry.path();
			auto version = Util::GetDllVersion(path.c_str());
			auto name = path.filename().string();
			std::string versionStr = version ? Util::GetFormattedVersion(*version) : "Unknown";
			Streamline::dllVersions.emplace_back(name, versionStr);
			if (version)
				logger::info("[Streamline] {} version: {}", name, versionStr);
			else
				logger::info("[Streamline] {} version: Unknown", name);
		}
	}

	logger::info("[Streamline] Initializing Streamline");

	sl::Preferences pref;
	const bool isVR = REL::Module::IsVR();
	std::vector<sl::Feature> featuresToLoad = { sl::kFeatureDLSS, sl::kFeatureDLSS_RR };
	if (!isVR) {
		// Reflex and PCL are not DLSS-G's; they were only ever loaded alongside it. They
		// reduce latency on their own, and Reflex's frame limiter in particular is the only
		// one in this tree the driver knows about, which is what makes it the right one to
		// use under a variable refresh rate. The markers placed around simulation and
		// present cost nothing extra to feed. Loading them for every non-VR session is what
		// lets the latency panel work on the FSR backend and with frame generation off.
		featuresToLoad.push_back(sl::kFeatureReflex);
		featuresToLoad.push_back(sl::kFeaturePCL);
	}
	if (!isVR && dlssGBackendSelectedAtBoot) {
		// DLSS-G owns the presentation path.  Do not request it when the
		// startup-selected backend is FSR3, otherwise both backends can claim
		// the same swap chain.
		featuresToLoad.push_back(sl::kFeatureDLSS_G);
		logger::info("[Streamline] Startup backend: DLSS-G (multiplier chosen at runtime; 2x until the driver reports more) + Reflex/PCL");
	} else {
		logger::info("[Streamline] Startup backend: {}{}",
			isVR ? "VR/no DLSS-G" : "FSR3/no DLSS-G", isVR ? "" : " + Reflex/PCL");
	}

	pref.featuresToLoad = featuresToLoad.data();
	pref.numFeaturesToLoad = static_cast<uint32_t>(featuresToLoad.size());

	// Set log level from settings
	switch (globals::features::upscaling.settings.streamlineLogLevel) {
	case 2:
		pref.logLevel = sl::LogLevel::eVerbose;
		break;
	case 1:
		pref.logLevel = sl::LogLevel::eDefault;
		break;
	case 0:
	default:
		pref.logLevel = sl::LogLevel::eOff;
		break;
	}
	pref.logMessageCallback = LoggingCallback;
	pref.showConsole = false;

	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "1.0.0";
	pref.projectId = "f8776929-c969-43bd-ac2b-294b4de58aac";

	pref.renderAPI = sl::RenderAPI::eD3D12;
	pref.flags = sl::PreferenceFlags::eUseManualHooking;
	if (dlssGBackendSelectedAtBoot)
		pref.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;

	// Hook up all of the functions exported by the SL Interposer Library
	slInit = (PFun_slInit*)GetProcAddress(interposer, "slInit");
	slShutdown = (PFun_slShutdown*)GetProcAddress(interposer, "slShutdown");
	slIsFeatureSupported = (PFun_slIsFeatureSupported*)GetProcAddress(interposer, "slIsFeatureSupported");
	slIsFeatureLoaded = (PFun_slIsFeatureLoaded*)GetProcAddress(interposer, "slIsFeatureLoaded");
	slSetFeatureLoaded = (PFun_slSetFeatureLoaded*)GetProcAddress(interposer, "slSetFeatureLoaded");
	slEvaluateFeature = (PFun_slEvaluateFeature*)GetProcAddress(interposer, "slEvaluateFeature");
	slAllocateResources = (PFun_slAllocateResources*)GetProcAddress(interposer, "slAllocateResources");
	slFreeResources = (PFun_slFreeResources*)GetProcAddress(interposer, "slFreeResources");
	slSetTag = (PFun_slSetTag*)GetProcAddress(interposer, "slSetTag");
	slSetTagForFrame = (PFun_slSetTagForFrame*)GetProcAddress(interposer, "slSetTagForFrame");
	slGetFeatureRequirements = (PFun_slGetFeatureRequirements*)GetProcAddress(interposer, "slGetFeatureRequirements");
	slGetFeatureVersion = (PFun_slGetFeatureVersion*)GetProcAddress(interposer, "slGetFeatureVersion");
	slUpgradeInterface = (PFun_slUpgradeInterface*)GetProcAddress(interposer, "slUpgradeInterface");
	slSetConstants = (PFun_slSetConstants*)GetProcAddress(interposer, "slSetConstants");
	slGetNativeInterface = (PFun_slGetNativeInterface*)GetProcAddress(interposer, "slGetNativeInterface");
	slGetFeatureFunction = (PFun_slGetFeatureFunction*)GetProcAddress(interposer, "slGetFeatureFunction");
	slGetNewFrameToken = (PFun_slGetNewFrameToken*)GetProcAddress(interposer, "slGetNewFrameToken");
	slSetD3DDevice = (PFun_slSetD3DDevice*)GetProcAddress(interposer, "slSetD3DDevice");

	const bool resourceTaggingReady = dlssGBackendSelectedAtBoot ? slSetTagForFrame != nullptr : slSetTag != nullptr;
	const bool coreFunctionsReady = slInit && slShutdown && slIsFeatureSupported && slIsFeatureLoaded &&
		slSetFeatureLoaded && slEvaluateFeature && slAllocateResources && slFreeResources && resourceTaggingReady &&
		slGetFeatureRequirements && slGetFeatureVersion && slUpgradeInterface && slSetConstants &&
		slGetNativeInterface && slGetFeatureFunction && slGetNewFrameToken && slSetD3DDevice;
	if (!coreFunctionsReady) {
		logger::critical("[Streamline] Required interposer exports are missing; disabling Streamline");
		FreeLibrary(interposer);
		interposer = nullptr;
		return;
	}

	if (SL_FAILED(res, slInit(pref, sl::kSDKVersion))) {
		logger::critical("[Streamline] Failed to initialize Streamline");
	} else {
		initialized = true;
		logger::info("[Streamline] Successfully initialized Streamline");
	}
}

sl::Result Streamline::SetD3DDevice(void* a_device)
{
	if (!slSetD3DDevice || a_device == nullptr)
		return sl::Result::eErrorInvalidParameter;

	const auto result = slSetD3DDevice(a_device);
	deviceRegistered = result == sl::Result::eOk;
	return result;
}

sl::Result Streamline::UpgradeInterface(void** a_interface)
{
	if (!slUpgradeInterface || a_interface == nullptr || *a_interface == nullptr)
		return sl::Result::eErrorInvalidParameter;

	return slUpgradeInterface(a_interface);
}

sl::Result Streamline::GetNativeInterface(void* a_proxyInterface, void** a_nativeInterface)
{
	if (!slGetNativeInterface || a_proxyInterface == nullptr || a_nativeInterface == nullptr)
		return sl::Result::eErrorInvalidParameter;

	*a_nativeInterface = nullptr;
	return slGetNativeInterface(a_proxyInterface, a_nativeInterface);
}

void Streamline::CheckFeatures(IDXGIAdapter* a_adapter)
{
	logger::info("[Streamline] Checking features");
	featureDLSS = false;
	featureDLSS_RR = false;
	featureDLSS_G = false;
	featureReflex = false;
	featurePCL = false;

	if (!initialized || !slIsFeatureLoaded || !slIsFeatureSupported || !slGetFeatureRequirements) {
		logger::error("[Streamline] Cannot check features before Streamline/device initialization");
		return;
	}
	if (a_adapter == nullptr) {
		logger::error("[Streamline] Cannot check features without a DXGI adapter");
		return;
	}

	DXGI_ADAPTER_DESC adapterDesc{};
	if (FAILED(a_adapter->GetDesc(&adapterDesc))) {
		logger::error("[Streamline] Failed to query DXGI adapter description");
		return;
	}

	sl::AdapterInfo adapterInfo{};
	adapterInfo.deviceLUID = reinterpret_cast<uint8_t*>(&adapterDesc.AdapterLuid);
	adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);

	auto checkFeature = [&](sl::Feature a_feature, const char* a_name, bool a_requested, bool& a_available) {
		a_available = false;
		if (!a_requested) {
			logger::info("[Streamline] {} feature was not requested", a_name);
			return;
		}

		bool loaded = false;
		const auto loadedResult = slIsFeatureLoaded(a_feature, loaded);
		if (loadedResult != sl::Result::eOk || !loaded) {
			logger::info("[Streamline] {} feature is not loaded ({})", a_name, magic_enum::enum_name(loadedResult));
			sl::FeatureRequirements requirements{};
			const auto requirementsResult = slGetFeatureRequirements(a_feature, requirements);
			if (requirementsResult != sl::Result::eOk)
				logger::info("[Streamline] {} feature requirements unavailable ({})", a_name, magic_enum::enum_name(requirementsResult));
			return;
		}

		const auto supportResult = slIsFeatureSupported(a_feature, adapterInfo);
		if (supportResult != sl::Result::eOk) {
			logger::info("[Streamline] {} feature is loaded but unsupported ({})", a_name, magic_enum::enum_name(supportResult));
			return;
		}

		a_available = true;
		logger::info("[Streamline] {} feature is loaded and supported", a_name);
	};

	checkFeature(sl::kFeatureDLSS, "DLSS", true, featureDLSS);
	checkFeature(sl::kFeatureDLSS_RR, "DLSS RR", true, featureDLSS_RR);

	const bool isVR = REL::Module::IsVR();
	const bool requestDLSSG = !isVR && dlssGBackendSelectedAtBoot;
	checkFeature(sl::kFeatureDLSS_G, "DLSS-G", requestDLSSG, featureDLSS_G);
	// Requested for every non-VR session, matching what Initialize now loads: these two
	// stand on their own and are no longer tied to the frame-generation backend.
	checkFeature(sl::kFeatureReflex, "Reflex", !isVR, featureReflex);
	checkFeature(sl::kFeaturePCL, "PCL", !isVR, featurePCL);

	logger::info("[Streamline] DLSS {} available", featureDLSS ? "is" : "is not");
	logger::info("[Streamline] DLSS RR {} available", featureDLSS_RR ? "is" : "is not");
	logger::info("[Streamline] DLSS-G {} available", featureDLSS_G ? "is" : "is not");
	logger::info("[Streamline] Reflex {} available", featureReflex ? "is" : "is not");
	logger::info("[Streamline] PCL {} available", featurePCL ? "is" : "is not");
}

void Streamline::PostDevice()
{
	dlssGFunctionsReady = false;
	reflexFunctionsReady = false;
	pclFunctionsReady = false;

	if (!initialized || !deviceRegistered || !slGetFeatureFunction) {
		logger::error("[Streamline] Cannot load feature functions before the device is registered");
		return;
	}

	auto load = [&](sl::Feature a_feature, const char* a_name, auto& a_target) {
		sl::Result result = sl::Result::eOk;
		if (!LoadFeatureFunction(slGetFeatureFunction, a_feature, a_name, a_target, result)) {
			logger::error("[Streamline] Failed to load {} ({})", a_name, magic_enum::enum_name(result));
			return false;
		}
		return true;
	};

	if (featureDLSS) {
		load(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", slDLSSGetOptimalSettings);
		load(sl::kFeatureDLSS, "slDLSSGetState", slDLSSGetState);
		load(sl::kFeatureDLSS, "slDLSSSetOptions", slDLSSSetOptions);
	}

	if (featureDLSS_RR) {
		load(sl::kFeatureDLSS_RR, "slDLSSDGetOptimalSettings", slDLSSDGetOptimalSettings);
		load(sl::kFeatureDLSS_RR, "slDLSSDGetState", slDLSSDGetState);
		load(sl::kFeatureDLSS_RR, "slDLSSDSetOptions", slDLSSDSetOptions);
	}

	if (featureDLSS_G) {
		const bool stateReady = load(sl::kFeatureDLSS_G, "slDLSSGGetState", slDLSSGGetState);
		const bool optionsReady = load(sl::kFeatureDLSS_G, "slDLSSGSetOptions", slDLSSGSetOptions);
		dlssGFunctionsReady = stateReady && optionsReady;
	}

	if (featureReflex) {
		const bool stateReady = load(sl::kFeatureReflex, "slReflexGetState", slReflexGetState);
		const bool sleepReady = load(sl::kFeatureReflex, "slReflexSleep", slReflexSleep);
		const bool optionsReady = load(sl::kFeatureReflex, "slReflexSetOptions", slReflexSetOptions);
		reflexFunctionsReady = stateReady && sleepReady && optionsReady;
	}

	if (featurePCL) {
		const bool stateReady = load(sl::kFeaturePCL, "slPCLGetState", slPCLGetState);
		const bool markerReady = load(sl::kFeaturePCL, "slPCLSetMarker", slPCLSetMarker);
		const bool optionsReady = load(sl::kFeaturePCL, "slPCLSetOptions", slPCLSetOptions);
		pclFunctionsReady = stateReady && markerReady && optionsReady;
	}

	// Driven by the saved settings rather than hardcoded to eLowLatency, and forced here so a
	// failure at this point still invalidates the feature the way the old fixed call did.
	if (reflexFunctionsReady && !globals::features::upscaling.ApplyReflexSettings(true))
		reflexFunctionsReady = false;
	if (pclFunctionsReady) {
		sl::PCLOptions options{};
		if (SL_FAILED(result, slPCLSetOptions(options))) {
			logger::error("[Streamline] Could not initialize PCL options ({})", magic_enum::enum_name(result));
			pclFunctionsReady = false;
		}
	}

	if (dlssGBackendSelectedAtBoot && !IsDLSSGReady())
		logger::error("[Streamline] DLSS-G startup backend is not ready; the caller must fall back to its non-DLSS-G path");
}

bool Streamline::IsDLSSGReady() const
{
	return !REL::Module::IsVR() && dlssGBackendSelectedAtBoot && initialized && deviceRegistered && featureDLSS_G && featureReflex && featurePCL &&
		!dlssGRuntimeFaulted && dlssGFunctionsReady && reflexFunctionsReady && pclFunctionsReady;
}

/**
 * @brief Updates and sets camera and frame constants for the current Streamline frame.
 *
 * Populates and submits camera parameters, projection matrices, motion vector settings, and other per-frame constants to the Streamline SDK for the current frame. Uses cached framebuffer data and global state to ensure correct configuration for upscaling and frame generation features.
 */
bool Streamline::BeginFrameToken()
{
	if (!initialized || !slGetNewFrameToken || globals::state == nullptr)
		return false;

	const uint32_t currentFrame = globals::state->frameCount;
	if (frameToken != nullptr && frameTokenIndex == currentFrame)
		return true;

	frameToken = nullptr;
	frameTokenIndex = UINT32_MAX;
	frameConstantsValid = false;
	if (SL_FAILED(res, slGetNewFrameToken(frameToken, &currentFrame)) || frameToken == nullptr) {
		logger::error("[Streamline] Could not acquire frame token for frame {}", currentFrame);
		frameToken = nullptr;
		return false;
	}
	frameTokenIndex = currentFrame;
	return true;
}

void Streamline::CheckFrameConstants()
{
	if (!slSetConstants || !BeginFrameToken())
		return;
	SubmitFrameConstants();
}

void Streamline::CheckFrameConstantsForLatchedFrame()
{
	// Present is entered after State::Reset has advanced the host counter.
	// Do not acquire that next token: resources still belong to the token
	// latched at SimulationStart for the frame being presented.
	if (!slSetConstants || frameToken == nullptr)
		return;
	SubmitFrameConstants();
}

void Streamline::RequestTemporalReset()
{
	temporalResetRequested = true;
}

void Streamline::SubmitFrameConstants()
{
	if (frameConstantsValid)
		return;
	if (globals::game::cameraNear == nullptr || globals::game::cameraFar == nullptr)
		return;

	auto state = globals::state;
	const uint32_t currentFrame = frameTokenIndex;

	sl::Constants slConstants = {};

	if (globals::game::isVR) {
		slConstants.cameraAspectRatio = (state->screenSize.x * 0.5f) / state->screenSize.y;
	} else {
		slConstants.cameraAspectRatio = state->screenSize.x / state->screenSize.y;
	}

	slConstants.cameraFOV = Util::GetVerticalFOVRad();
	slConstants.cameraNear = *globals::game::cameraNear;
	slConstants.cameraFar = *globals::game::cameraFar;

	auto viewMatrix = globals::game::frameBufferCached.GetCameraViewInverse().Transpose();
	auto cameraViewToClip = globals::game::frameBufferCached.GetCameraProjUnjittered().Transpose();

	slConstants.cameraMotionIncluded = sl::Boolean::eTrue;
	slConstants.cameraPinholeOffset = { 0.f, 0.f };
	slConstants.cameraRight = { viewMatrix._11, viewMatrix._12, viewMatrix._13 };
	slConstants.cameraUp = { viewMatrix._21, viewMatrix._22, viewMatrix._23 };
	slConstants.cameraFwd = { viewMatrix._31, viewMatrix._32, viewMatrix._33 };
	slConstants.cameraPos = *(sl::float3*)&globals::game::frameBufferCached.GetCameraPosAdjust();
	slConstants.cameraViewToClip = *(sl::float4x4*)&cameraViewToClip;
	slConstants.depthInverted = sl::Boolean::eFalse;

	recalculateCameraMatrices(slConstants);

	auto& upscaling = globals::features::upscaling;
	auto jitter = upscaling.jitter;
	slConstants.jitterOffset = { -jitter.x, -jitter.y };

	slConstants.reset = temporalResetRequested ? sl::Boolean::eTrue : sl::Boolean::eFalse;

	slConstants.mvecScale = { (globals::game::isVR ? 0.5f : 1.0f), 1 };
	slConstants.motionVectors3D = sl::Boolean::eFalse;
	slConstants.motionVectorsInvalidValue = FLT_MIN;
	// The world map uses a different camera path. Derive the projection type
	// from the submitted matrix instead of forcing perspective for every frame.
	const bool orthographicProjection = std::abs(cameraViewToClip._44) > 0.5f;
	slConstants.orthographicProjection = orthographicProjection ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	slConstants.motionVectorsDilated = sl::Boolean::eFalse;
	slConstants.motionVectorsJittered = sl::Boolean::eFalse;

	if (SL_FAILED(res, slSetConstants(slConstants, *frameToken, viewport))) {
		logger::error("[Streamline] Could not set constants for frame {}", currentFrame);
		frameConstantsValid = false;
		return;
	}

	if (temporalResetRequested) {
		logger::info("[Streamline] Submitted requested temporal reset for frame {}", currentFrame);
		temporalResetRequested = false;
	}
	frameConstantsValid = true;
}

sl::Result Streamline::SetTagsForCurrentFrame(const sl::ResourceTag* a_tags, uint32_t a_numTags, ID3D12GraphicsCommandList* a_commandList)
{
	CheckFrameConstants();
	if (dlssGBackendSelectedAtBoot)
		return SetTagsForLatchedFrame(a_tags, a_numTags, a_commandList);
	if (a_tags == nullptr || a_numTags == 0)
		return sl::Result::eErrorMissingInputParameter;
	if (slSetTag == nullptr)
		return sl::Result::eErrorFeatureMissing;
	return slSetTag(viewport, a_tags, a_numTags, a_commandList);
}

sl::Result Streamline::SetTagsForLatchedFrame(const sl::ResourceTag* a_tags, uint32_t a_numTags, ID3D12GraphicsCommandList* a_commandList)
{
	if (!frameConstantsValid || frameToken == nullptr)
		return sl::Result::eErrorCommonConstantsMissing;
	if (a_tags == nullptr || a_numTags == 0)
		return sl::Result::eErrorMissingInputParameter;
	if (slSetTagForFrame == nullptr)
		return sl::Result::eErrorFeatureMissing;

	return slSetTagForFrame(*frameToken, viewport, a_tags, a_numTags, a_commandList);
}

namespace
{
	/// (batch 43) The DLSSGStatus bits by name (sl_dlss_g.h), and the HDR one called out on its own
	/// line, since native HDR output is the first thing here that can trigger it.
	std::string DescribeDLSSGStatus(sl::DLSSGStatus a_status)
	{
		const auto bits = static_cast<uint32_t>(a_status);
		if (bits == 0)
			return "eOk";
		std::string text;
		auto add = [&](uint32_t a_bit, const char* a_name) {
			if (bits & a_bit) {
				if (!text.empty())
					text += " | ";
				text += a_name;
			}
		};
		add(static_cast<uint32_t>(sl::DLSSGStatus::eFailResolutionTooLow), "eFailResolutionTooLow");
		add(static_cast<uint32_t>(sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime), "eFailReflexNotDetectedAtRuntime");
		add(static_cast<uint32_t>(sl::DLSSGStatus::eFailHDRFormatNotSupported), "eFailHDRFormatNotSupported");
		add(static_cast<uint32_t>(sl::DLSSGStatus::eFailCommonConstantsInvalid), "eFailCommonConstantsInvalid");
		add(static_cast<uint32_t>(sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled), "eFailGetCurrentBackBufferIndexNotCalled");
		if (text.empty())
			text = std::format("0x{:X}", bits);
		return text;
	}

	void LogDLSSGStatusFailure(sl::DLSSGStatus a_status)
	{
		const auto bits = static_cast<uint32_t>(a_status);
		if (bits == 0)
			return;
		HDROutput::NoteDLSSGStatus(bits);
		if (bits & static_cast<uint32_t>(sl::DLSSGStatus::eFailHDRFormatNotSupported)) {
			const auto& swapChain = globals::features::upscaling.dx12SwapChain;
			logger::error("[HDR] DLSS-G reports eFailHDRFormatNotSupported: it does not accept swap-chain format {} with the current colour space (HDR output {})",
				static_cast<uint32_t>(swapChain.swapChainDesc.Format), HDROutput::IsOutputActive() ? "on" : "off");
		}
	}
}

void Streamline::FallbackDLSSG(const char* a_reason, sl::Result a_result, sl::DLSSGStatus a_status)
{
	if (dlssGStateFallbackApplied)
		return;

	dlssGStateFallbackApplied = true;
	dlssGRuntimeFaulted = true;
	dlssGActive = false;
	dlssGOptionsInitialized = true;
	LogDLSSGStatusFailure(a_status);
	logger::error("[Streamline] DLSS-G {} (result={}, status={} = {}); disabling DLSS-G for this session", a_reason, magic_enum::enum_name(a_result),
		static_cast<uint32_t>(a_status), DescribeDLSSGStatus(a_status));

	if (slDLSSGSetOptions != nullptr) {
		sl::DLSSGOptions options{};
		options.mode = sl::DLSSGMode::eOff;
		options.numFramesToGenerate = 1;
		options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
		const auto disableResult = slDLSSGSetOptions(viewport, options);
		if (disableResult != sl::Result::eOk)
			logger::critical("[Streamline] DLSS-G failed to enter the safe off state ({})", magic_enum::enum_name(disableResult));
	}
	dlssGAppliedFramesToGenerate = 1;
}

void Streamline::UpdateDLSSGFramesToGenerateMax(uint32_t a_reportedMax)
{
	// 0 is not a valid report (the minimum is 1, single-frame generation); treat it as "no
	// information" rather than as a ceiling below 2x.
	if (a_reportedMax == 0 || a_reportedMax == dlssGFramesToGenerateMax)
		return;

	logger::info("[Streamline] DLSS-G reports up to {} generated frames per rendered frame (up to {}x){}",
		a_reportedMax, a_reportedMax + 1,
		dlssGFramesToGenerateMax != 0 ? std::format(", was {}x", dlssGFramesToGenerateMax + 1) : std::string());
	dlssGFramesToGenerateMax = a_reportedMax;
	// A different ceiling means the system changed under us; a multiplier rejected under the
	// old one deserves one more attempt.
	dlssGRejectedFramesToGenerate = 0;
}

bool Streamline::RefreshDLSSGCapabilities()
{
	if (!IsDLSSGReady() || slDLSSGGetState == nullptr)
		return false;
	if (dlssGCapabilityQueryCooldown > 0) {
		--dlssGCapabilityQueryCooldown;
		return false;
	}

	sl::DLSSGState state{};
	const auto result = slDLSSGGetState(viewport, state, nullptr);
	if (result != sl::Result::eOk || state.numFramesToGenerateMax == 0) {
		// Not fatal and not judged: this only asks for the ceiling. Try again in about a second
		// of frames rather than on every Present.
		dlssGCapabilityQueryCooldown = 60;
		return false;
	}

	UpdateDLSSGFramesToGenerateMax(state.numFramesToGenerateMax);
	return true;
}

uint32_t Streamline::ResolveDLSSGFramesToGenerate(uint32_t a_requested) const
{
	// Never ask Streamline for more than it said it can do: its documented contract is that
	// numFramesToGenerate must lie in [1, numFramesToGenerateMax], and an out-of-range request
	// fails SetOptions. Before the first report the ceiling is unknown and only 2x is safe.
	uint32_t frames = std::max(a_requested, 1u);
	frames = std::min(frames, std::max(dlssGFramesToGenerateMax, 1u));
	if (frames > 1 && frames == dlssGRejectedFramesToGenerate)
		frames = 1;
	return frames;
}

bool Streamline::SetDLSSGMode(bool a_enable, bool a_retainResourcesWhenOff, uint32_t a_framesToGenerate)
{
	if (REL::Module::IsVR() || !dlssGBackendSelectedAtBoot || !featureDLSS_G || slDLSSGSetOptions == nullptr)
		return false;
	if (a_enable && !IsDLSSGReady())
		return false;
	if (a_enable && dlssGRuntimeFaulted)
		return false;

	const auto& swapChain = globals::features::upscaling.dx12SwapChain;
	if (a_enable && !swapChain.HasValidDLSSGInputExtent())
		return false;

	const auto inputWidth = swapChain.GetDLSSGInputWidth();
	const auto inputHeight = swapChain.GetDLSSGInputHeight();
	const bool inputExtentUnchanged = dlssGConfiguredInputWidth == inputWidth && dlssGConfiguredInputHeight == inputHeight;
	const bool outputExtentUnchanged = dlssGConfiguredOutputWidth == swapChain.swapChainDesc.Width &&
	                                   dlssGConfiguredOutputHeight == swapChain.swapChainDesc.Height;
	const bool retentionUnchanged = dlssGRetainResourcesWhenOff == a_retainResourcesWhenOff;
	// Off keeps the count last accepted rather than dropping to 1, so suspending for a menu and
	// resuming flips only the mode -- exactly what the fixed-2x path did, where both directions
	// always carried 1. The count only moves on an enabled call, and a change there is the whole
	// hot switch: one SetOptions with nothing reallocated on our side.
	uint32_t framesToGenerate = a_enable ? ResolveDLSSGFramesToGenerate(a_framesToGenerate) : dlssGAppliedFramesToGenerate;
	const bool framesUnchanged = dlssGAppliedFramesToGenerate == framesToGenerate;
	if (dlssGOptionsInitialized && dlssGActive == a_enable && retentionUnchanged && (!a_enable || (inputExtentUnchanged && outputExtentUnchanged && framesUnchanged)))
		return true;

	sl::DLSSGOptions options{};
	options.mode = a_enable ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
	options.numFramesToGenerate = framesToGenerate;
	// FinalColor already contains the scene plus UI. UI recomposition stays off
	// because this path only supplies an exact-format HUD-less snapshot.
	// This integration uses the fixed input ratio selected by the active
	// upscaler mode. Streamline explicitly advises against enabling its dynamic
	// resolution flag for fixed-ratio DLSS; the per-frame tags still need the
	// correct active input extent.
	options.flags = a_retainResourcesWhenOff ? sl::DLSSGFlags::eRetainResourcesWhenOff : sl::DLSSGFlags{};
	options.enableUserInterfaceRecomposition = sl::Boolean::eFalse;

	options.numBackBuffers = 2;
	options.mvecDepthWidth = inputWidth;
	options.mvecDepthHeight = inputHeight;
	options.colorWidth = swapChain.swapChainDesc.Width;
	options.colorHeight = swapChain.swapChainDesc.Height;
	options.colorBufferFormat = static_cast<uint32_t>(swapChain.swapChainDesc.Format);
	if (swapChain.motionVectorFrameGenerationShared12)
		options.mvecBufferFormat = static_cast<uint32_t>(swapChain.motionVectorFrameGenerationShared12->resource->GetDesc().Format);
	if (swapChain.depthBufferShared12)
		options.depthBufferFormat = static_cast<uint32_t>(swapChain.depthBufferShared12->resource->GetDesc().Format);
	if (swapChain.uiBufferWrapped)
		options.hudLessBufferFormat = static_cast<uint32_t>(swapChain.uiBufferWrapped->resource->GetDesc().Format);

	auto result = slDLSSGSetOptions(viewport, options);
	if (result != sl::Result::eOk && a_enable && framesToGenerate > 1) {
		// A refused multi-frame request must not cost the session its frame generation. The
		// ceiling check above already filters what Streamline documents as invalid, so this is
		// the case where the driver, the NGX runtime or an external unlock disagrees with the
		// reported ceiling. Drop to 2x -- the configuration this integration shipped with -- and
		// hold it there; only if 2x is refused too is the session-wide fallback below taken.
		logger::warn("[Streamline] DLSS-G rejected {}x frame generation ({}); retrying at 2x",
			framesToGenerate + 1, magic_enum::enum_name(result));
		dlssGRejectedFramesToGenerate = framesToGenerate;
		framesToGenerate = 1;
		options.numFramesToGenerate = framesToGenerate;
		result = slDLSSGSetOptions(viewport, options);
		if (result == sl::Result::eOk)
			logger::warn("[Streamline] DLSS-G is running at 2x; {}x stays disabled until it is selected again",
				dlssGRejectedFramesToGenerate + 1);
	} else if (result != sl::Result::eOk && !a_enable && framesToGenerate > 1) {
		// Turning generation off must not depend on the multiplier still being acceptable;
		// carry the 1 the off path always carried before multi-frame generation existed.
		logger::warn("[Streamline] DLSS-G refused eOff at {}x ({}); retrying with 2x options",
			framesToGenerate + 1, magic_enum::enum_name(result));
		framesToGenerate = 1;
		options.numFramesToGenerate = framesToGenerate;
		result = slDLSSGSetOptions(viewport, options);
	}
	if (result != sl::Result::eOk) {
		if (a_enable) {
			FallbackDLSSG("could not apply options", result, sl::DLSSGStatus::eOk);
		} else {
			logger::error("[Streamline] Could not disable DLSS-G ({})", magic_enum::enum_name(result));
		}
		return false;
	}

	dlssGActive = a_enable;
	dlssGOptionsInitialized = true;
	dlssGRetainResourcesWhenOff = a_retainResourcesWhenOff;
	dlssGAppliedFramesToGenerate = framesToGenerate;
	dlssGConfiguredInputWidth = inputWidth;
	dlssGConfiguredInputHeight = inputHeight;
	dlssGConfiguredOutputWidth = swapChain.swapChainDesc.Width;
	dlssGConfiguredOutputHeight = swapChain.swapChainDesc.Height;
	logger::info("[Streamline] DLSS-G mode {} at {}x with input extent {}x{} and output extent {}x{} (retainWhenOff={})",
		a_enable ? "enabled" : "disabled",
		framesToGenerate + 1,
		inputWidth,
		inputHeight,
		swapChain.swapChainDesc.Width,
		swapChain.swapChainDesc.Height,
		a_retainResourcesWhenOff);
	return true;
}

bool Streamline::GetDLSSGState(sl::DLSSGState& a_state)
{
	a_state = {};
	if (!IsDLSSGReady() || slDLSSGGetState == nullptr)
		return false;

	const auto result = slDLSSGGetState(viewport, a_state, nullptr);
	const bool failed = result != sl::Result::eOk || a_state.status != sl::DLSSGStatus::eOk;
	if (failed && dlssGActive && dlssGAppliedFramesToGenerate > 1) {
		// Same rule as a refused SetOptions: a multi-frame configuration that DLSS-G accepted but
		// then could not run is first taken back to 2x, which the next SetDLSSGMode applies on
		// the following Present. Only a failure at 2x reaches the session-wide fallback.
		LogDLSSGStatusFailure(a_state.status);
		logger::warn("[Streamline] DLSS-G reported a failure at {}x (result={}, status={} = {}); falling back to 2x",
			dlssGAppliedFramesToGenerate + 1, magic_enum::enum_name(result), static_cast<uint32_t>(a_state.status), DescribeDLSSGStatus(a_state.status));
		dlssGRejectedFramesToGenerate = dlssGAppliedFramesToGenerate;
		return false;
	}
	if (result != sl::Result::eOk) {
		FallbackDLSSG("state query failed", result, a_state.status);
		return false;
	}
	if (a_state.status != sl::DLSSGStatus::eOk) {
		FallbackDLSSG("reported an invalid runtime state", sl::Result::eOk, a_state.status);
		return false;
	}

	UpdateDLSSGFramesToGenerateMax(a_state.numFramesToGenerateMax);
	return true;
}

bool Streamline::TagDLSSGResources(const DLSSGFrameResources& a_resources, ID3D12GraphicsCommandList* a_commandList)
{
	if (REL::Module::IsVR() || !dlssGBackendSelectedAtBoot || !featureDLSS_G || dlssGRuntimeFaulted)
		return false;
	if (a_resources.depth == nullptr || a_resources.motionVectors == nullptr || a_resources.hudless == nullptr) {
		FallbackDLSSG("is missing a required frame resource", sl::Result::eErrorMissingInputParameter, sl::DLSSGStatus::eOk);
		return false;
	}

	// Required inputs are always submitted, including null tags when a frame
	// is invalid, so Streamline cannot retain stale resources across a menu or
	// loading transition.  UI alpha is preferred when both UI forms exist.
	sl::Resource depthResource{ sl::ResourceType::eTex2d, reinterpret_cast<void*>(a_resources.depth), static_cast<uint32_t>(a_resources.depthState) };
	sl::Resource motionVectorsResource{ sl::ResourceType::eTex2d, reinterpret_cast<void*>(a_resources.motionVectors), static_cast<uint32_t>(a_resources.motionVectorsState) };
	sl::Resource hudlessResource{ sl::ResourceType::eTex2d, reinterpret_cast<void*>(a_resources.hudless), static_cast<uint32_t>(a_resources.hudlessState) };
	sl::Resource uiColorAndAlphaResource{ sl::ResourceType::eTex2d, reinterpret_cast<void*>(a_resources.uiColorAndAlpha), static_cast<uint32_t>(a_resources.uiColorAndAlphaState) };
	sl::Resource uiAlphaResource{ sl::ResourceType::eTex2d, reinterpret_cast<void*>(a_resources.uiAlpha), static_cast<uint32_t>(a_resources.uiAlphaState) };

	sl::ResourceTag tags[] = {
		sl::ResourceTag{ a_resources.depth ? &depthResource : nullptr, sl::kBufferTypeDepth, sl::eValidUntilPresent, &a_resources.depthExtent },
		sl::ResourceTag{ a_resources.motionVectors ? &motionVectorsResource : nullptr, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent, &a_resources.motionVectorsExtent },
		sl::ResourceTag{ a_resources.hudless ? &hudlessResource : nullptr, sl::kBufferTypeHUDLessColor, sl::eValidUntilPresent, &a_resources.hudlessExtent },
		sl::ResourceTag{ a_resources.uiColorAndAlpha ? &uiColorAndAlphaResource : nullptr, sl::kBufferTypeUIColorAndAlpha, sl::eValidUntilPresent, &a_resources.uiColorAndAlphaExtent },
		sl::ResourceTag{ a_resources.uiAlpha ? &uiAlphaResource : nullptr, sl::kBufferTypeUIAlpha, sl::eValidUntilPresent, &a_resources.uiAlphaExtent }
	};

	// Present is entered after State::Reset has advanced the engine frame
	// counter. Reuse the token acquired at SimulationStart instead of asking
	// for the next frame here.
	const auto result = SetTagsForLatchedFrame(tags, _countof(tags), a_commandList);

	if (result != sl::Result::eOk) {
		FallbackDLSSG("could not tag its frame resources", result, sl::DLSSGStatus::eOk);
		return false;
	}

	if (dlssGTaggedInputWidth != a_resources.depthExtent.width ||
		dlssGTaggedInputHeight != a_resources.depthExtent.height) {
		dlssGTaggedInputWidth = a_resources.depthExtent.width;
		dlssGTaggedInputHeight = a_resources.depthExtent.height;
		logger::info("[Streamline] DLSS-G owns global Present tags at token {}: depth/mvec={}x{}, HUD-less={}x{}, lifecycle=valid-until-present",
			frameTokenIndex,
			a_resources.depthExtent.width,
			a_resources.depthExtent.height,
			a_resources.hudlessExtent.width,
			a_resources.hudlessExtent.height);
	}

	return true;
}

void Streamline::DestroyDLSSGResources(bool a_modeSwitch)
{
	if (a_modeSwitch && slDLSSGSetOptions != nullptr && featureDLSS_G) {
		sl::DLSSGOptions options{};
		options.mode = sl::DLSSGMode::eOff;
		options.numFramesToGenerate = 1;
		options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
		if (SL_FAILED(result, slDLSSGSetOptions(viewport, options)))
			logger::error("[Streamline] Could not disable DLSS-G before freeing resources ({})", magic_enum::enum_name(result));
	}

	dlssGActive = false;
	dlssGOptionsInitialized = false;
	dlssGAppliedFramesToGenerate = 1;
	if (slFreeResources != nullptr && featureDLSS_G) {
		if (SL_FAILED(result, slFreeResources(sl::kFeatureDLSS_G, viewport)))
			logger::error("[Streamline] Could not free DLSS-G resources ({})", magic_enum::enum_name(result));
	}
}

bool Streamline::GetReflexState(sl::ReflexState& a_state)
{
	a_state = {};
	if (!featureReflex || !slReflexGetState)
		return false;

	if (SL_FAILED(result, slReflexGetState(a_state))) {
		logger::error("[Streamline] Could not query Reflex state ({})", magic_enum::enum_name(result));
		return false;
	}
	lastLowLatencyAvailable = a_state.lowLatencyAvailable ? 1 : 0;
	lastLatencyReportAvailable = a_state.latencyReportAvailable ? 1 : 0;
	return true;
}

bool Streamline::SetReflexOptions(sl::ReflexMode a_mode, uint32_t a_frameLimitUs)
{
	if (!featureReflex || !slReflexSetOptions)
		return false;

	sl::ReflexOptions options{};
	options.mode = a_mode;
	options.frameLimitUs = a_frameLimitUs;
	if (SL_FAILED(result, slReflexSetOptions(options))) {
		logger::error("[Streamline] Could not set Reflex options ({})", magic_enum::enum_name(result));
		return false;
	}
	return true;
}

bool Streamline::ReflexSleep()
{
	if (!featureReflex || !slReflexSleep)
		return false;

	// Reflex sleep and PCL markers are bound to the frame token but do not
	// require rendering constants.  Requiring frameConstantsValid here made
	// every SimulationStart call fail immediately after BeginFrameToken().
	if (frameToken == nullptr)
		return false;

	if (SL_FAILED(result, slReflexSleep(*frameToken))) {
		if (!reflexSleepFailureLogged) {
			logger::error("[Streamline] Reflex sleep failed for frame {} ({})", frameTokenIndex, magic_enum::enum_name(result));
			reflexSleepFailureLogged = true;
		}
		return false;
	}
	return true;
}

bool Streamline::GetPCLState(sl::PCLState& a_state)
{
	a_state = {};
	if (!featurePCL || !slPCLGetState)
		return false;

	if (SL_FAILED(result, slPCLGetState(a_state))) {
		logger::error("[Streamline] Could not query PCL state ({})", magic_enum::enum_name(result));
		return false;
	}
	return true;
}

bool Streamline::SetPCLMarker(sl::PCLMarker a_marker)
{
	if (!featurePCL || !slPCLSetMarker)
		return false;

	if (frameToken == nullptr)
		return false;

	if (SL_FAILED(result, slPCLSetMarker(a_marker, *frameToken))) {
		if (!pclMarkerFailureLogged) {
			logger::error("[Streamline] PCL marker {} failed for frame {} ({})", static_cast<uint32_t>(a_marker), frameTokenIndex, magic_enum::enum_name(result));
			pclMarkerFailureLogged = true;
		}
		return false;
	}
	return true;
}

void Streamline::SetDLSSOptions()
{
	sl::DLSSOptions dlssOptions{};

	// Map quality mode to DLSS mode
	uint32_t qualityMode = globals::features::upscaling.settings.qualityMode;
	switch (qualityMode) {
	case 1:
		dlssOptions.mode = sl::DLSSMode::eMaxQuality;
		break;
	case 2:
		dlssOptions.mode = sl::DLSSMode::eBalanced;
		break;
	case 3:
		dlssOptions.mode = sl::DLSSMode::eMaxPerformance;
		break;
	case 4:
		dlssOptions.mode = sl::DLSSMode::eUltraPerformance;
		break;
	default:
		dlssOptions.mode = sl::DLSSMode::eDLAA;
		break;
	}

	auto state = globals::state;

	dlssOptions.outputWidth = (uint)state->screenSize.x;
	dlssOptions.outputHeight = (uint)state->screenSize.y;
	dlssOptions.colorBuffersHDR = sl::Boolean::eTrue;
	dlssOptions.useAutoExposure = sl::Boolean::eTrue;

	dlssOptions.preExposure = 1.0f;
	dlssOptions.sharpness = 0.0f;

	auto& settings = globals::features::upscaling.settings;
	auto requestedPresetValue = settings.DLSSPreset;
	if (requestedPresetValue >= kDLSSModelPresetCount) {
		logger::warn("[Streamline] Invalid DLSS model preset {}, falling back to K", requestedPresetValue);
		requestedPresetValue = static_cast<uint>(DLSSModelPreset::kK);
		settings.DLSSPreset = requestedPresetValue;
	}

	const auto requestedPreset = static_cast<DLSSModelPreset>(requestedPresetValue);
	static uint lastLoggedPreset = kUnreportedPreset;
	static uint lastRequestedPreset = kUnreportedPreset;
	static uint sessionFallbackPreset = kUnreportedPreset;
	static uint lastFallbackFailurePreset = kUnreportedPreset;
	if (lastRequestedPreset != requestedPresetValue) {
		lastRequestedPreset = requestedPresetValue;
		sessionFallbackPreset = kUnreportedPreset;
		lastFallbackFailurePreset = kUnreportedPreset;
	}

	if (sessionFallbackPreset == requestedPresetValue) {
		SetAllDLSSPresets(dlssOptions, sl::DLSSPreset::ePresetK);
		const auto fallbackResult = slDLSSSetOptions(viewport, dlssOptions);
		if (fallbackResult != sl::Result::eOk && lastFallbackFailurePreset != requestedPresetValue) {
			logger::critical("[Streamline] DLSS SR K fallback failed ({})", magic_enum::enum_name(fallbackResult));
			lastFallbackFailurePreset = requestedPresetValue;
		}
		return;
	}

	if (requestedPreset == DLSSModelPreset::kSDKDocumentedMapping)
		SetSDKDocumentedDLSSPresets(dlssOptions);
	else
		SetAllDLSSPresets(dlssOptions, GetForcedDLSSPreset(requestedPreset));

	const auto result = slDLSSSetOptions(viewport, dlssOptions);
	if (result == sl::Result::eOk) {
		if (lastLoggedPreset != requestedPresetValue) {
			logger::info("[Streamline] Requested DLSS SR model preset: {}", GetDLSSModelPresetName(requestedPreset));
			lastLoggedPreset = requestedPresetValue;
		}
		return;
	}

	logger::error("[Streamline] slDLSSSetOptions failed for requested DLSS SR model preset {} ({}); using K fallback for this selection", GetDLSSModelPresetName(requestedPreset), magic_enum::enum_name(result));
	sessionFallbackPreset = requestedPresetValue;
	SetAllDLSSPresets(dlssOptions, sl::DLSSPreset::ePresetK);
	const auto fallbackResult = slDLSSSetOptions(viewport, dlssOptions);
	if (fallbackResult == sl::Result::eOk) {
		logger::warn("[Streamline] DLSS SR is using K fallback after the requested {} preset failed", GetDLSSModelPresetName(requestedPreset));
	} else {
		logger::critical("[Streamline] DLSS SR K fallback failed ({})", magic_enum::enum_name(fallbackResult));
		lastFallbackFailurePreset = requestedPresetValue;
	}
}

void Streamline::Upscale(ID3D12Resource* a_inputColorTexture,
	ID3D12Resource* a_motionVectorTexture,
	ID3D12Resource* a_depthTexture,
	ID3D12Resource* a_reactiveMask,
	ID3D12Resource* a_transparencyCompositionMask,
	ID3D12Resource* a_outputTexture,
	ID3D12GraphicsCommandList* a_commandList)
{
	Util::FrameTrace::Lap lap;  // (46o) sl_setopts / sl_evaluate
	CheckFrameConstants();
	SetDLSSOptions();
	lap.Mark(Util::FrameTrace::Split::SlSetOpts);
	if (!frameConstantsValid || frameToken == nullptr) {
		logger::error("[Streamline] Cannot evaluate DLSS SR without constants and a frame token");
		return;
	}

	auto state = globals::state;
	auto screenSize = state->screenSize;
	auto renderSize = Util::ConvertToDynamic(screenSize);

	sl::Extent lowResExtent{ 0, 0, (uint)renderSize.x, (uint)renderSize.y };
	sl::Extent fullExtent{ 0, 0, (uint)screenSize.x, (uint)screenSize.y };

	sl::Resource colorIn = { sl::ResourceType::eTex2d, a_inputColorTexture, 0 };
	sl::Resource colorOut = { sl::ResourceType::eTex2d, a_outputTexture, 0 };
	sl::Resource depth = { sl::ResourceType::eTex2d, a_depthTexture, 0 };
	sl::Resource mvec = { sl::ResourceType::eTex2d, a_motionVectorTexture, 0 };
	sl::Resource reactiveMask = { sl::ResourceType::eTex2d, a_reactiveMask, 0 };
	sl::Resource transparencyCompositionMask = { sl::ResourceType::eTex2d, a_transparencyCompositionMask, 0 };

	sl::ResourceTag colorInTag = sl::ResourceTag{ &colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &lowResExtent };
	sl::ResourceTag colorOutTag = sl::ResourceTag{ &colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &fullExtent };
	sl::ResourceTag depthTag = sl::ResourceTag{ &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &lowResExtent };
	sl::ResourceTag mvecTag = sl::ResourceTag{ &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &lowResExtent };
	sl::ResourceTag reactiveMaskTag = sl::ResourceTag{ &reactiveMask, sl::kBufferTypeBiasCurrentColorHint, sl::ResourceLifecycle::eValidUntilPresent, &lowResExtent };
	sl::ResourceTag transparencyCompositionMaskTag = sl::ResourceTag{ &transparencyCompositionMask, sl::kBufferTypeTransparencyHint, sl::ResourceLifecycle::eValidUntilPresent, &lowResExtent };
	sl::ViewportHandle view(viewport);
	if (dlssGBackendSelectedAtBoot) {
		// DLSS-G owns the global Present tag namespace. Supplying every DLSS SR
		// input locally prevents SR depth/MV tags from leaking into that namespace.
		const sl::BaseStructure* inputs[] = {
			&view,
			&colorInTag,
			&colorOutTag,
			&depthTag,
			&mvecTag,
			&reactiveMaskTag,
			&transparencyCompositionMaskTag
		};
		const auto result = slEvaluateFeature(sl::kFeatureDLSS, *frameToken, inputs, _countof(inputs), a_commandList);
		lap.Mark(Util::FrameTrace::Split::SlEvaluate);
		if (result == sl::Result::eWarnOutOfVRAM)  // (46o) M3: the evaluation ran; the VRAM budget is used up
			Util::FrameTrace::OnOutOfVram();
		if (result != sl::Result::eOk) {
			logger::error("[Streamline] DLSS SR local-tag evaluation failed ({})", magic_enum::enum_name(result));
			return;
		}
		if (!dlssSRLocalTagsLogged) {
			logger::info("[Streamline] DLSS SR uses evaluate-local tags at token {}: input={}x{}, output={}x{}; global tags remain reserved for DLSS-G",
				frameTokenIndex,
				lowResExtent.width,
				lowResExtent.height,
				fullExtent.width,
				fullExtent.height);
			dlssSRLocalTagsLogged = true;
		}
		return;
	}

	sl::ResourceTag resourceTags[] = { colorInTag, colorOutTag, depthTag, mvecTag, reactiveMaskTag, transparencyCompositionMaskTag };
	if (SL_FAILED(result, SetTagsForCurrentFrame(resourceTags, _countof(resourceTags), a_commandList))) {
		logger::error("[Streamline] Failed to set DLSS SR resource tags ({})", magic_enum::enum_name(result));
		return;
	}
	const sl::BaseStructure* inputs[] = { &view };
	const auto evalResult = slEvaluateFeature(sl::kFeatureDLSS, *frameToken, inputs, _countof(inputs), a_commandList);
	lap.Mark(Util::FrameTrace::Split::SlEvaluate);
	if (evalResult == sl::Result::eWarnOutOfVRAM)  // (46o) M3
		Util::FrameTrace::OnOutOfVram();
	if (evalResult != sl::Result::eOk)
		logger::error("[Streamline] DLSS SR evaluation failed ({})", magic_enum::enum_name(evalResult));
}

void Streamline::SetDLSSRROptions()
{
	sl::DLSSDOptions dlssdOptions{};

	// Map quality mode to DLSS mode
	uint32_t qualityMode = globals::features::upscaling.settings.qualityMode;
	switch (qualityMode) {
	case 1:
		dlssdOptions.mode = sl::DLSSMode::eMaxQuality;
		break;
	case 2:
		dlssdOptions.mode = sl::DLSSMode::eBalanced;
		break;
	case 3:
		dlssdOptions.mode = sl::DLSSMode::eMaxPerformance;
		break;
	case 4:
		dlssdOptions.mode = sl::DLSSMode::eUltraPerformance;
		break;
	default:
		dlssdOptions.mode = sl::DLSSMode::eDLAA;
		break;
	}

	auto worldToCameraView = globals::game::frameBufferCached.GetCameraView().Transpose();
	auto cameraViewToWorld = globals::game::frameBufferCached.GetCameraViewInverse().Transpose();

	auto state = globals::state;

	dlssdOptions.outputWidth = (uint)state->screenSize.x;
	dlssdOptions.outputHeight = (uint)state->screenSize.y;
	dlssdOptions.colorBuffersHDR = sl::Boolean::eTrue;
	dlssdOptions.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
	dlssdOptions.alphaUpscalingEnabled = sl::Boolean::eFalse;

	dlssdOptions.worldToCameraView = sl::float4x4{
		sl::float4{ worldToCameraView._11, worldToCameraView._12, worldToCameraView._13, worldToCameraView._14 },
		sl::float4{ worldToCameraView._21, worldToCameraView._22, worldToCameraView._23, worldToCameraView._24 },
		sl::float4{ worldToCameraView._31, worldToCameraView._32, worldToCameraView._33, worldToCameraView._34 },
		sl::float4{ worldToCameraView._41, worldToCameraView._42, worldToCameraView._43, worldToCameraView._44 }
	};
	dlssdOptions.cameraViewToWorld = sl::float4x4{
		sl::float4{ cameraViewToWorld._11, cameraViewToWorld._12, cameraViewToWorld._13, cameraViewToWorld._14 },
		sl::float4{ cameraViewToWorld._21, cameraViewToWorld._22, cameraViewToWorld._23, cameraViewToWorld._24 },
		sl::float4{ cameraViewToWorld._31, cameraViewToWorld._32, cameraViewToWorld._33, cameraViewToWorld._34 },
		sl::float4{ cameraViewToWorld._41, cameraViewToWorld._42, cameraViewToWorld._43, cameraViewToWorld._44 }
	};
	const auto selectedPreset = static_cast<DLSSDModelPreset>(globals::features::upscaling.settings.DLSSDPreset);
	const auto rrPreset = GetForcedDLSSDPreset(selectedPreset);
	SetAllDLSSDPresets(dlssdOptions, rrPreset);

	// This runs every frame, so log only on change. It is the only place the requested model is
	// observable: the weights live in nvngx_dlssd.dll, and asking for a preset that DLL has no
	// weights for falls back to its default without reporting anything. Read this line together
	// with the "nvngx_dlssd.dll version" line Streamline logs at startup. The shipped DLL is
	// 310.9.1, which has the weights for presets D, E and F.
	static uint loggedPreset = static_cast<uint>(DLSSDModelPreset::kCount);
	if (loggedPreset != globals::features::upscaling.settings.DLSSDPreset) {
		loggedPreset = globals::features::upscaling.settings.DLSSDPreset;
		logger::info("[DLSS RR] Requested model preset {} = sl::DLSSDPreset({}) for all five quality modes",
			GetDLSSDModelPresetName(selectedPreset),
			static_cast<uint>(rrPreset));
	}

	if (SL_FAILED(result, slDLSSDSetOptions(viewport, dlssdOptions))) {
		logger::critical("[DLSS RR] Could not set DLSS RR options");
		return;
	}
}

void Streamline::RayReconstruction(ID3D12Resource* a_inputColorTexture,
	ID3D12Resource* a_motionVectorTexture,
	ID3D12Resource* a_depthTexture,
	ID3D12Resource* a_albedoTexture,
	ID3D12Resource* a_reflectanceTexture,
	ID3D12Resource* a_normalRoughness,
	ID3D12Resource* a_specularHitDistance,
	ID3D12Resource* a_colorBeforeTransparency,
	ID3D12Resource* a_sssGuide,
	ID3D12Resource* a_outputTexture,
	ID3D12GraphicsCommandList* a_commandList)
{
	if (!featureDLSS_RR)
		return;

	logger::debug("[DLSS RR] Starting Ray Reconstruction");

	Util::FrameTrace::Lap lap;  // (46o) sl_setopts / sl_evaluate
	CheckFrameConstants();
	logger::debug("[DLSS RR] Frame constants set");
	if (!frameConstantsValid || frameToken == nullptr) {
		logger::error("[DLSS RR] Cannot evaluate without constants and a frame token");
		return;
	}
	SetDLSSRROptions();
	logger::debug("[DLSS RR] DLSS RR options set");
	lap.Mark(Util::FrameTrace::Split::SlSetOpts);

	auto state = globals::state;

	auto screenSize = state->screenSize;
	auto renderSize = Util::ConvertToDynamic(screenSize);

	sl::Extent inputExtent{ 0, 0, (uint)renderSize.x, (uint)renderSize.y };
	sl::Extent outputExtent{ 0, 0, (uint)screenSize.x, (uint)screenSize.y };

	sl::Resource colorIn = { sl::ResourceType::eTex2d, a_inputColorTexture, 0 };
	sl::Resource colorOut = { sl::ResourceType::eTex2d, a_outputTexture, 0 };
	sl::Resource depth = { sl::ResourceType::eTex2d, a_depthTexture, 0 };
	sl::Resource mvec = { sl::ResourceType::eTex2d, a_motionVectorTexture, 0 };
	sl::Resource diffuseAlbedo = { sl::ResourceType::eTex2d, a_albedoTexture, 0 };
	sl::Resource specularAlbedo = { sl::ResourceType::eTex2d, a_reflectanceTexture, 0 };
	sl::Resource normalRoughness = { sl::ResourceType::eTex2d, a_normalRoughness, 0 };
	sl::Resource specHitDistance = { sl::ResourceType::eTex2d, a_specularHitDistance, 0 };
	sl::Resource colorBeforeTransparency = { sl::ResourceType::eTex2d, a_colorBeforeTransparency, 0 };
	sl::Resource sssGuide = { sl::ResourceType::eTex2d, a_sssGuide, 0 };

	sl::ResourceTag colorInTag = sl::ResourceTag{ &colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &inputExtent };
	sl::ResourceTag colorOutTag = sl::ResourceTag{ &colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &outputExtent };
	sl::ResourceTag depthTag = sl::ResourceTag{ &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent };
	sl::ResourceTag mvecTag = sl::ResourceTag{ &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent };
	sl::ResourceTag diffuseAlbedoTag = sl::ResourceTag{ &diffuseAlbedo, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent };
	sl::ResourceTag specularAlbedoTag = sl::ResourceTag{ &specularAlbedo, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent };
	sl::ResourceTag normalRoughnessTag = sl::ResourceTag{ &normalRoughness, sl::kBufferTypeNormalRoughness, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent };
	sl::ResourceTag specHitDistanceTag = sl::ResourceTag{ &specHitDistance, sl::kBufferTypeSpecularHitDistance, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent };
	sl::ResourceTag colorBeforeTransparencyTag = sl::ResourceTag{ &colorBeforeTransparency, sl::kBufferTypeColorBeforeTransparency, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent };
	sl::ResourceTag sssGuideTag = sl::ResourceTag{ &sssGuide, sl::kBufferTypeScreenSpaceSubsurfaceScatteringGuide, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent };

	sl::ViewportHandle view(viewport);
	if (dlssGBackendSelectedAtBoot) {
		const sl::BaseStructure* inputs[] = {
			&view,
			&colorInTag,
			&colorOutTag,
			&depthTag,
			&mvecTag,
			&diffuseAlbedoTag,
			&specularAlbedoTag,
			&normalRoughnessTag,
			&specHitDistanceTag,
			&colorBeforeTransparencyTag,
			&sssGuideTag
		};
		const auto result = slEvaluateFeature(sl::kFeatureDLSS_RR, *frameToken, inputs, _countof(inputs), a_commandList);
		lap.Mark(Util::FrameTrace::Split::SlEvaluate);
		if (result == sl::Result::eWarnOutOfVRAM)  // (46o) M3
			Util::FrameTrace::OnOutOfVram();
		if (result != sl::Result::eOk) {
			logger::error("[DLSS RR] Local-tag evaluation failed ({})", magic_enum::enum_name(result));
			return;
		}
		if (!dlssRRLocalTagsLogged) {
			logger::info("[DLSS RR] Uses evaluate-local tags at token {}: input={}x{}, output={}x{}; global tags remain reserved for DLSS-G",
				frameTokenIndex,
				inputExtent.width,
				inputExtent.height,
				outputExtent.width,
				outputExtent.height);
			dlssRRLocalTagsLogged = true;
		}
		return;
	}

	sl::ResourceTag resourceTags[] = { colorInTag, colorOutTag, depthTag, mvecTag, diffuseAlbedoTag, specularAlbedoTag, normalRoughnessTag, specHitDistanceTag, colorBeforeTransparencyTag, sssGuideTag };
	if (SL_FAILED(result, SetTagsForCurrentFrame(resourceTags, _countof(resourceTags), a_commandList))) {
		logger::error("[DLSS RR] Failed to set DLSS RR tags, error code: {}", (int)result);
		return;
	}
	const sl::BaseStructure* inputs[] = { &view };
	const auto evalResult = slEvaluateFeature(sl::kFeatureDLSS_RR, *frameToken, inputs, _countof(inputs), a_commandList);
	lap.Mark(Util::FrameTrace::Split::SlEvaluate);
	if (evalResult == sl::Result::eWarnOutOfVRAM)  // (46o) M3
		Util::FrameTrace::OnOutOfVram();
	if (evalResult != sl::Result::eOk) {
		logger::error("[DLSS RR] Failed to evaluate DLSS RR feature, error code: {}", (int)evalResult);
		return;
	}
	logger::debug("[DLSS RR] slEvaluateFeature executed successfully, output texture updated");
	logger::debug("[DLSS RR] slEvaluateFeature completed");
}

float2 Streamline::GetInputResolutionScale(uint32_t outputWidth, uint32_t outputHeight, uint32_t qualityMode)
{
	sl::DLSSMode dlssMode;
	switch (qualityMode) {
	case 1:
		dlssMode = sl::DLSSMode::eMaxQuality;
		break;
	case 2:
		dlssMode = sl::DLSSMode::eBalanced;
		break;
	case 3:
		dlssMode = sl::DLSSMode::eMaxPerformance;
		break;
	case 4:
		dlssMode = sl::DLSSMode::eUltraPerformance;
		break;
	default:
		dlssMode = sl::DLSSMode::eDLAA;
		break;
	}

	sl::DLSSOptions dlssOptions{};
	dlssOptions.mode = dlssMode;
	dlssOptions.outputWidth = outputWidth;
	dlssOptions.outputHeight = outputHeight;

	sl::DLSSOptimalSettings optimalSettings{};
	sl::Result result = slDLSSGetOptimalSettings(dlssOptions, optimalSettings);
	if (result != sl::Result::eOk) {
		logger::critical("[Streamline] Failed to get DLSS optimal settings, error code: {}", (int)result);
		return { 1.0f, 1.0f };
	}

	// Keep a fixed quality-mode ratio across pause and camera domains. Changing
	// to renderWidthMin while paused was an undeclared dynamic-resolution event
	// for DLSS-G and invalidated the retained Present history.
	return {
		(float)optimalSettings.optimalRenderWidth / (float)outputWidth,
		(float)optimalSettings.optimalRenderHeight / (float)outputHeight
	};
}

float2 Streamline::GetInputResolutionScaleRR(uint32_t outputWidth, uint32_t outputHeight, uint32_t qualityMode)
{
	logger::debug("[DLSS RR] Getting input resolution scale for output {}x{} and quality mode {}", outputWidth, outputHeight, qualityMode);
	sl::DLSSMode dlssMode;
	switch (qualityMode) {
	case 1:
		dlssMode = sl::DLSSMode::eMaxQuality;
		break;
	case 2:
		dlssMode = sl::DLSSMode::eBalanced;
		break;
	case 3:
		dlssMode = sl::DLSSMode::eMaxPerformance;
		break;
	case 4:
		dlssMode = sl::DLSSMode::eUltraPerformance;
		break;
	default:
		dlssMode = sl::DLSSMode::eDLAA;
		break;
	}

	sl::DLSSDOptions dlssdOptions{};
	dlssdOptions.mode = dlssMode;
	dlssdOptions.outputWidth = outputWidth;
	dlssdOptions.outputHeight = outputHeight;

	sl::DLSSDOptimalSettings optimalSettings{};
	sl::Result result = slDLSSDGetOptimalSettings(dlssdOptions, optimalSettings);
	if (result != sl::Result::eOk) {
		logger::critical("[Streamline] Failed to get DLSS RR optimal settings, error code: {}", (int)result);
		return { 1.0f, 1.0f };
	}

	return {
		(float)optimalSettings.optimalRenderWidth / (float)outputWidth,
		(float)optimalSettings.optimalRenderHeight / (float)outputHeight
	};
}

/**
 * @brief Releases DLSS resources and disables DLSS for the current viewport.
 *
 * Sets the DLSS mode to off and frees all DLSS-related resources associated with the viewport.
 */
void Streamline::DestroyDLSSResources(bool modeSwitch)
{
	if (modeSwitch) {
		sl::DLSSOptions dlssOptions{};
		dlssOptions.mode = sl::DLSSMode::eOff;
		slDLSSSetOptions(viewport, dlssOptions);
	}
	slFreeResources(sl::kFeatureDLSS, viewport);
}

void Streamline::DestroyDLSSRRResources(bool modeSwitch)
{
	logger::debug("[Streamline] Destroying DLSS-RR resources");
	if (modeSwitch) {
		sl::DLSSDOptions dlssdOptions{};
		dlssdOptions.mode = sl::DLSSMode::eOff;
		slDLSSDSetOptions(viewport, dlssdOptions);
	}
	slFreeResources(sl::kFeatureDLSS_RR, viewport);
}
