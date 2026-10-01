// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Concrete implementation of ImGuiVRHelperPluginAPI::IImGuiVRHelperInterface001:
// the client registry, per-client panel textures, combo matching/recording,
// focus arbitration, and per-frame dispatch. Portions are lifted from
// skyrim-community-shaders/src/Features/VR/{Input,InSceneOverlay,
// OverlayDrag,WandPointing,OpenVRDetection}.cpp.

#pragma once

#include "ImGuiVRHelperAPI.h"
#include "internal/InputLeases.h"

#include <optional>

namespace ImGuiVRHelper
{
	class HelperImpl final : public ImGuiVRHelperPluginAPI::IImGuiVRHelperInterface006
	{
	public:
		static HelperImpl& GetSingleton();

		// IImGuiVRHelperInterface001
		uint32_t GetBuildNumber() override;

		uint32_t RegisterClient(const char* name, const char* version,
			ImGuiVRHelperPluginAPI::OnFrameFn on_frame, void* user, uint32_t flags) override;
		void UnregisterClient(uint32_t client_id) override;

		bool GetPanel(uint32_t client_id, ImGuiVRHelperPluginAPI::PanelHandle* out) override;
		bool GetPointer(uint32_t client_id, float* u, float* v, uint32_t* device_idx) override;

		ImGuiVRHelperPluginAPI::ComboId RegisterCombo(uint32_t client_id,
			const ImGuiVRHelperPluginAPI::InputCombo* keys, std::size_t n,
			float timeout_s, const char* label,
			ImGuiVRHelperPluginAPI::ComboRebindFn on_rebind, void* user) override;
		bool ComboFired(ImGuiVRHelperPluginAPI::ComboId) override;
		void StartComboRecording(uint32_t client_id, const char* label,
			ImGuiVRHelperPluginAPI::ComboRecordedFn on_done, void* user,
			float timeout_s) override;
		void CancelComboRecording(uint32_t client_id) override;

		bool IsOverlayVisible() override;
		void RequestFocus(uint32_t client_id) override;
		void ReleaseFocus(uint32_t client_id) override;

		void TriggerHaptic(uint32_t client_id, uint32_t haptic_token,
			uint32_t duration_us, float frequency, float amplitude) override;

		bool ImportLegacySettings(const char* json_blob) override;

		void FeedVREvent(uint32_t device, uint32_t key_code, bool pressed,
			float thumbstick_x, float thumbstick_y) override;

		bool IsDashboardVisible() override;

		// IImGuiVRHelperInterface002 (VR text entry). Thin delegates to the
		// VRKeyboard bridge, which marshals OpenVR calls to the input thread.
		void SetKeyboardActive(uint32_t client_id, bool active, const char* seed_utf8) override;
		uint32_t GetKeyboardText(uint32_t client_id, char* out_utf8, uint32_t out_size) override;
		bool ConsumeKeyboardClosed(uint32_t client_id) override;

		// IImGuiVRHelperInterface003 (per-combo off-panel context).
		void SetComboOffPanel(ImGuiVRHelperPluginAPI::ComboId combo, bool off_panel) override;

		// IImGuiVRHelperInterface004 (world-anchored quads).
		void SubmitWorldQuads(uint32_t client_id,
			const ImGuiVRHelperPluginAPI::WorldQuad* quads, std::size_t count) override;

		// IImGuiVRHelperInterface005 (client-driven reposition drag).
		void RequestReposition(uint32_t client_id) override;

		ImGuiVRHelperPluginAPI::RenderHostResult QueryRenderHostCapabilities(ImGuiVRHelperPluginAPI::RenderHostCapabilities* out) override;
		ImGuiVRHelperPluginAPI::RenderHostResult RegisterRenderHost(const ImGuiVRHelperPluginAPI::RenderHostRegistration* registration, std::uint64_t* outHost) override;
		ImGuiVRHelperPluginAPI::RenderHostResult SetRenderHostActive(std::uint64_t host, std::uint32_t active) override;
		ImGuiVRHelperPluginAPI::RenderHostResult QueryRenderHostContent(std::uint64_t host, ImGuiVRHelperPluginAPI::RenderHostContent* out) override;
		ImGuiVRHelperPluginAPI::RenderHostResult BeginHostedFrame(std::uint64_t host, const ImGuiVRHelperPluginAPI::HostedFrameInfo* frame, ImGuiVRHelperPluginAPI::HostedFrameHandle* out) override;
		ImGuiVRHelperPluginAPI::RenderHostResult RenderHostedEye(std::uint64_t host, std::uint64_t cookie, const ImGuiVRHelperPluginAPI::HostedEyeContext* eye, ImGuiVRHelperPluginAPI::HostedEyeResult* out) override;
		ImGuiVRHelperPluginAPI::RenderHostResult EndHostedFrame(std::uint64_t host, std::uint64_t cookie) override;
		ImGuiVRHelperPluginAPI::RenderHostResult AbortHostedFrame(std::uint64_t host, std::uint64_t cookie) override;
		ImGuiVRHelperPluginAPI::RenderHostResult UnregisterRenderHost(std::uint64_t host) override;

