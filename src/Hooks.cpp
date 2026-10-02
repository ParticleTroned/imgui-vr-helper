// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Pattern lifted from SCS's src/Hooks.cpp:
//   stl::write_thunk_call<BSGraphics_Renderer_Init_InitD3D>(
//       REL::RelocationID(75595, 77226).address() + REL::Relocate(0x50, 0x2BC));
//
// Captures the renderer's device/context/swapchain into Globals after
// the original InitD3D returns, then installs the per-frame Present detour
// and the input poll thunk. The IVRCompositor::Submit detour owned by
// InSceneOverlay (which composites the focused client's panel into each eye
// render target) is installed LATER, from the first Present frames via
// DecideRenderPath(), so we can first detect whether Community Shaders is
// hosting the in-scene overlay and stand down if so (see the "Community
// Shaders coexistence" block below). Universal across SteamVR + OpenComposite
// — both runtimes route eye textures through Submit, and we render directly
// into those eye buffers rather than asking SteamVR's IVROverlay layer system
// to composite for us. This matches the canonical SCS path on
// upstream/dev:src/Features/VR/InSceneOverlay.cpp.

#include "pch.h"

#include "Hooks.h"

#include "Globals.h"
#include "HelperImpl.h"
#include "InSceneOverlay.h"
#include "Input.h"
#include "OpenVRDetection.h"
#include "RenderHost.h"
#include "RuntimeOverlay.h"
#include "SettingsUI.h"
#include "VRKeyboard.h"
#include "internal/Detour.h"

#include <RE/B/BSOpenVR.h>
#include <RE/B/BSOpenVRControllerDevice.h>
#include <RE/B/ButtonEvent.h>
#include <RE/I/InputEvent.h>
#include <RE/R/Renderer.h>
#include <RE/T/ThumbstickEvent.h>

#include <chrono>
#include <cstdint>
#include <dxgi.h>
#include <exception>
#include <utility>
#include <vector>

#pragma comment(lib, "version.lib")

namespace ImGuiVRHelper::Hooks
{
	namespace
	{
		// ---- IDXGISwapChain::Present detour -----------------------------
		//
		// Slot 8 in the IDXGISwapChain vtable. Drives the helper's per-frame
		// tick: builds a Frame snapshot and dispatches it to each registered
		// client before falling through to the original Present.

		using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
		PresentFn g_originalPresent = nullptr;

		std::chrono::steady_clock::time_point g_lastPresent;
		bool g_lastPresentValid = false;

		// Deferred render-path decision. See DecideRenderPath below: we wait
		// until our (outer) Present has chained into any other Present hook so a
		// lazily-installed peer overlay (Community Shaders) has already taken
		// IVRCompositor::Submit, then either install our overlay or stand down.
		void DecideRenderPath();
		void MonitorCSHandoff();
		bool g_renderDecisionMade = false;

		HRESULT WINAPI hk_Present(IDXGISwapChain* This, UINT SyncInterval, UINT Flags)
		{
			if (This == Globals::GetD3D().swapchain)
				RenderHost::ObserveRenderThread();
			const auto now = std::chrono::steady_clock::now();
			float dt = 0.016f;  // sane default for the very first frame
			if (g_lastPresentValid) {
				dt = std::chrono::duration<float>(now - g_lastPresent).count();
				dt = std::clamp(dt, 0.0001f, 0.5f);  // guard against pause / huge gaps
			}
			g_lastPresent = now;
			g_lastPresentValid = true;

			// Until the render-path decision is made, do NOT touch the VR
			// runtime. Chain to the original Present first (this runs any peer
			// overlay's Present hook, e.g. Community Shaders, which installs its
			// Submit hook lazily), then decide.
			if (!g_renderDecisionMade) {
				const HRESULT hr = g_originalPresent(This, SyncInterval, Flags);
				DecideRenderPath();
				return hr;
			}

			if (!RenderHost::IsActive())
				MonitorCSHandoff();

			if (!InSceneOverlay::IsRenderPathDisabled()) {
				// Latch the render path off (rather than crash) if a per-frame
				// VR call throws std::system_error ("device or resource busy").
				try {
					HelperImpl::GetSingleton().DispatchFrame(dt);
				} catch (const std::exception& e) {
					InSceneOverlay::DisableRenderPath("exception in DispatchFrame");
					logs::error("Hooks: render path disabled after DispatchFrame exception: {}",
						e.what());
				}
			}

			// Runs even while the render path is disabled so an already-shown
			// runtime overlay gets hidden; needs DispatchFrame's tinter output
			// when active, hence after the block above.
			RuntimeOverlay::RenderTick();

			return g_originalPresent(This, SyncInterval, Flags);
		}

