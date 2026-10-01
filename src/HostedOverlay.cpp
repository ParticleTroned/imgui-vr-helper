// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "HelperImpl.h"
#include "InSceneOverlay.h"
#include "OverlayTinter.h"
#include "RuntimeOverlay.h"
#include "internal/D3D11ResourceName.h"
#include "internal/HUDGeometry.h"
#include "internal/HostedOverlayShaders.h"
#include "internal/Profiler.h"
#include "internal/ScopedD3D11State.h"
#include "internal/VRUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <d3dcompiler.h>
#include <limits>

#if defined(_MSC_VER)
#	pragma float_control(precise, on, push)
#endif

namespace ImGuiVRHelper::InSceneOverlay
{
	namespace API = ImGuiVRHelperPluginAPI;
	using DirectX::SimpleMath::Matrix;
	using DirectX::SimpleMath::Quaternion;
	using DirectX::SimpleMath::Vector3;
	using DrawConstants = Internal::HostedOverlayShaders::DrawConstants;

	namespace
	{
		struct HostedPipeline
		{
			winrt::com_ptr<ID3D11Device> device;
			winrt::com_ptr<ID3D11VertexShader> vs;
			winrt::com_ptr<ID3D11PixelShader> ps;
			winrt::com_ptr<ID3D11Buffer> constants;
			winrt::com_ptr<ID3D11BlendState> blend;
			winrt::com_ptr<ID3D11DepthStencilState> depth;
			winrt::com_ptr<ID3D11RasterizerState> raster;
			winrt::com_ptr<ID3D11SamplerState> sampler;
		};

