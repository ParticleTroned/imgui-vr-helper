// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "internal/D3D11ResourceName.h"
#include "internal/HostedOverlayShaders.h"

namespace
{
	using Microsoft::WRL::ComPtr;
	namespace Shader = ImGuiVRHelper::Internal::HostedOverlayShaders;
	using ImGuiVRHelper::Util::SetResourceName;
	constexpr UINT kWidth = 64, kHeight = 32, kDepthWidth = 128, kDepthHeight = 64;
	struct Pixel
	{
		std::uint8_t red, green, blue, alpha;
		bool operator==(const Pixel&) const = default;
	};
	using Image = std::vector<Pixel>;

	void Require(bool condition, const char* message)
	{
		if (!condition)
			throw std::runtime_error(message);
	}

	void Check(HRESULT result, const char* operation)
	{
		if (FAILED(result))
			throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(static_cast<unsigned long>(result)));
	}

	void Identity(float (&matrix)[16])
	{
		std::fill(std::begin(matrix), std::end(matrix), 0.0f);
		matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
	}

	Shader::DrawConstants Constants(UINT eye, bool native)
	{
		Shader::DrawConstants constants;
		Identity(constants.model);
		constants.model[0] = 0.5f;
		constants.model[5] = 0.5f;
		constants.model[12] = -0.375f;
		constants.model[13] = 0.25f;
		constants.model[14] = 2.0f;
		Identity(constants.trackingToColorClip);
		constants.trackingToColorClip[10] = 0.25f;
		std::copy(std::begin(constants.trackingToColorClip), std::end(constants.trackingToColorClip), constants.trackingToDepthClip);
		constants.trackingToDepthMetres[2] = 1.0f;
		constants.depthRect[0] = static_cast<float>(eye * 64);
		constants.depthRect[2] = 64;
		constants.depthRect[3] = 32;
		constants.depthDecode[0] = native ? 10.0f / 9.0f : 1.0f;
		constants.depthDecode[1] = native ? 10.0f / 9.0f : 0.0f;
		constants.depthDecode[2] = 0.01f;
		constants.depthDecode[3] = native ? 0.0f : 1.0f;
		constants.options[0] = 1;
		return constants;
	}

	float Encode(float metres, bool native)
	{
		return native ? (10.0f - 10.0f / metres) / 9.0f : metres;
	}

	std::size_t OpaquePixels(const Image& image)
	{
		return static_cast<std::size_t>(std::count_if(image.begin(), image.end(), [](Pixel pixel) { return pixel.alpha > 127; }));
	}

	struct Fixture
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> context;
		ComPtr<ID3D11InfoQueue> debug;
		ComPtr<ID3D11VertexShader> vertex;
		ComPtr<ID3D11PixelShader> pixel;
		ComPtr<ID3D11Buffer> constants;
		ComPtr<ID3D11SamplerState> sampler;
		ComPtr<ID3D11RasterizerState> raster;
		ComPtr<ID3D11DepthStencilState> depthState;
		ComPtr<ID3D11Texture2D> target, readback, depthTexture, panelTexture;
		ComPtr<ID3D11RenderTargetView> targetView;
		ComPtr<ID3D11ShaderResourceView> depthView, panelView;

		Fixture()
		{
			const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
			auto create = [&](UINT flags) {
				return D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, &level, 1,
					D3D11_SDK_VERSION, &device, nullptr, &context);
			};
			HRESULT result = create(D3D11_CREATE_DEVICE_DEBUG);
			if (result == DXGI_ERROR_SDK_COMPONENT_MISSING) {
				std::cout << "SKIP debug-layer diagnostics: Windows Graphics Tools unavailable\n";
				device.Reset();
				context.Reset();
				result = create(0);
			}
			Check(result, "D3D11CreateDevice(WARP)");
			(void)device.As(&debug);
			const auto compile = [](const char* entry, const char* profile) {
				ComPtr<ID3DBlob> code, errors;
				const auto compiled = D3DCompile(Shader::source, std::strlen(Shader::source), "HostedOverlayShaders.h",
					nullptr, nullptr, entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
				if (FAILED(compiled) && errors)
					throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
				Check(compiled, "D3DCompile(hosted shader)");
				return code;
			};
			const auto vs = compile("VSMain", "vs_5_0"), ps = compile("PSMain", "ps_5_0");
			Check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex), "CreateVertexShader");
			Check(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel), "CreatePixelShader");
			SetResourceName(vertex.Get(), "HostedFixture::Vertex");
			SetResourceName(pixel.Get(), "HostedFixture::Pixel");
			D3D11_BUFFER_DESC buffer{};
			buffer.ByteWidth = sizeof(Shader::DrawConstants);
			buffer.Usage = D3D11_USAGE_DEFAULT;
			buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			Check(device->CreateBuffer(&buffer, nullptr, &constants), "CreateBuffer");
			SetResourceName(constants.Get(), "HostedFixture::Constants");
			D3D11_SAMPLER_DESC sampling{};
			sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
			sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampling.MaxLOD = D3D11_FLOAT32_MAX;
			Check(device->CreateSamplerState(&sampling, &sampler), "CreateSamplerState");
			SetResourceName(sampler.Get(), "HostedFixture::Sampler");
			D3D11_RASTERIZER_DESC rasterizer{};
			rasterizer.FillMode = D3D11_FILL_SOLID;
			rasterizer.CullMode = D3D11_CULL_NONE;
			rasterizer.DepthClipEnable = TRUE;
			Check(device->CreateRasterizerState(&rasterizer, &raster), "CreateRasterizerState");
			SetResourceName(raster.Get(), "HostedFixture::Raster");
			D3D11_DEPTH_STENCIL_DESC depthDescription{};
			Check(device->CreateDepthStencilState(&depthDescription, &depthState), "CreateDepthStencilState");
			SetResourceName(depthState.Get(), "HostedFixture::DepthState");
			CreateTextures();
		}

		~Fixture()
		{
			context->ClearState();
			context->Flush();
		}

		void CreateTextures()
		{
			D3D11_TEXTURE2D_DESC texture{};
			texture.Width = kWidth;
			texture.Height = kHeight;
			texture.MipLevels = texture.ArraySize = texture.SampleDesc.Count = 1;
			texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			texture.Usage = D3D11_USAGE_DEFAULT;
			texture.BindFlags = D3D11_BIND_RENDER_TARGET;
			Check(device->CreateTexture2D(&texture, nullptr, &target), "CreateTexture2D(target)");
			Check(device->CreateRenderTargetView(target.Get(), nullptr, &targetView), "CreateRenderTargetView");
			SetResourceName(target.Get(), "HostedFixture::Target");
			SetResourceName(targetView.Get(), "HostedFixture::Target RTV");
			texture.Usage = D3D11_USAGE_STAGING;
			texture.BindFlags = 0;
			texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			Check(device->CreateTexture2D(&texture, nullptr, &readback), "CreateTexture2D(readback)");
			SetResourceName(readback.Get(), "HostedFixture::Readback");
			texture.Width = kDepthWidth;
			texture.Height = kDepthHeight;
			texture.Format = DXGI_FORMAT_R32_FLOAT;
			texture.Usage = D3D11_USAGE_DEFAULT;
			texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			texture.CPUAccessFlags = 0;
			Check(device->CreateTexture2D(&texture, nullptr, &depthTexture), "CreateTexture2D(depth)");
			Check(device->CreateShaderResourceView(depthTexture.Get(), nullptr, &depthView), "CreateShaderResourceView(depth)");
			SetResourceName(depthTexture.Get(), "HostedFixture::Depth");
			SetResourceName(depthView.Get(), "HostedFixture::Depth SRV");
			texture.Width = texture.Height = 8;
			texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			std::array<Pixel, 64> panel{};
			for (UINT y = 0; y < 8; ++y) {
				for (UINT x = 0; x < 8; ++x)
					panel[y * 8 + x] = { 128, 64, 32, static_cast<std::uint8_t>(x == 1 || x == 6 || y == 3 || y == 4 ? 255 : 0) };
			}
			const D3D11_SUBRESOURCE_DATA data{ panel.data(), 8 * sizeof(Pixel), 0 };
			Check(device->CreateTexture2D(&texture, &data, &panelTexture), "CreateTexture2D(panel)");
			Check(device->CreateShaderResourceView(panelTexture.Get(), nullptr, &panelView), "CreateShaderResourceView(panel)");
			SetResourceName(panelTexture.Get(), "HostedFixture::Panel");
			SetResourceName(panelView.Get(), "HostedFixture::Panel SRV");
		}

		Image Draw(const Shader::DrawConstants& data, const std::vector<float>& depth)
		{
			context->ClearState();
			context->UpdateSubresource(depthTexture.Get(), 0, nullptr, depth.data(), kDepthWidth * sizeof(float), 0);
			context->UpdateSubresource(constants.Get(), 0, nullptr, &data, 0, 0);
			ID3D11RenderTargetView* output = targetView.Get();
			context->OMSetRenderTargets(1, &output, nullptr);
			constexpr FLOAT clear[4]{};
			context->ClearRenderTargetView(output, clear);
			context->OMSetDepthStencilState(depthState.Get(), 0);
			context->RSSetState(raster.Get());
			const D3D11_VIEWPORT viewport{ 0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0, 1 };
			context->RSSetViewports(1, &viewport);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->VSSetShader(vertex.Get(), nullptr, 0);
			context->PSSetShader(pixel.Get(), nullptr, 0);
			ID3D11Buffer* constant = constants.Get();
			context->VSSetConstantBuffers(0, 1, &constant);
			context->PSSetConstantBuffers(0, 1, &constant);
			ID3D11ShaderResourceView* resources[]{ panelView.Get(), depthView.Get() };
			context->PSSetShaderResources(0, 2, resources);
			ID3D11SamplerState* sampling = sampler.Get();
			context->PSSetSamplers(0, 1, &sampling);
			context->Draw(6, 0);
			context->OMSetRenderTargets(0, nullptr, nullptr);
			context->CopyResource(readback.Get(), target.Get());
			D3D11_MAPPED_SUBRESOURCE mapped{};
			Check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map(readback)");
			Image image(kWidth * kHeight);
			for (UINT y = 0; y < kHeight; ++y)
				std::memcpy(image.data() + y * kWidth, static_cast<const std::byte*>(mapped.pData) + y * mapped.RowPitch, kWidth * sizeof(Pixel));
			context->Unmap(readback.Get(), 0);
			return image;
		}

		void CheckDebug()
		{
			if (!debug)
				return;
			bool failed = false;
			for (UINT64 index = 0; index < debug->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index) {
				SIZE_T length = 0;
				Check(debug->GetMessage(index, nullptr, &length), "GetMessage(size)");
				std::vector<std::byte> storage(length);
				auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
				Check(debug->GetMessage(index, message, &length), "GetMessage");
				if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
					std::cerr << "D3D11: " << message->pDescription << '\n';
					failed = true;
				}
			}
			debug->ClearStoredMessages();
			Require(!failed, "Debug layer reported a graphics error/warning");
		}
	};

	void FillRect(std::vector<float>& depth, UINT x, UINT y, UINT width, UINT height, float value)
	{
		for (UINT row = y; row < y + height; ++row)
			std::fill_n(depth.begin() + row * kDepthWidth + x, width, value);
	}

	void StereoOcclusion(Fixture& fixture, bool native)
	{
		const std::vector<float> farDepth(kDepthWidth * kDepthHeight, Encode(8, native));
		auto offRay = farDepth;
		FillRect(offRay, 48, 0, 8, kDepthHeight, Encode(1, native));
		for (UINT eye = 0; eye < 2; ++eye) {
			auto constants = Constants(eye, native);
			const auto clean = fixture.Draw(constants, farDepth);
			Require(OpaquePixels(clean) > 0, "Subtitle baseline is empty");
			Require(fixture.Draw(constants, offRay) == clean, "Off-ray near object erased a glyph in one eye");
			auto wrongMapping = constants;
			wrongMapping.depthRect[0] = 0;
			wrongMapping.depthRect[2] = 128;
			const auto wrong = fixture.Draw(wrongMapping, offRay);
			Require(OpaquePixels(wrong) > 0 && OpaquePixels(wrong) < OpaquePixels(clean), "Fixture does not detect the global-coordinate occlusion error");
			auto intervening = farDepth;
			FillRect(intervening, eye * 64, 0, 64, 32, Encode(1, native));
			Require(OpaquePixels(fixture.Draw(constants, intervening)) == 0, "True intervening geometry failed to occlude");
			constants.options[0] = 0;
			Require(fixture.Draw(constants, intervening) == clean, "Diagnostic depth bypass did not restore the complete glyph");
		}
		fixture.CheckDebug();
		std::cout << "PASS both eyes / " << (native ? "native" : "linear") << " depth / off-ray and true occlusion / diagnostic bypass\n";
	}

	void CroppedDepthAndOutputFlips(Fixture& fixture)
	{
		auto constants = Constants(0, false);
		constants.depthRect[0] = 8;
		constants.depthRect[1] = 4;
		constants.depthRect[2] = 48;
		constants.depthRect[3] = 24;
		constants.options[1] = constants.options[2] = 1;
		const std::vector<float> farDepth(kDepthWidth * kDepthHeight, 8);
		const auto clean = fixture.Draw(constants, farDepth);
		Require(OpaquePixels(clean) > 0, "Flipped subtitle baseline is empty");
		auto offRay = farDepth;
		FillRect(offRay, 35, 16, 12, 6, 1);
		Require(fixture.Draw(constants, offRay) == clean, "Output orientation was incorrectly applied to depth addressing");
		auto intervening = farDepth;
		FillRect(intervening, 17, 10, 12, 6, 1);
		Require(OpaquePixels(fixture.Draw(constants, intervening)) == 0, "Cropped active depth rectangle ignored its origin");
		constants.trackingToDepthClip[12] = 10;
		Require(fixture.Draw(constants, intervening) == clean, "Out-of-eye depth samples fabricated near occlusion");
		fixture.CheckDebug();
		std::cout << "PASS cropped depth / independent output flips / invalid projection fallback\n";
	}

	void ColorTransfer(Fixture& fixture)
	{
		auto constants = Constants(0, false);
		constants.options[0] = 0;
		const std::vector<float> depth(kDepthWidth * kDepthHeight, 8);
		const auto gamma = fixture.Draw(constants, depth);
		constants.options[3] = 1;
		const auto linear = fixture.Draw(constants, depth);
		Require(OpaquePixels(gamma) == OpaquePixels(linear) && OpaquePixels(gamma) > 0, "Color transfer changed glyph coverage");
		for (std::size_t index = 0; index < gamma.size(); ++index) {
			if (gamma[index].alpha != 255)
				continue;
			Require(gamma[index] == Pixel{ 128, 64, 32, 255 }, "Gamma output changed client panel color");
			const auto& value = linear[index];
			Require(std::abs(static_cast<int>(value.red) - 55) <= 1 && std::abs(static_cast<int>(value.green) - 13) <= 1 &&
						std::abs(static_cast<int>(value.blue) - 4) <= 1 && value.alpha == 255,
				"Linear output did not decode gamma panel RGB");
		}
		fixture.CheckDebug();
		std::cout << "PASS explicit Gamma and Linear UNORM output transfer\n";
	}
}

int main()
{
	try {
		Fixture fixture;
		StereoOcclusion(fixture, false);
		StereoOcclusion(fixture, true);
		CroppedDepthAndOutputFlips(fixture);
		ColorTransfer(fixture);
		std::cout << "PASS production hosted shaders on WARP\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "FAIL hosted shader fixture: " << error.what() << '\n';
		return 1;
	}
}
