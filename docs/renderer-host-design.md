# Optional CSX renderer hosting

Status: implemented opt-in interface `006`, 1 October 2026. The helper owns
client content and composition; CSX owns scene provenance, writable final
images and compositor submission. The new path is active only after a CSX
host explicitly registers and activates a valid token. Installing the
helper, loading CSX or obtaining the interface does not activate it.

Interfaces `001` through `005` retain their declarations and virtual slots.
Existing clients continue to register panels, HUDs and world quads through
those interfaces. The legacy standalone renderer and its shaders remain in
place. The helper's Submit hook skips its automatic scene drawing only while
the negotiated host is active. Ordinary dispatch, input and focus handling
continue; host ownership does not set the render-failure latch.

This implementation addresses two source-level integration limitations:
automatic drawing can precede another renderer's final image replacement,
and scaling output coordinates cannot describe all separate-eye and atlas
depth layouts. These observations do not establish the cause of a specific
FloatingSubtitles defect. In-game reproduction remains a separate gate.

## Negotiation and ABI

Use `GetImGuiVRHelperInterface006()`. It returns null when the helper is
absent or predates revision `006`; do not gate on a build-number guess. The
new interface extends the existing single-inheritance chain and appends
methods after `005`. No old method is reordered or given new parameters.

[`ImGuiVRHelperRenderHost.h`](../api/ImGuiVRHelperRenderHost.h) defines the
Windows x64 contract. It uses fixed-width integers, explicit enums, opaque
nonzero tokens and POD descriptors. Compile-time checks fix sizes and
important offsets. Every input and output structure must start with its
default `structSize` and `contractVersion`; this revision accepts an exact
size/version match. Reserved fields and unknown flag bits are rejected.
Valid output structures are cleared before work, including failure returns.
C++ exceptions are translated into result codes at the DLL boundary.

Capabilities and registration are available before graphics initialization.
Only `RenderHostKind::CSX` is supported, with one registered owner at a time.
Registration reserves an inactive token. Unregister requires inactive
ownership and no open frame, and invalidates that token. Registration and
unregistration may occur off the render thread.

The first existing Present callback establishes the render thread. Content
queries, activation, deactivation and all frame operations must run on that
thread. Calls made before it is observed return `NotReady`; another thread
gets `WrongThread`. Activation also requires initialized graphics and the
helper's rendering services. Activation changes only between pairs. A
failed activation preserves the previous ownership policy. There is no
automatic opt-in based on another module's presence.

## Pair lifecycle and failure policy

The host follows this sequence:

1. Query candidate content between pairs. This works for a registered
   inactive host and performs no graphics work or client callbacks.
2. Activate at a render-thread pair boundary when its adapter can own the
   final output route. Keep activation separate from whether one pair has
   admissible world depth.
3. Supply both eyes and complete scene metadata to `BeginHostedFrame`.
   `Success` returns one cookie; `NoContent` and failures leave no open
   frame. An accepted preparation consumes its pair token even if freezing
   later fails.
4. Draw each eye with `RenderHostedEye`, using the returned cookie, matching
   pair and resource generation, a nonzero attempt ID and a compatible RTV.
5. Call `EndHostedFrame` after both eyes finish, or `AbortHostedFrame` to
   release an unfinished pair. `IncompletePair` from End still closes it.

Pair tokens must increase throughout a registration, including across
activation toggles. Altering scene metadata does not make a consumed token
fresh. Scene frame, resource generation and compositor cycle must be
nonzero; invalid frame sentinels are rejected. These identifiers are host
assertions: the helper cannot independently establish that a resource's
pixels belong to the stated scene.

Nested frames, eye calls during preparation/rendering, and lifecycle
changes during an open frame return `Busy`. Each successful eye may be
drawn only once. At most four attempts are allowed per eye. Retries require
both a new attempt ID and a different underlying target resource.
Reusing another view of a written resource is not a fresh attempt. The
capability query reports this bound.

`HostedEyeResult::targetWritten` means the target is no longer a clean
fallback. A later upload/device failure returns `PartialWrite` when a draw
may already have changed it. End and Abort release helper ownership; they
do not undo target pixels. The host must discard written failed targets.
Neither method submits to OpenVR. CSX must compose both scratch eye images
successfully before choosing either for its existing final Submit path, so
an eye failure cannot publish half of a decorated stereo pair.

