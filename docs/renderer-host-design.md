# Optional renderer hosting

Status: CSX integration design and tested private foundation, 1 October 2026.
This branch does not expose a renderer-host interface or claim to fix the
reported FloatingSubtitles occlusion. Interfaces `001` through `005`, the
client handshake, input ownership and autonomous submission remain intact.

Compatibility requirement: the implementation must not change behavior for
other helper users. New code is confined to an explicitly negotiated CSX
path. Merely installing this helper, loading CSX, or discovering an interface
does not opt in. Other renderers and all legacy clients keep the existing
path; there is no global replacement of their state handling or shaders.
The legacy `RenderForEye`, `RenderCursorIntoPanel` and interfaces `001`
through `005` retain their behavior. Future hosted entry points require a
valid, active CSX host token. Failed negotiation or frame admission must not
silently change the global rendering policy.

## Motivation and ownership

A renderer can replace or reconstruct an eye image after the helper's
automatic Submit hook has drawn onto it. The helper's world-depth lookup
also assumes that scaling output coordinates describes the depth layout.
That assumption cannot represent every separate-eye/atlas combination or
different input and output projection. Both are source-level limitations;
neither establishes the cause of a particular missing glyph.

The proposed optional interface lets CSX select a writable
final-eye target and supply matching depth and camera metadata. The host
owns scene rendering, resource lifetime and actual compositor submission.
The helper owns clients, textures, geometry, input and composition. Client
mods such as FloatingSubtitles keep their existing interfaces. Standalone
operation and Open Shaders' existing client integration remain supported.

The host calls composition synchronously. Composition does not invoke client
callbacks, update input, query fresh tracking poses or submit to OpenVR.
An absent or older helper retains its existing behavior. Registration alone
must not suppress autonomous drawing.

## First change: test private state isolation

Before exposing a new DLL boundary, a private scoped D3D11 state guard is
implemented and exercised by the standalone fixture. The current renderer
does not call it. It is reserved for the future CSX composition entry point,
so this foundation changes no game rendering behavior. The guard restores
bindings including null/default state and bindings the runtime would
otherwise remove because of resource aliasing. It disables inherited
geometry/tessellation shaders and predication during its scope, then restores
state on normal return and C++ exception unwinding.

This is texture composition on a serialized immediate FL11.0+ context. It does not
make concurrent context use safe, snapshot texture pixels, restore resource
contents or protect against device removal. Stream-output state is outside
the scope: the helper binds texture resources and disables the geometry
shader. UAV counters are preserved when restoring bindings. Constant-buffer
ranges are preserved where D3D11.1 offsetting is supported.

The future CSX path must reject failed constant-buffer uploads before any
dependent draw. The current world-quad depth upload can reuse stale data on
failure; changing that legacy behavior is deliberately outside this branch.

The [Windows WARP fixture](../tests/d3d11/README.md) exercises the guard independently of Skyrim.
The snapshot has a CPU cost, including retained COM references. No runtime
performance claim is made; profile it with empty content, world text and an
open panel before enabling it in the CSX path. Empty hosted composition must
return before creating the guard. Non-CSX operation must not construct the
guard, take pair snapshots or allocate hosted resources.

## Refined host contract

### Freeze data and pixels for a stereo pair

Existing clients can render outside the helper's callback and can use
deferred contexts. A retained texture reference or a paused `DispatchFrame`
does not freeze its contents. The implementation must either establish
serialized GPU production for the full pair or copy admitted textures into
bounded helper-owned pair snapshots before the first eye. Copies preserve
legacy clients' writable panel handles and need a defined ordering after
their writes; they do not repair unsynchronized immediate-context access.

Freeze the client/quad lists together under the registry lock, then release
that lock before graphics work. Also freeze settings, focus and flags,
world/room conversion, HMD/controller anchors, runtime-overlay eligibility,
selected tinter textures and the rebind modal. Both eyes consume those same
values. Do not equate helper Present ticks with the host's stereo-pair ID.

Content reporting means admitted draw candidates. Legacy APIs do not expose
a completed-paint fence or prove that a texture contains nontransparent
pixels. Preserve the API's persistent world-quad lists until replaced or
cleared by the client.

### Admit both eyes before drawing either

The initial `BeginHostedFrame` design needs either both immutable depth/view
descriptors or an explicit pair-wide admission decision backed by the host's
preflight. Per-eye validation alone cannot retract an already presented
first eye. Keep final writable RTVs borrowed for individual eye calls.

Use distinct scene identity, stereo-pair identity, resource generation,
helper frame cookie and fresh-surface attempt identity. Define missing-eye
closure, duplicate attempts and partial writes explicitly. An engine-frame
number or retained COM pointer alone does not prove that color and depth
contain matching scene data.

### Explicit projection and encoding

Project each tracking-space billboard point into both the final-color and
depth cameras. Depth lookup uses the supplied positive active eye rectangle,
not `SV_Position * globalScale`. Matrices use a documented row-vector
convention and tests with asymmetric views, nonzero origins and both eyes.
Output orientation and depth orientation are separate.

Decode depth with explicit checked rational coefficients into positive
axial view distance in metres. Capture world scale and origin with the
scene; do not read newer near/far, room or dynamic-resolution values during
each eye draw. Invalid projected samples must not become zero-valued loads
that fabricate near occluders. Unsupported or stale depth rejects world
content for the pair under the production policy; valid panel/HUD content
may still be admitted independently.

Start with single-sample D3D11 2D RTV/SRV views, positive rectangles,
validated mip dimensions and nonaliasing color/depth resources. Arrays,
MSAA, stale/reprojected scenes and incomplete foveated depth regions need
separate qualification, not silent interpretation.

