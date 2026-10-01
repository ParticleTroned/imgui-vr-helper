// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2025 ImGuiVRHelper contributors. See api/COPYING.LESSER.
//
// ImGuiVRHelper public API.
//
// Clients vendor api/ImGuiVRHelperAPI.h, api/ImGuiVRHelperAPI.cpp,
// api/ImGuiVRHelperTypes.h, and api/ImGuiVRHelperInput.h. At SKSE's
// kPostPostLoad, call GetImGuiVRHelperInterface001() to obtain a pointer to the
// helper's API. kPostPostLoad (not kPostLoad) is the safe point: it fires after
// every plugin's kPostLoad, so the helper's messaging listener is registered
// regardless of load order. The handshake is retryable if you must call earlier.
// Subsequent calls go through the returned vtable — SKSE messaging is only used
// for the handshake.
//
// Handshake pattern derived from
//   https://github.com/alandtse/SkyrimVRESL/blob/master/src/SkyrimVRESLAPI.h
// originally from
//   https://github.com/adamhynek/higgs

#pragma once

#include <cstddef>
#include <cstdint>

#include <SKSE/SKSE.h>

#include "ImGuiVRHelperInput.h"
#include "ImGuiVRHelperRenderHost.h"
#include "ImGuiVRHelperTypes.h"

namespace ImGuiVRHelperPluginAPI
{
	/// SKSE plugin name (filename without extension) used for messaging dispatch.
	constexpr const auto kPluginName = "ImGuiVRHelper";

	/// Handshake message exchanged between client and helper at kPostLoad.
	struct Message
	{
		/// Randomly-generated, fixed for the lifetime of the helper. Clients
		/// dispatch this message; the helper's listener fills in
		/// `GetApiFunction` and returns.
		enum : uint32_t
		{
			kMessage_GetInterface = 0xEACB2BFAu  // randomly generated, fixed
		};

		/// Filled in by the helper. Returns the requested interface revision
		/// or nullptr if the helper doesn't support that revision.
		void* (*GetApiFunction)(uint32_t revisionNumber) = nullptr;
	};

	struct IImGuiVRHelperInterface001;

	/// Handshake. Call at kPostPostLoad (retryable — a null result before the
	/// helper's listener registers is not fatal; call again). Returns nullptr if
	/// the helper isn't installed or is older than version 001.
	IImGuiVRHelperInterface001* GetImGuiVRHelperInterface001();

	/// Versioned interface. Future revisions extend by inheritance:
	///   struct IImGuiVRHelperInterface002 : IImGuiVRHelperInterface001 { ... };
	/// The helper's GetApiFunction returns the highest revision the
	/// installed build supports, or nullptr for revisions newer than itself.
	struct IImGuiVRHelperInterface001
	{
		/// Helper build number; informational. Clients should not key
		/// behavior off this — use interface revision instead.
		virtual uint32_t GetBuildNumber() = 0;

		// ---- Lifecycle --------------------------------------------------

		/// Register a client. Returns a non-zero client_id on success, 0 on
		/// failure (e.g. a null `name`).
		/// `name` and `version` are copied internally; pass `nullptr` for
		/// `version` to omit it. `flags` is a bitmask of ClientFlags.
		///
		/// Recommended `version` content: the calling mod's human-readable
		/// version (e.g. SKSE PluginVersionData::string()). The helper does
		/// not parse it — surfaced verbatim in the helper's "Registered
		/// Clients" diagnostic table.
		virtual uint32_t RegisterClient(const char* name, const char* version,
			OnFrameFn on_frame, void* user, uint32_t flags) = 0;

		/// Unregister a previously-registered client. Releases the panel
		/// render target, cancels any combo recording, and stops invoking
		/// `on_frame`. Safe to call from any SKSE message stage.
		virtual void UnregisterClient(uint32_t client_id) = 0;

		// ---- Texture handoff -------------------------------------------

		/// Get the helper-owned render target for this client's panel.
		/// Returns false if the client hasn't been issued one yet (e.g.
		/// before first frame). The returned `rtv` is valid until
		/// UnregisterClient. `width` and `height` may change between calls.
		virtual bool GetPanel(uint32_t client_id, PanelHandle* out) = 0;

		// ---- Pointer / cursor ------------------------------------------

