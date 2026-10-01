// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include <d3d11_1.h>
#include <winrt/base.h>

#include <array>
#include <cstddef>

namespace ImGuiVRHelper::Internal
{
	/// Isolates texture-backed overlay draws on a non-null FL11.0+ immediate context.
	/// Captures the bindings those draws change, including indirect SRV/UAV
	/// alias unbinding. Stream-output bindings, scissor rectangles and CS shaders stay
	/// untouched; callers must not mutate them within this scope.
	class ScopedD3D11State final
	{
	public:
		explicit ScopedD3D11State(ID3D11DeviceContext* context) noexcept :
			context_(context)
		{
			winrt::com_ptr<ID3D11Device> device;
			context_->GetDevice(device.put());
			uavSlotCount_ = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ?
			                    D3D11_1_UAV_SLOT_COUNT :
			                    D3D11_PS_CS_UAV_REGISTER_COUNT;
			D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
			if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))) &&
				options.ConstantBufferOffsetting) {
				context_->QueryInterface(__uuidof(ID3D11DeviceContext1), context1_.put_void());
			}

			context_->IAGetVertexBuffers(0, 2, vertexBuffers_.values.data(), strides_.data(), offsets_.data());
			context_->IAGetIndexBuffer(indexBuffer_.put(), &indexFormat_, &indexOffset_);
			context_->IAGetInputLayout(inputLayout_.put());
			context_->IAGetPrimitiveTopology(&topology_);
			context_->VSGetShader(vs_.shader.put(), vs_.instances.values.data(), &vs_.instanceCount);
			context_->PSGetShader(ps_.shader.put(), ps_.instances.values.data(), &ps_.instanceCount);
			context_->GSGetShader(gs_.shader.put(), gs_.instances.values.data(), &gs_.instanceCount);
			context_->HSGetShader(hs_.shader.put(), hs_.instances.values.data(), &hs_.instanceCount);
			context_->DSGetShader(ds_.shader.put(), ds_.instances.values.data(), &ds_.instanceCount);
			if (context1_) {
				context1_->VSGetConstantBuffers1(0, 1, vsConstantBuffer_.put(), &vsFirstConstant_, &vsNumConstants_);
				context1_->PSGetConstantBuffers1(0, 1, psConstantBuffer_.put(), &psFirstConstant_, &psNumConstants_);
			} else {
				context_->VSGetConstantBuffers(0, 1, vsConstantBuffer_.put());
				context_->PSGetConstantBuffers(0, 1, psConstantBuffer_.put());
			}
			context_->PSGetSamplers(0, 1, sampler_.put());
			CaptureResources();
			context_->OMGetBlendState(blend_.put(), blendFactor_.data(), &sampleMask_);
			context_->OMGetDepthStencilState(depth_.put(), &stencilRef_);
			context_->RSGetState(raster_.put());
			context_->RSGetViewports(&viewportCount_, viewports_.data());
			context_->GetPredication(predicate_.put(), &predicateValue_);

			// Outputs and inputs must be detached before either side of an alias is rebound.
			ClearResources();
			context_->GSSetShader(nullptr, nullptr, 0);
			context_->HSSetShader(nullptr, nullptr, 0);
			context_->DSSetShader(nullptr, nullptr, 0);
			context_->SetPredication(nullptr, FALSE);
		}

		~ScopedD3D11State() noexcept
		{
			ClearResources();
			RestoreResources();
			context_->IASetVertexBuffers(0, 2, vertexBuffers_.values.data(), strides_.data(), offsets_.data());
			context_->IASetIndexBuffer(indexBuffer_.get(), indexFormat_, indexOffset_);
			context_->IASetInputLayout(inputLayout_.get());
			context_->IASetPrimitiveTopology(topology_);
			context_->VSSetShader(vs_.shader.get(), vs_.instances.values.data(), vs_.instanceCount);
			context_->PSSetShader(ps_.shader.get(), ps_.instances.values.data(), ps_.instanceCount);
			context_->GSSetShader(gs_.shader.get(), gs_.instances.values.data(), gs_.instanceCount);
			context_->HSSetShader(hs_.shader.get(), hs_.instances.values.data(), hs_.instanceCount);
			context_->DSSetShader(ds_.shader.get(), ds_.instances.values.data(), ds_.instanceCount);
			auto* vsBuffer = vsConstantBuffer_.get();
			auto* psBuffer = psConstantBuffer_.get();
			if (context1_) {
				context1_->VSSetConstantBuffers1(0, 1, &vsBuffer, &vsFirstConstant_, &vsNumConstants_);
				context1_->PSSetConstantBuffers1(0, 1, &psBuffer, &psFirstConstant_, &psNumConstants_);
			} else {
				context_->VSSetConstantBuffers(0, 1, &vsBuffer);
				context_->PSSetConstantBuffers(0, 1, &psBuffer);
			}
			auto* sampler = sampler_.get();
			context_->PSSetSamplers(0, 1, &sampler);
			context_->OMSetBlendState(blend_.get(), blendFactor_.data(), sampleMask_);
			context_->OMSetDepthStencilState(depth_.get(), stencilRef_);
			context_->RSSetState(raster_.get());
			context_->RSSetViewports(viewportCount_, viewports_.data());
			context_->SetPredication(predicate_.get(), predicateValue_);
		}

		ScopedD3D11State(const ScopedD3D11State&) = delete;
		ScopedD3D11State& operator=(const ScopedD3D11State&) = delete;
		ScopedD3D11State(ScopedD3D11State&&) = delete;
		ScopedD3D11State& operator=(ScopedD3D11State&&) = delete;

	private:
		template <class T, std::size_t Count>
		struct BindingSlots
		{
			std::array<T*, Count> values{};

			~BindingSlots() noexcept
			{
				for (auto* value : values) {
					if (value)
						value->Release();
				}
			}
		};

		template <class T>
		struct ShaderBinding
		{
			winrt::com_ptr<T> shader;
			BindingSlots<ID3D11ClassInstance, D3D11_SHADER_MAX_INTERFACES> instances;
			UINT instanceCount = D3D11_SHADER_MAX_INTERFACES;
		};

		void CaptureResources() noexcept
		{
			context_->VSGetShaderResources(0, kSRVSlots, vsResources_.values.data());
			context_->PSGetShaderResources(0, kSRVSlots, psResources_.values.data());
			context_->GSGetShaderResources(0, kSRVSlots, gsResources_.values.data());
			context_->HSGetShaderResources(0, kSRVSlots, hsResources_.values.data());
			context_->DSGetShaderResources(0, kSRVSlots, dsResources_.values.data());
			context_->CSGetShaderResources(0, kSRVSlots, csResources_.values.data());
			context_->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, renderTargets_.values.data(), depthTarget_.put());
			context_->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, uavSlotCount_, omUAVs_.values.data());
			context_->CSGetUnorderedAccessViews(0, uavSlotCount_, csUAVs_.values.data());
		}

		void ClearResources() noexcept
		{
			context_->VSSetShaderResources(0, kSRVSlots, kNullSRVs.data());
			context_->PSSetShaderResources(0, kSRVSlots, kNullSRVs.data());
			context_->GSSetShaderResources(0, kSRVSlots, kNullSRVs.data());
			context_->HSSetShaderResources(0, kSRVSlots, kNullSRVs.data());
			context_->DSSetShaderResources(0, kSRVSlots, kNullSRVs.data());
			context_->CSSetShaderResources(0, kSRVSlots, kNullSRVs.data());
			context_->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, uavSlotCount_, kNullUAVs.data(), kKeepCounters.data());
			context_->CSSetUnorderedAccessViews(0, uavSlotCount_, kNullUAVs.data(), kKeepCounters.data());
		}

		void RestoreResources() noexcept
		{
			// RTV and OM UAV bind points overlap; trailing null RTVs are not occupied slots.
			UINT targetCount = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
			while (targetCount && !renderTargets_.values[targetCount - 1])
				--targetCount;
			if (targetCount < uavSlotCount_) {
				context_->OMSetRenderTargetsAndUnorderedAccessViews(targetCount, renderTargets_.values.data(),
					depthTarget_.get(), targetCount, uavSlotCount_ - targetCount,
					omUAVs_.values.data() + targetCount, kKeepCounters.data());
			} else {
				context_->OMSetRenderTargets(targetCount, renderTargets_.values.data(), depthTarget_.get());
			}
			context_->CSSetUnorderedAccessViews(0, uavSlotCount_, csUAVs_.values.data(), kKeepCounters.data());
			context_->VSSetShaderResources(0, kSRVSlots, vsResources_.values.data());
			context_->PSSetShaderResources(0, kSRVSlots, psResources_.values.data());
			context_->GSSetShaderResources(0, kSRVSlots, gsResources_.values.data());
			context_->HSSetShaderResources(0, kSRVSlots, hsResources_.values.data());
			context_->DSSetShaderResources(0, kSRVSlots, dsResources_.values.data());
			context_->CSSetShaderResources(0, kSRVSlots, csResources_.values.data());
		}

		static constexpr UINT kSRVSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
		static constexpr std::array<ID3D11ShaderResourceView*, kSRVSlots> kNullSRVs{};
		static constexpr std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> kNullUAVs{};
		static constexpr auto kKeepCounters = [] {
			std::array<UINT, D3D11_1_UAV_SLOT_COUNT> counts{};
			counts.fill(static_cast<UINT>(-1));
			return counts;
		}();

		ID3D11DeviceContext* context_;
		winrt::com_ptr<ID3D11DeviceContext1> context1_;
		UINT uavSlotCount_ = D3D11_PS_CS_UAV_REGISTER_COUNT;
		BindingSlots<ID3D11Buffer, 2> vertexBuffers_;
		std::array<UINT, 2> strides_{};
		std::array<UINT, 2> offsets_{};
		winrt::com_ptr<ID3D11Buffer> indexBuffer_;
		DXGI_FORMAT indexFormat_ = DXGI_FORMAT_UNKNOWN;
		UINT indexOffset_ = 0;
		winrt::com_ptr<ID3D11InputLayout> inputLayout_;
		D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		ShaderBinding<ID3D11VertexShader> vs_;
		ShaderBinding<ID3D11PixelShader> ps_;
		ShaderBinding<ID3D11GeometryShader> gs_;
		ShaderBinding<ID3D11HullShader> hs_;
		ShaderBinding<ID3D11DomainShader> ds_;
		winrt::com_ptr<ID3D11Buffer> vsConstantBuffer_;
		winrt::com_ptr<ID3D11Buffer> psConstantBuffer_;
		UINT vsFirstConstant_ = 0;
		UINT psFirstConstant_ = 0;
		UINT vsNumConstants_ = 0;
		UINT psNumConstants_ = 0;
		winrt::com_ptr<ID3D11SamplerState> sampler_;
		BindingSlots<ID3D11ShaderResourceView, kSRVSlots> vsResources_;
		BindingSlots<ID3D11ShaderResourceView, kSRVSlots> psResources_;
		BindingSlots<ID3D11ShaderResourceView, kSRVSlots> gsResources_;
		BindingSlots<ID3D11ShaderResourceView, kSRVSlots> hsResources_;
		BindingSlots<ID3D11ShaderResourceView, kSRVSlots> dsResources_;
		BindingSlots<ID3D11ShaderResourceView, kSRVSlots> csResources_;
		BindingSlots<ID3D11RenderTargetView, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> renderTargets_;
		winrt::com_ptr<ID3D11DepthStencilView> depthTarget_;
		BindingSlots<ID3D11UnorderedAccessView, D3D11_1_UAV_SLOT_COUNT> omUAVs_;
		BindingSlots<ID3D11UnorderedAccessView, D3D11_1_UAV_SLOT_COUNT> csUAVs_;
		winrt::com_ptr<ID3D11BlendState> blend_;
		std::array<FLOAT, 4> blendFactor_{};
		UINT sampleMask_ = 0;
		winrt::com_ptr<ID3D11DepthStencilState> depth_;
		UINT stencilRef_ = 0;
		winrt::com_ptr<ID3D11RasterizerState> raster_;
		std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports_{};
		UINT viewportCount_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		winrt::com_ptr<ID3D11Predicate> predicate_;
		BOOL predicateValue_ = FALSE;
	};
}
