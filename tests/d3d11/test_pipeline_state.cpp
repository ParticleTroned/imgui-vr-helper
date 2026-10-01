// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "internal/ScopedD3D11State.h"

using Microsoft::WRL::ComPtr;

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition)
			throw std::runtime_error(message);
	}

	void Check(HRESULT result, const char* operation)
	{
		if (FAILED(result)) {
			throw std::runtime_error(std::string(operation) + " failed (HRESULT " +
									 std::to_string(static_cast<unsigned long>(result)) + ")");
		}
	}

	ComPtr<ID3DBlob> Compile(const char* source, const char* profile)
	{
		ComPtr<ID3DBlob> code, errors;
		const HRESULT result = D3DCompile(source, strlen(source), "state-fixture", nullptr, nullptr,
			"main", profile, D3DCOMPILE_ENABLE_STRICTNESS, 0,
			&code, &errors);
		if (FAILED(result) && errors) {
			throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
		}
		Check(result, "D3DCompile");
		return code;
	}

	enum class Stage
	{
		VS,
		HS,
		DS,
		GS,
		PS,
		CS
	};
	constexpr std::array kStages{ Stage::VS, Stage::HS, Stage::DS, Stage::GS, Stage::PS, Stage::CS };
	constexpr UINT kAliasSlot = 37;

	void SetAlias(ID3D11DeviceContext* context, Stage stage, ID3D11ShaderResourceView* view)
	{
		switch (stage) {
		case Stage::VS:
			context->VSSetShaderResources(kAliasSlot, 1, &view);
			break;
		case Stage::HS:
			context->HSSetShaderResources(kAliasSlot, 1, &view);
			break;
		case Stage::DS:
			context->DSSetShaderResources(kAliasSlot, 1, &view);
			break;
		case Stage::GS:
			context->GSSetShaderResources(kAliasSlot, 1, &view);
			break;
		case Stage::PS:
			context->PSSetShaderResources(kAliasSlot, 1, &view);
			break;
		case Stage::CS:
			context->CSSetShaderResources(kAliasSlot, 1, &view);
			break;
		}
	}

	ComPtr<ID3D11ShaderResourceView> GetAlias(ID3D11DeviceContext* context, Stage stage)
	{
		ComPtr<ID3D11ShaderResourceView> view;
		switch (stage) {
		case Stage::VS:
			context->VSGetShaderResources(kAliasSlot, 1, &view);
			break;
		case Stage::HS:
			context->HSGetShaderResources(kAliasSlot, 1, &view);
			break;
		case Stage::DS:
			context->DSGetShaderResources(kAliasSlot, 1, &view);
			break;
		case Stage::GS:
			context->GSGetShaderResources(kAliasSlot, 1, &view);
			break;
		case Stage::PS:
			context->PSGetShaderResources(kAliasSlot, 1, &view);
			break;
		case Stage::CS:
			context->CSGetShaderResources(kAliasSlot, 1, &view);
			break;
		}
		return view;
	}

	struct Fixture
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> context;
		ComPtr<ID3D11DeviceContext1> context1;
		ComPtr<ID3D11InfoQueue> debug;
		D3D_FEATURE_LEVEL featureLevel{};
		bool rangedConstants = false;
		bool useRangedBindings = true;
		ComPtr<ID3D11VertexShader> vs;
		ComPtr<ID3D11PixelShader> ps;
		ComPtr<ID3D11GeometryShader> gs;
		ComPtr<ID3D11HullShader> hs;
		ComPtr<ID3D11DomainShader> ds;
		ComPtr<ID3D11ClassLinkage> linkage;
		ComPtr<ID3D11ClassInstance> instance;
		ComPtr<ID3D11InputLayout> layout;
		std::array<ComPtr<ID3D11Buffer>, 2> vertices;
		ComPtr<ID3D11Buffer> indices, constants;
		ComPtr<ID3D11SamplerState> sampler;
		ComPtr<ID3D11BlendState> blend;
		ComPtr<ID3D11DepthStencilState> depthState;
		ComPtr<ID3D11RasterizerState> rasterizer;
		ComPtr<ID3D11Predicate> predicate;
		ComPtr<ID3D11Texture2D> color, alias, depth;
		ComPtr<ID3D11RenderTargetView> rtv, aliasRTV;
		ComPtr<ID3D11ShaderResourceView> aliasSRV;
		ComPtr<ID3D11DepthStencilView> dsv;
		std::array<ComPtr<ID3D11Buffer>, 4> appendBuffers;
		std::array<ComPtr<ID3D11UnorderedAccessView>, 4> uavs;
		ComPtr<ID3D11Buffer> counterReadback;
		const std::array<UINT, 4> counters{ 7, 9, 11, 13 };
		const std::array<float, 4> blendFactor{ 0.2f, 0.4f, 0.6f, 0.8f };
		const std::array<D3D11_VIEWPORT, 2> viewports{ { { 1, 2, 12, 13, 0.1f, 0.9f }, { 3, 4, 8, 9, 0, 1 } } };
		const std::array<D3D11_RECT, 2> scissors{ { { 1, 2, 12, 13 }, { 3, 4, 8, 9 } } };

		explicit Fixture(D3D_FEATURE_LEVEL requestedLevel)
		{
			const D3D_FEATURE_LEVEL levels[]{ D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
			const bool requireLevel11 = requestedLevel == D3D_FEATURE_LEVEL_11_0;
			auto create = [&](UINT flags) {
				device.Reset();
				context.Reset();
				HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
					levels + (requireLevel11 ? 1 : 0), requireLevel11 ? 1 : 2,
					D3D11_SDK_VERSION, &device, &featureLevel, &context);
				if (result == E_INVALIDARG && !requireLevel11) {
					result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
						levels + 1, 1, D3D11_SDK_VERSION, &device, &featureLevel, &context);
				}
				return result;
			};
			HRESULT result = create(D3D11_CREATE_DEVICE_DEBUG);
			if (result == DXGI_ERROR_SDK_COMPONENT_MISSING) {
				std::cout << "SKIP debug-layer diagnostics: Windows Graphics Tools unavailable\n";
				result = create(0);
			}
			Check(result, "D3D11CreateDevice(WARP)");
			Require(!requireLevel11 || featureLevel == D3D_FEATURE_LEVEL_11_0,
				"Explicit feature level 11.0 request was not honored");
			(void)device.As(&debug);
			(void)context.As(&context1);
			D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
			rangedConstants = context1 && SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))) && options.ConstantBufferOffsetting;
			std::cout << "WARP requested feature level " << std::hex << requestedLevel
			          << "; selected " << featureLevel << std::dec
			          << "; debug layer " << (debug ? "enabled" : "unavailable") << '\n';
			CreateShaders();
			CreateResources();
		}

		~Fixture()
		{
			if (context) {
				context->ClearState();
				context->Flush();
			}
		}

		void CreateShaders()
		{
			auto vertex = Compile("float4 main(float3 p : POSITION) : SV_Position { return float4(p, 1); }", "vs_5_0");
			Check(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vs), "CreateVertexShader");
			const D3D11_INPUT_ELEMENT_DESC element{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
			Check(device->CreateInputLayout(&element, 1, vertex->GetBufferPointer(), vertex->GetBufferSize(), &layout), "CreateInputLayout");
			const char* pixel = R"(
interface IColor { float4 GetColor(); };
class SolidColor : IColor { float4 color; float4 GetColor() { return color; } };
IColor selectedColor;
float4 main() : SV_Target { return selectedColor.GetColor(); }
)";
			auto pixelCode = Compile(pixel, "ps_5_0");
			Check(device->CreateClassLinkage(&linkage), "CreateClassLinkage");
			Check(device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), linkage.Get(), &ps), "CreatePixelShader");
			Check(linkage->CreateClassInstance("SolidColor", 0, 0, 0, 0, &instance), "CreateClassInstance");
			auto geometry = Compile(R"(
struct V { float4 p : SV_Position; };
[maxvertexcount(3)] void main(triangle V input[3], inout TriangleStream<V> stream) {
    stream.Append(input[0]); stream.Append(input[1]); stream.Append(input[2]);
})",
				"gs_5_0");
			Check(device->CreateGeometryShader(geometry->GetBufferPointer(), geometry->GetBufferSize(), nullptr, &gs), "CreateGeometryShader");
			auto hull = Compile(R"(
struct V { float4 p : SV_Position; };
struct Patch { float edge[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
Patch constants(InputPatch<V, 3> input) {
    Patch p; p.edge[0] = p.edge[1] = p.edge[2] = p.inside = 1; return p;
}
[domain("tri")] [partitioning("integer")] [outputtopology("triangle_cw")]
[outputcontrolpoints(3)] [patchconstantfunc("constants")]
V main(InputPatch<V, 3> input, uint i : SV_OutputControlPointID) { return input[i]; }
)",
				"hs_5_0");
			Check(device->CreateHullShader(hull->GetBufferPointer(), hull->GetBufferSize(), nullptr, &hs), "CreateHullShader");
			auto domain = Compile(R"(
struct V { float4 p : SV_Position; };
struct Patch { float edge[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
[domain("tri")] float4 main(Patch patch, float3 uv : SV_DomainLocation, const OutputPatch<V, 3> input) : SV_Position {
    return input[0].p * uv.x + input[1].p * uv.y + input[2].p * uv.z;
})",
				"ds_5_0");
			Check(device->CreateDomainShader(domain->GetBufferPointer(), domain->GetBufferSize(), nullptr, &ds), "CreateDomainShader");
		}

		void CreateResources()
		{
			D3D11_BUFFER_DESC buffer{};
			buffer.ByteWidth = 4096;
			buffer.Usage = D3D11_USAGE_DEFAULT;
			buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
			for (auto& vertex : vertices) Check(device->CreateBuffer(&buffer, nullptr, &vertex), "CreateBuffer(vertex)");
			buffer.BindFlags = D3D11_BIND_INDEX_BUFFER;
			Check(device->CreateBuffer(&buffer, nullptr, &indices), "CreateBuffer(index)");
			buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			Check(device->CreateBuffer(&buffer, nullptr, &constants), "CreateBuffer(constants)");
			buffer.ByteWidth = 128;
			buffer.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
			buffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			buffer.StructureByteStride = 4;
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			uav.Buffer.NumElements = 32;
			uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
			for (size_t i = 0; i < uavs.size(); ++i) {
				Check(device->CreateBuffer(&buffer, nullptr, &appendBuffers[i]), "CreateBuffer(append)");
				Check(device->CreateUnorderedAccessView(appendBuffers[i].Get(), &uav, &uavs[i]), "CreateUnorderedAccessView");
			}
			buffer = {};
			buffer.ByteWidth = 4;
			buffer.Usage = D3D11_USAGE_STAGING;
			buffer.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			Check(device->CreateBuffer(&buffer, nullptr, &counterReadback), "CreateBuffer(readback)");

			D3D11_TEXTURE2D_DESC texture{};
			texture.Width = texture.Height = 16;
			texture.MipLevels = texture.ArraySize = texture.SampleDesc.Count = 1;
			texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			texture.Usage = D3D11_USAGE_DEFAULT;
			texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			Check(device->CreateTexture2D(&texture, nullptr, &color), "CreateTexture2D(color)");
			Check(device->CreateTexture2D(&texture, nullptr, &alias), "CreateTexture2D(alias)");
			Check(device->CreateRenderTargetView(color.Get(), nullptr, &rtv), "CreateRenderTargetView(color)");
			Check(device->CreateRenderTargetView(alias.Get(), nullptr, &aliasRTV), "CreateRenderTargetView(alias)");
			Check(device->CreateShaderResourceView(alias.Get(), nullptr, &aliasSRV), "CreateShaderResourceView(alias)");
			texture.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
			texture.BindFlags = D3D11_BIND_DEPTH_STENCIL;
			Check(device->CreateTexture2D(&texture, nullptr, &depth), "CreateTexture2D(depth)");
			Check(device->CreateDepthStencilView(depth.Get(), nullptr, &dsv), "CreateDepthStencilView");

			D3D11_SAMPLER_DESC sampling{};
			sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
			sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampling.ComparisonFunc = D3D11_COMPARISON_NEVER;
			sampling.MaxLOD = D3D11_FLOAT32_MAX;
			Check(device->CreateSamplerState(&sampling, &sampler), "CreateSamplerState");
			D3D11_BLEND_DESC blending{};
			blending.RenderTarget[0].BlendEnable = true;
			blending.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			blending.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			blending.RenderTarget[0].BlendOp = blending.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			blending.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
			blending.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
			blending.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			Check(device->CreateBlendState(&blending, &blend), "CreateBlendState");
			D3D11_DEPTH_STENCIL_DESC depthDescription{};
			depthDescription.DepthEnable = true;
			depthDescription.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
			depthDescription.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
			depthDescription.StencilReadMask = depthDescription.StencilWriteMask = 0xff;
			depthDescription.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
				D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS };
			depthDescription.BackFace = depthDescription.FrontFace;
			Check(device->CreateDepthStencilState(&depthDescription, &depthState), "CreateDepthStencilState");
			D3D11_RASTERIZER_DESC raster{};
			raster.FillMode = D3D11_FILL_WIREFRAME;
			raster.CullMode = D3D11_CULL_NONE;
			raster.DepthClipEnable = raster.ScissorEnable = true;
			Check(device->CreateRasterizerState(&raster, &rasterizer), "CreateRasterizerState");
			D3D11_QUERY_DESC query{ D3D11_QUERY_OCCLUSION_PREDICATE, 0 };
			Check(device->CreatePredicate(&query, &predicate), "CreatePredicate");
			context->Begin(predicate.Get());
			context->End(predicate.Get());
		}

		void Seed(bool populated, bool reset = true)
		{
			if (reset) {
				context->ClearState();
				ID3D11SamplerState* sentinel = sampler.Get();
				for (UINT slot : { 1u, 15u }) context->PSSetSamplers(slot, 1, &sentinel);
				context->RSSetScissorRects(static_cast<UINT>(scissors.size()), scissors.data());
			}
			if (!populated)
				return;
			ID3D11Buffer* vertexBuffers[]{ vertices[0].Get(), vertices[1].Get() };
			const UINT strides[]{ 12, 24 }, offsets[]{ 4, 8 };
			context->IASetVertexBuffers(0, 2, vertexBuffers, strides, offsets);
			context->IASetIndexBuffer(indices.Get(), DXGI_FORMAT_R32_UINT, 4);
			context->IASetInputLayout(layout.Get());
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
			context->VSSetShader(vs.Get(), nullptr, 0);
			ID3D11ClassInstance* classInstance = instance.Get();
			context->PSSetShader(ps.Get(), &classInstance, 1);
			context->GSSetShader(gs.Get(), nullptr, 0);
			context->HSSetShader(hs.Get(), nullptr, 0);
			context->DSSetShader(ds.Get(), nullptr, 0);
			ID3D11Buffer* constant = constants.Get();
			if (rangedConstants && useRangedBindings) {
				const UINT first = 16, count = 32;
				context1->VSSetConstantBuffers1(0, 1, &constant, &first, &count);
				context1->PSSetConstantBuffers1(0, 1, &constant, &first, &count);
			} else {
				context->VSSetConstantBuffers(0, 1, &constant);
				context->PSSetConstantBuffers(0, 1, &constant);
			}
			ID3D11SamplerState* sampling = sampler.Get();
			context->PSSetSamplers(0, 1, &sampling);
			context->OMSetBlendState(blend.Get(), blendFactor.data(), 0x12345678);
			context->OMSetDepthStencilState(depthState.Get(), 23);
			context->RSSetState(rasterizer.Get());
			context->RSSetViewports(static_cast<UINT>(viewports.size()), viewports.data());
			context->SetPredication(predicate.Get(), TRUE);
			ID3D11RenderTargetView* target = rtv.Get();
			std::array<ID3D11UnorderedAccessView*, 33> omOutputs{};
			std::array<UINT, 33> initialCounts{};
			initialCounts.fill(0xffffffff);
			omOutputs[0] = uavs[0].Get();
			initialCounts[0] = counters[0];
			const bool highSlots = featureLevel >= D3D_FEATURE_LEVEL_11_1;
			if (highSlots) {
				omOutputs[32] = uavs[2].Get();
				initialCounts[32] = counters[2];
			}
			context->OMSetRenderTargetsAndUnorderedAccessViews(1, &target, dsv.Get(), 5,
				highSlots ? 33 : 1, omOutputs.data(), initialCounts.data());
			ID3D11UnorderedAccessView* output = uavs[1].Get();
			context->CSSetUnorderedAccessViews(6, 1, &output, &counters[1]);
			if (featureLevel >= D3D_FEATURE_LEVEL_11_1) {
				output = uavs[3].Get();
				context->CSSetUnorderedAccessViews(37, 1, &output, &counters[3]);
			}
			for (Stage stage : kStages) SetAlias(context.Get(), stage, aliasSRV.Get());
		}

		void Verify(bool populated)
		{
			ID3D11Buffer* buffers[2]{};
			UINT strides[2]{}, offsets[2]{};
			context->IAGetVertexBuffers(0, 2, buffers, strides, offsets);
			for (size_t i = 0; i < 2; ++i) {
				ComPtr<ID3D11Buffer> retained;
				retained.Attach(buffers[i]);
				Require(retained.Get() == (populated ? vertices[i].Get() : nullptr), "IA vertex buffer changed");
				Require(strides[i] == (populated ? (i == 0 ? 12u : 24u) : 0u), "IA stride changed");
				Require(offsets[i] == (populated ? (i == 0 ? 4u : 8u) : 0u), "IA offset changed");
			}
			ComPtr<ID3D11Buffer> index;
			DXGI_FORMAT format{};
			UINT offset{};
			context->IAGetIndexBuffer(&index, &format, &offset);
			Require(index.Get() == (populated ? indices.Get() : nullptr), "IA index buffer changed");
			Require(format == (populated ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_UNKNOWN), "IA index format changed");
			Require(offset == (populated ? 4u : 0u), "IA index offset changed");
			ComPtr<ID3D11InputLayout> input;
			context->IAGetInputLayout(&input);
			Require(input.Get() == (populated ? layout.Get() : nullptr), "IA layout changed");
			D3D11_PRIMITIVE_TOPOLOGY topology{};
			context->IAGetPrimitiveTopology(&topology);
			Require(topology == (populated ? D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST : D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED), "IA topology changed");
			VerifyShaders(populated);
			for (bool pixel : { false, true }) {
				ComPtr<ID3D11Buffer> constant;
				UINT first{}, count{};
				if (rangedConstants) {
					if (pixel)
						context1->PSGetConstantBuffers1(0, 1, &constant, &first, &count);
					else
						context1->VSGetConstantBuffers1(0, 1, &constant, &first, &count);
					if (populated)
						Require(first == (useRangedBindings ? 16u : 0u) &&
									count == (useRangedBindings ? 32u : D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT),
							"Constant-buffer range changed");
				} else if (pixel)
					context->PSGetConstantBuffers(0, 1, &constant);
				else
					context->VSGetConstantBuffers(0, 1, &constant);
				Require(constant.Get() == (populated ? constants.Get() : nullptr), "Constant buffer changed");
			}
			{
				ComPtr<ID3D11SamplerState> sampling;
				context->PSGetSamplers(0, 1, &sampling);
				Require(sampling.Get() == (populated ? sampler.Get() : nullptr), "PS sampler changed");
			}
			for (UINT slot : { 1u, 15u }) {
				ComPtr<ID3D11SamplerState> sentinel;
				context->PSGetSamplers(slot, 1, &sentinel);
				Require(sentinel.Get() == sampler.Get(), "Untouched sampler sentinel changed");
			}
			for (Stage stage : kStages) Require(GetAlias(context.Get(), stage).Get() ==
													(populated ? aliasSRV.Get() : nullptr),
				"Stage slot37 SRV not restored");
			VerifyOutput(populated);
		}

		void VerifyShaders(bool populated)
		{
			ComPtr<ID3D11VertexShader> vertex;
			ComPtr<ID3D11PixelShader> pixel;
			ComPtr<ID3D11GeometryShader> geometry;
			ComPtr<ID3D11HullShader> hull;
			ComPtr<ID3D11DomainShader> domain;
			context->VSGetShader(&vertex, nullptr, nullptr);
			ID3D11ClassInstance* classes[8]{};
			UINT classCount = 8;
			context->PSGetShader(&pixel, classes, &classCount);
			std::array<ComPtr<ID3D11ClassInstance>, 8> retained;
			for (UINT i = 0; i < classCount; ++i) retained[i].Attach(classes[i]);
			context->GSGetShader(&geometry, nullptr, nullptr);
			context->HSGetShader(&hull, nullptr, nullptr);
			context->DSGetShader(&domain, nullptr, nullptr);
			Require(vertex.Get() == (populated ? vs.Get() : nullptr), "VS changed");
			Require(pixel.Get() == (populated ? ps.Get() : nullptr), "PS changed");
			Require(geometry.Get() == (populated ? gs.Get() : nullptr), "GS changed");
			Require(hull.Get() == (populated ? hs.Get() : nullptr), "HS changed");
			Require(domain.Get() == (populated ? ds.Get() : nullptr), "DS changed");
			Require(classCount == (populated ? 1u : 0u), "PS class count changed");
			if (populated)
				Require(retained[0].Get() == instance.Get(), "PS class instance changed");
		}

		void VerifyOutput(bool populated)
		{
			ComPtr<ID3D11RenderTargetView> target;
			ComPtr<ID3D11DepthStencilView> depthView;
			context->OMGetRenderTargets(1, &target, &depthView);
			Require(target.Get() == (populated ? rtv.Get() : nullptr), "RTV changed");
			Require(depthView.Get() == (populated ? dsv.Get() : nullptr), "DSV changed");
			ComPtr<ID3D11BlendState> blending;
			float factors[4]{};
			UINT sampleMask{};
			context->OMGetBlendState(&blending, factors, &sampleMask);
			Require(blending.Get() == (populated ? blend.Get() : nullptr), "Blend state changed");
			Require(sampleMask == (populated ? 0x12345678u : 0xffffffffu), "Sample mask changed");
			for (size_t i = 0; i < 4; ++i) Require(factors[i] == (populated ? blendFactor[i] : 1.0f), "Blend factor changed");
			ComPtr<ID3D11DepthStencilState> depthSetting;
			UINT reference{};
			context->OMGetDepthStencilState(&depthSetting, &reference);
			Require(depthSetting.Get() == (populated ? depthState.Get() : nullptr), "Depth state changed");
			Require(reference == (populated ? 23u : 0u), "Stencil reference changed");
			ComPtr<ID3D11RasterizerState> raster;
			context->RSGetState(&raster);
			Require(raster.Get() == (populated ? rasterizer.Get() : nullptr), "Rasterizer changed");
			std::array<D3D11_VIEWPORT, 16> actualViewports{};
			UINT count = 16;
			context->RSGetViewports(&count, actualViewports.data());
			Require(count == (populated ? 2u : 0u), "Viewport count changed");
			for (UINT i = 0; i < count; ++i) Require(memcmp(&actualViewports[i], &viewports[i], sizeof(D3D11_VIEWPORT)) == 0, "Viewport changed");
			std::array<D3D11_RECT, 16> actualScissors{};
			count = 16;
			context->RSGetScissorRects(&count, actualScissors.data());
			Require(count == 2, "Untouched scissor sentinel count changed");
			for (UINT i = 0; i < count; ++i) Require(memcmp(&actualScissors[i], &scissors[i], sizeof(D3D11_RECT)) == 0, "Untouched scissor sentinel changed");
			ComPtr<ID3D11Predicate> condition;
			BOOL value{};
			context->GetPredication(&condition, &value);
			Require(condition.Get() == (populated ? predicate.Get() : nullptr), "Predicate changed");
			Require(value == (populated ? TRUE : FALSE), "Predicate value changed");
			const size_t slots = featureLevel >= D3D_FEATURE_LEVEL_11_1 ? 4 : 2;
			for (size_t i = 0; i < slots; ++i) {
				ComPtr<ID3D11UnorderedAccessView> output;
				const UINT slot = i >= 2 ? 37 : (i == 0 ? 5 : 6);
				if ((i % 2) == 0)
					context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, slot, 1, &output);
				else
					context->CSGetUnorderedAccessViews(slot, 1, &output);
				Require(output.Get() == (populated ? uavs[i].Get() : nullptr), "UAV binding changed");
			}
		}

		void VerifyCounters()
		{
			const size_t count = featureLevel >= D3D_FEATURE_LEVEL_11_1 ? 4 : 2;
			context->SetPredication(nullptr, FALSE);
			for (size_t i = 0; i < count; ++i) {
				context->CopyStructureCount(counterReadback.Get(), 0, uavs[i].Get());
				D3D11_MAPPED_SUBRESOURCE data{};
				Check(context->Map(counterReadback.Get(), 0, D3D11_MAP_READ, 0, &data), "Map(counter)");
				const UINT actual = *static_cast<const UINT*>(data.pData);
				context->Unmap(counterReadback.Get(), 0);
				Require(actual == counters[i], "Append UAV counter changed");
			}
		}

		void CheckDebugMessages()
		{
			if (!debug)
				return;
			bool failed = false;
			for (UINT64 i = 0; i < debug->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
				SIZE_T length = 0;
				Check(debug->GetMessage(i, nullptr, &length), "GetMessage(size)");
				std::vector<std::byte> storage(length);
				auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
				Check(debug->GetMessage(i, message, &length), "GetMessage");
				if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
					std::cerr << "D3D11: " << message->pDescription << '\n';
					failed = true;
				}
			}
			Require(!failed, "D3D11 debug layer reported a warning/error");
			debug->ClearStoredMessages();
		}

		void ClearCoveredState()
		{
			ID3D11Buffer* buffers[2]{};
			UINT zeros[2]{};
			context->IASetVertexBuffers(0, 2, buffers, zeros, zeros);
			context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
			context->IASetInputLayout(nullptr);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED);
			context->VSSetShader(nullptr, nullptr, 0);
			context->PSSetShader(nullptr, nullptr, 0);
			context->GSSetShader(nullptr, nullptr, 0);
			context->HSSetShader(nullptr, nullptr, 0);
			context->DSSetShader(nullptr, nullptr, 0);
			context->VSSetConstantBuffers(0, 1, buffers);
			context->PSSetConstantBuffers(0, 1, buffers);
			ID3D11SamplerState* sampling = nullptr;
			context->PSSetSamplers(0, 1, &sampling);
			const UINT count = featureLevel >= D3D_FEATURE_LEVEL_11_1 ? D3D11_1_UAV_SLOT_COUNT : D3D11_PS_CS_UAV_REGISTER_COUNT;
			std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> outputs{};
			context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, count, outputs.data(), nullptr);
			context->CSSetUnorderedAccessViews(0, count, outputs.data(), nullptr);
			for (Stage stage : kStages) SetAlias(context.Get(), stage, nullptr);
			const FLOAT factors[]{ 1, 1, 1, 1 };
			context->OMSetBlendState(nullptr, factors, 0xffffffff);
			context->OMSetDepthStencilState(nullptr, 0);
			context->RSSetState(nullptr);
			context->RSSetViewports(0, nullptr);
			context->SetPredication(nullptr, FALSE);
		}

		void ExerciseHighestRTVSlot()
		{
			Seed(false);
			std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
			targets.back() = rtv.Get();
			context->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), dsv.Get());
			const auto verifyTargets = [&]() {
				std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> actual{};
				ComPtr<ID3D11DepthStencilView> actualDepth;
				context->OMGetRenderTargets(static_cast<UINT>(actual.size()), actual.data(), &actualDepth);
				std::array<ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> retained;
				for (size_t i = 0; i < actual.size(); ++i)
					retained[i].Attach(actual[i]);
				for (size_t i = 0; i < actual.size(); ++i)
					Require(retained[i].Get() == targets[i], "RTV slot7 or preceding null slots changed");
				Require(actualDepth.Get() == dsv.Get(), "DSV beside RTV slot7 changed");
			};
			verifyTargets();
			CheckDebugMessages();
			{
				ImGuiVRHelper::Internal::ScopedD3D11State restore(context.Get());
				ID3D11RenderTargetView* temporary = aliasRTV.Get();
				context->OMSetRenderTargets(1, &temporary, nullptr);
			}
			verifyTargets();
			context->OMSetRenderTargets(0, nullptr, nullptr);
			Verify(false);
			CheckDebugMessages();
			context->ClearState();
			std::cout << "PASS RTV slot7 with slots0-6 null / feature level "
			          << std::hex << featureLevel << std::dec << '\n';
		}

		void Exercise(bool populated, bool unwind)
		{
			Seed(populated);
			Verify(populated);
			CheckDebugMessages();
			struct ExpectedException
			{};
			try {
				ImGuiVRHelper::Internal::ScopedD3D11State restore(context.Get());
				ComPtr<ID3D11GeometryShader> geometry;
				ComPtr<ID3D11HullShader> hull;
				ComPtr<ID3D11DomainShader> domain;
				context->GSGetShader(&geometry, nullptr, nullptr);
				context->HSGetShader(&hull, nullptr, nullptr);
				context->DSGetShader(&domain, nullptr, nullptr);
				Require(!geometry && !hull && !domain, "Inherited geometry/tessellation stages not isolated");
				ComPtr<ID3D11Predicate> condition;
				context->GetPredication(&condition, nullptr);
				Require(!condition, "Inherited predication not isolated");
				ID3D11RenderTargetView* target = aliasRTV.Get();
				context->OMSetRenderTargets(1, &target, nullptr);
				const FLOAT clear[]{ 0, 0, 0, 1 };
				context->ClearRenderTargetView(target, clear);
				ClearCoveredState();
				if (!populated)
					Seed(true, false);
				if (unwind)
					throw ExpectedException{};
			} catch (const ExpectedException&) {
				Require(unwind, "Unexpected exception branch");
			}
			Verify(populated);
			if (populated)
				VerifyCounters();
			CheckDebugMessages();
			context->ClearState();
			std::cout << "PASS " << (populated ? "nondefault" : "null")
			          << " state / " << (unwind ? "exception unwind" : "normal return")
			          << " / " << (rangedConstants && useRangedBindings ? "ranged" : "base") << " constants\n";
		}
	};
}  // namespace

int main()
{
	try {
		for (D3D_FEATURE_LEVEL requestedLevel : { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 }) {
			Fixture fixture(requestedLevel);
			for (bool populated : { false, true }) {
				fixture.Exercise(populated, false);
				fixture.Exercise(populated, true);
			}
			if (fixture.rangedConstants) {
				fixture.useRangedBindings = false;
				fixture.Exercise(true, false);
				fixture.Exercise(true, true);
			}
			fixture.ExerciseHighestRTVSlot();
			if (!fixture.rangedConstants)
				std::cout << "SKIP constant-buffer ranges: D3D11.1 offsetting unavailable\n";
			if (fixture.featureLevel < D3D_FEATURE_LEVEL_11_1)
				std::cout << "SKIP UAV slots above 7: feature level 11.1 unavailable\n";
		}
		std::cout << "PASS ScopedD3D11State WARP regression\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "FAIL " << error.what() << '\n';
		return 1;
	}
}
