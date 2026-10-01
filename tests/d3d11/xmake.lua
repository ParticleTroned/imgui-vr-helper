-- SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
-- Standalone entry point avoids resolving the plugin's game dependencies.

set_xmakever("2.8.2")
set_project("ImGuiVRHelperD3DTests")
set_arch("x64")
add_rules("mode.debug", "mode.release")
set_defaultmode("release")
includes("target.lua")
