// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include "ImGuiVRHelperRenderHost.h"

namespace ImGuiVRHelper::RenderHost
{
	namespace API = ImGuiVRHelperPluginAPI;
	/// Observes the game swapchain's current Present thread without activating hosting or transferring an open pair.
	void ObserveRenderThread() noexcept;
	[[nodiscard]] bool IsActive() noexcept;
	API::RenderHostResult QueryCapabilities(API::RenderHostCapabilities* out) noexcept;
	API::RenderHostResult Register(const API::RenderHostRegistration* registration, std::uint64_t* outHost) noexcept;
	API::RenderHostResult SetActive(std::uint64_t host, std::uint32_t active) noexcept;
	API::RenderHostResult QueryContent(std::uint64_t host, API::RenderHostContent* out) noexcept;
	API::RenderHostResult Begin(std::uint64_t host, const API::HostedFrameInfo* frame, API::HostedFrameHandle* out) noexcept;
	API::RenderHostResult RenderEye(std::uint64_t host, std::uint64_t cookie, const API::HostedEyeContext* eye, API::HostedEyeResult* out) noexcept;
	API::RenderHostResult End(std::uint64_t host, std::uint64_t cookie, bool abort = false) noexcept;
	API::RenderHostResult Unregister(std::uint64_t host) noexcept;
}