		/// If a wand laser is currently intersecting this client's panel,
		/// fills `*u`/`*v` with normalized panel coordinates [0..1] and
		/// `*device_idx` with the source controller's tracked device index,
		/// then returns true. Otherwise returns false.
		virtual bool GetPointer(uint32_t client_id, float* u, float* v,
			uint32_t* device_idx) = 0;

		// ---- Combos -----------------------------------------------------

		/// Register a button combo for this client. Returns a ComboId the
		/// client polls each frame via ComboFired. The helper handles all
		/// matching, sequence timing, and one-shot edge detection.
		///
		/// `label` is a short human-readable name for the combo ("Open menu"),
		/// shown in the helper's controller-mapping UI so users can tell a
		/// client's combos apart. May be null/empty.
		///
		/// `on_rebind` (may be null) is invoked when the user rebinds this combo
		/// from the controller map: the helper updates the live keys AND calls
		/// back with the new chord so the client can persist it. `user` is passed
		/// through to that callback.
		virtual ComboId RegisterCombo(uint32_t client_id, const InputCombo* keys,
			std::size_t n, float timeout_s, const char* label,
			ComboRebindFn on_rebind, void* user) = 0;

		/// Edge-triggered: returns true exactly once per combo activation
		/// and resets internal latch.
		virtual bool ComboFired(ComboId) = 0;

		/// Open the helper's modal "press buttons now" capture overlay.
		/// While capture is active, the requesting client implicitly holds
		/// modal focus regardless of prior state. On completion or timeout,
		/// `on_done` is invoked and prior focus is restored.
		virtual void StartComboRecording(uint32_t client_id, const char* label,
			ComboRecordedFn on_done, void* user, float timeout_s) = 0;

		/// Cancels an in-progress recording for this client (if any).
		virtual void CancelComboRecording(uint32_t client_id) = 0;

		// ---- Focus / visibility ----------------------------------------

		/// True iff the user has the VR overlay open (any client visible).
		virtual bool IsOverlayVisible() = 0;

		/// Request modal focus for this client. Used when the client opens
		/// its menu in response to a hotkey. Subsequent input is delivered
		/// to this client and synthesized cursor input is suppressed.
		virtual void RequestFocus(uint32_t client_id) = 0;

		/// Release focus. The helper picks the next focused client based on
		/// LRU.
		virtual void ReleaseFocus(uint32_t client_id) = 0;

		// ---- Haptics ----------------------------------------------------

		/// Trigger a haptic pulse on the controller identified by
		/// `haptic_token` (taken from a Frame's Hand::haptic_token).
		virtual void TriggerHaptic(uint32_t client_id, uint32_t haptic_token,
			uint32_t duration_us, float frequency, float amplitude) = 0;

		// ---- Migration helper ------------------------------------------

		/// Import settings from a legacy client that was relinquishing
		/// control of overlay-side configuration to the helper. The helper
		/// merges these into its own JSON store. One-shot; subsequent
		/// imports are ignored.
		virtual bool ImportLegacySettings(const char* json_blob) = 0;

		// ---- Input ingestion -------------------------------------------

		/// Push one VR controller event into the helper's input state.
		/// Clients that own a PollInputDevices-style hook (e.g. SCS) call
		/// this once per VR event to keep the helper's controller state
		/// fresh; the helper then drives wand pointing, drag, and combo
		/// matching off it.
		///
		/// Parameters:
		///   - device:         RE::INPUT_DEVICE value (kVivePrimary,
		///                     kOculusSecondary, etc.); cast-compatible
		///                     with uint32_t for ABI stability.
		///   - key_code:       RE::BSOpenVRControllerDevice::Keys value
		///                     (kBY=1, kGrip=2, kXA=7, ...).
		///   - pressed:        true on press, false on release.
		///   - thumbstick_x/y: raw analog values [-1..1] for thumbstick
		///                     events; pass 0 for button events.
		///
		/// A future revision will install the PollInputDevices hook
		/// directly so clients can drop their own input plumbing.
		virtual void FeedVREvent(uint32_t device, uint32_t key_code, bool pressed,
			float thumbstick_x, float thumbstick_y) = 0;

		// ---- SteamVR Dashboard (removed) --------------------------------

		/// DEPRECATED, always returns false. The SteamVR Dashboard surface
		/// was removed; this method is retained only so existing clients
		/// keep linking against IImGuiVRHelperInterface001. Don't call it.
		virtual bool IsDashboardVisible() = 0;

