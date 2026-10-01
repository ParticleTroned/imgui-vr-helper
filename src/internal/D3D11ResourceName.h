// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include <d3d11.h>
#include <cstring>

#pragma comment(lib, "dxguid.lib")

namespace ImGuiVRHelper::Util
{
	/// Give helper-owned resources and states stable names in graphics captures.
	inline void SetResourceName(ID3D11DeviceChild* resource, const char* name) noexcept
	{
		if (resource && name)
			resource->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(std::strlen(name)), name);
	}
}