## Frozen content and resource ownership

Begin freezes the registry's admitted client/quad lists, settings, focus,
panel eligibility, geometry, tinter choice and relevant anchors once for the
pair. Both eyes use those values. HMD/world transforms come from the host's
scene descriptor; controller anchors use the helper's cached ordinary input
snapshot. Composition does not invoke client callbacks, poll fresh tracking
poses, change input ownership or submit an image.

The registry snapshot retains COM references, then releases its lock before
graphics work. Begin copies each selected panel texture into a helper-owned
GPU texture on the serialized immediate context. Both eyes sample the copied
pixels, not the client's mutable original. Snapshots are bounded to 256 MiB
and 16,384 admitted world quads. Client panel sources must be compatible
single-sample, single-slice RGBA8 textures. Snapshot failure rejects the
pair; no target eye has been written at that point.

The host must order Begin after client GPU writes. Copies preserve legacy
writable panel handles but cannot repair unsynchronized immediate-context
access or deferred command lists executed after the snapshot. Legacy client
unregistration remains registry removal, not a callback-drain barrier.

The immediate context and admitted depth SRVs are retained internally until
closure. The API still requires the host to keep their contents stable from
Begin through End/Abort. Retaining COM objects is not a depth-pixel snapshot.
Eye RTVs are borrowed only during their synchronous eye call. All resources
must belong to the helper's initialized D3D11 device.

Content counts describe candidates, not nontransparent pixels or completed
paint. Legacy APIs do not expose a paint-completion fence. Persistent world
lists remain active until replaced or cleared by their client. Candidate
queries can exceed the admitted snapshot limit; Begin is authoritative.
Empty admitted content returns before constructing a graphics state guard.

## Coordinates, depth and color

Matrices are row-major doubles with row-vector multiplication. The host
provides tracking-to-color-clip and tracking-to-depth-clip matrices per eye,
using D3D clip depth `[0,1]`. World positions first subtract the captured
`worldOrigin` before the captured `worldToTracking` transform; tracking units
are metres. `headToTracking` uses the same tracking space. Float narrowing,
nonfinite values and degenerate transforms are checked before shader use.

Color viewports have positive dimensions inside the declared target extent.
Flip flags orient final color independently from depth. Depth has its own
positive, half-open active rectangle in its SRV. The pixel shader projects
each tracking-space billboard point into the depth camera and floors that
point's normalized coordinates into the supplied rectangle. It does not
map global `SV_Position` into a combined depth image. Separate color eyes
may share an atlas depth SRV only with nonoverlapping eye rectangles.

`trackingToDepthMetres` gives positive axial view distance, not Euclidean
range. Native forward-Z samples decode as `scale / (offset - sample)`;
positive linear samples decode as `scale * sample`, with zero offset. World
occlusion uses a 0.02 metre bias. Invalid/out-of-rectangle projected samples
skip comparison instead of loading a fabricated zero-depth occluder.

With `worldLayerEnabled=1`, Begin validates both complete depth descriptors.
An invalid descriptor rejects the pair. A host that cannot prove matching
world depth can submit a UI-only frame with that flag zero, preserving
admissible panel/HUD/modal content. The development-only
`DisableWorldDepthTest` flag bypasses comparisons but still requires valid
world metadata; it is not a persistent occlusion workaround.

The first contract supports feature level 11.0+, immediate contexts,
single-sample 2D resources, mip-zero views and single-slice textures. Depth
views support `R32_FLOAT`, `R24_UNORM_X8_TYPELESS`, `R16_UNORM`, and
`R32_FLOAT_X8X24_TYPELESS`. Color RTVs support non-sRGB `R8G8B8A8_UNORM`,
`B8G8R8A8_UNORM`, `R16G16B16A16_FLOAT`, `R10G10B10A2_UNORM` and
`R11G11B10_FLOAT`. Extents, view dimensions, device identity and color/depth
aliases are checked; snapshot textures cannot be targets. Arrays, MSAA and
partial foveated depth coverage require a different admitted representation.