		// Helper-internal entry points (not part of the public API).

		/// Build the per-frame Frame snapshot via Input::BuildFrame and
		/// invoke each registered client's on_frame callback. Called from
		/// the helper's Present detour. `dt` is seconds since previous call.
		void DispatchFrame(float dt);

		/// Returns a strong ref to the ID3D11Texture2D backing a client's
		/// panel, or null if the client doesn't exist or its texture isn't
		/// allocated yet. Used by InSceneOverlay to feed the eye-render
		/// compositing pass. Strong ref (not a raw pointer) so the caller
		/// holding it is immune to a concurrent UnregisterClient releasing
		/// the last owning reference out from under an in-flight render.
		winrt::com_ptr<ID3D11Texture2D> GetClientPanelTexture(uint32_t client_id);

		/// Texture for the combo-rebind capture overlay while a recording is
		/// active (else null). InSceneOverlay composites it in a dedicated
		/// pass on top of the focused client so the capture reads as a modal.
		winrt::com_ptr<ID3D11Texture2D> GetActiveRebindTexture();

		/// Snapshot of all currently-registered HUD-mode clients
		/// (kClientFlag_HUDMode set in their flags), returned as
		/// (client_id, panel_texture) pairs. Both fields filled — the
		/// texture is the result of EnsureClientTextureLocked, so it's
		/// either a valid ID3D11Texture2D* or nullptr if D3D isn't
		/// ready / texture creation failed. InSceneOverlay::RenderForEye
		/// iterates this list and composites each as a full-viewport
		/// alpha-blended quad before drawing the focused panel-mode
		/// client. Iteration order matches registration order so HUD
		/// layers stack predictably.
		struct HUDClientSnapshot
		{
			uint32_t client_id;
			// Strong ref, mirroring WorldQuadClientSnapshot below — a raw
			// pointer here would race UnregisterClient the same way it did
			// before that fix was applied to the world-quad path only.
			winrt::com_ptr<ID3D11Texture2D> texture;
		};
		std::vector<HUDClientSnapshot> SnapshotHUDClients();

		/// Snapshot of every kClientFlag_WorldQuad client that submitted a
		/// non-empty billboard list this frame, as (client_id, panel_texture,
		/// quads). The texture is the EnsureClientTextureLocked result (the same
		/// panel the client renders into); InSceneOverlay::RenderForEye draws each
		/// quad as a camera-facing billboard at its world position, sampling the
		/// named sub-rect of that texture. Independent of focus / HUD gating.
		struct WorldQuadClientSnapshot
		{
			uint32_t client_id;
			std::string name;  // for GPU debug markers (RenderDoc/PIX event browser)
			// Strong ref so the panel texture can't be released by an UnregisterClient
			// on another thread between snapshot (m_mutex held) and use in RenderForEye.
			winrt::com_ptr<ID3D11Texture2D> texture;
			std::vector<ImGuiVRHelperPluginAPI::WorldQuad> quads;
		};
		std::vector<WorldQuadClientSnapshot> SnapshotWorldQuadClients();

		struct HostedClientSnapshot
		{
			uint64_t contentSerial = 0;
			uint32_t focusedClient = 0;
			uint32_t focusedFlags = 0;
			std::optional<ImGuiVRHelperPluginAPI::Frame> inputFrame;
			winrt::com_ptr<ID3D11Texture2D> panelTexture;
			winrt::com_ptr<ID3D11Texture2D> rebindTexture;
			std::vector<HUDClientSnapshot> hudClients;
			std::vector<WorldQuadClientSnapshot> worldClients;
		};

