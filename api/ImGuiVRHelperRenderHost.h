// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ImGuiVRHelper contributors. See api/COPYING.LESSER.

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11ShaderResourceView;

namespace ImGuiVRHelperPluginAPI
{
	inline constexpr std::uint32_t kRenderHostContractVersion = 1;

	enum class RenderHostResult : std::uint32_t
	{
		Success,
		NoContent,
		NotReady,
		Unsupported,
		InvalidArgument,
		InvalidToken,
		WrongThread,
		Busy,
		Inactive,
		StaleFrame,
		DuplicateAttempt,
		IncompletePair,
		RenderFailure,
		PartialWrite,
	};

	enum class RenderHostKind : std::uint32_t
	{
		CSX = 0x43535831
	};
	enum class RenderHostDepthEncoding : std::uint32_t
	{
		NativeForwardZ,
		PositiveLinear
	};
	enum class RenderHostColorSpace : std::uint32_t
	{
		Gamma,
		Linear
	};
	enum RenderHostLayer : std::uint32_t
	{
		RenderHostLayer_World = 1u << 0,
		RenderHostLayer_HUD = 1u << 1,
		RenderHostLayer_Panel = 1u << 2,
		RenderHostLayer_Modal = 1u << 3,
	};
	enum RenderHostOrientation : std::uint32_t
	{
		RenderHostOrientation_FlipX = 1u << 0,
		RenderHostOrientation_FlipY = 1u << 1,
	};
	enum RenderHostDiagnostic : std::uint32_t
	{
		/// Development-only scene diagnosis; this must not become a persisted occlusion workaround.
		RenderHostDiagnostic_DisableWorldDepthTest = 1u << 0,
	};

	struct RenderHostCapabilities
	{
		std::uint32_t structSize = sizeof(RenderHostCapabilities);
		std::uint32_t contractVersion = kRenderHostContractVersion;
		std::uint32_t supportedLayers = 0;
		std::uint32_t maxAttemptsPerEye = 0;
	};

	struct RenderHostRegistration
	{
		std::uint32_t structSize = sizeof(RenderHostRegistration);
		std::uint32_t contractVersion = kRenderHostContractVersion;
		RenderHostKind kind = RenderHostKind::CSX;
		std::uint32_t reserved = 0;
	};

	/// Candidate counts do not imply nontransparent pixels or invoke client callbacks.
	struct RenderHostContent
	{
		std::uint32_t structSize = sizeof(RenderHostContent);
		std::uint32_t contractVersion = kRenderHostContractVersion;
		std::uint32_t layers = 0;
		std::uint32_t worldQuadCount = 0;
	};

	struct RenderHostRect
	{
		std::uint32_t x = 0, y = 0, width = 0, height = 0;
	};

	struct RenderHostViewport
	{
		double x = 0, y = 0, width = 0, height = 0;
	};

	/// Matrices are row-major doubles with row-vector multiplication and D3D clip depth [0,1].
	/// The depth SRV and its contents remain stable from Begin through End/Abort.
	struct HostedEyeScene
	{
		std::uint32_t outputWidth = 0, outputHeight = 0;
		RenderHostViewport viewport{};
		std::uint32_t orientation = 0;
		/// Host supplies a non-sRGB RTV; the shader converts gamma panel RGB for Linear output.
		RenderHostColorSpace outputColorSpace = RenderHostColorSpace::Gamma;
		RenderHostDepthEncoding depthEncoding = RenderHostDepthEncoding::NativeForwardZ;
		std::uint32_t reserved = 0;
		ID3D11ShaderResourceView* depthSRV = nullptr;
		std::uint32_t depthWidth = 0, depthHeight = 0;
		RenderHostRect depthRect{};
		/// Native metres = scale/(offset-sample); linear metres = scale*sample (offset must be zero).
		double depthScale = 0, depthOffset = 0;
		double trackingToColorClip[16]{};
		double trackingToDepthClip[16]{};
		double trackingToDepthMetres[4]{};
	};

	/// Begin is render-thread-only and freezes a complete stereo pair without client callbacks.
	/// Context/SRVs are borrowed until End/Abort; targets are borrowed only during each eye call.
	struct HostedFrameInfo
	{
		std::uint32_t structSize = sizeof(HostedFrameInfo);
		std::uint32_t contractVersion = kRenderHostContractVersion;
		/// pairToken increases for every Begin attempt during one registration, including after reactivation.
		std::uint64_t pairToken = 0, sceneFrame = 0, resourceGeneration = 0, compositorCycle = 0;
		ID3D11DeviceContext* context = nullptr;
		std::uint32_t worldLayerEnabled = 0;
		std::uint32_t diagnostics = 0;
		/// Subtract worldOrigin from client positions before worldToTracking to preserve precision.
		double worldOrigin[3]{};
		double worldToTracking[16]{};
		double headToTracking[16]{};
		HostedEyeScene eyes[2]{};
	};

	struct HostedFrameHandle
	{
		std::uint32_t structSize = sizeof(HostedFrameHandle);
		std::uint32_t contractVersion = kRenderHostContractVersion;
		std::uint64_t cookie = 0;
		std::uint32_t layers = 0;
		std::uint32_t worldQuadCount = 0;
	};

	/// Each attempt names a fresh writable single-sample 2D eye surface; retries must use a new surface.
	struct HostedEyeContext
	{
		std::uint32_t structSize = sizeof(HostedEyeContext);
		std::uint32_t contractVersion = kRenderHostContractVersion;
		std::uint32_t eye = 0;
		std::uint32_t reserved = 0;
		std::uint64_t pairToken = 0, resourceGeneration = 0, attemptId = 0;
		ID3D11RenderTargetView* target = nullptr;
	};

	struct HostedEyeResult
	{
		std::uint32_t structSize = sizeof(HostedEyeResult);
		std::uint32_t contractVersion = kRenderHostContractVersion;
		std::uint32_t drawnLayers = 0;
		std::uint32_t skippedLayers = 0;
		/// Nonzero means the target cannot be reused as a clean fallback, even on failure.
		std::uint32_t targetWritten = 0;
		std::uint32_t reserved = 0;
	};

	static_assert(sizeof(void*) == 8, "The renderer-host ABI targets Windows x64");
	static_assert(std::is_standard_layout_v<HostedFrameInfo> && std::is_trivially_copyable_v<HostedFrameInfo>);
	static_assert(std::is_standard_layout_v<HostedEyeContext> && std::is_trivially_copyable_v<HostedEyeContext>);
	static_assert(sizeof(RenderHostRegistration) == 16 && sizeof(RenderHostCapabilities) == 16);
	static_assert(sizeof(RenderHostContent) == 16 && sizeof(HostedFrameHandle) == 24);
	static_assert(sizeof(HostedEyeContext) == 48 && sizeof(HostedEyeResult) == 24);
	static_assert(sizeof(HostedEyeScene) == 392 && sizeof(HostedFrameInfo) == 1120);
	static_assert(offsetof(HostedFrameInfo, context) == 40 && offsetof(HostedEyeContext, target) == 40);
	static_assert(offsetof(HostedFrameInfo, worldToTracking) == 80 && offsetof(HostedFrameInfo, eyes) == 336);
	static_assert(offsetof(HostedEyeScene, depthSRV) == 56 && offsetof(HostedEyeScene, trackingToColorClip) == 104);
}