		// ---- Community Shaders coexistence ------------------------------
		//
		// imgui-vr-helper's in-scene overlay is a fork of Community Shaders'
		// (open-shaders) VR overlay. Both hook IDXGISwapChain::Present AND
		// IVRCompositor::Submit and both drive the VR compositor every frame.
		// Two overlay hosts double-drive vrclient's CClientPropertyManager and
		// it throws std::system_error("device or resource busy") — see
		// crash-2026-06-20-01-15-08. So when CS owns the in-scene overlay we
		// stand down and let it host.
		//
		// Detection is by behavior, not by mere module presence: the future
		// coexist-aware open-shaders drops its own VR overlay and delegates to
		// imgui-vr-helper, so it will NOT hook Submit — only the incompatible
		// (current) version does. "CS owns the Submit hook" is therefore exactly
		// the signal that this is the conflicting version; plain presence is not.
		//
		// Ordering note: SKSE loads plugin DLLs alphabetically, so
		// CommunityShaders.dll loads before imgui-vr-helper.dll. That makes our
		// InitD3D/Present hooks install LAST and therefore run FIRST (outermost).
		// CS installs its Submit hook lazily from inside its own Present, so we
		// can only observe it AFTER we chain into the original Present. That is
		// why the decision runs post-chain in hk_Present, not in InitD3D.

		constexpr wchar_t kCSModuleName[] = L"CommunityShaders.dll";

		int g_compositorReadyFrames = 0;

		// [base, end) of a loaded module, or {0, 0} if not loaded. Reads
		// SizeOfImage straight from the PE header to avoid a psapi dependency.
		std::pair<std::uintptr_t, std::uintptr_t> ModuleRange(const wchar_t* name)
		{
			HMODULE h = GetModuleHandleW(name);
			if (!h)
				return { 0, 0 };
			auto base = reinterpret_cast<std::uintptr_t>(h);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
				reinterpret_cast<const std::uint8_t*>(h) + dos->e_lfanew);
			return { base, base + nt->OptionalHeader.SizeOfImage };
		}

		// True if IVRCompositor::Submit (vtable slot 5) currently points into
		// CommunityShaders.dll — i.e. CS owns the in-scene overlay.
		bool CSOwnsSubmitHook()
		{
			const auto [base, end] = ModuleRange(kCSModuleName);
			if (!base)
				return false;
			auto* compositor = RE::BSOpenVR::GetIVRCompositor();
			if (!compositor)
				return false;
			auto** vtable = *reinterpret_cast<void***>(compositor);
			const auto slot5 = reinterpret_cast<std::uintptr_t>(vtable[5]);
			return slot5 >= base && slot5 < end;
		}

		void LogCSVersion()
		{
			static bool logged = false;
			if (logged)
				return;
			logged = true;

			HMODULE h = GetModuleHandleW(kCSModuleName);
			wchar_t path[MAX_PATH]{};
			if (!h || !GetModuleFileNameW(h, path, MAX_PATH)) {
				logs::info("Community Shaders detected (path unavailable)");
				return;
			}
			DWORD ignored = 0;
			const DWORD sz = GetFileVersionInfoSizeW(path, &ignored);
			if (sz) {
				std::vector<std::uint8_t> buf(sz);
				VS_FIXEDFILEINFO* ffi = nullptr;
				UINT len = 0;
				if (GetFileVersionInfoW(path, 0, sz, buf.data()) &&
					VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) &&
					ffi) {
					logs::info("Community Shaders detected: v{}.{}.{}.{}",
						HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
						HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
					return;
				}
			}
			logs::info("Community Shaders detected (version unavailable)");
		}

		// Bring up our overlay render path: install the Submit detour, init the
		// settings UI and register the self-client.
		void InstallRenderPath()
		{
			InSceneOverlay::Install();
			SettingsUI::Init();
			HelperImpl::GetSingleton().EnsureSelfClient();
		}

