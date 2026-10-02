# D3D11 hosted-rendering regression fixtures

These independent Windows executables validate the state guard and actual
hosted shader source used by the explicitly activated CSX path. See the
[ownership and compatibility contract](../../docs/renderer-host-design.md).

They need xmake, Visual Studio with the C++ workload, and the Windows SDK.
WARP supplies a software D3D11 device; no game, headset, CommonLib or client
mods are needed. Run from this `tests/d3d11` directory:

```sh
xmake f -P . -F xmake.lua -m release
xmake build -P . -F xmake.lua ImGuiVRHelperD3DTests
xmake run -P . -F xmake.lua ImGuiVRHelperD3DTests
xmake build -P . -F xmake.lua ImGuiVRHelperHostedD3DTests
xmake run -P . -F xmake.lua ImGuiVRHelperHostedD3DTests
```

The explicit project and file arguments select this standalone project.
Omitting the file argument can discover the parent plugin project and alter
its local configuration. Both targets are also available from the root
project after its dependencies are configured and are excluded from the
headless test glob. Neither target has the plugin's deployment hook.

Install Windows Graphics Tools for debug-layer validation. Each executable
reports when that layer is unavailable; that run cannot validate the
absence of D3D11 warnings. Debug-enabled runs fail on warnings or errors.
Shader fixtures compile through the Windows SDK's D3D compiler.

The state fixture covers null and nondefault bindings, C++ exception
unwinding, class-linked pixel shaders, constant-buffer ranges, inherited
predication and geometry/tessellation shaders, all-stage SRV aliases,
OM/CS UAV bindings and append counters. Scissor rectangles and unrelated
sampler slots are sentinels for state the guard must leave untouched. It
runs at feature levels 11.1 and 11.0 where available.

The hosted shader fixture compiles `HostedOverlayShaders.h` directly and
uses its production constants. A synthetic glyph is rendered into separate
eye targets against shared atlas depth. Off-ray nearer geometry must leave
the glyph intact, while a true nearer occluder removes it. A deliberately
wrong global coordinate mapping must damage that glyph, providing a
positive control for the regression. Further checks cover native forward-Z
and positive linear depth, cropped rectangles, independently flipped color,
invalid projected depth, explicit Gamma/Linear transfer and the development
depth-comparison bypass. Tests read back the resulting color pixels.

These fixtures test state binding and shader behavior in controlled D3D11
inputs. They do not run the complete helper snapshot/controller path, prove
scene provenance, load legacy client binaries, reproduce a Skyrim subtitle
scene or measure headset output and VR performance.

Neither these fixtures nor an external-client hosted smoke test requires
opening, configuring or exercising the helper's own UI. Its optional UI
remains available. Use the actual external client, such as FloatingSubtitles,
for a game check; the helper's HUD demo does not validate that client's
callbacks or content. These fixtures do not validate automatic client
operation or activation/deactivation in Skyrim.