		std::shared_ptr<HostedPipeline> GetPipeline(ID3D11Device* device)
		{
			static std::shared_ptr<HostedPipeline> cached;
			if (cached && cached->device.get() == device)
				return cached;
			auto pipeline = std::make_shared<HostedPipeline>();
			pipeline->device.copy_from(device);
			const auto compile = [](const char* entry, const char* target, winrt::com_ptr<ID3DBlob>& blob) {
				winrt::com_ptr<ID3DBlob> errors;
				const HRESULT hr = D3DCompile(Internal::HostedOverlayShaders::source,
					sizeof(Internal::HostedOverlayShaders::source) - 1, "ImGuiVRHelper::HostedOverlay",
					nullptr, nullptr, entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, blob.put(), errors.put());
				if (FAILED(hr)) {
					logs::error("Hosted overlay {} compilation failed: {}", entry,
						errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no compiler message");
				}
				return SUCCEEDED(hr);
			};
			winrt::com_ptr<ID3DBlob> vs, ps;
			if (!compile("VSMain", "vs_5_0", vs) || !compile("PSMain", "ps_5_0", ps))
				return {};
			if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, pipeline->vs.put())))
				return {};
			Util::SetResourceName(pipeline->vs.get(), "ImGuiVRHelper::HostedVS");
			if (FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, pipeline->ps.put())))
				return {};
			Util::SetResourceName(pipeline->ps.get(), "ImGuiVRHelper::HostedPS");
			D3D11_BUFFER_DESC cb{};
			cb.ByteWidth = sizeof(DrawConstants);
			cb.Usage = D3D11_USAGE_DYNAMIC;
			cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(device->CreateBuffer(&cb, nullptr, pipeline->constants.put())))
				return {};
			Util::SetResourceName(pipeline->constants.get(), "ImGuiVRHelper::HostedConstants");
			D3D11_BLEND_DESC blend{};
			auto& target = blend.RenderTarget[0];
			target.BlendEnable = TRUE;
			target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
			target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			target.BlendOp = D3D11_BLEND_OP_ADD;
			target.SrcBlendAlpha = D3D11_BLEND_ONE;
			target.DestBlendAlpha = D3D11_BLEND_ZERO;
			target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
			target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			if (FAILED(device->CreateBlendState(&blend, pipeline->blend.put())))
				return {};
			Util::SetResourceName(pipeline->blend.get(), "ImGuiVRHelper::HostedBlend");
			D3D11_DEPTH_STENCIL_DESC depth{};
			depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
			if (FAILED(device->CreateDepthStencilState(&depth, pipeline->depth.put())))
				return {};
			Util::SetResourceName(pipeline->depth.get(), "ImGuiVRHelper::HostedDepthState");
			D3D11_RASTERIZER_DESC raster{};
			raster.FillMode = D3D11_FILL_SOLID;
			raster.CullMode = D3D11_CULL_NONE;
			raster.DepthClipEnable = TRUE;
			if (FAILED(device->CreateRasterizerState(&raster, pipeline->raster.put())))
				return {};
			Util::SetResourceName(pipeline->raster.get(), "ImGuiVRHelper::HostedRaster");
			D3D11_SAMPLER_DESC sampler{};
			sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
			sampler.MaxLOD = D3D11_FLOAT32_MAX;
			if (FAILED(device->CreateSamplerState(&sampler, pipeline->sampler.put())))
				return {};
			Util::SetResourceName(pipeline->sampler.get(), "ImGuiVRHelper::HostedSampler");
			cached = pipeline;
			return pipeline;
		}

		struct FrozenTexture
		{
			winrt::com_ptr<ID3D11Texture2D> texture;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;
			UINT width = 0, height = 0;
		};

		struct HostedDraw
		{
			Matrix model = Matrix::Identity;
			std::array<float, 4> uv{ 1.0f, 1.0f, 0.0f, 0.0f };
			std::uint32_t texture = 0;
			std::uint32_t layer = 0;
		};

		bool CopyFinite(const double* source, float* destination, std::size_t count)
		{
			for (std::size_t index = 0; index < count; ++index) {
				if (!std::isfinite(source[index]) || std::abs(source[index]) > std::numeric_limits<float>::max())
					return false;
				destination[index] = static_cast<float>(source[index]);
			}
			return true;
		}

		bool FiniteMatrix(const Matrix& matrix)
		{
			for (std::size_t row = 0; row < 4; ++row) {
				for (std::size_t column = 0; column < 4; ++column) {
					if (!std::isfinite(matrix.m[row][column]))
						return false;
				}
			}
			return true;
		}

		template <class T>
		bool InvertibleMatrix(const T (&matrix)[16])
		{
			double rows[4][4]{};
			for (std::size_t row = 0; row < 4; ++row) {
				double scale = 0;
				for (std::size_t column = 0; column < 4; ++column) {
					const double value = matrix[row * 4 + column];
					if (!std::isfinite(value))
						return false;
					scale = std::max(scale, std::abs(value));
				}
				if (scale == 0)
					return false;
				for (std::size_t column = 0; column < 4; ++column)
					rows[row][column] = matrix[row * 4 + column] / scale;
			}
			double scale = 1;
			for (std::size_t column = 0; column < 4; ++column) {
				std::size_t pivot = column;
				for (std::size_t row = column + 1; row < 4; ++row) {
					if (std::abs(rows[row][column]) > std::abs(rows[pivot][column]))
						pivot = row;
				}
				if (std::abs(rows[pivot][column]) <= 4 * std::numeric_limits<double>::epsilon() * scale)
					return false;
				for (std::size_t entry = 0; entry < 4; ++entry)
					std::swap(rows[column][entry], rows[pivot][entry]);
				for (std::size_t row = column + 1; row < 4; ++row) {
					const double factor = rows[row][column] / rows[column][column];
					for (std::size_t entry = column + 1; entry < 4; ++entry) {
						rows[row][entry] -= factor * rows[column][entry];
						if (!std::isfinite(rows[row][entry]))
							return false;
						scale = std::max(scale, std::abs(rows[row][entry]));
					}
				}
			}
			return true;
		}

		bool RectWithin(const API::RenderHostRect& rect, UINT width, UINT height)
		{
			return rect.width && rect.height && rect.x <= width && rect.y <= height &&
			       rect.width <= width - rect.x && rect.height <= height - rect.y;
		}

		bool ValidateScene(const API::HostedFrameInfo& frame)
		{
			winrt::com_ptr<ID3D11Device> device;
			frame.context->GetDevice(device.put());
			if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0 || !InvertibleMatrix(frame.headToTracking) ||
				frame.headToTracking[3] != 0 || frame.headToTracking[7] != 0 ||
				frame.headToTracking[11] != 0 || frame.headToTracking[15] != 1)
				return false;
			if (frame.worldLayerEnabled && (!InvertibleMatrix(frame.worldToTracking) ||
											   frame.worldToTracking[3] != 0 || frame.worldToTracking[7] != 0 ||
											   frame.worldToTracking[11] != 0 || frame.worldToTracking[15] != 1))
				return false;
			for (double origin : frame.worldOrigin) {
				if (!std::isfinite(origin))
					return false;
			}
			winrt::com_ptr<ID3D11Resource> depthResources[2];
			for (std::size_t eye = 0; eye < 2; ++eye) {
				const auto& scene = frame.eyes[eye];
				const auto& view = scene.viewport;
				if (!scene.outputWidth || !scene.outputHeight || scene.outputWidth > 16384 || scene.outputHeight > 16384 ||
					!std::isfinite(view.x) || !std::isfinite(view.y) || !std::isfinite(view.width) || !std::isfinite(view.height) ||
					view.x < 0 || view.y < 0 || view.width <= 0 || view.height <= 0 ||
					view.x > scene.outputWidth || view.y > scene.outputHeight ||
					view.width > scene.outputWidth - view.x || view.height > scene.outputHeight - view.y ||
					static_cast<float>(view.width) <= 0 || static_cast<float>(view.height) <= 0 ||
					scene.reserved || (scene.orientation & ~(API::RenderHostOrientation_FlipX | API::RenderHostOrientation_FlipY)) ||
					(scene.outputColorSpace != API::RenderHostColorSpace::Gamma && scene.outputColorSpace != API::RenderHostColorSpace::Linear) ||
					!InvertibleMatrix(scene.trackingToColorClip))
					return false;
				if (!frame.worldLayerEnabled)
					continue;
				if (!scene.depthSRV || !InvertibleMatrix(scene.trackingToDepthClip) ||
					!std::isfinite(scene.depthScale) || scene.depthScale <= 0 || !std::isfinite(scene.depthOffset) ||
					!RectWithin(scene.depthRect, scene.depthWidth, scene.depthHeight))
					return false;
				bool direction = false;
				for (std::size_t index = 0; index < 4; ++index) {
					if (!std::isfinite(scene.trackingToDepthMetres[index]))
						return false;
					if (index < 3 && scene.trackingToDepthMetres[index] != 0)
						direction = true;
				}
				if (!direction)
					return false;
				if (scene.depthEncoding == API::RenderHostDepthEncoding::NativeForwardZ) {
					if (scene.depthOffset <= 1)
						return false;
					const double nearMetres = scene.depthScale / scene.depthOffset;
					const double farMetres = scene.depthScale / (scene.depthOffset - 1);
					if (!std::isfinite(nearMetres) || !std::isfinite(farMetres) ||
						nearMetres <= 0 || farMetres <= nearMetres)
						return false;
				} else if (scene.depthEncoding != API::RenderHostDepthEncoding::PositiveLinear || scene.depthOffset != 0) {
					return false;
				}
				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
				scene.depthSRV->GetDesc(&srvDesc);
				if (srvDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
					srvDesc.Texture2D.MostDetailedMip != 0 || srvDesc.Texture2D.MipLevels != 1 ||
					(srvDesc.Format != DXGI_FORMAT_R32_FLOAT && srvDesc.Format != DXGI_FORMAT_R24_UNORM_X8_TYPELESS &&
						srvDesc.Format != DXGI_FORMAT_R16_UNORM && srvDesc.Format != DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS))
					return false;
				scene.depthSRV->GetResource(depthResources[eye].put());
				winrt::com_ptr<ID3D11Texture2D> texture;
				if (FAILED(depthResources[eye]->QueryInterface(IID_PPV_ARGS(texture.put()))))
					return false;
				winrt::com_ptr<ID3D11Device> depthDevice;
				texture->GetDevice(depthDevice.put());
				D3D11_TEXTURE2D_DESC desc{};
				texture->GetDesc(&desc);
				if (depthDevice.get() != device.get() || desc.SampleDesc.Count != 1 || desc.ArraySize != 1 ||
					desc.Width != scene.depthWidth || desc.Height != scene.depthHeight)
					return false;
			}
			if (depthResources[0] && depthResources[0] == depthResources[1]) {
				const auto& a = frame.eyes[0].depthRect;
				const auto& b = frame.eyes[1].depthRect;
				if (a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height)
					return false;
			}
			return true;
		}
	}

	struct HostedFrame
	{
		API::HostedFrameInfo description{};
		std::shared_ptr<HostedPipeline> pipeline;
		winrt::com_ptr<ID3D11DeviceContext> context;
		winrt::com_ptr<ID3D11ShaderResourceView> depth[2];
		DrawConstants constants[2]{};
		std::vector<FrozenTexture> textures;
		std::vector<HostedDraw> draws;
	};

	namespace
	{
		bool ValidateTarget(const HostedFrame& frame, const API::HostedEyeContext& eye)
		{
			D3D11_RENDER_TARGET_VIEW_DESC view{};
			eye.target->GetDesc(&view);
			if (view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || view.Texture2D.MipSlice != 0 ||
				(view.Format != DXGI_FORMAT_R8G8B8A8_UNORM && view.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
					view.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && view.Format != DXGI_FORMAT_R10G10B10A2_UNORM &&
					view.Format != DXGI_FORMAT_R11G11B10_FLOAT))
				return false;
			winrt::com_ptr<ID3D11Resource> resource;
			eye.target->GetResource(resource.put());
			winrt::com_ptr<ID3D11Texture2D> texture;
			if (FAILED(resource->QueryInterface(IID_PPV_ARGS(texture.put()))))
				return false;
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			winrt::com_ptr<ID3D11Device> device;
			texture->GetDevice(device.put());
			const auto& scene = frame.description.eyes[eye.eye];
			if (device.get() != frame.pipeline->device.get() || desc.SampleDesc.Count != 1 || desc.ArraySize != 1 ||
				desc.Width != scene.outputWidth || desc.Height != scene.outputHeight)
				return false;
			for (const auto& depth : frame.depth) {
				if (!depth)
					continue;
				winrt::com_ptr<ID3D11Resource> depthResource;
				depth->GetResource(depthResource.put());
				if (depthResource == resource)
					return false;
			}
			for (const auto& panel : frame.textures) {
				if (panel.texture.get() == texture.get())
					return false;
			}
			return true;
		}

		class FrameBuilder
		{
		public:
			explicit FrameBuilder(HostedFrame& frame) : frame_(frame) {}

			std::optional<std::uint32_t> Freeze(ID3D11Texture2D* source)
			{
				if (!source)
					return std::nullopt;
				if (auto found = copied_.find(source); found != copied_.end())
					return found->second;
				D3D11_TEXTURE2D_DESC desc{};
				source->GetDesc(&desc);
				winrt::com_ptr<ID3D11Device> device;
				source->GetDevice(device.put());
				if (device.get() != frame_.pipeline->device.get() || desc.SampleDesc.Count != 1 ||
					desc.ArraySize != 1 || desc.MipLevels != 1 || !desc.Width || !desc.Height ||
					desc.Width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || desc.Height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
					desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
					return std::nullopt;
				constexpr std::uint64_t maxSnapshotBytes = 256ull * 1024 * 1024;
				const std::uint64_t bytes = static_cast<std::uint64_t>(desc.Width) * desc.Height * 4;
				if (bytes > maxSnapshotBytes - snapshotBytes_)
					return std::nullopt;
				FrozenTexture frozen;
				frozen.width = desc.Width;
				frozen.height = desc.Height;
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				desc.CPUAccessFlags = 0;
				desc.MiscFlags = 0;
				if (FAILED(device->CreateTexture2D(&desc, nullptr, frozen.texture.put())))
					return std::nullopt;
				Util::SetResourceName(frozen.texture.get(), "ImGuiVRHelper::HostedPanelSnapshot");
				if (FAILED(device->CreateShaderResourceView(frozen.texture.get(), nullptr, frozen.srv.put())))
					return std::nullopt;
				Util::SetResourceName(frozen.srv.get(), "ImGuiVRHelper::HostedPanelSnapshot SRV");
				frame_.context->CopyResource(frozen.texture.get(), source);
				const auto index = static_cast<std::uint32_t>(frame_.textures.size());
				frame_.textures.push_back(std::move(frozen));
				copied_.emplace(source, index);
				snapshotBytes_ += bytes;
				return index;
			}

			bool Add(ID3D11Texture2D* texture, const Matrix& model, std::uint32_t layer,
				std::array<float, 4> uv = { 1, 1, 0, 0 })
			{
				if (!texture || !FiniteMatrix(model))
					return false;
				PendingDraw pending;
				pending.source.copy_from(texture);
				pending.draw = { model, uv, 0, layer };
				pending_.push_back(std::move(pending));
				return true;
			}

			bool Empty() const { return pending_.empty(); }

			bool FreezePixels()
			{
				frame_.draws.reserve(pending_.size());
				for (auto& pending : pending_) {
					const auto frozen = Freeze(pending.source.get());
					if (!frozen)
						return false;
					pending.draw.texture = *frozen;
					frame_.draws.push_back(pending.draw);
				}
				return true;
			}

		private:
			struct PendingDraw
			{
				winrt::com_ptr<ID3D11Texture2D> source;
				HostedDraw draw;
			};
			HostedFrame& frame_;
			std::vector<PendingDraw> pending_;
			std::uint64_t snapshotBytes_ = 0;
			std::unordered_map<ID3D11Texture2D*, std::uint32_t> copied_;
		};

		bool PrepareEyeConstants(HostedFrame& frame)
		{
			for (std::size_t eye = 0; eye < 2; ++eye) {
				const auto& scene = frame.description.eyes[eye];
				auto& constants = frame.constants[eye];
				if (!CopyFinite(scene.trackingToColorClip, constants.trackingToColorClip, 16) ||
					!InvertibleMatrix(constants.trackingToColorClip))
					return false;
				if (frame.description.worldLayerEnabled) {
					if (!CopyFinite(scene.trackingToDepthClip, constants.trackingToDepthClip, 16) ||
						!InvertibleMatrix(constants.trackingToDepthClip) ||
						!CopyFinite(scene.trackingToDepthMetres, constants.trackingToDepthMetres, 4) ||
						(constants.trackingToDepthMetres[0] == 0 && constants.trackingToDepthMetres[1] == 0 &&
							constants.trackingToDepthMetres[2] == 0) ||
						!CopyFinite(&scene.depthScale, constants.depthDecode, 1) ||
						!CopyFinite(&scene.depthOffset, constants.depthDecode + 1, 1) ||
						constants.depthDecode[0] <= 0 ||
						(scene.depthEncoding == API::RenderHostDepthEncoding::NativeForwardZ && constants.depthDecode[1] <= 1))
						return false;
					constants.depthDecode[2] = 0.02f;
					constants.depthDecode[3] = scene.depthEncoding == API::RenderHostDepthEncoding::PositiveLinear ? 1.0f : 0.0f;
					constants.depthRect[0] = static_cast<float>(scene.depthRect.x);
					constants.depthRect[1] = static_cast<float>(scene.depthRect.y);
					constants.depthRect[2] = static_cast<float>(scene.depthRect.width);
					constants.depthRect[3] = static_cast<float>(scene.depthRect.height);
					frame.depth[eye].copy_from(scene.depthSRV);
				}
				constants.options[1] = (scene.orientation & API::RenderHostOrientation_FlipX) != 0 ? 1.0f : 0.0f;
				constants.options[2] = (scene.orientation & API::RenderHostOrientation_FlipY) != 0 ? 1.0f : 0.0f;
				constants.options[3] = scene.outputColorSpace == API::RenderHostColorSpace::Linear ? 1.0f : 0.0f;
			}
			return true;
		}

		bool AddWorldContent(FrameBuilder& builder, const API::HostedFrameInfo& frame,
			const HelperImpl::HostedClientSnapshot& clients, const Matrix& head)
		{
			if (!frame.worldLayerEnabled)
				return true;
			const Vector3 hmd = head.Translation();
			for (const auto& client : clients.worldClients) {
				D3D11_TEXTURE2D_DESC desc{};
				client.texture->GetDesc(&desc);
				for (const auto& quad : client.quads) {
					if (!std::isfinite(quad.u0) || !std::isfinite(quad.v0) || !std::isfinite(quad.u1) ||
						!std::isfinite(quad.v1) || !std::isfinite(quad.height_m) ||
						quad.u0 < 0 || quad.v0 < 0 || quad.u1 > 1 || quad.v1 > 1 ||
						quad.u1 <= quad.u0 || quad.v1 <= quad.v0 || quad.height_m <= 0 || !desc.Height)
						continue;
					const double relative[3]{ quad.pos[0] - frame.worldOrigin[0],
						quad.pos[1] - frame.worldOrigin[1], quad.pos[2] - frame.worldOrigin[2] };
					double tracking[3]{};
					for (std::size_t axis = 0; axis < 3; ++axis) {
						tracking[axis] = relative[0] * frame.worldToTracking[axis] +
						                 relative[1] * frame.worldToTracking[4 + axis] +
						                 relative[2] * frame.worldToTracking[8 + axis] + frame.worldToTracking[12 + axis];
					}
					Vector3 center;
					if (!CopyFinite(tracking, &center.x, 3))
						continue;
					const float du = quad.u1 - quad.u0, dv = quad.v1 - quad.v0;
					const double width = static_cast<double>(quad.height_m) * du * desc.Width / (dv * desc.Height);
					if (!std::isfinite(width) || width <= 0 || width > std::numeric_limits<float>::max())
						continue;
					Vector3 forward = hmd - center;
					if (!std::isfinite(forward.LengthSquared()) || forward.LengthSquared() < 1e-6f)
						continue;
					forward.Normalize();
					Vector3 right = Vector3::UnitY.Cross(forward);
					if (right.LengthSquared() < 1e-6f)
						right = Vector3::UnitX;
					right.Normalize();
					const Vector3 up = forward.Cross(right);
					Matrix rotation = Matrix::Identity;
					rotation._11 = right.x;
					rotation._12 = right.y;
					rotation._13 = right.z;
					rotation._21 = up.x;
					rotation._22 = up.y;
					rotation._23 = up.z;
					rotation._31 = forward.x;
					rotation._32 = forward.y;
					rotation._33 = forward.z;
					const Matrix model = Matrix::CreateScale(static_cast<float>(width), quad.height_m, 1) * rotation * Matrix::CreateTranslation(center);
					if (!builder.Add(client.texture.get(), model, API::RenderHostLayer_World, { du, dv, quad.u0, quad.v0 }))
						return false;
				}
			}
			return true;
		}

		std::optional<Matrix> ControllerAnchor(const HelperImpl::HostedClientSnapshot& clients,
			const Overlay::Settings& settings, bool leftHanded)
		{
			if (!clients.inputFrame || (settings.attachController != API::InputDeviceType::Primary &&
										   settings.attachController != API::InputDeviceType::Secondary))
				return std::nullopt;
			const bool left = settings.attachController == API::InputDeviceType::Primary ? leftHanded : !leftHanded;
			const auto& hand = left ? clients.inputFrame->left : clients.inputFrame->right;
			if (!hand.connected || !(hand.pose.valid & 1))
				return std::nullopt;
			const auto& pose = hand.pose;
			Quaternion rotation(pose.orient[1], pose.orient[2], pose.orient[3], pose.orient[0]);
			if (!std::isfinite(rotation.LengthSquared()) || rotation.LengthSquared() < 1e-6f)
				return std::nullopt;
			rotation.Normalize();
			const Matrix model = Matrix::CreateTranslation(settings.controllerOffsetX, settings.controllerOffsetY, settings.controllerOffsetZ) *
			                     Matrix::CreateFromQuaternion(rotation) * Matrix::CreateTranslation(pose.pos[0], pose.pos[1], pose.pos[2]);
			if (!FiniteMatrix(model))
				return std::nullopt;
			return model;
		}

		bool AddPanelContent(FrameBuilder& builder, const HelperImpl::HostedClientSnapshot& clients,
			const Overlay::Settings& settings, const Overlay::State& state, const Matrix& head, bool runtimePanel)
		{
			if (!clients.panelTexture || settings.attachMode == Overlay::AttachMode::None || runtimePanel)
				return true;
			winrt::com_ptr<ID3D11Texture2D> panel = clients.panelTexture;
			if (state.dragState.dragging.load(std::memory_order_relaxed) && settings.enableDragToReposition) {
				if (auto* srv = OverlayTinter::GetOutputSRV()) {
					winrt::com_ptr<ID3D11Resource> resource;
					srv->GetResource(resource.put());
					winrt::com_ptr<ID3D11Texture2D> tinted;
					if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(tinted.put()))))
						panel = std::move(tinted);
				}
			}
			std::optional<Matrix> anchors[2];
			if (settings.attachMode == Overlay::AttachMode::HMDOnly || settings.attachMode == Overlay::AttachMode::Both) {
				anchors[0] = settings.positioningMethod == Overlay::PositioningMethod::FixedWorld ? state.fixedWorld.m :
				                                                                                    Matrix::CreateTranslation(settings.hmdOffsetX, settings.hmdOffsetY, settings.hmdOffsetZ) * head;
			}
			if (settings.attachMode == Overlay::AttachMode::ControllerOnly || settings.attachMode == Overlay::AttachMode::Both) {
				anchors[1] = ControllerAnchor(clients, settings, state.lastKnownLeftHandedMode);
				if (anchors[1]) {
					const auto& anchor = *anchors[1];
					const Vector3 normal(anchor._31, anchor._32, anchor._33);
					if (normal.Dot(head.Translation() - anchor.Translation()) <= 0)
						anchors[1].reset();
				}
			}
			for (const auto& anchor : anchors) {
				if (anchor && !builder.Add(panel.get(), Overlay::Config::CreateScaleMatrix(settings.menuScale) * *anchor, API::RenderHostLayer_Panel))
					return false;
			}
			const auto& wand = state.wandState;
			if (!wand.isIntersecting.load(std::memory_order_relaxed) || (clients.focusedFlags & API::kClientFlag_OwnCursor) != 0 ||
				!Overlay::IsAttachModeCompatible(wand.matchedOverlayType, settings.attachMode))
				return true;
			const auto& anchor = anchors[wand.matchedOverlayType == Overlay::OverlayType::HMD ? 0 : 1];
			if (!anchor)
				return true;
			const float size = 0.016f * settings.cursorSize;
			const Matrix marker = Matrix::CreateScale(size, size / Overlay::Config::kOverlayAspect, 1) *
			                      Matrix::CreateTranslation(wand.uvCoordinatesX.load(std::memory_order_relaxed) - 0.5f,
									  0.5f - wand.uvCoordinatesY.load(std::memory_order_relaxed), 0) *
			                      Overlay::Config::CreateScaleMatrix(settings.menuScale) * *anchor;
			const auto cursor = GetHostedCursorTexture(settings);
			return cursor && builder.Add(cursor.get(), marker, API::RenderHostLayer_Panel);
		}

		std::optional<Matrix> HUDModel(const Overlay::Settings& settings, const Matrix& head)
		{
			float left[4]{}, right[4]{};
			if (!Util::CachedProjectionRaw(vr::Eye_Left, left[0], left[1], left[2], left[3]) ||
				!Util::CachedProjectionRaw(vr::Eye_Right, right[0], right[1], right[2], right[3]))
				return std::nullopt;
			const float depth = std::max(0.3f, settings.hudDepth);
			const auto quad = ComputeHUDQuad(left, right, depth,
				std::clamp(settings.hudCoverage, Overlay::Config::kMinHUDCoverage, Overlay::Config::kMaxHUDCoverage));
			const Matrix model = Matrix::CreateScale(quad.width, quad.height, 1) *
			                     Matrix::CreateTranslation(0, quad.centerY, -depth) * head;
			if (!FiniteMatrix(model))
				return std::nullopt;
			return model;
		}
	}

	API::RenderHostContent QueryHostedContent()
	{
		API::RenderHostContent content;
		HelperImpl::GetSingleton().QueryHostedContent(content.layers, content.worldQuadCount);
		if (Overlay::State::GetSingleton().settings.attachMode == Overlay::AttachMode::None || RuntimeOverlay::IsHostingPanel())
			content.layers &= ~API::RenderHostLayer_Panel;
		return content;
	}

	std::shared_ptr<HostedFrame> PrepareHostedFrame(const API::HostedFrameInfo& description,
		API::RenderHostContent& content, API::RenderHostResult& result)
	{
		ZoneScopedN("InScene::PrepareHostedFrame");
		content = {};
		result = API::RenderHostResult::RenderFailure;
		if (!description.context || description.context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
			return {};
		if (!ValidateScene(description)) {
			result = API::RenderHostResult::InvalidArgument;
			return {};
		}
		const auto clients = HelperImpl::GetSingleton().SnapshotHostedClients(description.worldLayerEnabled != 0);
		if (!clients)
			return {};
		const auto& state = Overlay::State::GetSingleton();
		const auto settings = state.settings;
		const bool runtimePanel = RuntimeOverlay::IsHostingPanel();
		const bool panelCandidate = clients->panelTexture && settings.attachMode != Overlay::AttachMode::None && !runtimePanel;
		if ((!description.worldLayerEnabled || clients->worldClients.empty()) &&
			clients->hudClients.empty() && !clients->rebindTexture && !panelCandidate) {
			result = API::RenderHostResult::NoContent;
			return {};
		}
		auto frame = std::make_shared<HostedFrame>();
		frame->description = description;
		frame->context.copy_from(description.context);
		if (!PrepareEyeConstants(*frame)) {
			result = API::RenderHostResult::Unsupported;
			return {};
		}
		float headValues[16];
		if (!CopyFinite(description.headToTracking, headValues, 16) || !InvertibleMatrix(headValues)) {
			result = API::RenderHostResult::InvalidArgument;
			return {};
		}
		Matrix head;
		std::memcpy(&head._11, headValues, sizeof(headValues));
		FrameBuilder builder(*frame);
		if (!AddWorldContent(builder, description, *clients, head))
			return {};
		std::optional<Matrix> hud;
		if (!clients->hudClients.empty() || clients->rebindTexture)
			hud = HUDModel(settings, head);
		if ((!clients->hudClients.empty() || clients->rebindTexture) && !hud)
			return {};
		for (const auto& client : clients->hudClients) {
			if (!builder.Add(client.texture.get(), *hud, API::RenderHostLayer_HUD))
				return {};
		}
		if (!AddPanelContent(builder, *clients, settings, state, head, runtimePanel))
			return {};
		if (clients->rebindTexture && !builder.Add(clients->rebindTexture.get(), *hud, API::RenderHostLayer_Modal))
			return {};
		if (builder.Empty()) {
			result = API::RenderHostResult::NoContent;
			return {};
		}
		winrt::com_ptr<ID3D11Device> device;
		description.context->GetDevice(device.put());
		frame->pipeline = GetPipeline(device.get());
		if (!frame->pipeline)
			return {};
		Internal::ScopedD3D11State isolated(description.context);
		if (!builder.FreezePixels())
			return {};
		if (FAILED(device->GetDeviceRemovedReason()))
			return {};
		for (const auto& draw : frame->draws) {
			content.layers |= draw.layer;
			if (draw.layer == API::RenderHostLayer_World)
				++content.worldQuadCount;
		}
		result = frame->draws.empty() ? API::RenderHostResult::NoContent : API::RenderHostResult::Success;
		return frame;
	}

	API::RenderHostResult RenderHostedEye(const HostedFrame& frame, const API::HostedEyeContext& eye, API::HostedEyeResult& result)
	{
		ZoneScopedN("InScene::RenderHostedEye");
		result = {};
		if (frame.draws.empty())
			return API::RenderHostResult::NoContent;
		if (eye.eye > 1 || !eye.target || eye.pairToken != frame.description.pairToken ||
			eye.resourceGeneration != frame.description.resourceGeneration)
			return API::RenderHostResult::InvalidArgument;
		if (!ValidateTarget(frame, eye))
			return API::RenderHostResult::InvalidArgument;
		auto* context = frame.context.get();
		Internal::ScopedD3D11State isolated(context);
		const auto& pipeline = *frame.pipeline;
		const auto& viewport = frame.description.eyes[eye.eye].viewport;
		const D3D11_VIEWPORT d3dViewport{ static_cast<float>(viewport.x), static_cast<float>(viewport.y),
			static_cast<float>(viewport.width), static_cast<float>(viewport.height), 0, 1 };
		context->OMSetRenderTargets(1, &eye.target, nullptr);
		context->RSSetViewports(1, &d3dViewport);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(pipeline.vs.get(), nullptr, 0);
		context->PSSetShader(pipeline.ps.get(), nullptr, 0);
		context->OMSetBlendState(pipeline.blend.get(), nullptr, 0xFFFFFFFF);
		context->OMSetDepthStencilState(pipeline.depth.get(), 0);
		context->RSSetState(pipeline.raster.get());
		auto* buffer = pipeline.constants.get();
		auto* sampler = pipeline.sampler.get();
		context->VSSetConstantBuffers(0, 1, &buffer);
		context->PSSetConstantBuffers(0, 1, &buffer);
		context->PSSetSamplers(0, 1, &sampler);
		for (const auto& draw : frame.draws) {
			auto constants = frame.constants[eye.eye];
			std::memcpy(constants.model, &draw.model._11, sizeof(constants.model));
			std::memcpy(constants.uvTransform, draw.uv.data(), sizeof(constants.uvTransform));
			constants.options[0] = draw.layer == API::RenderHostLayer_World &&
			                               (frame.description.diagnostics & API::RenderHostDiagnostic_DisableWorldDepthTest) == 0 ?
			                           1.0f :
			                           0.0f;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
				return result.targetWritten ? API::RenderHostResult::PartialWrite : API::RenderHostResult::RenderFailure;
			std::memcpy(mapped.pData, &constants, sizeof(constants));
			context->Unmap(buffer, 0);
			ID3D11ShaderResourceView* resources[]{ frame.textures[draw.texture].srv.get(), frame.depth[eye.eye].get() };
			context->PSSetShaderResources(0, 2, resources);
			result.targetWritten = 1;
			context->Draw(6, 0);
			result.drawnLayers |= draw.layer;
		}
		return FAILED(pipeline.device->GetDeviceRemovedReason()) ? API::RenderHostResult::PartialWrite : API::RenderHostResult::Success;
	}
}

#if defined(_MSC_VER)
#	pragma float_control(pop)
#endif
