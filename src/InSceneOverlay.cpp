// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Lifted from skyrim-community-shaders/src/Features/VR/InSceneOverlay.cpp
// with relicensing under GPL-3.0-or-later WITH the modding exception.
//
// Adapted: shaders are embedded as inline string literals (compiled with
// D3DCompile at init) so the helper has no on-disk shader file dependency.
// SCS-specific bits dropped: ApplyHighlightTintToTexture, IsWelcomeOverlayVisible,
// SetResourceName. menu texture source is the focused client's panel.

#include "pch.h"

#include "InSceneOverlay.h"

#include "Globals.h"
#include "HelperImpl.h"
#include "Overlay.h"
#include "OverlayTinter.h"
#include "RuntimeOverlay.h"
#include "RenderHost.h"
#include "VrikCompat.h"
#include "internal/Detour.h"
#include "internal/HUDGeometry.h"
#include "internal/Profiler.h"
#include "internal/VRUtils.h"

#include <RE/B/BSOpenVR.h>
#include <RE/B/BSShaderRenderTargets.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/R/Renderer.h>

#include <DirectXMath.h>
#include <SimpleMath.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <exception>
#include <optional>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

namespace ImGuiVRHelper::InSceneOverlay
{
	using DirectX::XMFLOAT2;
	using DirectX::XMFLOAT3;
	using DirectX::SimpleMath::Matrix;
	using DirectX::SimpleMath::Vector3;

	namespace
	{
		// ---- Inline shader sources --------------------------------------
		// Pulled in source-form from
		// skyrim-community-shaders/package/Shaders/VR/InSceneOverlay.{vs,ps}.hlsl
		// so the helper has no on-disk shader file dependency.

		// Tint blending lives in OverlayTinter's compute shader pass (run
		// once per frame on the focused client's panel before composite).
		// Composite shaders here are pure pass-through.

		constexpr const char* kVertexShader = R"(
cbuffer MatrixBuffer : register(b0)
{
	matrix wvp;
	float2 uvScale;   // map unit-quad uv into a panel sub-rect (1,1 = whole panel)
	float2 uvOffset;  // sub-rect origin (0,0 = whole panel)
};
struct VS_INPUT
{
	float3 pos: POSITION;
	float2 uv: TEXCOORD0;
};
struct PS_INPUT
{
	float4 pos: SV_POSITION;
	float2 uv: TEXCOORD0;
};
PS_INPUT main(VS_INPUT input)
{
	PS_INPUT output;
	output.pos = mul(float4(input.pos, 1.0f), wvp);
	output.uv = input.uv * uvScale + uvOffset;
	return output;
}
)";

		// Instanced variant of the vertex shader. The wvp matrix (4 rows) and uv-transform arrive
		// as per-instance vertex data (slot 1) instead of the constant buffer, so a whole client's
		// billboards draw in one DrawIndexedInstanced. Same packing as the single-quad path: the
		// rows are (model*vpWorldSpace).Transpose(), transposed back here so mul(pos, wvp) matches.
		constexpr const char* kInstancedVertexShader = R"(
struct VS_INPUT
{
	float3 pos: POSITION;
	float2 uv: TEXCOORD0;
	float4 wvp0: TEXCOORD1;
	float4 wvp1: TEXCOORD2;
	float4 wvp2: TEXCOORD3;
	float4 wvp3: TEXCOORD4;
	float4 uvxform: TEXCOORD5;  // uvScale.xy, uvOffset.xy
};
struct PS_INPUT
{
	float4 pos: SV_POSITION;
	float2 uv: TEXCOORD0;
};
PS_INPUT main(VS_INPUT input)
{
	PS_INPUT output;
	matrix wvp = transpose(matrix(input.wvp0, input.wvp1, input.wvp2, input.wvp3));
	output.pos = mul(float4(input.pos, 1.0f), wvp);
	output.uv = input.uv * input.uvxform.xy + input.uvxform.zw;
	return output;
}
)";

		constexpr const char* kPixelShader = R"(
Texture2D shaderTexture : register(t0);
SamplerState sampleType : register(s0);
struct PS_INPUT
{
	float4 pos: SV_POSITION;
	float2 uv: TEXCOORD0;
};
float4 main(PS_INPUT input) : SV_TARGET
{
	return shaderTexture.Sample(sampleType, input.uv);
}
)";

		// World-quad pixel shader with scene-depth occlusion. Samples the game's kMAIN depth at
		// the billboard pixel (scaled by depthScale — see the SceneDepthInfo comment, since an
		// upscaling pipeline can leave depth at a lower resolution than the submit target) and
		// discards the pixel when the billboard sits behind the scene. Both depths are linearized
		// to view-space metres — the scene from its standard-Z game projection (cameraData), the
		// billboard from the helper's OpenVR projection (helperNear/Far) — so the compare is
		// unit-consistent.
		constexpr const char* kWorldQuadDepthPixelShader = R"(