		/// Freeze registry content under one lock; texture pixels are copied afterward by the renderer.
		std::optional<HostedClientSnapshot> SnapshotHostedClients(bool includeWorld);

		/// Count candidates without invoking clients, allocating textures or copying their lists.
		void QueryHostedContent(uint32_t& layers, uint32_t& worldQuadCount);

		/// Snapshot of every registered client for diagnostic display.
		/// Used by the helper's settings UI to show a 'Registered
		/// Clients' section. Read-only — modifying a client's flags
		/// or focus state via this snapshot does nothing.
		struct ClientSnapshot
		{
			uint32_t client_id;
			std::string name;
			std::string version;  ///< client-supplied at registration; empty if not provided
			uint32_t flags;
			bool has_texture;         ///< texture was allocated (client has called GetPanel)
			bool has_focus;           ///< this client currently holds focus
			bool hud_compositing;     ///< HUD-mode client painted recently → being composited this frame
			bool hud_force_disabled;  ///< debug: compositing suppressed by the HUD-layers debug UI
		};
		std::vector<ClientSnapshot> SnapshotClients();

		/// Snapshot of every registered combo across all clients, for the
		/// controller-mapping UI. `conflict` is set when another combo shares
		/// an identical chord (same keys), so the UI can warn about clashes.
		struct ComboSnapshot
		{
			ImGuiVRHelperPluginAPI::ComboId combo_id;
			uint32_t client_id;
			std::string client_name;
			std::string label;
			std::vector<ImGuiVRHelperPluginAPI::InputCombo> keys;
			bool conflict;
			bool off_panel;  ///< only acts while the wand is off the owning client's panel
		};
		std::vector<ComboSnapshot> SnapshotCombos();

		/// Returns the currently-focused client_id, or 0 if no client
		/// holds focus. Used by InSceneOverlay to pick which client's
		/// panel to draw on each Submit.
		uint32_t GetFocusedClientId();

		/// Registered flags for a client (0 if unknown). Used by the cursor
		/// pass to honor kClientFlag_OwnCursor without a full snapshot.
		uint32_t GetClientFlags(uint32_t client_id);

		/// Returns the registered client's version string.
		std::string GetClientVersion(uint32_t client_id);

		/// Quick select menu state query
		[[nodiscard]] bool IsQuickSelectActive() const { return m_quickSelectActive; }
		[[nodiscard]] int GetQuickSelectHoveredIdx() const { return m_quickSelectHoveredIdx; }

		/// Cycle VR focus to the previous/next overlay (direction -1/+1),
		/// wrapping through the helper's own settings UI and every non-HUD
		/// client. Backs the Prev/Next buttons and the off-panel stick-click
		/// shortcut.
		void CycleOverlay(int direction);

		/// Ordered cyclable overlays — the helper's own UI first, then every
		/// non-HUD client in the user's saved order — as (client_id, display
		/// name). Backs CycleOverlay, the swap-toast counter, the open combo's
		/// first-mod pick, and the "Overlay order" UI.
		std::vector<std::pair<uint32_t, std::string>> BuildOverlayOrder();

		/// Debug: force-hide a HUD-mode client from compositing so a developer
		/// can isolate which layer draws what. No-op for unknown ids.
		void SetHudForceDisabled(uint32_t client_id, bool disabled);

		/// Replace an existing combo's keys in place (same ComboId, so the owning
		/// client keeps polling the same handle). Backs the controller-map UI's
		/// Rebind buttons and the client SDK's bindings table via ComboRecording.
		/// Updates the live keys and invokes the combo's on_rebind so the owner
		/// can persist the new chord.
		void RebindCombo(ImGuiVRHelperPluginAPI::ComboId combo,
			const ImGuiVRHelperPluginAPI::InputCombo* keys, std::size_t n) override;

		/// Allocate the helper's own self-client (so the helper's settings
		/// UI gets a panel texture via the same per-client allocation
		/// pipeline as external clients). Idempotent.
		void EnsureSelfClient();

		/// True iff the helper's own settings UI is currently visible.
		bool IsSelfUIVisible();

		/// client_id of the helper's synthetic self-client, or 0 before
		/// EnsureSelfClient has run.
		uint32_t GetSelfClientId() const;