		// ---- Combo rebinding -------------------------------------------

		/// Rebind a previously-registered combo to a new chord (e.g. driven by an
		/// in-app bindings table). Updates the live keys the helper matches AND
		/// invokes the combo's `on_rebind` callback so the owner can persist it.
		/// Pass `n == 0` (keys may be null) to CLEAR the binding — the combo
		/// stays registered but never fires until rebound. No-op if `combo` is
		/// unknown or `n` is out of range (> 8).
		virtual void RebindCombo(ComboId combo, const InputCombo* keys,
			std::size_t n) = 0;
	};

	/// Revision 002. Adds VR text entry (the SteamVR / OpenComposite system
	/// keyboard) for client text fields. Obtain via GetImGuiVRHelperInterface002();
	/// it returns nullptr against a helper older than 002, so fall back to 001 (no
	/// VR keyboard). The SDK's Client::PumpKeyboard wraps all of this.
	struct IImGuiVRHelperInterface002 : IImGuiVRHelperInterface001
	{
		/// Show (active=true) or hide (active=false) the VR keyboard for this
		/// client's focused text field. Call every active frame; `seed_utf8` is the
		/// field's current text, applied on the first activating frame. Safe to call
		/// from your on_frame/render thread — the helper marshals the OpenVR calls to
		/// its input thread. No-op if the runtime exposes no keyboard (logged once;
		/// OpenComposite-Unleashed or SteamVR provide one, vanilla OpenComposite
		/// does not).
		virtual void SetKeyboardActive(uint32_t client_id, bool active,
			const char* seed_utf8) = 0;

		/// Copy the keyboard's current text (UTF-8, null-terminated) into `out_utf8`
		/// (capacity `out_size`) and return its byte length. The runtime keyboard is
		/// authoritative — feed this back into your field each active frame. 0 if no
		/// text is available.
		virtual uint32_t GetKeyboardText(uint32_t client_id, char* out_utf8,
			uint32_t out_size) = 0;

		/// Returns true exactly once after the user dismissed the keyboard (Done /
		/// close) for this client, so the client can commit and defocus its field.
		virtual bool ConsumeKeyboardClosed(uint32_t client_id) = 0;
	};

	/// Handshake for revision 002 (see GetImGuiVRHelperInterface001). Returns
	/// nullptr if the installed helper predates 002.
	IImGuiVRHelperInterface002* GetImGuiVRHelperInterface002();

	/// Revision 003. Adds per-combo context: a client can declare a combo "off-panel",
	/// so the SDK's Fired() reports it only while the wand is off the client's panel and
	/// the controller map tags it. Obtain via GetImGuiVRHelperInterface003().
	struct IImGuiVRHelperInterface003 : IImGuiVRHelperInterface002
	{
		/// Mark a registered combo as off-panel (true) or global (false, the default).
		/// An off-panel combo acts only while the wand is off the owning client's panel
		/// (the SDK's Fired() enforces the gate); the controller map shows the tag.
		virtual void SetComboOffPanel(ComboId combo, bool off_panel) = 0;
	};

	/// Handshake for revision 003. Returns nullptr if the installed helper predates 003.
	IImGuiVRHelperInterface003* GetImGuiVRHelperInterface003();

	/// Revision 004. Adds world-anchored quads: a kClientFlag_WorldQuad client can
	/// submit a per-frame list of billboards drawn at world positions (so they stay
	/// fixed in the scene instead of swimming on the head-locked HUD). Obtain via
	/// GetImGuiVRHelperInterface004(); nullptr against a helper older than 004.
	struct IImGuiVRHelperInterface004 : IImGuiVRHelperInterface003
	{
		/// Submit this frame's world-billboard list for a kClientFlag_WorldQuad
		/// client. Replaces the previous frame's list wholesale; `count == 0`
		/// (quads may be null) clears it so nothing draws. Each WorldQuad samples
		/// a sub-rect of the client's own panel texture (rendered as usual via
		/// GetPanel / RenderToPanel) and is drawn as a camera-facing billboard at
		/// `pos` (Skyrim world-space, game units — the helper converts to OpenVR
		/// tracking space itself at Submit time). Call every frame the client wants
		/// its quads shown; a frame with no call keeps the prior list, so clients
		/// that go idle should submit `count == 0`. No-op for unknown ids or
		/// clients that didn't set kClientFlag_WorldQuad. The helper caps the list
		/// at 4096 quads per client; any beyond that are ignored.
		virtual void SubmitWorldQuads(uint32_t client_id, const WorldQuad* quads,
			std::size_t count) = 0;
	};