Panel pixels are gamma encoded. `Gamma` preserves their RGB values;
`Linear` converts RGB before blending into a linear target. The target view
must not itself apply an sRGB encode. This does not infer the underlying
scene's transfer function or perform HDR tonemapping; the host must declare
the actual target convention.

## D3D11 state isolation

Hosted preparation and drawing use the private scoped state guard. It
restores null/default bindings, class-linked shaders, D3D11.1 constant-buffer
ranges and bindings removed by runtime alias handling. Inherited geometry,
tessellation and predication are disabled within the scope. Restoration
runs on ordinary returns and C++ exception unwinding. Constant-buffer Map
failure prevents the dependent draw.

The guard preserves UAV counters while restoring bindings. It does not
snapshot resource contents, recover a removed device or make concurrent
context use safe. Stream-output state is outside this texture-only pass's
scope; the geometry shader is disabled. Legacy rendering does not construct
this guard or allocate hosted snapshots while no host is active. State
capture and texture copies have a cost; no VR performance claim is made.

## Validation and remaining runtime gates

The implementation starts from helper source
`91f63900d0e6896617dd326ce1feeb914308bc42`. The separate CSX adapter starts
from `main-VR` source `dab1874a76fd39175dcefdc52110ba69d7284e12`. Source
identities alone are not identities of installed DLLs.

Executed locally with xmake `3.0.9+HEAD.2b184e178`, Visual Studio 2026 and the
Windows SDK, with both plugin deployment environment variables cleared:

-   `xmake build -y ImGuiVRHelper`: compiled and linked the hosted helper.
    Log: `build/validation/hosted-helper-build.txt`.
-   `xmake build -y ImGuiVRHelperTests` and `xmake run ImGuiVRHelperTests`:
    138 assertions in 16 headless lifecycle, ABI-layout and existing math
    cases passed. Log: `build/validation/hosted-headless-run.txt`.
-   The `001` through `005` API declaration prefix matches the baseline
    exactly, excluding only the new header include and appended `006`.
-   From `tests/d3d11`, `xmake build -P . -F xmake.lua ImGuiVRHelperHostedD3DTests`
    and `xmake run -P . -F xmake.lua ImGuiVRHelperHostedD3DTests`: passed using
    the actual production shader source and constants. Both eyes with separate
    color targets/shared atlas depth passed off-ray and true-occluder checks
    for native and linear depth. A deliberately wrong global mapping erased
    the synthetic glyph, confirming the fixture detects that failure class.
    Cropped depth, flipped output, invalid projection, diagnostic bypass and
    explicit Gamma/Linear transfer passed. The debug layer emitted no warnings
    or errors. Logs: `build/validation/hosted-shader-{build,run}.txt`.
-   The independent state-guard WARP fixture previously passed all 14
    scenarios, seven each at feature levels 11.1 and 11.0, with the debug layer
    enabled and no warnings/errors. High UAV slots were exercised on 11.1 and
    inapplicable on 11.0. See the [fixture instructions](../tests/d3d11/README.md).
-   Scoped whitespace, line-ending, large-file, EOF, clang-format and
    Prettier checks passed. The StyLua hook installer hit a release-download
    connection reset; the existing StyLua 2.5.2 binary independently passed
    `--check --config-path stylua.toml tests/d3d11/target.lua`.

The local xmake dependency resolution includes ImGui 1.92.7, Catch2 3.15.2
and OpenVR 2.15.6. These builds do not prove the original lockfile's exact
dependency versions. No dependency update is intended by this change.

These checks do not validate a loaded old `005` binary, both hook-install
orders, activation/deactivation in Skyrim, Open Shaders coexistence, client
callback concurrency, device-loss recovery, actual headset output or VR
frame cost. Runtime qualification must reproduce the reported subtitle
scene and a true occluder on native and vendor routes, inspect both eyes,
exercise ordinary standalone/input behavior, and record exact installed
builds and capture provenance. The CSX adapter must separately prove scene
provenance and retained-output exclusion, preserve complete OpenVR payloads,
and meet its affected render-scale qualification requirements. No game
deployment or measured interoperability fix is implied by the local tests.