		void DecideRenderPath()
		{
			if (g_renderDecisionMade)
				return;

			// Need the compositor before we can inspect the Submit slot or
			// install our own hook. Wait without consuming the grace window.
			if (!RE::BSOpenVR::GetIVRCompositor())
				return;

			const bool csPresent = ModuleRange(kCSModuleName).first != 0;
			if (!csPresent) {
				InstallRenderPath();
				g_renderDecisionMade = true;
				return;
			}

			LogCSVersion();

			// CS already owns Submit (e.g. its submit-stage VR upscaling).
			// Our thunk chains onto whatever's in the slot without touching
			// texture format/dimensions, so it's safe to install on top
			// rather than stand down.
			if (CSOwnsSubmitHook()) {
				logs::info(
					"Community Shaders owns IVRCompositor::Submit; chaining "
					"imgui-vr-helper's in-scene overlay onto Community Shaders' "
					"Submit hook.");
				InstallRenderPath();
				g_renderDecisionMade = true;
				return;
			}

			// CS present but hasn't hooked Submit. Either it's the coexist-aware
			// version that delegates to us (so it never will), or it's the
			// conflicting version that installs lazily and hasn't run its first
			// Present yet. Give it a short grace window to declare itself before
			// we claim Submit ourselves.
			if (++g_compositorReadyFrames < 30)
				return;

			logs::info(
				"Community Shaders present but not hosting the in-scene overlay; "
				"installing imgui-vr-helper's overlay.");
			InstallRenderPath();
			g_renderDecisionMade = true;
		}

		int g_postInstallCheckCounter = 0;
		constexpr int kPostInstallCheckIntervalFrames = 30;

		void MonitorCSHandoff()
		{
			if (InSceneOverlay::IsRenderPathDisabled())
				return;
			if (++g_postInstallCheckCounter < kPostInstallCheckIntervalFrames)
				return;
			g_postInstallCheckCounter = 0;

			if (CSOwnsSubmitHook()) {
				logs::info(
					"Community Shaders claimed IVRCompositor::Submit after "
					"imgui-vr-helper had already installed its own in-scene "
					"overlay hook; both are already chained, nothing to do.");
			}
		}

		// ---- BSInputDeviceManager::PollInputDevices thunk ---------------
		//
		// Walks the InputEvent linked list, feeds VR button/thumbstick
		// events into Input::FeedVREvent so Overlay::State's controller
		// state stays current. Chains to the original (and any other
		// plugin's thunk), so nothing else is disturbed.

		bool IsVRControllerDevice(RE::INPUT_DEVICE d)
		{
			switch (d) {
			case RE::INPUT_DEVICE::kVivePrimary:
			case RE::INPUT_DEVICE::kViveSecondary:
			case RE::INPUT_DEVICE::kOculusPrimary:
			case RE::INPUT_DEVICE::kOculusSecondary:
			case RE::INPUT_DEVICE::kWMRPrimary:
			case RE::INPUT_DEVICE::kWMRSecondary:
				return true;
			default:
				return false;
			}
		}

		// DirectInput scan code for F4. Combined with a held Shift modifier
		// (checked via GetAsyncKeyState at press time) so the toggle is
		// Shift+F4 — chosen to avoid the F4 single-keypress conflict with
		// other mods that bind F4 (and to keep accidental presses during
		// normal play from popping the helper open). Keyboard is the only
		// reliable toggle path at the main menu, where VR controller
		// buttons drive menu nav and may be consumed before reaching us.
		// Future: make this user-configurable via the TOML config.
		constexpr std::uint32_t kKeyboardToggleDIK = 0x3E;  // DIK_F4

