#include "WndProcPseudoHandleFix.h"

#include <intrin.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#pragma intrinsic(_ReturnAddress)

namespace WndProcPseudoHandleFix
{
	namespace
	{
		// Which CallWindowProc flavour a forwarder must use. A pseudo-handle is
		// handed to the caller of the matching API flavour, and that caller will
		// invoke the forwarder with messages in its own character set.
		enum Charset : std::size_t
		{
			kAnsi = 0,
			kWide = 1,
			kCharsetCount
		};

		constexpr std::array<const char*, kCharsetCount> kSetNames{ "SetWindowLongPtrA", "SetWindowLongPtrW" };
		constexpr std::array<const char*, kCharsetCount> kGetNames{ "GetWindowLongPtrA", "GetWindowLongPtrW" };

		// Forwarder slots per charset. Distinct pseudo-handles only exist per
		// (window procedure, charset) pair, so a handful are used in practice.
		constexpr std::size_t kSlotCount = 64;

		// 0 = free. Slots are claimed once and never released, so a forwarder
		// address handed out stays valid for the lifetime of the process.
		std::array<std::array<std::atomic<std::uintptr_t>, kSlotCount>, kCharsetCount> g_slots{};

		template <std::size_t C, std::size_t N>
		LRESULT CALLBACK Forwarder(HWND a_hwnd, UINT a_msg, WPARAM a_wParam, LPARAM a_lParam)
		{
			const auto target = reinterpret_cast<WNDPROC>(g_slots[C][N].load(std::memory_order_acquire));
			if constexpr (C == kAnsi) {
				return target ? CallWindowProcA(target, a_hwnd, a_msg, a_wParam, a_lParam) : DefWindowProcA(a_hwnd, a_msg, a_wParam, a_lParam);
			} else {
				return target ? CallWindowProcW(target, a_hwnd, a_msg, a_wParam, a_lParam) : DefWindowProcW(a_hwnd, a_msg, a_wParam, a_lParam);
			}
		}

		template <std::size_t C, std::size_t... N>
		constexpr std::array<WNDPROC, sizeof...(N)> MakeForwarders(std::index_sequence<N...>)
		{
			return { &Forwarder<C, N>... };
		}

		const std::array<std::array<WNDPROC, kSlotCount>, kCharsetCount> g_forwarders{
			MakeForwarders<kAnsi>(std::make_index_sequence<kSlotCount>{}),
			MakeForwarders<kWide>(std::make_index_sequence<kSlotCount>{})
		};

		using GetWindowLongPtr_t = LONG_PTR(WINAPI*)(HWND, int);
		using SetWindowLongPtr_t = LONG_PTR(WINAPI*)(HWND, int, LONG_PTR);

		std::array<GetWindowLongPtr_t, kCharsetCount> g_originalGet{};
		std::array<SetWindowLongPtr_t, kCharsetCount> g_originalSet{};

		std::wstring g_windowsDirectory;  // lower-case, with trailing backslash