		/// Replace the self-toggle combo with `keys` (length `n`). Used
		/// by SettingsUI's "Rebind toggle" button after combo recording
		/// completes. No-op if `n` == 0.
		void RebindSelfToggle(const ImGuiVRHelperPluginAPI::InputCombo* keys, std::size_t n);

		/// Replace the helper-owned open- / close-menu combos (the chords that
		/// bring up / dismiss the active mod overlay) with `keys`. Updates the
		/// live combo and persists to Settings. No-op if `n` == 0.
		void RebindOpenMenu(const ImGuiVRHelperPluginAPI::InputCombo* keys, std::size_t n);
		void RebindCloseMenu(const ImGuiVRHelperPluginAPI::InputCombo* keys, std::size_t n);

		/// Called from the PollInputDevices thunk on a keyboard-toggle
		/// key-down. Equivalent to firing the controller toggle combo —
		/// flips the helper's settings-UI visibility and adjusts focus.
		/// Exists because the main menu can swallow controller buttons
		/// before they reach our combo matcher; keyboard input always
		/// reaches us.
		void OnKeyboardToggle();

		/// Called from the SKSE messaging listener on kPostLoadGame/kNewGame.
		/// Dismisses the startup welcome banner once the user enters the world.
		void NotifyEnteredGame();

		/// True iff VR controller input should be intercepted from the
		/// game this frame — i.e. some helper UI is up and consuming
		/// trigger / grip / stick events. The PollInputDevices thunk
		/// swallows controller events when this returns true so menu
		/// clicks don't also fire bows / scroll thumbsticks don't move
		/// the player camera. Mirrors SCS Menu::ShouldSwallowInput.
		bool ShouldSwallowInput() const;

		/// True when the focused client opted into kClientFlag_LiveTool. The
		/// PollInputDevices thunk uses this to forward locomotion (thumbstick)
		/// to the game while still swallowing buttons for the panel, so a live
		/// tool can keep driving the unpaused world (e.g. fly a free camera).
		bool IsLiveToolFocused() const;

		/// One-shot JSON dump of live input/focus state for the devbench bridge
		/// (wand hit, focus, clients + panel dims, held masks, lease strips,
		/// drag state, relevant settings). Diagnostic reads only — values may be
		/// one frame stale; called from devbench's listener thread.
		std::string DiagnosticsJson() const;

		/// Queue a client's panel texture to be saved as a PNG (devbench bridge).
		/// Thread-safe; the write happens on the render thread in the next
		/// DispatchFrame (ServicePanelDumps) since it uses the immediate context.
		void RequestPanelDump(uint32_t client_id, const std::string& path);

	private:
		HelperImpl() = default;

		mutable std::mutex m_mutex;  // mutable: DiagnosticsJson() is const but must lock to snapshot clients
		uint32_t m_next_client_id = 1;
		std::unordered_map<uint32_t, struct ClientRecord> m_clients;
		std::unordered_map<ImGuiVRHelperPluginAPI::ComboId,
			struct ComboRecord>
			m_combos;
		ImGuiVRHelperPluginAPI::ComboId m_next_combo_id = 1;
		ImGuiVRHelperPluginAPI::ComboId m_combo_id_salt = 0;
		bool m_combo_id_salt_seeded = false;

		/// Mints the next ComboId returned by RegisterCombo. The ids are
		/// salted and permuted to avoid trivial enumeration.
		ImGuiVRHelperPluginAPI::ComboId NextComboIdLocked();
		uint32_t m_focused_client = 0;
		uint64_t m_frameCounter = 0;  ///< ++ each DispatchFrame; HUD-idle skipping uses it
		std::optional<ImGuiVRHelperPluginAPI::Frame> m_hostedInputFrame;
		uint64_t m_hostedInputFrameSerial = 0;

		/// Allocate (or return existing) per-client overlay texture
		/// resources. Returns true on success, false if D3D isn't ready
		/// or texture creation failed. Must be called with m_mutex held.
		bool EnsureClientTextureLocked(struct ClientRecord& rec);

		// Pixel size of a client's panel texture: the configured base resolution
		// (Settings::baseWidth/Height), supersampled (Settings::hudSupersample)
		// for view-filling panels — HUD-mode layers and the self/settings panel,
		// which carries the toasts. Other (focusable, 1:1) panels render at base.
		void PanelPixelSize(const struct ClientRecord& rec, unsigned int& width, unsigned int& height) const;