		struct PollInputDevices_t
		{
			static void thunk(RE::BSTEventSource<RE::InputEvent*>* a_dispatcher,
				RE::InputEvent* const* a_events)
			{
				// Before the disabled early-return so a shown runtime overlay
				// still gets hidden after the render path latches off.
				RuntimeOverlay::InputTick();

				// If the overlay render path is disabled (e.g. Community Shaders
				// hosts the in-scene overlay, or a VR fault latched us off),
				// don't feed or swallow input — just chain through so nothing
				// else is disturbed.
				if (InSceneOverlay::IsRenderPathDisabled()) {
					func(a_dispatcher, a_events);
					return;
				}

				// Pump the VR keyboard on the input thread — IVROverlay calls must
				// not run on the render thread (the vrclient "device busy" race).
				VRKeyboard::Tick();

				// Synthetic devbench-injected buttons land here — same thread and
				// same state as real events, ahead of this tick's frame build.
				Input::DrainInjected();

				// Always feed the menu its input first, even when we're
				// about to swallow the events from the game side. This is
				// what lets ImGui see the trigger pull as a click while
				// Skyrim sees nothing.
				bool sawVRController = false;
				if (a_events) {
					for (auto* e = *a_events; e; e = e->next) {
						const auto device = e->GetDevice();

						// Keyboard toggle: Shift+F4. Fires the same
						// SettingsUI::Toggle path a controller combo
						// would, so it works at the main menu where
						// controller buttons may be intercepted.
						// GetAsyncKeyState polls the OS-level shift
						// state at the moment F4 is pressed, which is
						// simpler than tracking shift edges through
						// the event stream and matches what
						// SKSE/Skyrim itself does for hotkey modifiers.
						if (device == RE::INPUT_DEVICE::kKeyboard &&
							e->GetEventType() == RE::INPUT_EVENT_TYPE::kButton) {
							if (auto* btn = e->AsButtonEvent();
								btn && btn->GetIDCode() == kKeyboardToggleDIK &&
								btn->IsDown()) {
								const bool shiftHeld =
									(GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
								if (shiftHeld) {
									HelperImpl::GetSingleton().OnKeyboardToggle();
								}
							}
							continue;
						}

						if (!IsVRControllerDevice(device))
							continue;
						sawVRController = true;

						switch (e->GetEventType()) {
						case RE::INPUT_EVENT_TYPE::kButton:
							{
								if (auto* btn = e->AsButtonEvent()) {
									Input::FeedVREvent(static_cast<uint32_t>(device),
										btn->GetIDCode(),
										btn->IsPressed(),
										0.0f, 0.0f);
								}
								break;
							}
						case RE::INPUT_EVENT_TYPE::kThumbstick:
							{
								if (auto* ts = e->AsThumbstickEvent()) {
									Input::FeedVREvent(static_cast<uint32_t>(device),
										ts->GetIDCode(),
										false,
										ts->xValue, ts->yValue);
								}
								break;
							}
						default:
							break;
						}
					}
				}

				// Swallow VR controller input from the game while the helper is
				// interactive — otherwise trigger pulls used to click menu buttons
				// also fire bows / cast spells, scroll thumb drifts the player
				// camera, etc. Pattern lifted from SCS (origin/vr_imgui
				// src/Hooks.cpp:566-607), but filtered PER EVENT rather than
				// substituting a dummy {nullptr} for the whole dispatch: Skyrim can
				// batch unrelated devices' events (mouse, keyboard, a non-VR
				// gamepad) together with a VR controller event in the same
				// dispatch call, so replacing the whole chain silently ate real
				// mouse/keyboard input whenever it happened to land in the same
				// batch as a VR event -- e.g. a desktop-mouse click on a
				// flat-screen-capable client's menu (Community Shaders) while a
				// controller was also reporting thumbstick noise that frame.
				if (sawVRController && HelperImpl::GetSingleton().ShouldSwallowInput()) {
					// Live-tool clients (e.g. VR photo mode) keep driving the
					// unpaused world, so their thumbstick still passes through for
					// locomotion; everyone else's VR thumbstick is swallowed too
					// (menu-driven scroll would otherwise drift the camera). VR
					// buttons stay reserved for laser-driven UI either way.
					const bool liveTool = HelperImpl::GetSingleton().IsLiveToolFocused();
					RE::InputEvent* head = nullptr;
					RE::InputEvent* tail = nullptr;
					if (a_events) {
						for (auto* e = *a_events; e; e = e->next) {
							const bool isVR = IsVRControllerDevice(e->GetDevice());
							const bool passThrough = !isVR ||
							                         (liveTool && e->GetEventType() == RE::INPUT_EVENT_TYPE::kThumbstick);
							if (passThrough) {
								if (tail) {
									tail->next = e;
								} else {
									head = e;
								}
								tail = e;
							}
						}
						if (tail) {
							tail->next = nullptr;
						}
					}
					RE::InputEvent* const forwarded[] = { head };
					func(a_dispatcher, forwarded);
					return;
				}
				func(a_dispatcher, a_events);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// ---- BSGraphics::Renderer::InitD3D thunk ------------------------

		struct BSGraphics_Renderer_Init_InitD3D
		{
			static void thunk()
			{
				logs::info("BSGraphics::Renderer::InitD3D - calling original");
				func();

				auto* manager = RE::BSGraphics::Renderer::GetSingleton();
				if (!manager) {
					logs::warn("InitD3D thunk: BSGraphics::Renderer singleton missing");
					return;
				}

				auto& runtime = manager->GetRuntimeData();
				auto* device = reinterpret_cast<ID3D11Device*>(runtime.forwarder);
				auto* context = reinterpret_cast<ID3D11DeviceContext*>(runtime.context);
				auto* swapchain = reinterpret_cast<IDXGISwapChain*>(runtime.renderWindows[0].swapChain);

				if (!device || !context || !swapchain) {
					logs::warn(
						"InitD3D thunk: incomplete renderer runtime data "
						"(device={}, context={}, swapchain={})",
						static_cast<void*>(device),
						static_cast<void*>(context),
						static_cast<void*>(swapchain));
					return;
				}

				Globals::SetD3D(device, context, swapchain);

				// The startup probe (main.cpp, kPostPostLoad) can run before
				// the game's VR_Init has loaded vrclient_x64.dll, misreporting
				// real SteamVR as OpenComposite (the stock Valve openvr_api.dll
				// matches OpenComposite's version/size heuristic on some
				// installs). D3D init happens after VR_Init for a VR game, so
				// vrclient is loaded by now if it's ever going to be -- re-probe
				// and correct the cached result/log if the verdict changed.
				const auto previousRuntime = VRDetection::LastResult().runtimeType;
				const auto info = VRDetection::Detect();
				if (info.runtimeType != previousRuntime) {
					logs::info("OpenVR runtime detection corrected after D3D init:");
					VRDetection::LogDetectionResult(info);
				}

				// Now that we have the swapchain, install the Present
				// detour so the helper has a per-frame tick.
				g_originalPresent = Util::DetourClassVTable<PresentFn>(swapchain, 8, &hk_Present);
				logs::info("IDXGISwapChain::Present detour installed (original={})",
					reinterpret_cast<void*>(g_originalPresent));

				// NOTE: the IVRCompositor::Submit detour, settings UI and
				// self-client are NOT installed here. They are deferred to the
				// first Present frames via DecideRenderPath() so we can first
				// observe whether Community Shaders (which installs its own
				// Submit hook lazily) is hosting the in-scene overlay, and stand
				// down if so. See the "Community Shaders coexistence" block.
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	bool EnsureHostedServices()
	{
		if (!Globals::IsReady() || !RE::BSOpenVR::GetIVRCompositor())
			return false;
		if (!g_renderDecisionMade) {
			InstallRenderPath();
			g_renderDecisionMade = true;
		}
		return true;
	}

	void Install()
	{
		// Skyrim has SE/AE/VR address-library variants. The helper is VR-only
		// for now, so we use the VR offset directly. If the helper ever
		// becomes multi-runtime, extend with REL::RelocationID(SE, AE, VR).
		if (!REL::Module::IsVR()) {
			logs::warn("Hooks::Install skipped: not running in Skyrim VR");
			return;
		}

		SKSE::AllocTrampoline(28);

		logs::info("Hooks::Install - writing BSGraphics::Renderer::InitD3D thunk");
		// VR offset 77226 + 0x2BC matches the SCS Hooks.cpp install site for VR.
		stl::write_thunk_call<BSGraphics_Renderer_Init_InitD3D>(
			REL::RelocationID(75595, 77226).address() + REL::Relocate(0x50, 0x2BC));

		logs::info("Hooks::Install - writing BSInputDeviceManager::PollInputDevices thunk");
		// Same install site SCS uses; SKSE trampoline chains correctly when
		// multiple plugins hook here.
		stl::write_thunk_call<PollInputDevices_t>(
			REL::RelocationID(67315, 68617).address() + REL::Relocate(0x7B, 0x7B, 0x81));
	}
}
