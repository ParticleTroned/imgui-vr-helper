// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// SKSE plugin entry point. Registers a messaging listener for the public
// API handshake (kMessage_GetInterface) and answers with a function pointer
// that returns the requested IImGuiVRHelperInterfaceNNN* singleton.

#include "pch.h"

#include "DevBenchBridge.h"
#include "HelperImpl.h"
#include "Hooks.h"
#include "OpenVRDetection.h"
#include "Overlay.h"
#include "PluginVersion.h"
#include "VrikCompat.h"

#include <exception>

namespace
{
	using namespace ImGuiVRHelperPluginAPI;

	/// Returns an interface pointer for the requested revision, or nullptr if
	/// unsupported.
	void* GetApiFunction(uint32_t revision) noexcept
	{
		try {
			switch (revision) {
			case 1:
				return static_cast<IImGuiVRHelperInterface001*>(
					&ImGuiVRHelper::HelperImpl::GetSingleton());
			case 2:
				return static_cast<IImGuiVRHelperInterface002*>(
					&ImGuiVRHelper::HelperImpl::GetSingleton());
			case 3:
				return static_cast<IImGuiVRHelperInterface003*>(
					&ImGuiVRHelper::HelperImpl::GetSingleton());
			case 4:
				return static_cast<IImGuiVRHelperInterface004*>(
					&ImGuiVRHelper::HelperImpl::GetSingleton());
			case 5:
				return static_cast<IImGuiVRHelperInterface005*>(
					&ImGuiVRHelper::HelperImpl::GetSingleton());
			case 6:
				return static_cast<IImGuiVRHelperInterface006*>(
					&ImGuiVRHelper::HelperImpl::GetSingleton());
			default:
				logs::warn("GetApiFunction: unsupported interface revision {}", revision);
				return nullptr;
			}
		} catch (...) {
			logs::error("GetApiFunction: exception fetching revision {}", revision);
			return nullptr;
		}
	}

	// Two listeners are registered below:
	//   * OnSKSELifecycle: filter="SKSE", catches kPostLoad/kPostPostLoad/
	//     kInputLoaded/kDataLoaded — what we actually act on.
	//   * OnPluginMessage: wildcard filter, catches client handshakes
	//     (kMessage_GetInterface) sent by SCS / other API consumers.
	//
	// We split them because passing nullptr as the sender filter to the
	// proxy turned out NOT to be a true wildcard for SKSE-emitted
	// messages — empirically it delivered plugin broadcasts but skipped
	// SKSE's own lifecycle dispatches (sender="SKSE"). The single-arg
	// RegisterListener overload defaults to filter="SKSE", which is the
	// canonical pattern every other SKSE plugin uses for lifecycle.

	// Defined below; registered at kPostLoad (see the kPostLoad case).
	void OnPluginMessage(SKSE::MessagingInterface::Message* msg);

	void OnSKSELifecycle(SKSE::MessagingInterface::Message* msg)
	try {
		if (!msg)
			return;

		logs::info("SKSE lifecycle: type={} sender='{}' dataLen={}",
			static_cast<uint32_t>(msg->type),
			msg->sender ? msg->sender : "<null>",
			msg->dataLen);

		switch (msg->type) {
		case SKSE::MessagingInterface::kPostLoad:
			// Load persisted settings as early as possible so InitD3D-time
			// resource allocation sees the user's configured offsets/scale.
			// Also write a default config to disk on first run so users
			// have a discoverable file to edit (e.g. to bump logLevel for
			// diagnostics without rebuilding).
			ImGuiVRHelper::Overlay::LoadSettings();
			ImGuiVRHelper::Overlay::WriteDefaultsIfMissing();
			ImGuiVRHelper::Overlay::ApplyLogLevel();

			// Register the client-handshake (wildcard) listener HERE, not in
			// SKSEPluginLoad. In SKSEVR a null-sender listener is attached only
			// to plugins ALREADY LOADED at registration time (PluginManager
			// copies it into each existing sender's list). Registering during
			// SKSEPlugin_Load — mid-load — therefore reaches only clients that
			// sort before us; clients loading later (e.g. SKSEMenuFramework)
			// could never hand-shake. By kPostLoad every plugin has finished
			// loading, so the listener reaches all of them regardless of load
			// order. This is the canonical pattern (cf. SkyrimVRESL, and the
			// SKSE PluginAPI.h messaging notes). Clients correspondingly
			// request the interface at kPostPostLoad.
			if (auto* m = SKSE::GetMessagingInterface();
				!m || !m->RegisterListener(nullptr, OnPluginMessage)) {
				logs::error("failed to register client handshake listener");
			}
			break;

		case SKSE::MessagingInterface::kPostPostLoad:
			{
				// Install hooks here, NOT at kDataLoaded — by the time
				// kDataLoaded fires, Skyrim's renderer has already
				// initialized (BSGraphics::Renderer::InitD3D has been
				// called) and the thunk we'd write to that call site
				// would never execute. kPostPostLoad fires before the
				// renderer is up, so the thunk catches the original
				// call. This matches SCS's XSEPlugin.cpp:81-86 pattern.
				VRDetection::LogDetectionResult(VRDetection::Detect());

				ImGuiVRHelper::Hooks::Install();

				// All plugins have finished loading by kPostPostLoad, so the handshake reaches
				// VRIK regardless of load order. Safe no-op if VRIK isn't installed.
				ImGuiVRHelper::VrikCompat::TryFetchInterface();

				// Same timing argument for devbench: its listener is up by now.
				// Runtime no-op when devbench isn't installed.
				ImGuiVRHelper::DevBenchBridge::Install();
			}
			break;

		case SKSE::MessagingInterface::kInputLoaded:
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			// Entered the world (loaded a save or started new) — dismiss the
			// startup welcome banner.
			ImGuiVRHelper::HelperImpl::GetSingleton().NotifyEnteredGame();
			break;
		default:
			break;
		}
	} catch (const std::exception& e) {
		logs::error("OnSKSELifecycle: exception handling message type {}: {}",
			msg ? static_cast<uint32_t>(msg->type) : 0u, e.what());
	} catch (...) {
		logs::error("OnSKSELifecycle: unknown exception handling message type {}",
			msg ? static_cast<uint32_t>(msg->type) : 0u);
	}