### Add the public ABI only when it works

`005` is the highest revision at the inspected helper baseline. Reserve no
new public vtable until the complete hosted renderer and ownership
controller work together. The next revision should append to the existing
inheritance chain without changing old layouts or virtual slots.

Specify fixed-width fields, struct size/version checks, reserved bits,
initialized outputs, bounded strings, opaque nonzero tokens, borrowing and
exception translation. Activation belongs to the established render thread
at pair boundaries. Reject nested frames and lifecycle transitions during
an open frame. Keep negotiated ownership separate from the persistent
render-failure latch so ordinary dispatch and input continue.

One remaining client-lifecycle concern is that registry removal is not a
callback-drain barrier: dispatch snapshots raw callback/user pointers. The
host implementation must not rely on stronger unregister guarantees than
the legacy API provides.

## CSX producer audit and integration boundary

The helper fork and upstream `main` were checked at
`91f63900d0e6896617dd326ce1feeb914308bc42`.
The handover's CSX source was `dab1874a76fd39175dcefdc52110ba69d7284e12`.
The locally available CSX checkout was instead branch `main-vr-nr`, commit
`042b7c05d6d9efb82b90de1bdef8929c5b0403b5`. These are source identities,
not identities of installed DLLs. Recheck the intended CSX integration base
before implementing its adapter.

Read-only inspection of that local source adds these constraints:

-   `EncodeTexturesCS.hlsl` copies native device depth unchanged into both
    vendor intermediates. `vrIntermediateLinearDepth` is not evidence of
    linear metres; its name must not select the decoding policy.
-   Foveated encoding can write only an ROI. Resource dimensions alone do not
    establish current full-eye depth coverage. The completed opaque depth
    selector is a candidate provider, subject to content-lifetime proof.
-   Physical render-scale mode forces engine dynamic ratios to one; classic
    reduced-resolution mode uses different ratio/lock semantics. Capture
    explicit rectangles rather than multiplying live ratios again.
-   Retained neural outputs exist on this branch. A successful vendor-return
    value or current observation frame does not automatically admit current
    world geometry over those images.
-   The central retained Submit packet preserves lifetime, complete OpenVR
    payloads and presentation acknowledgements. Integrate before that existing
    boundary with typed candidate eligibility. Do not decorate protected,
    loading, quarantined or retained continuity outputs by default.
-   Current presentation copies are SRV/UAV-only and gated on CSX-owned
    content. Hosted raster work needs compatible RTV capability and independent
    helper-content gating, including native rendering with the CSX menu closed.

## Remaining implementation and evidence

1. Implement and test the private ownership controller, complete pair
   snapshots and explicit projection/depth renderer. Include synthetic
   off-ray and true-occluder cases, both eyes, state/error paths and a bounded
   development depth-bypass control.
2. Expose the next additive interface with ABI checks and an independently
   built old `005` client fixture. Test both hook-installation orders,
   activation, deactivation, failure, standalone dispatch and input.
3. Add the separately reviewed CSX adapter using a pinned API-only
   dependency, authoritative producer packet and final-eye composition on
   safe writable surfaces. Preserve existing presentation policy and
   complete OpenVR metadata.
4. Reproduce the reported subtitle scene and compare native/vendor paths,
   Open Shaders, true occlusion and both eyes. Record exact builds, settings,
   capture provenance, lifecycle stress and measured CPU/GPU cost. Follow
   CSX's qualification requirements for its affected render-scale code.

No game deployment, subtitle reproduction, old-client binary test, CSX
adapter test or performance measurement is implied by this foundation.
The separate host implementation must complete those applicable gates before
being described as an interoperability fix.

## Foundation validation

Executed on Windows with xmake `3.0.9+HEAD.2b184e178`, Visual Studio 2026,
and the Windows SDK. Source base is the helper commit above plus this
foundation. Raw build/test output remains local under `build/validation/`.

-   `xmake f -P tests/d3d11 -F xmake.lua -m release`: passed using the
    standalone project, without resolving plugin dependencies.
-   `xmake build -P tests/d3d11 -F xmake.lua ImGuiVRHelperD3DTests`: passed.
-   `xmake run -P tests/d3d11 -F xmake.lua ImGuiVRHelperD3DTests`: all 14
    scenarios passed, seven each at feature levels 11.1 and 11.0. The debug
    layer was enabled for both and reported no warnings or errors. High UAV
    slots were exercised on 11.1 and correctly inapplicable on 11.0.
-   `xmake build -y ImGuiVRHelper`: passed in `release` mode with SE, AE and
    VR support enabled. Both deployment environment variables were cleared;
    the build reported no deployment target.
-   `xmake build -y ImGuiVRHelperTests` and `xmake run ImGuiVRHelperTests`:
    passed, 38 assertions in eight existing headless cases.
-   Repository pre-commit hooks: passed on the changed files only.
-   Diff audit: existing renderer, hooks, public API, helper implementation
    and dependency lockfile remain unchanged. The new guard has no production
    call sites.

The local VS2026 configuration caused xmake to regenerate its dependency
resolution keys. The DLL/headless builds used that saved local resolution
(including ImGui 1.92.7, Catch2 3.15.2 and OpenVR 2.15.6); they are not proof
of a build against the original lockfile's exact versions. The original
lockfile was restored and no dependency update is included in this change.
The standalone D3D fixture depends only on the Windows SDK.

The final local DLL was 4,837,888 bytes, SHA-256
`3AC1B3197CA7F5BC588E55D9EDB8683415E1ABC3AB7E865995F2013826D52F39`.
It was not installed or loaded into Skyrim. Game/runtime compatibility and
performance remain untested; this patch introduces no runtime integration.