		/// client_id assigned to the helper itself for its settings UI.
		/// 0 until EnsureSelfClient runs. Reserved combo IDs and the
		/// helper's panel texture are owned by this slot.
		uint32_t m_self_client_id = 0;

		/// Combo IDs the helper registers for its own toggle hotkey.
		ImGuiVRHelperPluginAPI::ComboId m_self_toggle_combo = 0;

		/// Helper-owned combos for opening / closing the active overlay (the
		/// menu activation taken over from clients). Keys persist in
		/// Overlay::Settings; rebinds route through their on_rebind.
		ImGuiVRHelperPluginAPI::ComboId m_open_menu_combo = 0;
		ImGuiVRHelperPluginAPI::ComboId m_close_menu_combo = 0;

		/// Last client overlay that held focus (the helper's own settings UI is
		/// excluded — it has its own toggle). The open combo reopens this; 0
		/// falls back to the first mod overlay.
		uint32_t m_lastOpenedOverlay = 0;

		/// Resolve what the open combo should focus: the last-opened client
		/// overlay if still registered, else the first non-helper overlay.
		uint32_t PickOpenTarget();

		// Previous stick-click state per hand, for edge-detecting the off-panel
		// overlay-cycle shortcut (left = prev, right = next).
		bool m_prevLeftStickClick = false;
		bool m_prevRightStickClick = false;

		/// Synthetic HUD-mode client used by the Settings::showHUDDemo
		/// smoke test. Registered once during EnsureSelfClient. Each
		/// frame DispatchFrame checks the toggle and clears this
		/// client's panel RTV with a red wash so the user can verify
		/// the kClientFlag_HUDMode composite path end-to-end before
		/// any real client connects.
		uint32_t m_hud_demo_client_id = 0;

		/// HUD-mode client for the swap toast: a transient banner naming the
		/// overlay focus just switched to, composited over whatever is focused.
		uint32_t m_toast_client_id = 0;
		std::string m_toastText;
		float m_toastRemaining = 0.0f;     ///< seconds left on the toast; counts down by dt
		uint32_t m_lastFocusForToast = 0;  ///< last focused id, to detect swaps

		/// Panel for the combo-rebind capture overlay. Registered HUD-mode so
		/// it's excluded from cycling, but skipped by SnapshotHUDClients — it
		/// renders in its own InSceneOverlay pass ON TOP of the focused client
		/// (not the head-locked HUD pass), so the client's menu stays behind it.
		uint32_t m_rebind_client_id = 0;
		bool m_rebindWasActive = false;  ///< for the active→inactive clear-once

		// After a rebind capture ends, mask client input until all buttons clear
		// so the keys held during capture don't leak into the focused menu.
		bool m_prevRecording = false;
		bool m_inputSettling = false;

		// Pending devbench panel-dump requests (client_id -> PNG path). Queued from
		// the listener thread; drained on the render thread in ServicePanelDumps.
		std::mutex m_dumpMutex;
		std::vector<std::pair<uint32_t, std::string>> m_pendingDumps;
		void ServicePanelDumps();

		// Same masking treatment for an in-progress overlay reposition drag: grip both starts that
		// drag and maps to a client's right-click, so if the wand ray sweeps onto the panel mid-drag
		// the still-held grip would otherwise be forwarded as a right-click the user never intended,
		// and the drag's eventual grip release could leak in as a spurious click of its own.
		// (Legacy path only — with settings.useInputLeases the lease table below replaces
		// both settle latches and the SuppressInputForwarding flag.)
		bool m_prevDragging = false;
		bool m_dragInputSettling = false;

		// Route-on-press button leases: a press claims its route (client / drag /
		// modal) and the hold+release follow it unconditionally, so gating-state
		// changes mid-hold can no longer strand a half-delivered press/release
		// pair (the "clicks stop registering after grip-move" class).
		InputLeases::Table m_leases;
		bool m_prevLeftHanded = false;  ///< for detecting the handedness-flip Reset() edge
		// Mirror of m_leases.StrippedBits() for DiagnosticsJson() (devbench's listener
		// thread): m_leases itself is plain (non-atomic) state mutated only from
		// DispatchToClients on the render thread, so a cross-thread read of it would
		// race. Updated alongside every m_leases.Update() call.
		std::atomic<uint32_t> m_diagStripLeft{ 0 };
		std::atomic<uint32_t> m_diagStripRight{ 0 };

