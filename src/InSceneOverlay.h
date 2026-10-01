// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// In-scene overlay rendering: hooks IVRCompositor::Submit and draws the
// focused client's panel texture as a textured quad directly into each
// eye render target.
//
// This is the helper's PRIMARY (and only) rendering path. Universal across
// SteamVR and OpenComposite because every runtime drives its eye textures
// through Submit. SCS uses this approach exclusively; the helper follows
// suit.

#pragma once

#include <SimpleMath.h>
#include <openvr.h>
#include <memory>
#include <winrt/base.h>

#include "Overlay.h"
#include "ImGuiVRHelperRenderHost.h"

struct ID3D11Texture2D;

namespace ImGuiVRHelper::InSceneOverlay
{
	/// Resolve the focused panel's anchor transform for one attach point.
	/// Shared by the in-scene passes and the runtime-overlay path so the two
	/// can't drift. `outAnchor` is the panel transform BEFORE the menu scale
	/// is applied; `outHeadSpace` is true when it's in HMD-local space
	/// (compose with the HMD pose for a tracking-space transform), false when
	/// it's already in tracking space. Returns false when the attach point's
	/// tracking data isn't available this frame.
	bool ResolveAnchorWorld(Overlay::OverlayType type, const Overlay::Settings& s,
		const Overlay::State& state, DirectX::SimpleMath::Matrix& outAnchor,
		bool& outHeadSpace);

	/// Install the IVRCompositor::Submit detour. Idempotent. Call once
	/// after OpenVR is available (end of InitD3D thunk).
	void Install();

	/// Render the overlay quad into one eye's render target. Called from
	/// the Submit hook for each eye on each frame. `bounds` is the source
	/// rectangle the runtime asked Submit to use (relevant for SBS / texture
	/// arrays); pass nullptr to render to the full target.
	void RenderForEye(vr::EVREye eye, ID3D11Texture2D* targetTexture,
		const vr::VRTextureBounds_t* bounds);

	/// Draw the helper's wand cursor marker directly into a panel texture at
	/// the current wand hit UV (same style/size the in-scene cursor pass
	/// uses). Used by the runtime-overlay path, where the panel is composited
	/// above the eye buffers and an in-scene marker would be occluded. The
	/// texture needs D3D11_BIND_RENDER_TARGET. Render thread only. No-op
	/// while the wand is off the panel.
	void RenderCursorIntoPanel(ID3D11Texture2D* panel);

	struct HostedFrame;

	/// Freeze admitted clients, pixels and geometry once for an explicitly hosted stereo pair.
	std::shared_ptr<HostedFrame> PrepareHostedFrame(
		const ImGuiVRHelperPluginAPI::HostedFrameInfo& frame,
		ImGuiVRHelperPluginAPI::RenderHostContent& content,
		ImGuiVRHelperPluginAPI::RenderHostResult& result);

	/// Compose frozen content without consulting live helper or engine state.
	ImGuiVRHelperPluginAPI::RenderHostResult RenderHostedEye(
		const HostedFrame& frame, const ImGuiVRHelperPluginAPI::HostedEyeContext& eye,
		ImGuiVRHelperPluginAPI::HostedEyeResult& result);

	/// Read candidate counts without graphics calls or client callbacks.
	ImGuiVRHelperPluginAPI::RenderHostContent QueryHostedContent();

	/// Retain the current helper cursor artwork for a hosted pair's private pixel copy.
	winrt::com_ptr<ID3D11Texture2D> GetHostedCursorTexture(const Overlay::Settings& settings);

	/// Render-path fault latch. Disabled when another VR overlay host (e.g.
	/// Community Shaders) already owns the in-scene overlay, or when a vrclient
	/// call throws "device or resource busy" under contention. Once disabled,
	/// the Submit hook and per-frame VR access stop for the rest of the session
	/// so we never double-drive the VR runtime.
	bool IsRenderPathDisabled();
	void DisableRenderPath(const char* reason);
}
