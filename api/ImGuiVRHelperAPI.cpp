// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2025 ImGuiVRHelper contributors. See api/COPYING.LESSER.
//
// Client-side handshake stub. Compiled into the client mod's binary. Dispatches
// an SKSE message to the helper and caches the resulting interface pointer.
// The handshake is RETRYABLE: an early call can return null if the helper's
// messaging listener isn't registered yet (plugin load-order race), so failure
// is NOT latched — keep calling (e.g. once per frame until Connect succeeds).

#include "ImGuiVRHelperAPI.h"

namespace ImGuiVRHelperPluginAPI
{
	namespace
	{
		IImGuiVRHelperInterface001* g_interface001 = nullptr;
		IImGuiVRHelperInterface002* g_interface002 = nullptr;
		IImGuiVRHelperInterface003* g_interface003 = nullptr;
		IImGuiVRHelperInterface004* g_interface004 = nullptr;
		IImGuiVRHelperInterface005* g_interface005 = nullptr;
		IImGuiVRHelperInterface006* g_interface006 = nullptr;

		// Run the SKSE handshake and return the helper's GetApiFunction, or nullptr
		// if the helper isn't up yet (retryable — never latched).
		void* (*Handshake())(uint32_t)
		{
			const auto* messaging = SKSE::GetMessagingInterface();
			if (!messaging) {
				return nullptr;
			}
			Message msg{};
			messaging->Dispatch(
				Message::kMessage_GetInterface,
				static_cast<void*>(&msg),
				sizeof(Message*),
				kPluginName);
			return msg.GetApiFunction;
		}
	}

	IImGuiVRHelperInterface001* GetImGuiVRHelperInterface001()
	{
		if (g_interface001) {
			return g_interface001;
		}
		auto* getApi = Handshake();
		if (!getApi) {
			return nullptr;  // helper not ready yet — safe to retry next call
		}
		g_interface001 = static_cast<IImGuiVRHelperInterface001*>(getApi(1));
		return g_interface001;
	}

	IImGuiVRHelperInterface002* GetImGuiVRHelperInterface002()
	{
		if (g_interface002) {
			return g_interface002;
		}
		auto* getApi = Handshake();
		if (!getApi) {
			return nullptr;  // helper not ready yet — retry; or simply older than 002
		}
		g_interface002 = static_cast<IImGuiVRHelperInterface002*>(getApi(2));
		return g_interface002;
	}

	IImGuiVRHelperInterface003* GetImGuiVRHelperInterface003()
	{
		if (g_interface003) {
			return g_interface003;
		}
		auto* getApi = Handshake();
		if (!getApi) {
			return nullptr;  // helper not ready yet — retry; or simply older than 003
		}
		g_interface003 = static_cast<IImGuiVRHelperInterface003*>(getApi(3));
		return g_interface003;
	}

	IImGuiVRHelperInterface004* GetImGuiVRHelperInterface004()
	{
		if (g_interface004) {
			return g_interface004;
		}
		auto* getApi = Handshake();
		if (!getApi) {
			return nullptr;  // helper not ready yet — retry; or simply older than 004
		}
		g_interface004 = static_cast<IImGuiVRHelperInterface004*>(getApi(4));
		return g_interface004;
	}

	IImGuiVRHelperInterface005* GetImGuiVRHelperInterface005()
	{
		if (g_interface005) {
			return g_interface005;
		}
		auto* getApi = Handshake();
		if (!getApi) {
			return nullptr;  // helper not ready yet — retry; or simply older than 005
		}
		g_interface005 = static_cast<IImGuiVRHelperInterface005*>(getApi(5));
		return g_interface005;
	}

	IImGuiVRHelperInterface006* GetImGuiVRHelperInterface006()
	{
		if (g_interface006)
			return g_interface006;
		auto* getApi = Handshake();
		if (!getApi)
			return nullptr;
		g_interface006 = static_cast<IImGuiVRHelperInterface006*>(getApi(6));
		return g_interface006;
	}
}
