// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "RuntimeOverlay.h"

#include "Globals.h"
#include "HelperImpl.h"
#include "InSceneOverlay.h"
#include "OpenVRDetection.h"
#include "Overlay.h"
#include "OverlayTinter.h"
#include "internal/VRUtils.h"

#include <RE/B/BSOpenVR.h>
#include <openvr.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace ImGuiVRHelper::RuntimeOverlay
{
	using DirectX::SimpleMath::Matrix;

	namespace
	{
		enum class SubmitThread
		{
			Undecided,
			Render,
			Input,
			Disabled,
		};

		// Written on the render thread (lazy decision / failure latch), read on
		// both threads every tick.
		std::atomic<SubmitThread> g_submitThread{ SubmitThread::Undecided };

		// One runtime overlay per attach point (AttachMode::Both shows two).
		// Touched only from the submitting thread selected above.
		struct AnchorSlot
		{
			const char* key;
			vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
			bool shown = false;
		};
		AnchorSlot g_slots[2] = {
			{ "imguivrhelper.panel.hmd" },
			{ "imguivrhelper.panel.controller" },
		};

		// Desired per-frame overlay state. In Input mode the render thread
		// stages it under g_stageMutex and InputTick consumes it; in Render
		// mode it's applied in place. The com_ptr keeps the texture alive
		// across the handoff. A device-relative pose lets the runtime lock the
		// overlay to the HMD/controller with zero reprojection latency;
		// absolute is the fallback where device-relative isn't honored
		// (OpenComposite) or doesn't apply (fixed-world).
		struct SlotPose
		{
			bool visible = false;
			bool deviceRelative = false;
			vr::TrackedDeviceIndex_t device = vr::k_unTrackedDeviceIndexInvalid;
			vr::HmdMatrix34_t transform{};
		};
		struct DesiredFrame
		{
			SlotPose slot[2];
			float widthMeters = 1.0f;
			winrt::com_ptr<ID3D11Texture2D> texture;
		};
		std::mutex g_stageMutex;
		DesiredFrame g_staged;

		// Double-buffered staging copies for the Input (SteamVR) path: the
		// compositor reads the published buffer cross-process while the render
		// thread writes the other one.
		winrt::com_ptr<ID3D11Texture2D> g_staging[2];
		UINT g_stagingW = 0;
		UINT g_stagingH = 0;
		int g_stagingNext = 0;

		// True once the submitting thread has the overlays shown; feeds
		// IsHostingPanel so the in-scene panel pass only stands down after the
		// runtime is actually displaying the panel.
		std::atomic<bool> g_shownAny{ false };
		std::atomic<bool> g_hosting{ false };
		// Written from both the render thread (StagePanelCopy) and, in Input
		// mode, the input thread (ApplyToRuntime's CreateOverlay failure path).
		std::atomic<bool> g_failLogged{ false };

		// True on a null / headless SteamVR driver, where the overlay
		// subsystem is only half-initialized and IVROverlay calls walk into
		// garbage inside vrclient (access violation on an NVIDIA driver
		// worker thread, no Skyrim/plugin frames). Detected once via the
		// HMD's tracking-system name.
		bool IsNullDriver(const Util::OpenVRContext& ctx)
		{
			char buf[vr::k_unMaxPropertyStringSize] = {};
			vr::ETrackedPropertyError perr = vr::TrackedProp_Success;
			ctx.system->GetStringTrackedDeviceProperty(
				vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_TrackingSystemName_String,
				buf, sizeof(buf), &perr);
			return perr == vr::TrackedProp_Success && _stricmp(buf, "null") == 0;
		}

		SubmitThread DecideSubmitThread()
		{
			// Null / headless driver: the overlay subsystem isn't safe to touch
			// on any thread. Checked first, ahead of the vrclient probe below,
			// so runtime-overlay hosting never even attempts a
			// CreateOverlay/SetOverlayTexture call.
			{
				Util::OpenVRContext ctx;
				if (ctx.IsValid() && IsNullDriver(ctx)) {
					logs::info(
						"RuntimeOverlay: null-driver / headless SteamVR detected; staying "
						"in-scene");
					return SubmitThread::Disabled;
				}
			}

			// SteamVR: IVROverlay calls must stay off the render thread (the
			// vrclient contention race). Check vrclient_x64.dll live rather
			// than trust VRDetection::LastResult() -- that's a one-shot probe
			// from early startup, before the game's VR_Init has necessarily
			// loaded vrclient_x64.dll, so it can go stale with a false
			// OpenComposite verdict on a real SteamVR install (the stock
			// Valve openvr_api.dll matches the OpenComposite version/size
			// heuristic on some installs). This runs lazily, well after
			// startup, so vrclient is loaded by now if it's ever going to be.
			if (VRDetection::IsVrclientLoaded()) {
				logs::info("RuntimeOverlay: SteamVR detected; submitting from the input thread");
				return SubmitThread::Input;
			}

			// OpenComposite-family runtimes are in-process, so fetching the
			// interface here (render thread) is safe; their SetOverlayTexture
			// needs the render thread's D3D context anyway.
			// VR_IsInterfaceVersionValid is unreliable on these builds, so ask
			// the game's OpenVR context directly rather than trust the probe.
			Util::OpenVRContext ctx;
			if (!ctx.HasOverlay()) {
				logs::info("RuntimeOverlay: runtime exposes no IVROverlay; staying in-scene");
				return SubmitThread::Disabled;
			}
			logs::info(
				"RuntimeOverlay: in-process runtime (OpenComposite); submitting from the render thread");
			return SubmitThread::Render;
		}

		// Tracking space for SetOverlayTransformAbsolute, cached once (property
		// queries lock vrclient; see the VR property cache note in VRUtils.h).
		vr::ETrackingUniverseOrigin TrackingOrigin()
		{
			static vr::ETrackingUniverseOrigin origin = [] {
				if (auto* compositor = RE::BSOpenVR::GetIVRCompositor())
					return compositor->GetTrackingSpace();
				return vr::TrackingUniverseStanding;
			}();
			return origin;
		}

		// Resolve the desired visibility/pose for both anchors. Device-relative
		// poses (zero-latency lock) where the runtime honors them; otherwise the
		// same absolute anchor math the in-scene passes use. Returns true if any
		// anchor is visible this frame.
		bool ComputeDesired(DesiredFrame& out, bool deviceRelativeOk)
		{
			auto& state = Overlay::State::GetSingleton();
			const auto& s = state.settings;
			if (HelperImpl::GetSingleton().GetFocusedClientId() == 0 ||
				s.attachMode == Overlay::AttachMode::None)
				return false;

			vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
			bool posesValid = false;
			bool posesQueried = false;
			const auto queryPoses = [&] {
				if (!posesQueried) {
					posesQueried = true;
					posesValid = Util::GetDeviceToAbsoluteTrackingPoseCompatible(
						vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
				}
				return posesValid;
			};

			// HMD anchor.
			if (s.attachMode == Overlay::AttachMode::HMDOnly ||
				s.attachMode == Overlay::AttachMode::Both) {
				auto& slot = out.slot[0];
				if (s.positioningMethod == Overlay::PositioningMethod::FixedWorld) {
					slot.visible = true;
					slot.transform = Util::MatrixToHmdMatrix34(state.fixedWorld.m);
				} else if (deviceRelativeOk) {
					slot.visible = true;
					slot.deviceRelative = true;
					slot.device = vr::k_unTrackedDeviceIndex_Hmd;
					slot.transform = Util::MatrixToHmdMatrix34(
						Matrix::CreateTranslation(s.hmdOffsetX, s.hmdOffsetY, s.hmdOffsetZ));
				} else if (queryPoses() && poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid) {
					Matrix anchor;
					bool headSpace = false;
					if (InSceneOverlay::ResolveAnchorWorld(Overlay::OverlayType::HMD, s, state, anchor, headSpace)) {
						slot.visible = true;
						slot.transform = Util::MatrixToHmdMatrix34(
							anchor * Util::HmdMatrix34ToMatrix(
										 poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking));
					}
				}
			}

			// Controller anchor.
			if (s.attachMode == Overlay::AttachMode::ControllerOnly ||
				s.attachMode == Overlay::AttachMode::Both) {
				auto& slot = out.slot[1];
				const auto attachIdx = Util::GetControllerIndexForDevice(
					s.attachController, state.lastKnownLeftHandedMode);
				if (attachIdx != vr::k_unTrackedDeviceIndexInvalid) {
					if (deviceRelativeOk) {
						slot.visible = true;
						slot.deviceRelative = true;
						slot.device = attachIdx;
						slot.transform = Util::MatrixToHmdMatrix34(Matrix::CreateTranslation(
							s.controllerOffsetX, s.controllerOffsetY, s.controllerOffsetZ));
					} else {
						Matrix anchor;
						bool headSpace = false;
						if (InSceneOverlay::ResolveAnchorWorld(Overlay::OverlayType::Controller,
								s, state, anchor, headSpace)) {
							slot.visible = true;
							slot.transform = Util::MatrixToHmdMatrix34(anchor);
						}
					}
				}
			}

			out.widthMeters = std::clamp(s.menuScale,
				Overlay::Config::kMinMenuScale, Overlay::Config::kMaxMenuScale);
			return out.slot[0].visible || out.slot[1].visible;
		}

		// Perform the actual IVROverlay calls for the desired frame. Runs on
		// whichever thread the runtime policy selected.
		void ApplyToRuntime(const DesiredFrame& f)
		{
			Util::OpenVRContext ctx;
			if (!ctx.HasOverlay())
				return;
			auto* ov = ctx.overlay;
			bool shownAny = false;
			for (int i = 0; i < 2; ++i) {
				auto& slot = g_slots[i];
				const auto& pose = f.slot[i];
				if (!pose.visible || !f.texture) {
					if (slot.shown && slot.handle != vr::k_ulOverlayHandleInvalid) {
						ov->HideOverlay(slot.handle);
						slot.shown = false;
					}
					continue;
				}
				if (slot.handle == vr::k_ulOverlayHandleInvalid) {
					if (ov->CreateOverlay(slot.key, "ImGuiVRHelper Panel", &slot.handle) !=
						vr::VROverlayError_None) {
						slot.handle = vr::k_ulOverlayHandleInvalid;
						if (!g_failLogged) {
							logs::warn("RuntimeOverlay: CreateOverlay failed; staying in-scene");
							g_failLogged = true;
						}
						// Hide anything already shown before latching off — the
						// Disabled state stops all further ticks.
						for (auto& other : g_slots) {
							if (other.shown && other.handle != vr::k_ulOverlayHandleInvalid) {
								ov->HideOverlay(other.handle);
								other.shown = false;
							}
						}
						g_submitThread.store(SubmitThread::Disabled, std::memory_order_release);
						g_shownAny.store(false, std::memory_order_relaxed);
						return;
					}
				}
				ov->SetOverlayWidthInMeters(slot.handle, f.widthMeters);
				if (pose.deviceRelative) {
					ov->SetOverlayTransformTrackedDeviceRelative(slot.handle, pose.device,
						&pose.transform);
				} else {
					ov->SetOverlayTransformAbsolute(slot.handle, TrackingOrigin(), &pose.transform);
				}

				// SetOverlayTexture needs the runtime's unproxied interface: the
				// game's proxied IVROverlay (ctx.overlay) lacks texture-submission
				// permission on SteamVR and silently fails with
				// VROverlayError_PermissionDenied (see BSOpenVR.h's
				// GetCleanIVROverlay doc). Every other call above is fine on the
				// proxy.
				auto* texOverlay = RE::BSOpenVR::GetCleanIVROverlay();
				if (!texOverlay)
					texOverlay = ov;
				vr::Texture_t tex{ f.texture.get(), vr::TextureType_DirectX, vr::ColorSpace_Auto };
				if (const auto texErr = texOverlay->SetOverlayTexture(slot.handle, &tex);
					texErr != vr::VROverlayError_None) {
					if (!g_failLogged) {
						logs::warn("RuntimeOverlay: SetOverlayTexture failed ({}); staying in-scene",
							ov->GetOverlayErrorNameFromEnum(texErr));
						g_failLogged = true;
					}
					continue;  // blank texture -- don't show or mark hosted
				}

				if (!slot.shown) {
					if (const auto showErr = ov->ShowOverlay(slot.handle);
						showErr != vr::VROverlayError_None) {
						if (!g_failLogged) {
							logs::warn("RuntimeOverlay: ShowOverlay failed ({}); staying in-scene",
								ov->GetOverlayErrorNameFromEnum(showErr));
							g_failLogged = true;
						}
						continue;
					}
					slot.shown = true;
					// One-shot positive confirmation the overlay is actually
					// showing pixels, so a working session doesn't rely on
					// absence of failure logs.
					static std::atomic<bool> successLogged{ false };
					if (!successLogged.exchange(true)) {
						logs::info("RuntimeOverlay: panel now hosted via IVROverlay '{}'", slot.key);
					}
				}
				shownAny = true;
			}
			g_shownAny.store(shownAny, std::memory_order_relaxed);
		}

		// Copy the panel into one of two shareable staging textures SteamVR's
		// compositor can read cross-process; alternating buffers keeps it off
		// the copy the render thread writes next frame. Returns nullptr on
		// allocation failure.
		winrt::com_ptr<ID3D11Texture2D> StagePanelCopy(ID3D11Texture2D* src)
		{
			auto* device = Globals::GetD3D().device;
			auto* d3dCtx = Globals::GetD3D().context;
			if (!device || !d3dCtx || !src)
				return nullptr;
			D3D11_TEXTURE2D_DESC srcDesc{};
			src->GetDesc(&srcDesc);
			if (!g_staging[0] || g_stagingW != srcDesc.Width || g_stagingH != srcDesc.Height) {
				winrt::com_ptr<ID3D11Texture2D> fresh[2];
				D3D11_TEXTURE2D_DESC desc{};
				desc.Width = srcDesc.Width;
				desc.Height = srcDesc.Height;
				desc.MipLevels = 1;
				desc.ArraySize = 1;
				desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				desc.SampleDesc.Count = 1;
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
				for (auto& t : fresh) {
					if (FAILED(device->CreateTexture2D(&desc, nullptr, t.put()))) {
						if (!g_failLogged) {
							logs::warn(
								"RuntimeOverlay: staging texture allocation failed; staying in-scene");
							g_failLogged = true;
						}
						return nullptr;
					}
				}
				g_staging[0] = std::move(fresh[0]);
				g_staging[1] = std::move(fresh[1]);
				g_stagingW = srcDesc.Width;
				g_stagingH = srcDesc.Height;
				g_stagingNext = 0;
			}
			auto target = g_staging[g_stagingNext];
			g_stagingNext ^= 1;
			d3dCtx->CopyResource(target.get(), src);
			// The compositor opens this D3D11_RESOURCE_MISC_SHARED handle
			// cross-process from vrclient's own thread; without a flush it may
			// see a not-yet-submitted copy.
			d3dCtx->Flush();
			return target;
		}

		// The submitted panel: the tinter output (always refreshed while a
		// client is focused), with the helper's wand cursor baked in — the
		// runtime layer composites above the eye buffers, so an in-scene
		// marker would be occluded. Clients that opt out via
		// kClientFlag_OwnCursor draw their own into the panel already.
		ID3D11Texture2D* PreparePanel()
		{
			auto* out = OverlayTinter::GetOutputTexture();
			if (!out)
				return nullptr;
			auto& helper = HelperImpl::GetSingleton();
			const uint32_t focused = helper.GetFocusedClientId();
			if (focused && !(helper.GetClientFlags(focused) &
							   ImGuiVRHelperPluginAPI::kClientFlag_OwnCursor)) {
				InSceneOverlay::RenderCursorIntoPanel(out);
			}
			return out;
		}
	}

	void RenderTick()
	{
		const auto& s = Overlay::State::GetSingleton().settings;

		auto mode = g_submitThread.load(std::memory_order_acquire);
		if (mode == SubmitThread::Undecided) {
			if (!s.useRuntimeOverlay)
				return;  // don't decide (or log) until the feature is asked for
			mode = DecideSubmitThread();
			g_submitThread.store(mode, std::memory_order_release);
		}
		if (mode == SubmitThread::Disabled || mode == SubmitThread::Input) {
			// Input mode: stage for InputTick. Disabled: staging stays
			// invisible so a previously-shown overlay gets hidden.
			DesiredFrame f;
			if (mode == SubmitThread::Input && s.useRuntimeOverlay &&
				!InSceneOverlay::IsRenderPathDisabled() &&
				ComputeDesired(f, /*deviceRelativeOk=*/true)) {
				f.texture = StagePanelCopy(PreparePanel());
			}
			const bool visible = f.texture && (f.slot[0].visible || f.slot[1].visible);
			{
				std::scoped_lock lk(g_stageMutex);
				g_staged = f;
			}
			g_hosting.store(visible && g_shownAny.load(std::memory_order_relaxed),
				std::memory_order_relaxed);
			return;
		}

		// Render mode (OpenComposite): apply in place. OpenComposite's
		// SetOverlayTexture copies at call time on this thread, so no staging
		// copy is needed; device-relative poses aren't honored there, so
		// absolute transforms are composed per frame.
		DesiredFrame f;
		if (s.useRuntimeOverlay && !InSceneOverlay::IsRenderPathDisabled() &&
			ComputeDesired(f, /*deviceRelativeOk=*/false)) {
			if (auto* out = PreparePanel())
				f.texture.copy_from(out);
		}
		try {
			ApplyToRuntime(f);
		} catch (const std::exception& e) {
			logs::error("RuntimeOverlay: disabled after runtime exception: {}", e.what());
			g_submitThread.store(SubmitThread::Disabled, std::memory_order_release);
			g_shownAny.store(false, std::memory_order_relaxed);
		}
		g_hosting.store((f.slot[0].visible || f.slot[1].visible) && f.texture &&
							g_shownAny.load(std::memory_order_relaxed),
			std::memory_order_relaxed);
	}

	void InputTick()
	{
		if (g_submitThread.load(std::memory_order_acquire) != SubmitThread::Input)
			return;
		DesiredFrame f;
		{
			std::scoped_lock lk(g_stageMutex);
			f = g_staged;
		}
		if (InSceneOverlay::IsRenderPathDisabled())
			f = DesiredFrame{};  // hide everything
		try {
			ApplyToRuntime(f);
		} catch (const std::exception& e) {
			logs::error("RuntimeOverlay: disabled after runtime exception: {}", e.what());
			g_submitThread.store(SubmitThread::Disabled, std::memory_order_release);
			g_shownAny.store(false, std::memory_order_relaxed);
		}
	}

	bool IsHostingPanel()
	{
		return g_hosting.load(std::memory_order_relaxed);
	}
}