	void OnPluginMessage(SKSE::MessagingInterface::Message* msg)
	try {
		if (!msg)
			return;

		// Skip SKSE-emitted lifecycle messages here — OnSKSELifecycle owns
		// those. Without this guard we'd double-process if the wildcard
		// filter happens to also deliver them.
		if (msg->sender && std::string_view(msg->sender) == "SKSE") {
			return;
		}

		logs::info("Plugin message: type={} sender='{}' dataLen={}",
			static_cast<uint32_t>(msg->type),
			msg->sender ? msg->sender : "<null>",
			msg->dataLen);

		// Handshake from a client requesting our API surface. Gated on IsVR so a client on flat
		// Skyrim sees "not installed" (client()->IsConnected() stays false — GetApiFunction is
		// never left null, so it can't return a real interface either) rather than a false
		// "connected" that Hooks::Install() (also IsVR-gated) never actually backs with a
		// working render path — a hybrid SE/AE/VR client would otherwise believe a fully broken
		// helper is live and skip its own working flat-native fallback.
		if (msg->type == Message::kMessage_GetInterface &&
			msg->dataLen >= sizeof(Message*) && REL::Module::IsVR()) {
			auto* req = static_cast<Message*>(msg->data);
			req->GetApiFunction = &GetApiFunction;
			logs::info("Handshake from '{}' -> GetApiFunction installed",
				msg->sender ? msg->sender : "<unknown>");
		}
	} catch (const std::exception& e) {
		logs::error("OnPluginMessage: exception from sender '{}': {}",
			msg && msg->sender ? msg->sender : "<unknown>", e.what());
	} catch (...) {
		logs::error("OnPluginMessage: unknown exception from sender '{}'",
			msg && msg->sender ? msg->sender : "<unknown>");
	}

}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);  // initializes logging via the bundled CommonLibSSE-NG default

	logs::info("ImGuiVRHelper {} loading", IMGUI_VR_HELPER_VERSION_STRING);

	if (!REL::Module::IsVR()) {
		logs::warn("Not running in Skyrim VR; helper will remain dormant.");
		// We still register the messaging listener so clients dispatching
		// the handshake don't time out. The interface methods will simply
		// return failure for everything until a VR runtime is detected.
	}

	auto* messaging = SKSE::GetMessagingInterface();
	if (!messaging) {
		SKSE::stl::report_and_fail("SKSE messaging interface unavailable"sv);
	}
	// Single-arg overload internally sets sender filter = "SKSE", which is
	// the only filter SKSE's dispatcher reliably matches against its own
	// lifecycle messages (kPostLoad/kPostPostLoad/kInputLoaded/kDataLoaded).
	if (!messaging->RegisterListener(OnSKSELifecycle)) {
		SKSE::stl::report_and_fail("failed to register SKSE lifecycle listener"sv);
	}
	// NOTE: the wildcard client-handshake listener (RegisterListener(nullptr,
	// OnPluginMessage)) is intentionally NOT registered here. In SKSEVR a
	// null-sender listener only attaches to plugins already loaded at
	// registration time, so registering mid-load would miss any client that
	// loads after us. It is registered at kPostLoad instead (see
	// OnSKSELifecycle), once all plugins exist.

	logs::info("ImGuiVRHelper loaded");
	return true;
}