		// Startup welcome banner (HUD-mode). Shows once at launch, then
		// dismisses after a timeout, on entering the game, or if disabled.
		uint32_t m_welcome_client_id = 0;
		bool m_welcomeStarted = false;
		bool m_welcomeDismissed = false;
		bool m_welcomeWasShown = false;
		float m_welcomeRemaining = 0.0f;
		bool m_enteredGame = false;  ///< set from the SKSE message thread (one-way latch)

		// Quick Select Menu variables
		float m_leftStickClickDuration = 0.0f;
		float m_rightStickClickDuration = 0.0f;
		bool m_quickSelectActive = false;
		bool m_quickSelectLeftHand = false;
		int m_quickSelectHoveredIdx = 0;
		float m_quickSelectScrollTimer = 0.0f;
		bool m_quickSelectStickReleased = false;
		uint32_t m_quickSelectOriginalFocus = 0;

		// DispatchFrame phases, run in order each frame. Split out only for
		// readability; each touches helper state directly.
		void UpdateWandPointer();
		/// Poke (direct-touch) click hysteresis, called from UpdateWandPointer
		/// with its fresh isIntersecting/depthMeters. On an engage/release
		/// transition, synthesizes a real kTrigger press/release via
		/// Input::SetSyntheticButtonState so every client -- old or new -- sees
		/// it through the same TriggerClick path a physical trigger pull already uses.
		void UpdatePokeContact(bool isPokeCandidate, float depthMeters);
		bool m_pokeContactLatched = false;
		void ReconcileSelfFocusAndCombos();
		/// Edge-detect the off-panel stick-click overlay-cycle shortcut and
		/// dispatch CycleOverlay. Runs every frame; no-ops while the wand is on
		/// the panel so it doesn't fight menu interaction.
		void ProcessOverlayCycleInput(float dt);
		/// Snapshot clients under the lock, render the self UI, then run each
		/// client's on_frame. Returns the focused client id used this frame.
		uint32_t DispatchToClients(const ImGuiVRHelperPluginAPI::Frame& baseFrame, float dt);
		void PostProcessFrame(uint32_t focused, float dt);
	};

	struct ClientRecord
	{
		std::string name;
		std::string version;  ///< client-supplied at registration; empty if not provided
		ImGuiVRHelperPluginAPI::OnFrameFn on_frame = nullptr;
		void* user = nullptr;
		uint32_t flags = 0;

		// HUD compositing bookkeeping. lastPanelFrame is the frame counter at the
		// most recent GetPanel call; the HUD pass composites a HUD-mode client
		// only when it painted recently, so idle HUD layers cost nothing.
		// hudForceDisabled lets the debug UI hide a layer to isolate it.
		uint64_t lastPanelFrame = 0;
		bool hudForceDisabled = false;

		// Helper-owned panel render target. Allocated lazily on the first
		// GetPanel call after InitD3D has fired. Format is fixed at
		// R8G8B8A8_UNORM; dimensions match Overlay::Config::kOverlayWidth/
		// kOverlayHeight (1920×1080).
		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<ID3D11RenderTargetView> rtv;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;

		// Per-frame world-billboard list for kClientFlag_WorldQuad clients,
		// replaced wholesale by SubmitWorldQuads. Empty for everyone else.
		std::vector<ImGuiVRHelperPluginAPI::WorldQuad> worldQuads;
	};

	struct ComboRecord
	{
		uint32_t client_id = 0;
		std::string label;  ///< human-readable name, for the controller-mapping UI
		std::vector<ImGuiVRHelperPluginAPI::InputCombo> keys;
		float timeout_s = 0.0f;
		bool latched = false;                                       ///< edge-fired, cleared on read
		bool was_matched = false;                                   ///< previous-frame match, for rising edge detection
		bool off_panel = false;                                     ///< context: off-panel-only combo (Interface003)
		ImGuiVRHelperPluginAPI::ComboRebindFn on_rebind = nullptr;  ///< client persist hook
		void* on_rebind_user = nullptr;
	};
}
