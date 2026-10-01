// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Helper hook installation. Hooks land at SKSE post-data-load via the
// SKSE::AllocTrampoline + write_thunk_call pattern that SCS uses.

#pragma once

namespace ImGuiVRHelper::Hooks
{
	/// Install all helper hooks. Call once after kDataLoaded but before
	/// the first frame renders. Allocates the trampoline and writes
	/// thunk calls for BSGraphics::Renderer::InitD3D.
	void Install();

	/// Initialize ordinary dispatch/input services for an explicitly negotiated render-thread host.
	bool EnsureHostedServices();
}