		bool IsExecutableAddress(std::uintptr_t a_address)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (VirtualQuery(reinterpret_cast<LPCVOID>(a_address), &mbi, sizeof(mbi)) == 0) {
				return false;
			}
			if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
				return false;
			}
			constexpr DWORD kExecute = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
			return (mbi.Protect & kExecute) != 0;
		}

		// Cheap shape test first (runs on every GWLP_WNDPROC query), then the
		// VirtualQuery confirmation so a real procedure is never wrapped.
		bool IsPseudoHandle(std::uintptr_t a_value)
		{
			const auto low = static_cast<std::uint32_t>(a_value);
			const auto high = static_cast<std::uint32_t>(a_value >> 32);
			if (high != 0 || (low & 0xFFFF0000u) != 0xFFFF0000u) {
				return false;
			}
			return !IsExecutableAddress(a_value);
		}

		struct CallerInfo
		{
			std::uintptr_t moduleBase = 0;
			std::string name = "<unknown module>";
			bool isSystem = false;
		};

		std::string WideToUtf8(const std::wstring& a_text)
		{
			if (a_text.empty()) {
				return {};
			}
			const int size = WideCharToMultiByte(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), nullptr, 0, nullptr, nullptr);
			if (size <= 0) {
				return "<unicode conversion error>";
			}
			std::string result(static_cast<std::size_t>(size), '\0');
			WideCharToMultiByte(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), result.data(), size, nullptr, nullptr);
			return result;
		}

		CallerInfo IdentifyCaller(void* a_returnAddress)
		{
			CallerInfo info;
			HMODULE module = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					static_cast<LPCWSTR>(a_returnAddress), &module) ||
				!module) {
				return info;  // dynamically generated code: treat as third party
			}
			info.moduleBase = reinterpret_cast<std::uintptr_t>(module);

			std::wstring path(1024, L'\0');
			const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
			if (length == 0) {
				return info;
			}
			path.resize(length);

			std::wstring lower = path;
			CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));
			info.isSystem = !g_windowsDirectory.empty() && lower.starts_with(g_windowsDirectory);

			const auto slash = path.find_last_of(L"\\/");
			info.name = WideToUtf8(slash == std::wstring::npos ? path : path.substr(slash + 1));
			return info;
		}

		// Returns the forwarder bound to a_handle, claiming a free slot if needed.
		// Lock-free; the same handle always maps to the same forwarder because
		// slots are filled in order and never change once claimed.
		WNDPROC AcquireForwarder(Charset a_charset, std::uintptr_t a_handle, std::size_t& a_slot, bool& a_isNew)
		{
			auto& slots = g_slots[a_charset];
			for (std::size_t i = 0; i < kSlotCount; ++i) {
				std::uintptr_t current = slots[i].load(std::memory_order_acquire);
				if (current == 0) {
					if (slots[i].compare_exchange_strong(current, a_handle, std::memory_order_acq_rel, std::memory_order_acquire)) {
						a_slot = i;
						a_isNew = true;
						return g_forwarders[a_charset][i];
					}
					// Lost the race; `current` now holds the winner's handle.
				}
				if (current == a_handle) {
					a_slot = i;
					a_isNew = false;
					return g_forwarders[a_charset][i];
				}
			}
			return nullptr;
		}

		// Log each (caller module, API, handle) combination once so a plugin that
		// polls GetWindowLongPtr every frame does not flood the log.
		bool ShouldLog(std::uintptr_t a_moduleBase, const char* a_api, std::uintptr_t a_handle)
		{
			struct Key
			{
				std::uintptr_t moduleBase;
				const char* api;
				std::uintptr_t handle;
			};
			static std::mutex mutex;
			static std::vector<Key> seen;

			std::scoped_lock lock(mutex);
			for (const auto& key : seen) {
				if (key.moduleBase == a_moduleBase && key.api == a_api && key.handle == a_handle) {
					return false;
				}
			}
			seen.push_back({ a_moduleBase, a_api, a_handle });
			return true;
		}

		LONG_PTR Filter(Charset a_charset, const char* a_api, LONG_PTR a_result, void* a_returnAddress)
		{
			const auto handle = static_cast<std::uintptr_t>(a_result);
			if (!IsPseudoHandle(handle)) {
				return a_result;
			}

			// Callers may inspect GetLastError() afterwards; keep what user32 set.
			const DWORD lastError = GetLastError();
			LONG_PTR result = a_result;

			const auto caller = IdentifyCaller(a_returnAddress);
			const auto callerOffset = caller.moduleBase ? reinterpret_cast<std::uintptr_t>(a_returnAddress) - caller.moduleBase : reinterpret_cast<std::uintptr_t>(a_returnAddress);

			if (caller.isSystem) {
				// OS components know how to use pseudo-handles; leave them untouched.
				if (ShouldLog(caller.moduleBase, a_api, handle)) {
					logger::debug("[WndProc Fix] {} returned pseudo-handle {:#x} to system module {}, left unchanged", a_api, handle, caller.name);
				}
			} else {
				std::size_t slot = 0;
				bool isNew = false;
				if (const auto forwarder = AcquireForwarder(a_charset, handle, slot, isNew)) {
					result = reinterpret_cast<LONG_PTR>(forwarder);
					if (ShouldLog(caller.moduleBase, a_api, handle)) {
						logger::info("[WndProc Fix] {} returned pseudo-handle {:#x} to {}+{:#x}; replaced with callable forwarder #{}{}",
							a_api, handle, caller.name, callerOffset, slot, isNew ? "" : " (reused)");
					}
				} else if (ShouldLog(caller.moduleBase, a_api, handle)) {
					logger::error("[WndProc Fix] {} returned pseudo-handle {:#x} to {}+{:#x}, but all {} forwarder slots are in use; returned unchanged",
						a_api, handle, caller.name, callerOffset, kSlotCount);
				}
			}

			SetLastError(lastError);
			return result;
		}

		LONG_PTR WINAPI Hook_GetWindowLongPtrA(HWND a_hwnd, int a_index)
		{
			const LONG_PTR result = g_originalGet[kAnsi](a_hwnd, a_index);
			return a_index == GWLP_WNDPROC ? Filter(kAnsi, kGetNames[kAnsi], result, _ReturnAddress()) : result;
		}

		LONG_PTR WINAPI Hook_GetWindowLongPtrW(HWND a_hwnd, int a_index)
		{
			const LONG_PTR result = g_originalGet[kWide](a_hwnd, a_index);
			return a_index == GWLP_WNDPROC ? Filter(kWide, kGetNames[kWide], result, _ReturnAddress()) : result;
		}

		LONG_PTR WINAPI Hook_SetWindowLongPtrA(HWND a_hwnd, int a_index, LONG_PTR a_newLong)
		{
			const LONG_PTR result = g_originalSet[kAnsi](a_hwnd, a_index, a_newLong);
			return a_index == GWLP_WNDPROC ? Filter(kAnsi, kSetNames[kAnsi], result, _ReturnAddress()) : result;
		}

		LONG_PTR WINAPI Hook_SetWindowLongPtrW(HWND a_hwnd, int a_index, LONG_PTR a_newLong)
		{
			const LONG_PTR result = g_originalSet[kWide](a_hwnd, a_index, a_newLong);
			return a_index == GWLP_WNDPROC ? Filter(kWide, kSetNames[kWide], result, _ReturnAddress()) : result;
		}
	}

	void Install()
	{
		static std::once_flag once;
		std::call_once(once, [] {
			std::wstring windowsDirectory(MAX_PATH, L'\0');
			const UINT length = GetSystemWindowsDirectoryW(windowsDirectory.data(), static_cast<UINT>(windowsDirectory.size()));
			if (length > 0 && length < windowsDirectory.size()) {
				windowsDirectory.resize(length);
				if (!windowsDirectory.ends_with(L'\\')) {
					windowsDirectory.push_back(L'\\');
				}
				CharLowerBuffW(windowsDirectory.data(), static_cast<DWORD>(windowsDirectory.size()));
				g_windowsDirectory = std::move(windowsDirectory);
			}

			HMODULE user32 = GetModuleHandleW(L"user32.dll");
			if (!user32) {
				user32 = LoadLibraryW(L"user32.dll");
			}
			if (!user32) {
				logger::error("[WndProc Fix] user32.dll not available, not installed");
				return;
			}

			for (std::size_t c = 0; c < kCharsetCount; ++c) {
				g_originalGet[c] = reinterpret_cast<GetWindowLongPtr_t>(GetProcAddress(user32, kGetNames[c]));
				g_originalSet[c] = reinterpret_cast<SetWindowLongPtr_t>(GetProcAddress(user32, kSetNames[c]));
				if (!g_originalGet[c] || !g_originalSet[c]) {
					logger::error("[WndProc Fix] Could not resolve {}/{}, not installed", kGetNames[c], kSetNames[c]);
					return;
				}
			}

			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			LONG error = DetourAttach(reinterpret_cast<PVOID*>(&g_originalGet[kAnsi]), reinterpret_cast<PVOID>(&Hook_GetWindowLongPtrA));
			if (error == NO_ERROR) {
				error = DetourAttach(reinterpret_cast<PVOID*>(&g_originalGet[kWide]), reinterpret_cast<PVOID>(&Hook_GetWindowLongPtrW));
			}
			if (error == NO_ERROR) {
				error = DetourAttach(reinterpret_cast<PVOID*>(&g_originalSet[kAnsi]), reinterpret_cast<PVOID>(&Hook_SetWindowLongPtrA));
			}
			if (error == NO_ERROR) {
				error = DetourAttach(reinterpret_cast<PVOID*>(&g_originalSet[kWide]), reinterpret_cast<PVOID>(&Hook_SetWindowLongPtrW));
			}
			if (error != NO_ERROR) {
				DetourTransactionAbort();
				logger::error("[WndProc Fix] DetourAttach failed ({}), not installed", error);
				return;
			}
			error = DetourTransactionCommit();
			if (error != NO_ERROR) {
				logger::error("[WndProc Fix] DetourTransactionCommit failed ({}), not installed", error);
				return;
			}

			logger::info("[WndProc Fix] Installed: Get/SetWindowLongPtrA/W(GWLP_WNDPROC) pseudo-handles are made callable for third-party callers");
		});
	}
}
