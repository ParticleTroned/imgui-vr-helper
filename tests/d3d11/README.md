# D3D11 state regression fixture

This independent Windows executable validates the private state guard for
the planned opt-in CSX renderer path. No production renderer currently calls
that guard. See the [design and compatibility contract](../../docs/renderer-host-design.md).

The fixture needs xmake, Visual Studio with the C++ workload, and the Windows
SDK. WARP supplies a software D3D11 device; no game, headset, CommonLib or
client mods are needed. Run from the repository root:

```sh
xmake f -P tests/d3d11 -F xmake.lua -m release
xmake build -P tests/d3d11 -F xmake.lua ImGuiVRHelperD3DTests
xmake run -P tests/d3d11 -F xmake.lua ImGuiVRHelperD3DTests
```

Both `-P` and `-F` select the nested standalone project explicitly. Without
the file argument xmake can discover the parent plugin project. The same
`ImGuiVRHelperD3DTests` target is also available from the root project after
its dependencies are configured; it is excluded from the headless test glob.
Neither test target has the plugin's deployment hook.

Install Windows Graphics Tools for debug-layer validation. The executable
reports whether that layer was available; a run without it cannot validate
the absence of D3D11 warnings. Supported runs fail on any debug-layer warning
or error. Shader fixtures compile through the Windows SDK's D3D compiler.

Coverage includes null and nondefault bindings, C++ exception unwinding,
class-linked pixel shaders, constant-buffer ranges, inherited predication
and geometry/tessellation shaders, all-stage SRV aliases, OM/CS UAV bindings
and append counters. Scissor rectangles and unrelated sampler slots serve
as sentinels for state that the guard must leave untouched.

This fixture checks binding correctness. It does not validate subtitle
occlusion, actual hosted drawing, old client binaries, headset output or VR
performance.