	/// Handshake for revision 004. Returns nullptr if the installed helper predates 004.
	IImGuiVRHelperInterface004* GetImGuiVRHelperInterface004();

	/// Revision 005. Adds a client-driven reposition drag: an alternative to the helper's
	/// off-panel grip-drag gesture for clients whose panel coexists with a game menu that binds
	/// grip to something else (e.g. Skyrim's dialogue menu closes on grip), so the normal
	/// off-panel gesture can't be used at all while that menu is up. Obtain via
	/// GetImGuiVRHelperInterface005(); nullptr against a helper older than 005.
	struct IImGuiVRHelperInterface005 : IImGuiVRHelperInterface004
	{
		/// Call every frame you want a reposition drag active — e.g. while a wand-clickable
		/// "Move" button in your own panel is held down with the trigger (ImGui's IsItemActive()
		/// idiom: stays true for the whole hold, even after the wand ray drifts off the button
		/// or off the panel entirely, which it will as the user physically moves their hand to
		/// drag — don't gate this call on the wand still being on-panel). Uses the same drag
		/// machinery as the off-panel grip gesture (same settings persisted on release), driven
		/// by whichever controller was pointing at your panel when the drag started. No-op if
		/// `client_id` doesn't currently hold focus or if drag-to-reposition is disabled in
		/// settings. This is a heartbeat, not a toggle: stop calling (e.g. on trigger release) to
		/// end the drag and persist the new position — there is no separate "release" call.
		virtual void RequestReposition(uint32_t client_id) = 0;
	};

	/// Handshake for revision 005. Returns nullptr if the installed helper predates 005.
	IImGuiVRHelperInterface005* GetImGuiVRHelperInterface005();

	/// Revision 006 adds explicitly negotiated CSX composition without changing legacy clients.
	/// Render-thread methods are synchronous; no client callbacks or OpenVR submission occur within them.
	struct IImGuiVRHelperInterface006 : IImGuiVRHelperInterface005
	{
		/// Available before renderer initialization; outputs require their default size and version fields.
		virtual RenderHostResult QueryRenderHostCapabilities(RenderHostCapabilities* out) = 0;
		/// Registration reserves an inactive owner and leaves autonomous rendering enabled.
		virtual RenderHostResult RegisterRenderHost(const RenderHostRegistration* registration, std::uint64_t* outHost) = 0;
		/// Activate/deactivate only on the established render thread between stereo pairs.
		virtual RenderHostResult SetRenderHostActive(std::uint64_t host, std::uint32_t active) = 0;
		/// Render-thread candidate query between pairs; allowed for registered inactive hosts.
		virtual RenderHostResult QueryRenderHostContent(std::uint64_t host, RenderHostContent* out) = 0;
		/// Validates both eyes and freezes content; Success returns a cookie that must be closed.
		virtual RenderHostResult BeginHostedFrame(std::uint64_t host, const HostedFrameInfo* frame, HostedFrameHandle* out) = 0;
		/// Draws one eye synchronously; a failed written target must be discarded by the host.
		virtual RenderHostResult RenderHostedEye(std::uint64_t host, std::uint64_t cookie, const HostedEyeContext* eye, HostedEyeResult* out) = 0;
		/// Closes a prepared frame between eye calls; IncompletePair also closes when an eye did not finish.
		virtual RenderHostResult EndHostedFrame(std::uint64_t host, std::uint64_t cookie) = 0;
		/// Releases a prepared frame without submission; the host must discard any already written targets.
		virtual RenderHostResult AbortHostedFrame(std::uint64_t host, std::uint64_t cookie) = 0;
		/// Requires inactive ownership and no open frame; lifecycle registration may occur off the render thread.
		virtual RenderHostResult UnregisterRenderHost(std::uint64_t host) = 0;
	};

	/// Returns nullptr against an absent or older helper; never gates on GetBuildNumber.
	IImGuiVRHelperInterface006* GetImGuiVRHelperInterface006();

}  // namespace ImGuiVRHelperPluginAPI
