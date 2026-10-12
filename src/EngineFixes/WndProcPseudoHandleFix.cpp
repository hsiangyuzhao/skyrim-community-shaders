#include "WndProcPseudoHandleFix.h"

#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "Utils/UI.h"

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
		using RegisterRawInputDevices_t = BOOL(WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT);
		using SetWindowsHookExA_t = HHOOK(WINAPI*)(int, HOOKPROC, HINSTANCE, DWORD);
		using SetWindowsHookExW_t = HHOOK(WINAPI*)(int, HOOKPROC, HINSTANCE, DWORD);
		using SetForegroundWindow_t = BOOL(WINAPI*)(HWND);
		using RegisterHotKey_t = BOOL(WINAPI*)(HWND, int, UINT, UINT);

		std::array<GetWindowLongPtr_t, kCharsetCount> g_originalGet{};
		std::array<SetWindowLongPtr_t, kCharsetCount> g_originalSet{};
		RegisterRawInputDevices_t g_originalRegisterRawInputDevices = nullptr;
		SetWindowsHookExA_t g_originalSetWindowsHookExA = nullptr;
		SetWindowsHookExW_t g_originalSetWindowsHookExW = nullptr;
		SetForegroundWindow_t g_originalSetForegroundWindow = nullptr;
		RegisterHotKey_t g_originalRegisterHotKey = nullptr;

		std::wstring g_windowsDirectory;  // lower-case, with trailing backslash

		Settings g_settings;
		std::atomic<uint32_t> g_hotkeyFlagsRemoved{ 0 };
		std::mutex g_eventsMutex;
		std::vector<std::string> g_events;  // (44d) what was seen on the window, for the settings page
		constexpr std::size_t kMaxEvents = 48;

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

		CallerInfo IdentifyCaller(const void* a_returnAddress)
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

		/// "module+0xoffset", or the bare address for code outside any module.
		std::string DescribeAddress(std::uintptr_t a_address)
		{
			if (a_address == 0)
				return "null";
			const auto info = IdentifyCaller(reinterpret_cast<const void*>(a_address));
			if (info.moduleBase == 0)
				return std::format("{:#x} (no module)", a_address);
			return std::format("{}+{:#x}", info.name, a_address - info.moduleBase);
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

		/// (44d) An info line and an entry for the settings page.
		void NoteEvent(const std::string& a_text)
		{
			logger::info("[Window] {}", a_text);
			std::scoped_lock lock(g_eventsMutex);
			if (g_events.size() < kMaxEvents)
				g_events.push_back(a_text);
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
				// OS components (and drivers' in-process parts, which live under the Windows
				// directory) know how to use pseudo-handles; leave them untouched. (44d) Logged at
				// info so the log shows every module on the window, the driver's overlay included.
				if (ShouldLog(caller.moduleBase, a_api, handle)) {
					logger::info("[WndProc Fix] {} returned pseudo-handle {:#x} to system module {}+{:#x}, left unchanged", a_api, handle, caller.name, callerOffset);
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

		/// (44d) Who subclasses the window, once per (module, API, new procedure).
		void NoteSubclass(const char* a_api, HWND a_hwnd, LONG_PTR a_newProc, LONG_PTR a_previous, void* a_returnAddress)
		{
			if (!g_settings.logWindowHooks)
				return;
			const auto caller = IdentifyCaller(a_returnAddress);
			if (!ShouldLog(caller.moduleBase, a_api, static_cast<std::uintptr_t>(a_newProc)))
				return;
			const auto previous = static_cast<std::uintptr_t>(a_previous);
			const DWORD lastError = GetLastError();
			NoteEvent(std::format("{} by {}: window {:#x} procedure -> {} (previous {})", a_api,
				DescribeAddress(reinterpret_cast<std::uintptr_t>(a_returnAddress)), reinterpret_cast<std::uintptr_t>(a_hwnd),
				DescribeAddress(static_cast<std::uintptr_t>(a_newProc)),
				IsPseudoHandle(previous) ? std::format("pseudo-handle {:#x}", previous) : DescribeAddress(previous)));
			SetLastError(lastError);
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
			if (a_index != GWLP_WNDPROC)
				return result;
			NoteSubclass(kSetNames[kAnsi], a_hwnd, a_newLong, result, _ReturnAddress());
			return Filter(kAnsi, kSetNames[kAnsi], result, _ReturnAddress());
		}

		LONG_PTR WINAPI Hook_SetWindowLongPtrW(HWND a_hwnd, int a_index, LONG_PTR a_newLong)
		{
			const LONG_PTR result = g_originalSet[kWide](a_hwnd, a_index, a_newLong);
			if (a_index != GWLP_WNDPROC)
				return result;
			NoteSubclass(kSetNames[kWide], a_hwnd, a_newLong, result, _ReturnAddress());
			return Filter(kWide, kSetNames[kWide], result, _ReturnAddress());
		}

		// ---- (44d) input and focus ----------------------------------------------------------------

		constexpr USHORT kUsagePageGeneric = 0x01;
		constexpr USHORT kUsageKeyboard = 0x06;

		BOOL WINAPI Hook_RegisterRawInputDevices(PCRAWINPUTDEVICE a_devices, UINT a_count, UINT a_size)
		{
			if (!a_devices || a_count == 0 || a_size != sizeof(RAWINPUTDEVICE) || a_count > 64)
				return g_originalRegisterRawInputDevices(a_devices, a_count, a_size);

			void* const returnAddress = _ReturnAddress();
			std::array<RAWINPUTDEVICE, 64> devices{};
			bool changed = false;
			std::string description;
			for (UINT i = 0; i < a_count; ++i) {
				devices[i] = a_devices[i];
				auto& d = devices[i];
				const bool keyboard = d.usUsagePage == kUsagePageGeneric && d.usUsage == kUsageKeyboard;
				const bool blocksHotkeys = keyboard && (d.dwFlags & RIDEV_REMOVE) == 0 && (d.dwFlags & RIDEV_NOHOTKEYS) != 0;
				description += std::format("{}{:#x}/{:#x} flags {:#x}{}", i ? ", " : "", d.usUsagePage, d.usUsage, d.dwFlags,
					blocksHotkeys ? (g_settings.overlayKeyMode != 0 ? " (NOHOTKEYS removed)" : " (NOHOTKEYS kept)") : "");
				if (blocksHotkeys && g_settings.overlayKeyMode != 0) {
					d.dwFlags &= ~static_cast<DWORD>(RIDEV_NOHOTKEYS);
					changed = true;
				}
			}
			if (changed)
				g_hotkeyFlagsRemoved.fetch_add(1, std::memory_order_relaxed);

			const BOOL result = g_originalRegisterRawInputDevices(changed ? devices.data() : a_devices, a_count, a_size);
			if (g_settings.logWindowHooks || changed) {
				const DWORD lastError = GetLastError();
				const auto caller = IdentifyCaller(returnAddress);
				std::size_t key = std::hash<std::string>{}(description);
				if (ShouldLog(caller.moduleBase, "RegisterRawInputDevices", key))
					NoteEvent(std::format("RegisterRawInputDevices by {}: {}{}", DescribeAddress(reinterpret_cast<std::uintptr_t>(returnAddress)), description,
						result ? "" : " (failed)"));
				SetLastError(lastError);
			}
			return result;
		}

		const char* HookName(int a_id)
		{
			switch (a_id) {
			case WH_KEYBOARD:
				return "WH_KEYBOARD";
			case WH_KEYBOARD_LL:
				return "WH_KEYBOARD_LL";
			case WH_MOUSE:
				return "WH_MOUSE";
			case WH_MOUSE_LL:
				return "WH_MOUSE_LL";
			case WH_GETMESSAGE:
				return "WH_GETMESSAGE";
			case WH_CALLWNDPROC:
				return "WH_CALLWNDPROC";
			case WH_CALLWNDPROCRET:
				return "WH_CALLWNDPROCRET";
			case WH_CBT:
				return "WH_CBT";
			case WH_MSGFILTER:
				return "WH_MSGFILTER";
			case WH_SHELL:
				return "WH_SHELL";
			default:
				return "other";
			}
		}

		void NoteWindowsHook(const char* a_api, int a_id, HOOKPROC a_proc, DWORD a_thread, HHOOK a_result, void* a_returnAddress)
		{
			if (!g_settings.logWindowHooks)
				return;
			const DWORD lastError = GetLastError();
			const auto caller = IdentifyCaller(a_returnAddress);
			if (ShouldLog(caller.moduleBase, a_api, static_cast<std::uintptr_t>(a_id)))
				NoteEvent(std::format("{} by {}: {} ({}), procedure {}, thread {}{}", a_api, DescribeAddress(reinterpret_cast<std::uintptr_t>(a_returnAddress)),
					HookName(a_id), a_id, DescribeAddress(reinterpret_cast<std::uintptr_t>(a_proc)), a_thread, a_result ? "" : " (failed)"));
			SetLastError(lastError);
		}

		// ---- (44d) DirectInput's keyboard hook ------------------------------------------------------
		//
		// Windows' dinput8.dll reads the keyboard through a WH_KEYBOARD_LL hook on its own thread.
		// Its hook procedure (Windows 11 26200, dinput8.dll+0x22130) records the key, calls
		// CallNextHookEx, and then, while a keyboard is acquired exclusively, returns 1 -- "do not
		// process" -- for every key except Alt+Tab and an Alt key-up. Skyrim's keyboard is exclusive
		// (BSWin32KeyboardDevice::Initialize, SkyrimSE 1.5.97 0x140C1A0BE: SetCooperativeLevel 0x15 =
		// EXCLUSIVE | FOREGROUND | NOWINKEY). So Windows never registers Alt as held, and an Alt+Z
		// hotkey of another program (RegisterHotKey, or a hook that asks for LLKHF_ALTDOWN /
		// GetAsyncKeyState) never fires. It only gets through when that hook thread is late and
		// Windows stops waiting for it -- "Alt+Z works sometimes".
		// The wrapper lets the game's hook run unchanged (the game still gets every key) and, for the
		// keys the setting lists, answers "process it" instead of the 1. CallNextHookEx is not called a
		// second time: dinput8 already did.

		constexpr std::size_t kKeyboardHookSlots = 8;
		std::array<std::atomic<std::uintptr_t>, kKeyboardHookSlots> g_keyboardHookProcs{};
		std::atomic<uint64_t> g_keysLetThrough{ 0 };
		std::atomic<uint32_t> g_keyboardHooksWrapped{ 0 };

		bool IsModifier(DWORD a_vk)
		{
			switch (a_vk) {
			case VK_MENU:
			case VK_LMENU:
			case VK_RMENU:
			case VK_CONTROL:
			case VK_LCONTROL:
			case VK_RCONTROL:
			case VK_SHIFT:
			case VK_LSHIFT:
			case VK_RSHIFT:
				return true;
			default:
				return false;
			}
		}

		bool IsOverlayKey(DWORD a_vk)
		{
			switch (a_vk) {
			case 'Z':  // overlay
			case 'R':  // performance overlay
			case VK_F1:  // screenshot
			case VK_F2:  // photo mode
			case VK_F3:  // filters
			case VK_F9:  // record
			case VK_F10:  // instant replay
			case VK_F12:  // FPS counter
				return true;
			default:
				return false;
			}
		}

		bool ShouldLetThrough(const KBDLLHOOKSTRUCT& a_key)
		{
			const uint32_t mode = g_settings.overlayKeyMode;
			if (mode == 0)
				return false;
			const DWORD vk = a_key.vkCode;
			if (vk == VK_LWIN || vk == VK_RWIN)
				return false;  // the game asked for no Windows key
			// Modifiers both ways, so Windows' idea of what is held stays right.
			if (IsModifier(vk))
				return true;
			const bool up = (a_key.flags & LLKHF_UP) != 0;
			const bool altHeld = (a_key.flags & LLKHF_ALTDOWN) != 0;
			if (mode == 1)
				return IsOverlayKey(vk) && (altHeld || up);
			// mode 2: every Alt combination, minus the ones that act on the window itself.
			if (vk == VK_F4 || vk == VK_SPACE || vk == VK_RETURN || vk == VK_ESCAPE)
				return false;
			return altHeld || (up && IsOverlayKey(vk));
		}

		template <std::size_t N>
		LRESULT CALLBACK KeyboardHookWrapper(int a_code, WPARAM a_wParam, LPARAM a_lParam)
		{
			const auto proc = reinterpret_cast<HOOKPROC>(g_keyboardHookProcs[N].load(std::memory_order_acquire));
			const LRESULT result = proc ? proc(a_code, a_wParam, a_lParam) : CallNextHookEx(nullptr, a_code, a_wParam, a_lParam);
			if (result != 0 && a_code == HC_ACTION && a_lParam && ShouldLetThrough(*reinterpret_cast<const KBDLLHOOKSTRUCT*>(a_lParam))) {
				g_keysLetThrough.fetch_add(1, std::memory_order_relaxed);
				return 0;
			}
			return result;
		}

		template <std::size_t... N>
		constexpr std::array<HOOKPROC, sizeof...(N)> MakeKeyboardWrappers(std::index_sequence<N...>)
		{
			return { &KeyboardHookWrapper<N>... };
		}

		const std::array<HOOKPROC, kKeyboardHookSlots> g_keyboardWrappers = MakeKeyboardWrappers(std::make_index_sequence<kKeyboardHookSlots>{});

		HOOKPROC WrapKeyboardHook(HOOKPROC a_proc)
		{
			const auto value = reinterpret_cast<std::uintptr_t>(a_proc);
			for (std::size_t i = 0; i < kKeyboardHookSlots; ++i) {
				std::uintptr_t current = g_keyboardHookProcs[i].load(std::memory_order_acquire);
				if (current == 0 && g_keyboardHookProcs[i].compare_exchange_strong(current, value, std::memory_order_acq_rel, std::memory_order_acquire))
					return g_keyboardWrappers[i];
				if (current == value)
					return g_keyboardWrappers[i];
			}
			return nullptr;
		}

		/// DirectInput's own keyboard hook: WH_KEYBOARD_LL installed from the system dinput8.dll.
		bool IsDirectInputKeyboardHook(int a_id, HOOKPROC a_proc, void* a_returnAddress)
		{
			if (a_id != WH_KEYBOARD_LL || !a_proc)
				return false;
			const auto caller = IdentifyCaller(a_returnAddress);
			return caller.isSystem && _stricmp(caller.name.c_str(), "dinput8.dll") == 0;
		}

		HHOOK InstallWindowsHook(SetWindowsHookExW_t a_original, const char* a_api, int a_id, HOOKPROC a_proc, HINSTANCE a_module, DWORD a_thread, void* a_returnAddress)
		{
			HOOKPROC proc = a_proc;
			if (IsDirectInputKeyboardHook(a_id, a_proc, a_returnAddress)) {
				if (auto wrapper = WrapKeyboardHook(a_proc)) {
					proc = wrapper;
					g_keyboardHooksWrapped.fetch_add(1, std::memory_order_relaxed);
				}
			}
			const HHOOK result = a_original(a_id, proc, a_module, a_thread);
			NoteWindowsHook(a_api, a_id, a_proc, a_thread, result, a_returnAddress);
			if (proc != a_proc && result && g_keyboardHooksWrapped.load(std::memory_order_relaxed) == 1)
				logger::info("[Window] DirectInput's keyboard hook is wrapped: modifier keys and overlay hotkeys can reach Windows (Overlay Hotkeys mode {})", g_settings.overlayKeyMode);
			return result;
		}

		HHOOK WINAPI Hook_SetWindowsHookExA(int a_id, HOOKPROC a_proc, HINSTANCE a_module, DWORD a_thread)
		{
			return InstallWindowsHook(g_originalSetWindowsHookExA, "SetWindowsHookExA", a_id, a_proc, a_module, a_thread, _ReturnAddress());
		}

		HHOOK WINAPI Hook_SetWindowsHookExW(int a_id, HOOKPROC a_proc, HINSTANCE a_module, DWORD a_thread)
		{
			return InstallWindowsHook(g_originalSetWindowsHookExW, "SetWindowsHookExW", a_id, a_proc, a_module, a_thread, _ReturnAddress());
		}

		BOOL WINAPI Hook_SetForegroundWindow(HWND a_hwnd)
		{
			const BOOL result = g_originalSetForegroundWindow(a_hwnd);
			if (g_settings.logWindowHooks) {
				const DWORD lastError = GetLastError();
				void* const returnAddress = _ReturnAddress();
				const auto caller = IdentifyCaller(returnAddress);
				if (ShouldLog(caller.moduleBase, "SetForegroundWindow", 0))
					NoteEvent(std::format("SetForegroundWindow({:#x}) by {}{} (logged once per module)", reinterpret_cast<std::uintptr_t>(a_hwnd),
						DescribeAddress(reinterpret_cast<std::uintptr_t>(returnAddress)), result ? "" : " (refused)"));
				SetLastError(lastError);
			}
			return result;
		}

		BOOL WINAPI Hook_RegisterHotKey(HWND a_hwnd, int a_id, UINT a_modifiers, UINT a_vk)
		{
			const BOOL result = g_originalRegisterHotKey(a_hwnd, a_id, a_modifiers, a_vk);
			if (g_settings.logWindowHooks) {
				const DWORD lastError = GetLastError();
				void* const returnAddress = _ReturnAddress();
				const auto caller = IdentifyCaller(returnAddress);
				if (ShouldLog(caller.moduleBase, "RegisterHotKey", (static_cast<std::uintptr_t>(a_modifiers) << 16) | a_vk))
					NoteEvent(std::format("RegisterHotKey by {}: modifiers {:#x}, key {:#x}{}", DescribeAddress(reinterpret_cast<std::uintptr_t>(returnAddress)),
						a_modifiers, a_vk, result ? "" : " (failed: another program owns it)"));
				SetLastError(lastError);
			}
			return result;
		}

		template <class T>
		bool Resolve(HMODULE a_module, const char* a_name, T& a_out)
		{
			a_out = reinterpret_cast<T>(GetProcAddress(a_module, a_name));
			if (!a_out)
				logger::warn("[Window] Could not resolve {}; not watched", a_name);
			return a_out != nullptr;
		}
	}

	Settings& GetSettings()
	{
		return g_settings;
	}

	void Load(const nlohmann::json& a_json)
	{
		if (!a_json.is_object())
			return;
		if (a_json.contains("Overlay Hotkeys") && a_json["Overlay Hotkeys"].is_number_unsigned())
			g_settings.overlayKeyMode = std::min(a_json["Overlay Hotkeys"].get<uint32_t>(), 2u);
		if (a_json.contains("Log Window Hooks") && a_json["Log Window Hooks"].is_boolean())
			g_settings.logWindowHooks = a_json["Log Window Hooks"].get<bool>();
	}

	nlohmann::json Save()
	{
		nlohmann::json j = nlohmann::json::object();
		j["Overlay Hotkeys"] = g_settings.overlayKeyMode;
		j["Log Window Hooks"] = g_settings.logWindowHooks;
		return j;
	}

	bool BlockKeyMenu()
	{
		return g_settings.overlayKeyMode != 0 && g_keyboardHooksWrapped.load(std::memory_order_relaxed) != 0;
	}

	void DrawSettings()
	{
		const char* modes[] = { "Off (as vanilla)", "NVIDIA overlay keys (Alt+Z and its other defaults)", "Every Alt combination (except Alt+F4)" };
		int mode = static_cast<int>(std::min(g_settings.overlayKeyMode, 2u));
		if (ImGui::Combo("Overlay Hotkeys", &mode, modes, IM_ARRAYSIZE(modes)))
			g_settings.overlayKeyMode = static_cast<uint32_t>(mode);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Skyrim takes the keyboard for itself, and Windows then never sees Alt held, so hotkeys of other programs");
			ImGui::TextUnformatted("such as the NVIDIA overlay's Alt+Z only work by luck. This lets Alt, Ctrl, Shift and the overlay's keys");
			ImGui::TextUnformatted("(Alt + Z, R, F1, F2, F3, F9, F10, F12) through to Windows as well. The game still gets every key. Works at once.");
			ImGui::TextUnformatted("\"Every Alt combination\" is for a changed overlay hotkey. Alt+F4, Alt+Space, Alt+Enter and Alt+Esc stay blocked.");
		}
		ImGui::TextDisabled("DirectInput keyboard hook wrapped: %s; keys let through so far: %llu",
			g_keyboardHooksWrapped.load(std::memory_order_relaxed) ? "yes" : "not yet (it appears once the game has the keyboard)",
			static_cast<unsigned long long>(g_keysLetThrough.load(std::memory_order_relaxed)));
		if (g_hotkeyFlagsRemoved.load(std::memory_order_relaxed))
			ImGui::TextDisabled("Raw-input keyboard registrations changed: %u", g_hotkeyFlagsRemoved.load(std::memory_order_relaxed));

		ImGui::Checkbox("Log Window Hooks", &g_settings.logWindowHooks);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Writes to the log, once per program part, who attaches to the game window, the keyboard and the");
			ImGui::TextUnformatted("display (overlays, frame-generation mods, widgets). Diagnostics only.");
		}

		if (ImGui::TreeNodeEx("Seen on the game window")) {
			std::scoped_lock lock(g_eventsMutex);
			if (g_events.empty())
				ImGui::TextDisabled("Nothing logged yet.");
			for (const auto& e : g_events)
				ImGui::TextWrapped("%s", e.c_str());
			ImGui::TreePop();
		}
	}

	void ProbePresentHooks(IUnknown* a_swapChain, const char* a_label)
	{
		if (!a_swapChain || !a_label || !g_settings.logWindowHooks)
			return;
		static std::mutex mutex;
		static std::vector<std::pair<std::string, std::string>> last;

		auto describe = [](std::uintptr_t a_function) {
			std::string text = DescribeAddress(a_function);
			if (!IsExecutableAddress(a_function))
				return text;
			// A detour patches the first instruction into a jump; follow one level of it.
			const auto* code = reinterpret_cast<const std::uint8_t*>(a_function);
			std::uintptr_t target = 0;
			if (code[0] == 0xE9) {
				target = a_function + 5 + static_cast<std::intptr_t>(*reinterpret_cast<const std::int32_t*>(code + 1));
			} else if (code[0] == 0xFF && code[1] == 0x25) {
				const auto slot = a_function + 6 + static_cast<std::intptr_t>(*reinterpret_cast<const std::int32_t*>(code + 2));
				MEMORY_BASIC_INFORMATION mbi{};
				if (VirtualQuery(reinterpret_cast<LPCVOID>(slot), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0)
					target = *reinterpret_cast<const std::uintptr_t*>(slot);
			} else if (code[0] == 0x48 && code[1] == 0xB8 && code[10] == 0xFF && code[11] == 0xE0) {
				target = *reinterpret_cast<const std::uintptr_t*>(code + 2);
			}
			if (target)
				text += std::format(" -> patched to jump to {}", DescribeAddress(target));
			return text;
		};

		const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(a_swapChain);
		// IDXGISwapChain::Present is slot 8, IDXGISwapChain1::Present1 slot 22.
		const std::string now = std::format("Present {}, Present1 {}", describe(vtable[8]), describe(vtable[22]));

		std::scoped_lock lock(mutex);
		for (auto& [label, text] : last) {
			if (label == a_label) {
				if (text != now) {
					NoteEvent(std::format("{} swap chain hooks changed: {}", a_label, now));
					text = now;
				}
				return;
			}
		}
		last.emplace_back(a_label, now);
		NoteEvent(std::format("{} swap chain: {}", a_label, now));
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

			// (44d) Input and focus watch, in its own transaction so a failure here can never take the
			// pseudo-handle guard above with it.
			bool resolved = Resolve(user32, "RegisterRawInputDevices", g_originalRegisterRawInputDevices);
			resolved = Resolve(user32, "SetWindowsHookExA", g_originalSetWindowsHookExA) && resolved;
			resolved = Resolve(user32, "SetWindowsHookExW", g_originalSetWindowsHookExW) && resolved;
			resolved = Resolve(user32, "SetForegroundWindow", g_originalSetForegroundWindow) && resolved;
			resolved = Resolve(user32, "RegisterHotKey", g_originalRegisterHotKey) && resolved;
			if (!resolved)
				return;
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			error = DetourAttach(reinterpret_cast<PVOID*>(&g_originalRegisterRawInputDevices), reinterpret_cast<PVOID>(&Hook_RegisterRawInputDevices));
			if (error == NO_ERROR)
				error = DetourAttach(reinterpret_cast<PVOID*>(&g_originalSetWindowsHookExA), reinterpret_cast<PVOID>(&Hook_SetWindowsHookExA));
			if (error == NO_ERROR)
				error = DetourAttach(reinterpret_cast<PVOID*>(&g_originalSetWindowsHookExW), reinterpret_cast<PVOID>(&Hook_SetWindowsHookExW));
			if (error == NO_ERROR)
				error = DetourAttach(reinterpret_cast<PVOID*>(&g_originalSetForegroundWindow), reinterpret_cast<PVOID>(&Hook_SetForegroundWindow));
			if (error == NO_ERROR)
				error = DetourAttach(reinterpret_cast<PVOID*>(&g_originalRegisterHotKey), reinterpret_cast<PVOID>(&Hook_RegisterHotKey));
			if (error != NO_ERROR) {
				DetourTransactionAbort();
				logger::error("[Window] DetourAttach failed ({}); input and focus are not watched", error);
				return;
			}
			error = DetourTransactionCommit();
			if (error != NO_ERROR) {
				logger::error("[Window] DetourTransactionCommit failed ({}); input and focus are not watched", error);
				return;
			}
			logger::info("[Window] Watching raw input, window hooks, focus and hotkeys; Overlay Hotkeys mode {}", g_settings.overlayKeyMode);
		});
	}
}