Texture2D shaderTexture : register(t0);
Texture2D<float> sceneDepth : register(t1);
SamplerState sampleType : register(s0);
cbuffer DepthCB : register(b0)
{
	float4 cameraData;      // x=far y=near z=far-near w=far*near  (game units)
	float  gameToMeter;     // game units -> metres
	float  helperNear;      // helper projection near (metres)
	float  helperFar;       // helper projection far (metres)
	float  depthBias;       // metres of slack to avoid z-fighting on contact
	float  depthTestEnable; // 1 = occlude against the scene, 0 = passthrough
	float2 depthScale;      // sceneDepth texel size / submit texel size (upscaling can leave the
	                        // depth buffer at a lower internal resolution than the submit target)
	float  _pad;
};
struct PS_INPUT
{
	float4 pos: SV_POSITION;
	float2 uv: TEXCOORD0;
};
float4 main(PS_INPUT input) : SV_TARGET
{
	float4 col = shaderTexture.Sample(sampleType, input.uv);
	if (depthTestEnable > 0.5f) {
		float sd = sceneDepth.Load(int3(int2(input.pos.xy * depthScale), 0));
		float sceneMetres = (cameraData.w / (cameraData.x - sd * cameraData.z)) * gameToMeter;
		float billMetres = (helperNear * helperFar) / (helperFar - input.pos.z * (helperFar - helperNear));
		if (billMetres > sceneMetres + depthBias)
			discard;
	}
	return col;
}
)";

		struct ConstantBufferData
		{
			Matrix wvp;
			// UV sub-rect transform (panel uv = quad uv * scale + offset). Defaults
			// map the whole panel, so existing full-panel callers need not set them.
			float uvScale[2] = { 1.0f, 1.0f };
			float uvOffset[2] = { 0.0f, 0.0f };
		};

		// Per-instance data for the instanced world-quad path; layout must match the
		// slot-1 input elements (4 wvp rows -> TEXCOORD1..4, uv transform -> TEXCOORD5).
		struct InstanceData
		{
			Matrix wvp;                          // (model * vpWorldSpace).Transpose()
			float uvScale[2] = { 1.0f, 1.0f };   // TEXCOORD5.xy
			float uvOffset[2] = { 0.0f, 0.0f };  // TEXCOORD5.zw
		};

		// Mirrors the DepthCB cbuffer in kWorldQuadDepthPixelShader (48 bytes, 16-byte aligned).
		struct WorldQuadDepthParams
		{
			float cameraData[4] = { 0.0f, 0.0f, 0.0f, 0.0f };  // far, near, far-near, far*near (game units)
			float gameToMeter = 0.01428f;                      // Skyrim game unit -> metres
			float helperNear = 0.1f;                           // must match ComputeEyeMatrices nearZ
			float helperFar = 1000.0f;                         // must match ComputeEyeMatrices farZ
			float depthBias = 0.02f;                           // metres of contact slack
			float depthTestEnable = 0.0f;
			// sceneDepth texel size / submit texel size — see the SceneDepthInfo comment below.
			float depthScale[2] = { 1.0f, 1.0f };
			float pad = 0.0f;
		};

		struct Resources
		{
			winrt::com_ptr<ID3D11VertexShader> vs;
			winrt::com_ptr<ID3D11PixelShader> ps;
			winrt::com_ptr<ID3D11InputLayout> inputLayout;
			winrt::com_ptr<ID3D11Buffer> vb;
			winrt::com_ptr<ID3D11Buffer> ib;
			winrt::com_ptr<ID3D11Buffer> cb;
			winrt::com_ptr<ID3D11BlendState> blendState;
			// RenderCursorIntoPanel composites onto OverlayTinter's panel canvas, where the
			// destination's existing alpha must be preserved (see its own blend-state comment);
			// blendState above draws onto the live eye buffer, whose alpha is inert, so it keeps
			// the simpler overwrite semantics rather than taking on panel-canvas requirements.
			winrt::com_ptr<ID3D11BlendState> panelCompositeBlendState;
			winrt::com_ptr<ID3D11DepthStencilState> depthState;
			winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
			winrt::com_ptr<ID3D11SamplerState> sampler;

			// Instanced world-quad path: a client's billboards draw in one
			// DrawIndexedInstanced. Per-instance data (wvp rows + uv transform)
			// lives in instanceBuffer (slot 1), grown on demand.
			winrt::com_ptr<ID3D11VertexShader> instancedVS;
			winrt::com_ptr<ID3D11InputLayout> instancedInputLayout;
			winrt::com_ptr<ID3D11Buffer> instanceBuffer;
			uint32_t instanceCapacity = 0;

			// Scene-depth occlusion for the world-quad pass. Separate PS so the HUD/panel passes
			// keep the plain sampler; depthCB feeds it the linearization params.
			winrt::com_ptr<ID3D11PixelShader> worldQuadDepthPS;
			winrt::com_ptr<ID3D11Buffer> depthCB;

			// GPU debug markers (RenderDoc/PIX event browser) for the passes below. Cached once
			// via QueryInterface; null (and every marker a no-op) on a device without D3D11.1.
			// Both this and tracyCtx below only exist in a --tracy=y build (see GPUMarker /
			// HELPER_GPU_PASS) — zero cost in the shipped mod.
#if defined(IMGUI_VR_HELPER_TRACY)
			winrt::com_ptr<ID3DUserDefinedAnnotation> annotation;
			// GPU-side timestamp queries per pass, visible in Tracy's GPU timeline — separate from
			// the CPU-side ZoneScopedN dispatch-cost zones already at each call site.
			TracyD3D11Ctx tracyCtx = nullptr;
#endif

			// Cached per-eye RTVs keyed by target texture pointer (rebuilt
			// when SteamVR rotates eye textures).
			struct CachedRTV
			{
				winrt::com_ptr<ID3D11RenderTargetView> rtv;
				ID3D11Texture2D* texture = nullptr;
				// Last texture pointer for which RTV creation failed, so we
				// log once per distinct texture instead of every frame.
				ID3D11Texture2D* lastFailedTexture = nullptr;
			};
			CachedRTV cachedEyeRTVs[2];

			// Cached SRV for the menu texture — rebuilt when the focused
			// client changes (different panel texture).
			winrt::com_ptr<ID3D11ShaderResourceView> menuSRV;
			ID3D11Texture2D* cachedMenuTexture = nullptr;

			// Procedurally-generated marker for the wand-panel intersection point (a filled
			// circle with a dark outline), composited over whichever panel is currently shown so
			// the laser's aim is legible — the default ImGui software cursor a client (or the
			// settings UI) might additionally draw into its own panel pixels is unreadable at
			// panel scale/distance. Built once at InitResources; static content, no per-frame
			// update needed.
			winrt::com_ptr<ID3D11Texture2D> cursorTexture;
			winrt::com_ptr<ID3D11ShaderResourceView> cursorSRV;
			// Alternate pointer style (Settings::CursorStyle::Arrow); same 64px
			// quad, hotspot baked at the texture center so RenderCursorPass just
			// swaps the SRV with no positioning change.
			winrt::com_ptr<ID3D11Texture2D> cursorArrowTexture;
			winrt::com_ptr<ID3D11ShaderResourceView> cursorArrowSRV;
			// Tint last baked into cursorTexture/cursorArrowTexture; EnsureCursorColorCurrent
			// regenerates both only when Settings::cursorColor no longer matches this.
			float cursorColorApplied[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

			bool initialized = false;
		};

		Resources g_res;

#if defined(IMGUI_VR_HELPER_TRACY)
		// RAII scope fanning one pass name to both RenderDoc/PIX (ID3DUserDefinedAnnotation, so a
		// capture's event list reads "WorldQuadPass" / "HUDPass" / client names instead of requiring
		// a blind search by shader/resource signature) and Tracy's GPU timeline (real GPU-side pass
		// cost, not just the CPU dispatch cost the existing ZoneScopedN zones already cover). Gated
		// on the same build flag as those CPU zones, so the shipped mod pays zero cost — use via the
		// HELPER_GPU_PASS(name) macro so __LINE__/__FILE__ are captured at the call site.
		struct GPUMarker
		{
			ID3DUserDefinedAnnotation* anno;
			std::optional<tracy::D3D11ZoneScope> gpuZone;
			GPUMarker(const char* name, uint32_t line, const char* file, size_t fileSz) :
				anno(g_res.annotation.get())
			{
				const size_t nameLen = std::strlen(name);
				if (anno) {
					// Naive ASCII widen: pass names are compile-time literals or client-registration
					// names, expected ASCII; a non-ASCII name just mangles this debug-only label.
					const std::wstring wname(name, name + nameLen);
					anno->BeginEvent(wname.c_str());
				}
				if (g_res.tracyCtx) {
					gpuZone.emplace(g_res.tracyCtx, line, file, fileSz, "GPUMarker",
						sizeof("GPUMarker") - 1, name, nameLen, 0, true);
				}
			}
			~GPUMarker()
			{
				if (anno)
					anno->EndEvent();
			}
			GPUMarker(const GPUMarker&) = delete;
			GPUMarker& operator=(const GPUMarker&) = delete;
		};
#	define HELPER_GPU_PASS_CONCAT_IMPL(a, b) a##b
#	define HELPER_GPU_PASS_CONCAT(a, b) HELPER_GPU_PASS_CONCAT_IMPL(a, b)
#	define HELPER_GPU_PASS(name) \
		GPUMarker HELPER_GPU_PASS_CONCAT(_gpuMarker_, __LINE__)(name, __LINE__, __FILE__, sizeof(__FILE__) - 1)
#else
#	define HELPER_GPU_PASS(name)
#endif

		// ---- Cursor marker texture ---------------------------------------

		constexpr int kCursorSize = 64;

		// Upload a 64x64 RGBA bitmap as an immutable texture + SRV.
		bool UploadCursorBitmap(ID3D11Device* device, const std::vector<uint8_t>& pixels,
			winrt::com_ptr<ID3D11Texture2D>& texOut, winrt::com_ptr<ID3D11ShaderResourceView>& srvOut,
			const char* label)
		{
			D3D11_TEXTURE2D_DESC desc = {};
			desc.Width = kCursorSize;
			desc.Height = kCursorSize;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_IMMUTABLE;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

			D3D11_SUBRESOURCE_DATA initData = {};
			initData.pSysMem = pixels.data();
			initData.SysMemPitch = kCursorSize * 4;

			if (FAILED(device->CreateTexture2D(&desc, &initData, texOut.put()))) {
				logs::error("InSceneOverlay: {} texture creation failed", label);
				return false;
			}
			if (FAILED(device->CreateShaderResourceView(texOut.get(), nullptr, srvOut.put()))) {
				logs::error("InSceneOverlay: {} SRV creation failed", label);
				return false;
			}
			return true;
		}

		// Fill color crossfading to a dark outline, alpha-feathered at the outer edge so it
		// reads cleanly over any panel content. Matches the marker client mods (PhotoMode,
		// DialogueHistory) were independently drawing themselves via ImGui's foreground draw
		// list — built once here instead so every RendersOnFocus client (and the settings UI)
		// gets a legible wand-aim indicator for free. Hotspot is the texture center. tint is
		// user-configurable (Settings::cursorColor); the outline stays dark regardless, for
		// contrast against any panel content or fill color.
		bool CreateDotTexture(ID3D11Device* device, const float tint[4])
		{
			constexpr int kSize = kCursorSize;
			constexpr float kCenter = (kSize - 1) * 0.5f;
			constexpr float kOuterR = kSize * 0.42f;  // dark outline outer edge
			constexpr float kInnerR = kSize * 0.34f;  // fill / outline boundary
			constexpr float kFeather = 1.5f;          // px of AA falloff at each edge
			const float fillR = tint[0] * 255.0f, fillG = tint[1] * 255.0f, fillB = tint[2] * 255.0f;

			std::vector<uint8_t> pixels(static_cast<size_t>(kSize) * kSize * 4, 0);
			for (int y = 0; y < kSize; ++y) {
				for (int x = 0; x < kSize; ++x) {
					const float dx = (static_cast<float>(x) - kCenter);
					const float dy = (static_cast<float>(y) - kCenter);
					const float d = std::sqrt(dx * dx + dy * dy);

					uint8_t r = 0, g = 0, b = 0, a = 0;
					if (d <= kOuterR) {
						// Fill crossfading to the dark (0,0,0) outline across kFeather (AA on
						// the color boundary); alpha feathers ONLY at the outer edge.
						// Feathering both alphas down to zero at the fill/outline boundary
						// left a transparent seam ring showing panel content through the
						// marker — the opposite of the legibility goal.
						const float tEdge = std::clamp((d - (kInnerR - kFeather)) / kFeather, 0.0f, 1.0f);
						const float tOut = std::clamp((kOuterR - d) / kFeather, 0.0f, 1.0f);
						r = static_cast<uint8_t>(fillR * (1.0f - tEdge));
						g = static_cast<uint8_t>(fillG * (1.0f - tEdge));
						b = static_cast<uint8_t>(fillB * (1.0f - tEdge));
						a = static_cast<uint8_t>((255.0f - 20.0f * tEdge) * tOut * tint[3]);
					}
					const size_t i = (static_cast<size_t>(y) * kSize + x) * 4;
					pixels[i + 0] = r;
					pixels[i + 1] = g;
					pixels[i + 2] = b;
					pixels[i + 3] = a;
				}
			}
			return UploadCursorBitmap(device, pixels, g_res.cursorTexture, g_res.cursorSRV, "cursor dot");
		}

		// Classic desktop arrow: tinted fill, dark outline. Drawn as a filled polygon whose
		// TIP is at the texture center (kCursorSize/2, kCursorSize/2) so it shares the dot's
		// center hotspot — swapping the SRV in RenderCursorPass needs no repositioning. The
		// arrow body extends down-right from the tip (standard pointer orientation).
		bool CreateArrowTexture(ID3D11Device* device, const float tint[4])
		{
			constexpr int kSize = kCursorSize;
			constexpr float kHot = kSize * 0.5f;  // tip at center
			constexpr float kS = kSize * 0.34f;   // arrow length from tip

			// Arrow outline in tip-local units (tip at 0,0; +x right, +y down), the classic
			// Windows pointer: down the left edge, notch to the tail, out to the tail tip,
			// back up the right shaft. Multiplied by kS and offset to the hotspot below.
			const ImVec2 poly[7] = {
				{ 0.00f, 0.00f },  // tip
				{ 0.00f, 1.00f },  // straight down the left edge
				{ 0.28f, 0.73f },  // inner notch
				{ 0.46f, 1.10f },  // down the tail's left side
				{ 0.66f, 1.02f },  // tail tip
				{ 0.40f, 0.62f },  // up the tail's right side to the shaft
				{ 0.72f, 0.55f },  // out along the top-right shaft edge
			};
			ImVec2 pts[7];
			for (int i = 0; i < 7; ++i)
				pts[i] = ImVec2(kHot + poly[i].x * kS, kHot + poly[i].y * kS);

			auto pointInPoly = [&](float px, float py) {
				bool in = false;
				for (int i = 0, j = 6; i < 7; j = i++) {
					if (((pts[i].y > py) != (pts[j].y > py)) &&
						(px < (pts[j].x - pts[i].x) * (py - pts[i].y) / (pts[j].y - pts[i].y) + pts[i].x))
						in = !in;
				}
				return in;
			};
			// Distance from a point to the polygon boundary (for the dark outline + AA).
			auto edgeDist = [&](float px, float py) {
				float best = 1e9f;
				for (int i = 0, j = 6; i < 7; j = i++) {
					const float ax = pts[j].x, ay = pts[j].y, bx = pts[i].x, by = pts[i].y;
					const float vx = bx - ax, vy = by - ay;
					const float wx = px - ax, wy = py - ay;
					const float len2 = vx * vx + vy * vy;
					float t = len2 > 0.0f ? (wx * vx + wy * vy) / len2 : 0.0f;
					t = std::clamp(t, 0.0f, 1.0f);
					const float dx = px - (ax + t * vx), dy = py - (ay + t * vy);
					best = std::min(best, std::sqrt(dx * dx + dy * dy));
				}
				return best;
			};

			constexpr float kOutline = 2.0f;  // px dark border
			constexpr float kFeather = 1.0f;  // px AA
			const float fillR = tint[0] * 255.0f, fillG = tint[1] * 255.0f, fillB = tint[2] * 255.0f;
			std::vector<uint8_t> pixels(static_cast<size_t>(kSize) * kSize * 4, 0);
			for (int y = 0; y < kSize; ++y) {
				for (int x = 0; x < kSize; ++x) {
					const float px = static_cast<float>(x) + 0.5f;
					const float py = static_cast<float>(y) + 0.5f;
					const bool inside = pointInPoly(px, py);
					const float ed = edgeDist(px, py);

					uint8_t r = 0, g = 0, b = 0, a = 0;
					if (inside) {
						// Tinted interior; the outermost kOutline px darken to the border.
						const float tBorder = std::clamp((kOutline - ed) / kOutline, 0.0f, 1.0f);
						r = static_cast<uint8_t>(fillR * (1.0f - tBorder));
						g = static_cast<uint8_t>(fillG * (1.0f - tBorder));
						b = static_cast<uint8_t>(fillB * (1.0f - tBorder));
						a = static_cast<uint8_t>(255.0f * tint[3]);
					} else if (ed < kFeather) {
						// Just outside: dark, alpha-feathered edge for AA.
						a = static_cast<uint8_t>(255.0f * (1.0f - ed / kFeather) * tint[3]);
					}
					const size_t i = (static_cast<size_t>(y) * kSize + x) * 4;
					pixels[i + 0] = r;
					pixels[i + 1] = g;
					pixels[i + 2] = b;
					pixels[i + 3] = a;
				}
			}
			return UploadCursorBitmap(
				device, pixels, g_res.cursorArrowTexture, g_res.cursorArrowSRV, "cursor arrow");
		}

		bool CreateCursorTexture(ID3D11Device* device, const float tint[4])
		{
			return CreateDotTexture(device, tint) && CreateArrowTexture(device, tint);
		}

		// Regenerate the cursor textures when Settings::cursorColor changes (a cheap 4-float
		// compare every frame; the textures themselves are only rebuilt on an actual edit,
		// e.g. dragging the color picker in the settings menu).
		void EnsureCursorColorCurrent(ID3D11Device* device, const float tint[4])
		{
			if (g_res.cursorColorApplied[0] == tint[0] && g_res.cursorColorApplied[1] == tint[1] &&
				g_res.cursorColorApplied[2] == tint[2] && g_res.cursorColorApplied[3] == tint[3]) {
				return;
			}
			if (CreateCursorTexture(device, tint)) {
				for (int i = 0; i < 4; ++i)
					g_res.cursorColorApplied[i] = tint[i];
			}
		}

		// ---- Resource init ----------------------------------------------

		bool InitResources()
		{
			if (g_res.initialized)
				return true;
			if (!Globals::IsReady())
				return false;

			// Release anything a prior failed attempt left behind so the com_ptr .put() calls
			// below overwrite null and do not leak earlier references when we retry.
#if defined(IMGUI_VR_HELPER_TRACY)
			// TracyD3D11Ctx isn't RAII-owned by a com_ptr — destroy it explicitly before the
			// blanket reset below, or a retry after a partial failure leaks its GPU query pool.
			if (g_res.tracyCtx)
				TracyD3D11Destroy(g_res.tracyCtx);
#endif
			g_res = Resources{};

			auto& d3d = Globals::GetD3D();
			auto* device = d3d.device;

#if defined(IMGUI_VR_HELPER_TRACY)
			// GPU debug markers + Tracy GPU timeline (see GPUMarker). Fine if either fails —
			// annotation/tracyCtx stay null and markers become no-ops.
			if (d3d.context) {
				d3d.context->QueryInterface(IID_PPV_ARGS(g_res.annotation.put()));
				g_res.tracyCtx = TracyD3D11Context(device, d3d.context);
			}
#endif

			// Compile shaders from embedded strings.
			winrt::com_ptr<ID3DBlob> vsBlob, psBlob, errorBlob;
			HRESULT hr = D3DCompile(
				kVertexShader, std::strlen(kVertexShader), "InSceneOverlay.vs", nullptr, nullptr,
				"main", "vs_5_0",
				D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
				vsBlob.put(), errorBlob.put());
			if (FAILED(hr)) {
				logs::error("InSceneOverlay VS compile failed: {}",
					errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "<no message>");
				return false;
			}
			errorBlob = nullptr;
			hr = D3DCompile(
				kPixelShader, std::strlen(kPixelShader), "InSceneOverlay.ps", nullptr, nullptr,
				"main", "ps_5_0",
				D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
				psBlob.put(), errorBlob.put());
			if (FAILED(hr)) {
				logs::error("InSceneOverlay PS compile failed: {}",
					errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "<no message>");
				return false;
			}

			if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
					nullptr, g_res.vs.put()))) {
				logs::error("InSceneOverlay: CreateVertexShader failed");
				return false;
			}
			if (FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
					nullptr, g_res.ps.put()))) {
				logs::error("InSceneOverlay: CreatePixelShader failed");
				return false;
			}

			// Input layout.
			D3D11_INPUT_ELEMENT_DESC layout[2] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
					D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT,
					D3D11_INPUT_PER_VERTEX_DATA, 0 },
			};
			if (FAILED(device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(),
					vsBlob->GetBufferSize(), g_res.inputLayout.put()))) {
				logs::error("InSceneOverlay: CreateInputLayout failed");
				return false;
			}

			// Instanced world-quad pipeline: per-vertex POSITION/TEXCOORD0 from slot 0 (the unit
			// quad VB), per-instance wvp rows + uv transform from slot 1 (instanceBuffer).
			winrt::com_ptr<ID3DBlob> instVsBlob;
			errorBlob = nullptr;
			hr = D3DCompile(
				kInstancedVertexShader, std::strlen(kInstancedVertexShader), "InSceneOverlay.inst.vs",
				nullptr, nullptr, "main", "vs_5_0",
				D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
				instVsBlob.put(), errorBlob.put());
			if (FAILED(hr)) {
				logs::error("InSceneOverlay instanced VS compile failed: {}",
					errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "<no message>");
				return false;
			}
			if (FAILED(device->CreateVertexShader(instVsBlob->GetBufferPointer(),
					instVsBlob->GetBufferSize(), nullptr, g_res.instancedVS.put()))) {
				logs::error("InSceneOverlay: CreateVertexShader (instanced) failed");
				return false;
			}
			D3D11_INPUT_ELEMENT_DESC instLayout[7] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
				{ "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
				{ "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
				{ "TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
				{ "TEXCOORD", 5, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
			};
			if (FAILED(device->CreateInputLayout(instLayout, 7, instVsBlob->GetBufferPointer(),
					instVsBlob->GetBufferSize(), g_res.instancedInputLayout.put()))) {
				logs::error("InSceneOverlay: CreateInputLayout (instanced) failed");
				return false;
			}

			// World-quad depth-occlusion pixel shader + its params buffer.
			winrt::com_ptr<ID3DBlob> depthPsBlob;
			errorBlob = nullptr;
			hr = D3DCompile(
				kWorldQuadDepthPixelShader, std::strlen(kWorldQuadDepthPixelShader), "InSceneOverlay.depth.ps",
				nullptr, nullptr, "main", "ps_5_0",
				D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
				depthPsBlob.put(), errorBlob.put());
			if (FAILED(hr)) {
				logs::error("InSceneOverlay depth PS compile failed: {}",
					errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "<no message>");
				return false;
			}
			if (FAILED(device->CreatePixelShader(depthPsBlob->GetBufferPointer(), depthPsBlob->GetBufferSize(),
					nullptr, g_res.worldQuadDepthPS.put()))) {
				logs::error("InSceneOverlay: CreatePixelShader (depth) failed");
				return false;
			}
			{
				D3D11_BUFFER_DESC dcbDesc = {};
				dcbDesc.Usage = D3D11_USAGE_DYNAMIC;
				dcbDesc.ByteWidth = sizeof(WorldQuadDepthParams);
				dcbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				dcbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				if (FAILED(device->CreateBuffer(&dcbDesc, nullptr, g_res.depthCB.put()))) {
					logs::error("InSceneOverlay: depth constant buffer creation failed");
					return false;
				}
			}

			// Quad vertices (XY plane, z=0, size=1).
			struct VertexType
			{
				XMFLOAT3 position;
				XMFLOAT2 texture;
			};
			VertexType vertices[4] = {
				{ { -0.5f, -0.5f, 0.0f }, { 0.0f, 1.0f } },  // Bottom Left
				{ { -0.5f, 0.5f, 0.0f }, { 0.0f, 0.0f } },   // Top Left
				{ { 0.5f, 0.5f, 0.0f }, { 1.0f, 0.0f } },    // Top Right
				{ { 0.5f, -0.5f, 0.0f }, { 1.0f, 1.0f } },   // Bottom Right
			};

			D3D11_BUFFER_DESC vbDesc = {};
			vbDesc.Usage = D3D11_USAGE_DEFAULT;
			vbDesc.ByteWidth = sizeof(vertices);
			vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
			D3D11_SUBRESOURCE_DATA vbData = { vertices };
			if (FAILED(device->CreateBuffer(&vbDesc, &vbData, g_res.vb.put()))) {
				logs::error("InSceneOverlay: vertex buffer creation failed");
				return false;
			}

			uint32_t indices[6] = { 0, 1, 2, 0, 2, 3 };
			D3D11_BUFFER_DESC ibDesc = {};
			ibDesc.Usage = D3D11_USAGE_DEFAULT;
			ibDesc.ByteWidth = sizeof(indices);
			ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
			D3D11_SUBRESOURCE_DATA ibData = { indices };
			if (FAILED(device->CreateBuffer(&ibDesc, &ibData, g_res.ib.put()))) {
				logs::error("InSceneOverlay: index buffer creation failed");
				return false;
			}

			D3D11_BUFFER_DESC cbDesc = {};
			cbDesc.Usage = D3D11_USAGE_DYNAMIC;
			cbDesc.ByteWidth = sizeof(ConstantBufferData);
			cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(device->CreateBuffer(&cbDesc, nullptr, g_res.cb.put()))) {
				logs::error("InSceneOverlay: constant buffer creation failed");
				return false;
			}

			// Dynamic per-instance buffer for the world-quad pass; grown on demand in the pass.
			{
				constexpr uint32_t kInitialInstances = 256;
				D3D11_BUFFER_DESC instBufDesc = {};
				instBufDesc.Usage = D3D11_USAGE_DYNAMIC;
				instBufDesc.ByteWidth = sizeof(InstanceData) * kInitialInstances;
				instBufDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
				instBufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				if (FAILED(device->CreateBuffer(&instBufDesc, nullptr, g_res.instanceBuffer.put()))) {
					logs::error("InSceneOverlay: instance buffer creation failed");
					return false;
				}
				g_res.instanceCapacity = kInitialInstances;
			}

			// Blend state: standard alpha blending so the menu doesn't paint
			// black over the eye render where it's transparent. Draws with this
			// state land on the live eye buffer, which is never a target this
			// code clears -- its own alpha is inert, so overwriting dest alpha
			// (ZERO) rather than accumulating it is fine here.
			D3D11_BLEND_DESC blendDesc = {};
			blendDesc.RenderTarget[0].BlendEnable = TRUE;
			blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
			blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
			blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			blendDesc.RenderTarget[0].RenderTargetWriteMask = 0x0F;
			if (FAILED(device->CreateBlendState(&blendDesc, g_res.blendState.put()))) {
				logs::error("InSceneOverlay: blend state creation failed");
				return false;
			}

			// Standard "over" compositing for both RGB and alpha, used only by
			// RenderCursorIntoPanel: it draws onto OverlayTinter's panel canvas
			// AFTER the panel's own alpha is already there, so DestBlendAlpha
			// must accumulate (INV_SRC_ALPHA) instead of discarding (ZERO)
			// whatever alpha the destination already had.
			blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
			if (FAILED(device->CreateBlendState(&blendDesc, g_res.panelCompositeBlendState.put()))) {
				logs::error("InSceneOverlay: panel composite blend state creation failed");
				return false;
			}

			// Depth always pass, no write — overlay always on top.
			D3D11_DEPTH_STENCIL_DESC depthDesc = {};
			depthDesc.DepthEnable = FALSE;
			depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
			if (FAILED(device->CreateDepthStencilState(&depthDesc, g_res.depthState.put()))) {
				logs::error("InSceneOverlay: depth stencil state creation failed");
				return false;
			}

			D3D11_RASTERIZER_DESC rasterDesc = {};
			rasterDesc.FillMode = D3D11_FILL_SOLID;
			rasterDesc.CullMode = D3D11_CULL_NONE;
			rasterDesc.FrontCounterClockwise = FALSE;
			rasterDesc.DepthClipEnable = TRUE;
			if (FAILED(device->CreateRasterizerState(&rasterDesc, g_res.rasterizerState.put()))) {
				logs::error("InSceneOverlay: rasterizer state creation failed");
				return false;
			}

			D3D11_SAMPLER_DESC samplerDesc = {};
			samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
			samplerDesc.MinLOD = 0;
			samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
			if (FAILED(device->CreateSamplerState(&samplerDesc, g_res.sampler.put()))) {
				logs::error("InSceneOverlay: sampler creation failed");
				return false;
			}

			// Default white tint; RenderCursorPass regenerates on the render thread if the
			// user has set a custom Settings::cursorColor (EnsureCursorColorCurrent).
			constexpr float kDefaultTint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
			if (!CreateCursorTexture(device, kDefaultTint)) {
				return false;
			}

			g_res.initialized = true;
			logs::info("InSceneOverlay resources initialized");
			return true;
		}

		// ---- Cached RTV/SRV helpers -------------------------------------

		ID3D11RenderTargetView* GetEyeRTV(vr::EVREye eye, ID3D11Texture2D* tex)
		{
			const int eyeIdx = static_cast<int>(eye);
			if (eyeIdx < 0 || eyeIdx >= 2)  // only Eye_Left / Eye_Right are valid
				return nullptr;
			auto& cached = g_res.cachedEyeRTVs[eyeIdx];
			if (cached.texture == tex && cached.rtv)
				return cached.rtv.get();

			cached.rtv = nullptr;
			cached.texture = nullptr;

			D3D11_TEXTURE2D_DESC texDesc;
			tex->GetDesc(&texDesc);

			// A texture without this bind flag can never become an RTV; warn
			// once and bail instead of retrying (and logging) every frame.
			if (!(texDesc.BindFlags & D3D11_BIND_RENDER_TARGET)) {
				static std::atomic<bool> warned{ false };
				if (!warned.exchange(true, std::memory_order_relaxed)) {
					logs::warn(
						"InSceneOverlay: eye texture isn't render-target capable (bindFlags={:#x}); "
						"in-scene eye overlay disabled for this session (common under OpenComposite)",
						texDesc.BindFlags);
				}
				return nullptr;
			}

			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
			rtvDesc.Format = texDesc.Format;
			if (texDesc.ArraySize > 1) {
				if (texDesc.SampleDesc.Count > 1) {
					rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
					rtvDesc.Texture2DMSArray.FirstArraySlice = static_cast<UINT>(eye);
					rtvDesc.Texture2DMSArray.ArraySize = 1;
				} else {
					rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
					rtvDesc.Texture2DArray.FirstArraySlice = static_cast<UINT>(eye);
					rtvDesc.Texture2DArray.ArraySize = 1;
					rtvDesc.Texture2DArray.MipSlice = 0;
				}
			} else if (texDesc.SampleDesc.Count > 1) {
				rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
			} else {
				rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
				rtvDesc.Texture2D.MipSlice = 0;
			}

			if (FAILED(Globals::GetD3D().device->CreateRenderTargetView(tex, &rtvDesc, cached.rtv.put()))) {
				if (cached.lastFailedTexture != tex) {
					cached.lastFailedTexture = tex;
					logs::error("InSceneOverlay: failed to create eye RTV (fmt={}, samples={})",
						static_cast<uint32_t>(texDesc.Format), texDesc.SampleDesc.Count);
				}
				return nullptr;
			}
			cached.texture = tex;
			cached.lastFailedTexture = nullptr;
			return cached.rtv.get();
		}

		ID3D11ShaderResourceView* GetMenuSRV(ID3D11Texture2D* menuTex)
		{
			if (!menuTex)
				return nullptr;
			if (g_res.cachedMenuTexture == menuTex && g_res.menuSRV) {
				return g_res.menuSRV.get();
			}
			g_res.menuSRV = nullptr;
			if (FAILED(Globals::GetD3D().device->CreateShaderResourceView(menuTex, nullptr, g_res.menuSRV.put()))) {
				logs::error("InSceneOverlay: failed to create menu SRV");
				return nullptr;
			}
			g_res.cachedMenuTexture = menuTex;
			return g_res.menuSRV.get();
		}

		// ---- Render-path fault latch ------------------------------------

		std::atomic<bool> g_renderDisabled{ false };

		// ---- Submit hook ------------------------------------------------

		struct SubmitDetour
		{
			using FnType = vr::EVRCompositorError (*)(
				vr::IVRCompositor*, vr::EVREye, const vr::Texture_t*,
				const vr::VRTextureBounds_t*, vr::EVRSubmitFlags);

			static FnType original;

			static vr::EVRCompositorError thunk(vr::IVRCompositor* self, vr::EVREye eye,
				const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds,
				vr::EVRSubmitFlags flags)
			{
				if (!RenderHost::IsActive() && !g_renderDisabled.load(std::memory_order_relaxed) &&
					texture && texture->handle && texture->eType == vr::TextureType_DirectX) {
					// Guard against vrclient throwing std::system_error
					// ("device or resource busy") under runtime contention. On
					// first fault we latch the render path off for the session
					// rather than letting the exception crash the game.
					try {
						RenderForEye(eye, static_cast<ID3D11Texture2D*>(texture->handle), bounds);
					} catch (const std::exception& e) {
						DisableRenderPath("exception in RenderForEye");
						logs::error("InSceneOverlay: render path disabled after exception: {}", e.what());
					}
				}
				return original(self, eye, texture, bounds, flags);
			}
		};

		SubmitDetour::FnType SubmitDetour::original = nullptr;

		bool g_hookInstalled = false;

		// ---- Helper: per-eye view-projection matrices -------------------

		struct EyeMatrices
		{
			Matrix vpHeadSpace;   // for HMD-relative attach mode
			Matrix vpWorldSpace;  // for controller-attached and fixed-world
			Vector3 hmdPos;       // HMD position (standing space) from the SAME pose as vpWorldSpace
			bool valid = false;
		};

		EyeMatrices ComputeEyeMatrices(vr::EVREye eye)
		{
			EyeMatrices result;
			auto* openvr = RE::BSOpenVR::GetSingleton();
			if (!openvr || !openvr->vrSystem)
				return result;

			vr::TrackedDevicePose_t pose[vr::k_unMaxTrackedDeviceCount];
			auto* compositor = RE::BSOpenVR::GetIVRCompositor();
			if (!compositor)
				return result;
			if (compositor->GetLastPoses(pose, vr::k_unMaxTrackedDeviceCount, nullptr, 0) !=
				vr::VRCompositorError_None) {
				return result;
			}
			const auto& hmdPose = pose[vr::k_unTrackedDeviceIndex_Hmd];
			if (!hmdPose.bPoseIsValid)
				return result;

			// Eye-to-head and projection are read from the property cache
			// (populated on the input thread) rather than queried here — calling
			// IVRSystem property methods from the render thread races
			// skyrimvrtools on vrclient's CClientPropertyManager and crashes.
			vr::HmdMatrix34_t eyeToHeadRaw;
			if (!Util::CachedEyeToHead(eye, eyeToHeadRaw))
				return result;  // cache not populated yet (very early startup)
			Matrix eyeToHead = Util::HmdMatrix34ToMatrix(eyeToHeadRaw);
			Matrix hmdWorld = Util::HmdMatrix34ToMatrix(hmdPose.mDeviceToAbsoluteTracking);

			// GetProjectionRaw returns left/right/bottom/top tangents (note
			// Valve's known parameter-name mismatch — the 3rd param is
			// actually bottom, the 4th is top).
			float left, right, bottom, top;
			if (!Util::CachedProjectionRaw(eye, left, right, bottom, top))
				return result;
			constexpr float nearZ = 0.1f;
			constexpr float farZ = 1000.0f;
			Matrix proj = DirectX::XMMatrixPerspectiveOffCenterRH(
				left * nearZ, right * nearZ, bottom * nearZ, top * nearZ, nearZ, farZ);

			result.vpHeadSpace = eyeToHead.Invert() * proj;
			Matrix eyeToWorld = eyeToHead * hmdWorld;
			result.vpWorldSpace = eyeToWorld.Invert() * proj;
			result.hmdPos = hmdWorld.Translation();  // share the vpWorldSpace pose with world-quad facing
			result.valid = true;
			return result;
		}

		// ---- D3D11 state save/restore -----------------------------------

		struct StateBackup
		{
			ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
			ID3D11DepthStencilView* dsv = nullptr;
			D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			UINT numViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			ID3D11RasterizerState* raster = nullptr;
			ID3D11BlendState* blend = nullptr;
			FLOAT blendFactor[4]{};
			UINT sampleMask = 0;
			ID3D11DepthStencilState* depth = nullptr;
			UINT stencilRef = 0;

			void Save(ID3D11DeviceContext* ctx)
			{
				ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
				ctx->RSGetViewports(&numViewports, viewports);
				ctx->RSGetState(&raster);
				ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
				ctx->OMGetDepthStencilState(&depth, &stencilRef);
			}

			void Restore(ID3D11DeviceContext* ctx)
			{
				ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
				ctx->RSSetViewports(numViewports, viewports);
				ctx->OMSetBlendState(blend, blendFactor, sampleMask);
				ctx->OMSetDepthStencilState(depth, stencilRef);
				if (raster) {
					ctx->RSSetState(raster);
					raster->Release();
				}
				if (blend)
					blend->Release();
				if (depth)
					depth->Release();
				for (auto* r : rtvs) {
					if (r)
						r->Release();
				}
				if (dsv)
					dsv->Release();
			}
		};

		// ---- Quad draw --------------------------------------------------

		void DrawQuad(ID3D11DeviceContext* ctx, const ConstantBufferData& cbData,
			ID3D11ShaderResourceView* srv, ID3D11BlendState* blendState = nullptr)
		{
			D3D11_MAPPED_SUBRESOURCE mapped;
			if (SUCCEEDED(ctx->Map(g_res.cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				std::memcpy(mapped.pData, &cbData, sizeof(cbData));
				ctx->Unmap(g_res.cb.get(), 0);
			} else {
				logs::warn("InSceneOverlay::DrawQuad: Map constant buffer failed; skipping draw");
				return;
			}

			ctx->VSSetShader(g_res.vs.get(), nullptr, 0);
			ctx->PSSetShader(g_res.ps.get(), nullptr, 0);

			ID3D11Buffer* cb = g_res.cb.get();
			ctx->VSSetConstantBuffers(0, 1, &cb);

			struct VT
			{
				XMFLOAT3 p;
				XMFLOAT2 t;
			};
			UINT stride = sizeof(VT);
			UINT offset = 0;
			ID3D11Buffer* vb = g_res.vb.get();
			ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
			ctx->IASetIndexBuffer(g_res.ib.get(), DXGI_FORMAT_R32_UINT, 0);
			ctx->IASetInputLayout(g_res.inputLayout.get());
			ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			ctx->OMSetBlendState(blendState ? blendState : g_res.blendState.get(), nullptr, 0xFFFFFFFF);
			ctx->OMSetDepthStencilState(g_res.depthState.get(), 0);
			ctx->RSSetState(g_res.rasterizerState.get());

			ctx->PSSetShaderResources(0, 1, &srv);
			ID3D11SamplerState* sampler = g_res.sampler.get();
			ctx->PSSetSamplers(0, 1, &sampler);

			ctx->DrawIndexed(6, 0, 0);
		}
	}  // namespace

	// ---- Public entry points --------------------------------------------

	void Install()
	{
		if (g_hookInstalled)
			return;

		auto* openvr = RE::BSOpenVR::GetSingleton();
		auto* compositor = openvr ? RE::BSOpenVR::GetIVRCompositor() : nullptr;
		if (!compositor) {
			logs::warn("InSceneOverlay::Install: IVRCompositor unavailable");
			return;
		}

		// IVRCompositor::Submit is vtable slot 5 (after SetTrackingSpace,
		// GetTrackingSpace, WaitGetPoses, GetLastPoses,
		// GetLastPoseForTrackedDeviceIndex).
		SubmitDetour::original = Util::DetourClassVTable<SubmitDetour::FnType>(
			compositor, 5, &SubmitDetour::thunk);
		g_hookInstalled = true;
		logs::info("InSceneOverlay: IVRCompositor::Submit detour installed (original={})",
			reinterpret_cast<void*>(SubmitDetour::original));
	}

	bool IsRenderPathDisabled()
	{
		return g_renderDisabled.load(std::memory_order_relaxed);
	}

	void DisableRenderPath(const char* reason)
	{
		// Latch once; the first caller wins and logs the reason.
		bool expected = false;
		if (g_renderDisabled.compare_exchange_strong(expected, true,
				std::memory_order_relaxed)) {
			logs::error("InSceneOverlay: render path disabled for this session ({})",
				reason ? reason : "unspecified");
		}
	}

	namespace
	{
		// Per-client SRV cache for HUD-mode panels. Keyed by texture
		// pointer so we rebuild only when a client's RTV is reallocated.
		// The single 'menuSRV' field above only handles the focused
		// panel client; HUD clients need their own SRV slot each.
		struct HUDSRVCache
		{
			ID3D11Texture2D* texture = nullptr;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;
		};
		std::unordered_map<uint32_t /*client_id*/, HUDSRVCache> g_hudSRVs;

		ID3D11ShaderResourceView* GetOrCreateHUDSRV(uint32_t client_id, ID3D11Texture2D* tex)
		{
			if (!tex)
				return nullptr;
			auto& cache = g_hudSRVs[client_id];
			if (cache.texture == tex && cache.srv)
				return cache.srv.get();
			cache.srv = nullptr;
			if (FAILED(Globals::GetD3D().device->CreateShaderResourceView(tex, nullptr, cache.srv.put()))) {
				logs::error("InSceneOverlay: failed to create HUD SRV for client_id={}", client_id);
				return nullptr;
			}
			cache.texture = tex;
			return cache.srv.get();
		}
	}

	// HUD pass: every HUD-mode client drawn as one eye-independent, head-locked
	// panel (see ComputeHUDQuad) so both eyes fuse it at hudDepth. The caller has
	// already bound the eye RTV + viewport.
	void RenderHUDPass(ID3D11DeviceContext* ctx, const EyeMatrices& matrices,
		const Overlay::Settings& s,
		const std::vector<HelperImpl::HUDClientSnapshot>& hudClients)
	{
		ZoneScopedN("InScene::HUDPass");
		HELPER_GPU_PASS("ImGuiVRHelper::HUDPass");
		const float hudDepth = std::max(0.3f, s.hudDepth);  // sanity floor
		const float coverage = std::clamp(s.hudCoverage, Overlay::Config::kMinHUDCoverage, Overlay::Config::kMaxHUDCoverage);

		float projL[4], projR[4];
		Util::CachedProjectionRaw(vr::Eye_Left, projL[0], projL[1], projL[2], projL[3]);
		Util::CachedProjectionRaw(vr::Eye_Right, projR[0], projR[1], projR[2], projR[3]);
		const HUDQuad hudQuad = ComputeHUDQuad(projL, projR, hudDepth, coverage);

		const Matrix hudModel =
			Matrix::CreateScale(hudQuad.width, hudQuad.height, 1.0f) *
			Matrix::CreateTranslation(0.0f, hudQuad.centerY, -hudDepth);
		const Matrix hudWvp = (hudModel * matrices.vpHeadSpace).Transpose();

		// Iteration = registration order, so later HUD layers draw on top.
		for (const auto& hud : hudClients) {
			if (!hud.texture)
				continue;
			auto* hudSRV = GetOrCreateHUDSRV(hud.client_id, hud.texture.get());
			if (!hudSRV)
				continue;
			ConstantBufferData cb;
			cb.wvp = hudWvp;
			DrawQuad(ctx, cb, hudSRV);
		}
	}

	// Reserved HUD-SRV cache key for the single rebind-capture texture (it's
	// not in the HUD client list, so it needs its own slot).
	constexpr uint32_t kRebindSRVKey = 0xFFFFFFFEu;

	// Rebind pass: the combo-capture overlay, drawn as a head-locked panel like
	// the HUD pass but AFTER the focused panel pass so it composites ON TOP of
	// the focused client (depth test is off → draw order decides). No-op unless
	// a recording is active.
	void RenderRebindPass(ID3D11DeviceContext* ctx, const EyeMatrices& matrices,
		const Overlay::Settings& s)
	{
		ZoneScopedN("InScene::RebindPass");
		HELPER_GPU_PASS("ImGuiVRHelper::RebindPass");
		auto tex = HelperImpl::GetSingleton().GetActiveRebindTexture();
		if (!tex)
			return;
		auto* srv = GetOrCreateHUDSRV(kRebindSRVKey, tex.get());
		if (!srv)
			return;

		const float hudDepth = std::max(0.3f, s.hudDepth);
		const float coverage = std::clamp(s.hudCoverage, Overlay::Config::kMinHUDCoverage, Overlay::Config::kMaxHUDCoverage);
		float projL[4], projR[4];
		Util::CachedProjectionRaw(vr::Eye_Left, projL[0], projL[1], projL[2], projL[3]);
		Util::CachedProjectionRaw(vr::Eye_Right, projR[0], projR[1], projR[2], projR[3]);
		const HUDQuad q = ComputeHUDQuad(projL, projR, hudDepth, coverage);

		const Matrix model = Matrix::CreateScale(q.width, q.height, 1.0f) *
		                     Matrix::CreateTranslation(0.0f, q.centerY, -hudDepth);
		ConstantBufferData cb;
		cb.wvp = (model * matrices.vpHeadSpace).Transpose();
		DrawQuad(ctx, cb, srv);
	}

	// Panel pass: the focused panel-mode client as a 3D quad, attached to the
	// HMD and/or the controller per the user's attachMode.
	// Resolves the anchor (world matrix, BEFORE Config::CreateScaleMatrix(menuScale) is applied)
	// and the view-projection to render/hit-test against, for whichever attach point (HMD or
	// controller) is requested. Shared by RenderPanelPass and RenderCursorPass so "which anchor
	// for which attach mode" lives in exactly one place — this pair used to be hand-duplicated
	// between the two passes, which is exactly the kind of drift that caused the marker/panel
	// divergence investigation. Returns false if the requested attach point's tracking data isn't
	// available this frame (caller should skip its draw).
	bool ResolveAnchorWorld(Overlay::OverlayType type, const Overlay::Settings& s,
		const Overlay::State& state, Matrix& outAnchor, bool& outHeadSpace)
	{
		outHeadSpace = false;
		if (type == Overlay::OverlayType::HMD) {
			if (s.positioningMethod == Overlay::PositioningMethod::FixedWorld) {
				outAnchor = state.fixedWorld.m;
			} else {
				outAnchor = Matrix::CreateTranslation(s.hmdOffsetX, s.hmdOffsetY, s.hmdOffsetZ);
				outHeadSpace = true;
			}
			return true;
		}

		const auto attachIdx = Util::GetControllerIndexForDevice(
			s.attachController, state.lastKnownLeftHandedMode);
		if (attachIdx == vr::k_unTrackedDeviceIndexInvalid)
			return false;

		vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
		if (!Util::GetDeviceToAbsoluteTrackingPoseCompatible(
				vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount) ||
			!poses[attachIdx].bPoseIsValid)
			return false;

		Matrix controllerWorld = Util::HmdMatrix34ToMatrix(poses[attachIdx].mDeviceToAbsoluteTracking);
		Matrix offset = Matrix::CreateTranslation(
			s.controllerOffsetX, s.controllerOffsetY, s.controllerOffsetZ);
		outAnchor = offset * controllerWorld;
		return true;
	}

	bool ResolveAnchor(Overlay::OverlayType type, const Overlay::Settings& s,
		const Overlay::State& overlayState, const EyeMatrices& matrices,
		Matrix& outAnchor, Matrix& outVpMat)
	{
		bool headSpace = false;
		if (!ResolveAnchorWorld(type, s, overlayState, outAnchor, headSpace))
			return false;
		outVpMat = headSpace ? matrices.vpHeadSpace : matrices.vpWorldSpace;
		return true;
	}

	void RenderPanelPass(ID3D11DeviceContext* ctx, vr::EVREye eye,
		const EyeMatrices& matrices, const Overlay::Settings& s,
		const Overlay::State& overlayState, ID3D11ShaderResourceView* panelSRV)
	{
		ZoneScopedN("InScene::PanelPass");
		HELPER_GPU_PASS("ImGuiVRHelper::PanelPass");
		// HMD-attached.
		if (s.attachMode == Overlay::AttachMode::HMDOnly ||
			s.attachMode == Overlay::AttachMode::Both) {
			Matrix anchor, vpMat;
			if (ResolveAnchor(Overlay::OverlayType::HMD, s, overlayState, matrices, anchor, vpMat)) {
				Matrix model = Overlay::Config::CreateScaleMatrix(s.menuScale) * anchor;
				ConstantBufferData cb;
				cb.wvp = (model * vpMat).Transpose();
				DrawQuad(ctx, cb, panelSRV);
			}
		}

		// Controller-attached (with backface culling).
		if (s.attachMode != Overlay::AttachMode::ControllerOnly &&
			s.attachMode != Overlay::AttachMode::Both)
			return;

		Matrix anchor, vpMat;
		if (!ResolveAnchor(Overlay::OverlayType::Controller, s, overlayState, matrices, anchor, vpMat))
			return;
		Matrix model = Overlay::Config::CreateScaleMatrix(s.menuScale) * anchor;

		// Backface culling: hide the overlay when viewed from behind. Needs the controller/HMD
		// poses again (ResolveAnchor doesn't expose them) — a second OpenVR pose query, not a
		// re-derivation of the anchor math itself, so it doesn't reintroduce the drift risk above.
		const auto attachIdx = Util::GetControllerIndexForDevice(
			s.attachController, overlayState.lastKnownLeftHandedMode);
		vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
		if (attachIdx == vr::k_unTrackedDeviceIndexInvalid ||
			!Util::GetDeviceToAbsoluteTrackingPoseCompatible(
				vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount) ||
			!poses[attachIdx].bPoseIsValid)
			return;

		Vector3 overlayNormal(anchor._31, anchor._32, anchor._33);
		overlayNormal.Normalize();
		Matrix hmdWorld = Util::HmdMatrix34ToMatrix(
			poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking);
		// Cached eye-to-head (input thread); querying IVRSystem here would race
		// vrclient from the render thread and could deref a null vrSystem.
		vr::HmdMatrix34_t eyeToHeadRaw{};
		Util::CachedEyeToHead(eye, eyeToHeadRaw);
		Matrix eyeToHead = Util::HmdMatrix34ToMatrix(eyeToHeadRaw);
		Matrix eyeWorld = eyeToHead * hmdWorld;
		Vector3 toEye = eyeWorld.Translation() - anchor.Translation();
		toEye.Normalize();
		if (overlayNormal.Dot(toEye) > 0.0f) {
			ConstantBufferData cb;
			cb.wvp = (model * vpMat).Transpose();
			DrawQuad(ctx, cb, panelSRV);
		}
	}

	// Marker pass: a small quad carrying the cursor texture, placed at the wand's current
	// panel-UV hit (Overlay::State::wandState) using the SAME anchor transform as the panel quad
	// it lands on, so it rides along with the panel exactly (position, drag, scale). No-op if the
	// wand isn't currently on the panel — GetPointer/PumpInput already treat "off panel" as
	// belonging to the client/game, so there's nothing useful to point at.
	//
	// Local-space convention matches WandPointing::ComputeIntersectionForOverlayType: the panel
	// quad is a unit square in [-0.5,0.5] BEFORE Config::CreateScaleMatrix(menuScale) is applied,
	// and that's exactly the space the ray-plane hit test resolves in (uv.x = hit.x+0.5, uv.y =
	// 0.5-hit.y), so inverting that gives the marker's position in the same pre-scale local space
	// the panel's own vertices live in — composing it with the SAME CreateScaleMatrix(menuScale) *
	// anchor chain keeps the two in lockstep.
	void RenderCursorPass(ID3D11DeviceContext* ctx,
		const EyeMatrices& matrices, const Overlay::Settings& s, const Overlay::State& overlayState)
	{
		const auto& wand = overlayState.wandState;
		if (!wand.isIntersecting)
			return;
		if (auto* device = Globals::GetD3D().device)
			EnsureCursorColorCurrent(device, s.cursorColor);
		ID3D11ShaderResourceView* cursorSRV = s.cursorStyle == Overlay::CursorStyle::Arrow ?
		                                          g_res.cursorArrowSRV.get() :
		                                          g_res.cursorSRV.get();
		if (!cursorSRV)
			return;

		// Settings changed since the hit was computed this tick (e.g. attach mode switched
		// mid-frame) — skip rather than resolve a stale anchor for a mode that's no longer active.
		if (!Overlay::IsAttachModeCompatible(wand.matchedOverlayType, s.attachMode))
			return;

		Matrix anchor, vpMat;
		if (!ResolveAnchor(wand.matchedOverlayType, s, overlayState, matrices, anchor, vpMat))
			return;
		// No backface cull here (unlike the panel quad): a hit was only recorded if the ray
		// crossed the panel plane in front of the controller, which in practice means the same
		// side the player is viewing from.

		// Marker size as a fraction of panel local space, pre-compensated by the inverse of
		// CreateScaleMatrix's Y stretch so composing it with that same scale yields a round dot
		// instead of one squashed/stretched by the panel's aspect correction.
		constexpr float kMarkerLocalSize = 0.016f;
		const float markerSize = kMarkerLocalSize * s.cursorSize;
		const float localX = wand.uvCoordinatesX.load(std::memory_order_relaxed) - 0.5f;
		const float localY = 0.5f - wand.uvCoordinatesY.load(std::memory_order_relaxed);
		Matrix markerLocal =
			Matrix::CreateScale(markerSize, markerSize / Overlay::Config::kOverlayAspect, 1.0f) *
			Matrix::CreateTranslation(localX, localY, 0.0f);
		Matrix model = markerLocal * Overlay::Config::CreateScaleMatrix(s.menuScale) * anchor;

		// TEMP diagnostic: log the wand UV/marker NDC on the actual trigger-press edge (ground
		// truth -- a real click, not an arbitrary periodic sample) so it can be reconciled against
		// the client's own log of what it thinks got clicked (io.MousePos / hovered widget) for
		// the SAME press. A periodic sample only proves this pass's own math is self-consistent
		// (marker vs panel computed with the identical anchor/vpMat, so of course they agree) --
		// it says nothing about whether the underlying wand UV corresponds to where content
		// actually is, which is the open question here.
		{
			using Keys = RE::BSOpenVRControllerDevice::Keys;
			static bool prevTriggerHeld = false;
			const bool triggerHeld = overlayState.primaryControllerState[Keys::kTrigger].isPressed ||
			                         overlayState.secondaryControllerState[Keys::kTrigger].isPressed;
			if (triggerHeld && !prevTriggerHeld) {
				Matrix panelModel = Overlay::Config::CreateScaleMatrix(s.menuScale) * anchor;
				Matrix panelMvp = panelModel * vpMat;
				Matrix markerMvp = model * vpMat;
				auto toNDC = [&](Vector3 local, const Matrix& mvp) {
					DirectX::XMVECTOR v = DirectX::XMVector3TransformCoord(local, mvp);
					DirectX::XMFLOAT3 f;
					DirectX::XMStoreFloat3(&f, v);
					return f;
				};
				auto l = toNDC({ -0.5f, 0.0f, 0.0f }, panelMvp);
				auto c = toNDC({ 0.0f, 0.0f, 0.0f }, panelMvp);
				auto r = toNDC({ 0.5f, 0.0f, 0.0f }, panelMvp);
				auto m = toNDC({ 0.0f, 0.0f, 0.0f }, markerMvp);
				logs::debug(
					"TriggerPress NDC: uv=({:.3f},{:.3f}) local=({:.3f},{:.3f}) marker=({:.3f},{:.3f}) "
					"panelLeft=({:.3f},{:.3f}) panelCenter=({:.3f},{:.3f}) panelRight=({:.3f},{:.3f})",
					wand.uvCoordinatesX.load(std::memory_order_relaxed), wand.uvCoordinatesY.load(std::memory_order_relaxed), localX, localY, m.x, m.y,
					l.x, l.y, c.x, c.y, r.x, r.y);
			}
			prevTriggerHeld = triggerHeld;
		}

		ConstantBufferData cb;
		cb.wvp = (model * vpMat).Transpose();
		DrawQuad(ctx, cb, cursorSRV);
	}

	// Scene depth for world-quad occlusion. kMAIN is the game's live main depth. Its resolution is
	// NOT always the submit target's: an upscaling pipeline (e.g. an FSR performance tier) can
	// leave depth at the lower internal render resolution while only color gets upscaled for the
	// final submit (confirmed via RenderDoc — a 1.5x mismatch cut billboards off incorrectly), so
	// depthW/depthH (the SRV's actual resource size) must be read each frame, not assumed 1:1.
	// camNear/camFar come from the game camera for standard-Z linearization (offsets per
	// open-shaders' GetCameraData).
	struct SceneDepthInfo
	{
		ID3D11ShaderResourceView* srv = nullptr;
		float camNear = 0.0f;
		float camFar = 0.0f;
		float depthW = 0.0f;
		float depthH = 0.0f;
	};
	SceneDepthInfo GetSceneDepthInfo()
	{
		SceneDepthInfo info;
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer)
			return info;
		info.srv = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN].depthSRV;
		static REL::Relocation<std::uintptr_t> cameraData{ REL::RelocationID(517032, 403540) };
		info.camNear = *reinterpret_cast<const float*>(cameraData.address() + 0x40);
		info.camFar = *reinterpret_cast<const float*>(cameraData.address() + 0x44);
		if (info.srv) {
			// The underlying resource's dimensions don't change without the SRV pointer itself
			// changing (a new resource on an upscale-mode/resolution switch), so cache by pointer
			// instead of paying GetResource+QueryInterface+GetDesc every eye, every frame.
			static ID3D11ShaderResourceView* cachedSRV = nullptr;
			static float cachedW = 0.0f, cachedH = 0.0f;
			if (info.srv != cachedSRV) {
				winrt::com_ptr<ID3D11Resource> res;
				info.srv->GetResource(res.put());
				winrt::com_ptr<ID3D11Texture2D> tex;
				if (res && SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(tex.put())))) {
					D3D11_TEXTURE2D_DESC desc;
					tex->GetDesc(&desc);
					cachedW = static_cast<float>(desc.Width);
					cachedH = static_cast<float>(desc.Height);
				}
				cachedSRV = info.srv;
			}
			info.depthW = cachedW;
			info.depthH = cachedH;
		}
		return info;
	}

	// Skyrim-world -> OpenVR-tracking conversion for world-quad billboards, read fresh in this
	// same pass (the same call that builds vpWorldSpace from a fresh compositor pose) rather than
	// by the client at an earlier point in the frame. RoomNode's world transform (the play-space
	// origin) moves every frame the player walks; a client-side conversion snapshotting it earlier
	// desyncs from the pose vpWorldSpace uses and shows up as visible bobbing while moving.
	struct SkyrimToTrackingXform
	{
		bool valid = false;
		RE::NiPoint3 origin;
		RE::NiMatrix3 rotate;
		float invScale = 1.0f;
	};
	SkyrimToTrackingXform GetSkyrimToTrackingXform()
	{
		SkyrimToTrackingXform x;
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* nodeData = player ? player->GetVRNodeData() : nullptr;
		auto room = nodeData ? nodeData->RoomNode.get() : nullptr;
		if (!room)
			return x;
		x.origin = room->world.translate;
		x.rotate = room->world.rotate;
		x.invScale = (room->world.scale > 0.0f) ? (1.0f / room->world.scale) : 1.0f;

		// VRIK's head-bob shifts its camera node's local Z each frame to make Skyrim's own scene
		// render sway (see VrikCompat.h) — it never touches RoomNode or the real OpenVR pose
		// (confirmed: origin/rotate above are provably stable through active movement even with
		// VRIK's head-bob on). Add VRIK's offset to origin.z: since relz = pos.z - origin.z below,
		// increasing origin.z by the same amount the camera rose decreases every anchor's relz by
		// that amount too — matching how a fixed world point appears to sink, from the camera's
		// perspective, when the camera itself rises. A no-op (offset is zero) without VRIK, or
		// with VRIK's head-bob disabled.
		x.origin.z += VrikCompat::GetCameraOffset().z;

		x.valid = true;
		return x;
	}

	// Skyrim world units -> metres. Must match FloatingSubtitles::ImGui::Renderer::kGameUnitToMeter.
	constexpr float kSkyrimUnitToMeter = 0.01428f;

	// Room-local (world-aligned, x-right/y-forward/z-up) -> OpenVR tracking (x-right/y-up/
	// z-toward-user) is an axis swap (x, z, -y); the room origin is the OpenVR standing origin, so
	// this is the tracking-space position directly once in room-local units.
	Vector3 SkyrimToTracking(const SkyrimToTrackingXform& x, const float pos[3])
	{
		const float relx = pos[0] - x.origin.x;
		const float rely = pos[1] - x.origin.y;
		const float relz = pos[2] - x.origin.z;
		const auto& r = x.rotate.entry;
		const float rx = (r[0][0] * relx + r[1][0] * rely + r[2][0] * relz) * x.invScale;
		const float ry = (r[0][1] * relx + r[1][1] * rely + r[2][1] * relz) * x.invScale;
		const float rz = (r[0][2] * relx + r[1][2] * rely + r[2][2] * relz) * x.invScale;
		return Vector3(rx, rz, -ry) * kSkyrimUnitToMeter;
	}

	// World-quad pass: each kClientFlag_WorldQuad client's submitted billboards, drawn at their
	// Skyrim world positions (converted to tracking space above) facing the HMD, so they stay
	// anchored in the scene instead of swimming on the head-locked HUD. Occlusion is per-pixel
	// against the game's scene depth (worldQuadDepthPS), so a billboard behind world geometry is
	// correctly hidden.
	void RenderWorldQuadPass(ID3D11DeviceContext* ctx, const EyeMatrices& matrices,
		const std::vector<HelperImpl::WorldQuadClientSnapshot>& worldClients, float submitW, float submitH)
	{
		ZoneScopedN("InScene::WorldQuadPass");
		if (worldClients.empty())
			return;
		HELPER_GPU_PASS("ImGuiVRHelper::WorldQuadPass");

		// Read fresh in this same pass — same call, same instant as matrices.hmdPos/vpWorldSpace
		// below — so client billboards and the eye projection are never built from room-transform
		// snapshots taken at different points in the frame.
		const SkyrimToTrackingXform skyrimXf = GetSkyrimToTrackingXform();
		if (!skyrimXf.valid)
			return;

		// HMD position (standing space) for billboard facing — reuse the SAME pose
		// vpWorldSpace was built from (matrices.hmdPos), so the quad's orientation can't
		// diverge from its projection within the pass. Shared across both eyes (only
		// vpWorldSpace differs); a per-eye yaw would break stereo fusion.
		const Vector3 hmdPos = matrices.hmdPos;

		auto* device = Globals::GetD3D().device;
		if (!device)
			return;

		// Bind the instanced quad pipeline ONCE for the pass. Slot 0 = the unit-quad VB
		// (per-vertex); slot 1 = the per-instance buffer (wvp rows + uv transform), bound per
		// client below since it can be regrown. Each client's billboards then draw in a single
		// DrawIndexedInstanced. State is restored by the caller's StateBackup.
		struct VT
		{
			XMFLOAT3 p;
			XMFLOAT2 t;
		};
		ctx->VSSetShader(g_res.instancedVS.get(), nullptr, 0);
		ctx->PSSetShader(g_res.worldQuadDepthPS.get(), nullptr, 0);
		ctx->IASetIndexBuffer(g_res.ib.get(), DXGI_FORMAT_R32_UINT, 0);
		ctx->IASetInputLayout(g_res.instancedInputLayout.get());
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->OMSetBlendState(g_res.blendState.get(), nullptr, 0xFFFFFFFF);
		ctx->OMSetDepthStencilState(g_res.depthState.get(), 0);
		ctx->RSSetState(g_res.rasterizerState.get());
		ID3D11SamplerState* samp = g_res.sampler.get();
		ctx->PSSetSamplers(0, 1, &samp);

		// Scene-depth occlusion: bind the game depth (t1) + linearization params (b0) for the whole
		// pass. Disabled gracefully (params.depthTestEnable stays 0) if the depth SRV isn't ready or
		// the camera near/far look invalid, so the billboards still draw un-occluded.
		SceneDepthInfo depthInfo = GetSceneDepthInfo();
		WorldQuadDepthParams depthParams;
		if (depthInfo.srv && depthInfo.camNear > 0.0f && depthInfo.camFar > depthInfo.camNear) {
			depthParams.cameraData[0] = depthInfo.camFar;
			depthParams.cameraData[1] = depthInfo.camNear;
			depthParams.cameraData[2] = depthInfo.camFar - depthInfo.camNear;
			depthParams.cameraData[3] = depthInfo.camFar * depthInfo.camNear;
			// Game unit -> OpenVR metre, scaled by the same room-scale factor skyrimXf's invScale
			// already carries, so the depth compare matches how billboards are positioned.
			depthParams.gameToMeter = kSkyrimUnitToMeter * skyrimXf.invScale;
			// Dynamic resolution shrinks the rendered region without resizing the depth texture.
			// Combine its current scale with the depth/submit resource-size ratio.
			if (depthInfo.depthW > 0.0f && depthInfo.depthH > 0.0f && submitW > 0.0f && submitH > 0.0f) {
				const auto& state = RE::BSGraphics::State::GetSingleton()->GetRuntimeData();
				depthParams.depthScale[0] = depthInfo.depthW * state.dynamicResolutionWidthRatio / submitW;
				depthParams.depthScale[1] = depthInfo.depthH * state.dynamicResolutionHeightRatio / submitH;
				depthParams.depthTestEnable = 1.0f;
			}
		}
		D3D11_MAPPED_SUBRESOURCE depthMap;
		if (SUCCEEDED(ctx->Map(g_res.depthCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &depthMap))) {
			std::memcpy(depthMap.pData, &depthParams, sizeof(depthParams));
			ctx->Unmap(g_res.depthCB.get(), 0);
		}
		ID3D11Buffer* dcb = g_res.depthCB.get();
		ctx->PSSetConstantBuffers(0, 1, &dcb);
		if (depthParams.depthTestEnable > 0.5f) {
			ID3D11ShaderResourceView* dsrv = depthInfo.srv;
			ctx->PSSetShaderResources(1, 1, &dsrv);  // t1 = scene depth (t0 = panel, set per client)
		}

		std::vector<InstanceData> instances;

		for (const auto& client : worldClients) {
			if (!client.texture)
				continue;
			// The temporary string's .c_str() is safe here: it lives through the whole
			// declaration-with-initializer full-expression, which encompasses the entire
			// GPUMarker constructor call — and that constructor consumes the name synchronously
			// (BeginEvent by COM contract; Tracy's AllocSourceLocation memcpy's it immediately).
			HELPER_GPU_PASS(("WorldQuad:" + client.name).c_str());
			auto* srv = GetOrCreateHUDSRV(client.client_id, client.texture.get());
			if (!srv)
				continue;
			D3D11_TEXTURE2D_DESC texDesc;
			client.texture->GetDesc(&texDesc);
			const float texW = static_cast<float>(texDesc.Width);
			const float texH = static_cast<float>(texDesc.Height);

			instances.clear();
			instances.reserve(client.quads.size());
			for (const auto& q : client.quads) {
				// Reject non-finite inputs so NaN/Inf never reach the transform or UVs.
				if (!std::isfinite(q.u0) || !std::isfinite(q.u1) || !std::isfinite(q.v0) ||
					!std::isfinite(q.v1) || !std::isfinite(q.height_m) ||
					!std::isfinite(q.pos[0]) || !std::isfinite(q.pos[1]) || !std::isfinite(q.pos[2]))
					continue;
				const float du = q.u1 - q.u0;
				const float dv = q.v1 - q.v0;
				if (du <= 0.0f || dv <= 0.0f || q.height_m <= 0.0f || texH <= 0.0f)
					continue;
				// Width from the requested height via the sub-rect's pixel aspect.
				const float aspect = (du * texW) / (dv * texH);
				const float height = q.height_m;
				const float width = height * aspect;
				if (!std::isfinite(width))
					continue;  // pathological sub-rect (dv near zero) blew up the aspect

				const Vector3 center = SkyrimToTracking(skyrimXf, q.pos);
				Vector3 forward = hmdPos - center;
				if (forward.LengthSquared() < 1e-6f)
					continue;
				forward.Normalize();
				Vector3 right = Vector3(0.0f, 1.0f, 0.0f).Cross(forward);
				if (right.LengthSquared() < 1e-6f)
					right = Vector3(1.0f, 0.0f, 0.0f);  // looking straight up/down
				right.Normalize();
				const Vector3 up = forward.Cross(right);

				// Rotation rows map local axes (x=right, y=up, z=forward) to world,
				// so the unit quad (XY plane, +Z normal) faces the HMD, upright and
				// un-mirrored.
				Matrix rot = Matrix::Identity;
				rot._11 = right.x;
				rot._12 = right.y;
				rot._13 = right.z;
				rot._21 = up.x;
				rot._22 = up.y;
				rot._23 = up.z;
				rot._31 = forward.x;
				rot._32 = forward.y;
				rot._33 = forward.z;

				const Matrix model = Matrix::CreateScale(width, height, 1.0f) * rot *
				                     Matrix::CreateTranslation(center);

				// Cull quads behind the camera (clip w <= 0) so we drop them from the instance
				// batch rather than relying on the GPU to clip them. The client is expected to
				// cull off-screen content too; this guards clients that don't.
				const Vector4 clip = Vector4::Transform(Vector4(center.x, center.y, center.z, 1.0f), matrices.vpWorldSpace);
				if (clip.w <= 0.0f)
					continue;

				InstanceData inst;
				inst.wvp = (model * matrices.vpWorldSpace).Transpose();
				inst.uvScale[0] = du;
				inst.uvScale[1] = dv;
				inst.uvOffset[0] = q.u0;
				inst.uvOffset[1] = q.v0;
				instances.push_back(inst);
			}

			if (instances.empty())
				continue;

			// Grow the per-instance buffer if this client needs more than the current capacity.
			if (instances.size() > g_res.instanceCapacity) {
				uint32_t newCap = g_res.instanceCapacity ? g_res.instanceCapacity : 256u;
				while (newCap < instances.size())
					newCap *= 2u;
				D3D11_BUFFER_DESC instBufDesc = {};
				instBufDesc.Usage = D3D11_USAGE_DYNAMIC;
				instBufDesc.ByteWidth = static_cast<UINT>(sizeof(InstanceData) * newCap);
				instBufDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
				instBufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				winrt::com_ptr<ID3D11Buffer> grown;
				if (FAILED(device->CreateBuffer(&instBufDesc, nullptr, grown.put())))
					continue;  // keep the old buffer; skip this client this frame
				g_res.instanceBuffer = grown;
				g_res.instanceCapacity = newCap;
			}

			// Upload this client's instances and draw them all in one instanced call.
			D3D11_MAPPED_SUBRESOURCE mapped;
			if (FAILED(ctx->Map(g_res.instanceBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
				continue;
			std::memcpy(mapped.pData, instances.data(), sizeof(InstanceData) * instances.size());
			ctx->Unmap(g_res.instanceBuffer.get(), 0);

			ID3D11Buffer* vbs[2] = { g_res.vb.get(), g_res.instanceBuffer.get() };
			UINT strides[2] = { sizeof(VT), sizeof(InstanceData) };
			UINT offsets[2] = { 0, 0 };
			ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
			ctx->PSSetShaderResources(0, 1, &srv);
			ctx->DrawIndexedInstanced(6, static_cast<UINT>(instances.size()), 0, 0, 0);
		}

		// Release the scene-depth SRV so the game can rebind kMAIN as a depth target next frame
		// without a bind conflict on our leftover t1 binding.
		ID3D11ShaderResourceView* nullSRV = nullptr;
		ctx->PSSetShaderResources(1, 1, &nullSRV);
	}

	void RenderForEye(vr::EVREye eye, ID3D11Texture2D* targetTexture,
		const vr::VRTextureBounds_t* bounds)
	{
		ZoneScopedN("InScene::RenderForEye");
		if (!targetTexture)
			return;
		HELPER_GPU_PASS(eye == vr::Eye_Left ? "ImGuiVRHelper::RenderForEye(Left)" : "ImGuiVRHelper::RenderForEye(Right)");
#if defined(IMGUI_VR_HELPER_TRACY)
		// Drain completed GPU timestamp queries. Called once per eye (twice per frame); harmless
		// to call more than strictly once per frame, just means slightly more eager collection.
		if (g_res.tracyCtx)
			TracyD3D11Collect(g_res.tracyCtx);
#endif

		auto& overlayState = Overlay::State::GetSingleton();
		const auto& s = overlayState.settings;

		// Two passes per eye:
		//
		// 1. HUD pass: every kClientFlag_HUDMode client gets composited as
		//    a full-viewport alpha-blended quad. Always rendered, no
		//    focus / attachMode gating — HUD content (subtitles, damage
		//    numbers, world-anchored 2D labels) is meant to be persistent.
		// 2. Panel pass: the focused (panel-mode) client gets rendered
		//    as a 3D quad in head/world/controller space, gated by the
		//    user's attachMode / positioningMethod settings.
		//
		// Either pass is allowed to be empty. If both are empty, we skip
		// the whole frame.
		const uint32_t focused = HelperImpl::GetSingleton().GetFocusedClientId();
		auto hudClients = HelperImpl::GetSingleton().SnapshotHUDClients();
		auto worldClients = HelperImpl::GetSingleton().SnapshotWorldQuadClients();

		// Focused panel-mode client gate (existing semantics). Held as a
		// strong ref for the rest of this function so a concurrent
		// UnregisterClient on another thread can't free it out from under
		// GetMenuSRV's CreateShaderResourceView call below. When the runtime
		// hosts the panel (RuntimeOverlay), the panel and cursor passes stand
		// down — the runtime composites the panel with the cursor baked in.
		winrt::com_ptr<ID3D11Texture2D> menuTex;
		bool wantPanelPass = false;
		if (focused != 0 && s.attachMode != Overlay::AttachMode::None &&
			!RuntimeOverlay::IsHostingPanel()) {
			menuTex = HelperImpl::GetSingleton().GetClientPanelTexture(focused);
			wantPanelPass = (menuTex != nullptr);
		}

		if (!wantPanelPass && hudClients.empty() && worldClients.empty())
			return;

		if (!InitResources())
			return;

		auto* ctx = Globals::GetD3D().context;
		if (!ctx)
			return;

		auto* rtv = GetEyeRTV(eye, targetTexture);
		if (!rtv)
			return;

		// While dragging, the OverlayTinter compute pass writes the
		// tinted panel into its post-process texture each Present.
		// Sample that instead so the drag highlight appears. Only
		// applies to the panel-mode pass.
		const bool dragging = overlayState.dragState.dragging.load(std::memory_order_relaxed) && s.enableDragToReposition;
		ID3D11ShaderResourceView* panelSRV = nullptr;
		if (wantPanelPass) {
			panelSRV = dragging ? OverlayTinter::GetOutputSRV() : GetMenuSRV(menuTex.get());
			if (!panelSRV)
				panelSRV = GetMenuSRV(menuTex.get());
		}

		// Eye view-projection now needed for BOTH passes — HUD-mode
		// renders its content as a 3D quad at HMD-relative depth so
		// per-eye stereo converges (without per-eye projection, the
		// same content at the same eye-buffer pixel coords lands at
		// different physical directions through each lens and the
		// brain can't fuse the two images). Skyrim's vanilla HUD does
		// this same thing — composites onto a virtual plane at a fixed
		// distance, per-eye projection.
		EyeMatrices matrices;
		if (wantPanelPass || !hudClients.empty() || !worldClients.empty()) {
			matrices = ComputeEyeMatrices(eye);
			if (!matrices.valid) {
				wantPanelPass = false;
				hudClients.clear();
				worldClients.clear();
			}
		}

		if (!wantPanelPass && hudClients.empty() && worldClients.empty())
			return;

		// Save current render state so we don't clobber Skyrim's submit.
		StateBackup backup;
		backup.Save(ctx);

		// Bind our RTV; no DSV (we don't read/write depth).
		ctx->OMSetRenderTargets(1, &rtv, nullptr);

		// Viewport: respect bounds if provided (SBS / texture array layouts).
		D3D11_TEXTURE2D_DESC texDesc;
		targetTexture->GetDesc(&texDesc);
		D3D11_VIEWPORT vp = {};
		if (bounds) {
			vp.TopLeftX = bounds->uMin * texDesc.Width;
			vp.TopLeftY = bounds->vMin * texDesc.Height;
			vp.Width = (bounds->uMax - bounds->uMin) * texDesc.Width;
			vp.Height = (bounds->vMax - bounds->vMin) * texDesc.Height;
		} else {
			vp.Width = static_cast<float>(texDesc.Width);
			vp.Height = static_cast<float>(texDesc.Height);
		}
		vp.MaxDepth = 1.0f;
		ctx->RSSetViewports(1, &vp);

		// World-anchored billboards first (they sit in the scene), then the
		// head-locked HUD/panel surfaces on top.
		RenderWorldQuadPass(ctx, matrices, worldClients, static_cast<float>(texDesc.Width), static_cast<float>(texDesc.Height));

		RenderHUDPass(ctx, matrices, s, hudClients);

		if (wantPanelPass) {
			RenderPanelPass(ctx, eye, matrices, s, overlayState, panelSRV);
			// The helper composites the pointer for the focused client by default;
			// a client only draws its own if it opts out (kClientFlag_OwnCursor).
			auto& helper = HelperImpl::GetSingleton();
			const uint32_t focusedId = helper.GetFocusedClientId();
			if (focusedId && !(helper.GetClientFlags(focusedId) &
								 ImGuiVRHelperPluginAPI::kClientFlag_OwnCursor)) {
				RenderCursorPass(ctx, matrices, s, overlayState);
			}
		}

		// Drawn last so the rebind capture composites on top of the focused menu.
		RenderRebindPass(ctx, matrices, s);

		backup.Restore(ctx);
	}

	winrt::com_ptr<ID3D11Texture2D> GetHostedCursorTexture(const Overlay::Settings& settings)
	{
		if (!InitResources())
			return nullptr;
		EnsureCursorColorCurrent(Globals::GetD3D().device, settings.cursorColor);
		return settings.cursorStyle == Overlay::CursorStyle::Arrow ? g_res.cursorArrowTexture : g_res.cursorTexture;
	}

	void RenderCursorIntoPanel(ID3D11Texture2D* panel)
	{
		auto& overlayState = Overlay::State::GetSingleton();
		const auto& s = overlayState.settings;
		const auto& wand = overlayState.wandState;
		if (!panel || !wand.isIntersecting)
			return;
		if (!InitResources())
			return;
		auto* ctx = Globals::GetD3D().context;
		auto* device = Globals::GetD3D().device;
		if (!ctx || !device)
			return;

		EnsureCursorColorCurrent(device, s.cursorColor);
		ID3D11ShaderResourceView* cursorSRV = s.cursorStyle == Overlay::CursorStyle::Arrow ?
		                                          g_res.cursorArrowSRV.get() :
		                                          g_res.cursorSRV.get();
		if (!cursorSRV)
			return;

		// RTV cache keyed by texture pointer (the panel is reallocated only on
		// a size change).
		static winrt::com_ptr<ID3D11RenderTargetView> s_rtv;
		static ID3D11Texture2D* s_rtvKey = nullptr;
		if (s_rtvKey != panel) {
			s_rtv = nullptr;
			if (FAILED(device->CreateRenderTargetView(panel, nullptr, s_rtv.put()))) {
				s_rtvKey = nullptr;
				return;
			}
			s_rtvKey = panel;
		}

		D3D11_TEXTURE2D_DESC desc{};
		panel->GetDesc(&desc);
		if (!desc.Width || !desc.Height)
			return;

		StateBackup backup;
		backup.Save(ctx);
		ID3D11RenderTargetView* rtv = s_rtv.get();
		ctx->OMSetRenderTargets(1, &rtv, nullptr);
		D3D11_VIEWPORT vp{};
		vp.Width = static_cast<float>(desc.Width);
		vp.Height = static_cast<float>(desc.Height);
		vp.MaxDepth = 1.0f;
		ctx->RSSetViewports(1, &vp);

		// Same on-panel footprint as the in-scene marker: a square whose side
		// is kMarkerLocalSize of the panel width, centered on the hit UV.
		constexpr float kMarkerLocalSize = 0.016f;
		const float side = kMarkerLocalSize * s.cursorSize;
		const float u = wand.uvCoordinatesX.load(std::memory_order_relaxed);
		const float v = wand.uvCoordinatesY.load(std::memory_order_relaxed);
		Matrix model = Matrix::CreateScale(
						   2.0f * side,
						   2.0f * side * static_cast<float>(desc.Width) / static_cast<float>(desc.Height),
						   1.0f) *
		               Matrix::CreateTranslation(u * 2.0f - 1.0f, 1.0f - v * 2.0f, 0.0f);
		ConstantBufferData cb;
		cb.wvp = model.Transpose();
		DrawQuad(ctx, cb, cursorSRV, g_res.panelCompositeBlendState.get());
		backup.Restore(ctx);
	}
}
