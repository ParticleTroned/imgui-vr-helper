// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "Globals.h"
#include "Hooks.h"
#include "InSceneOverlay.h"
#include "RenderHost.h"
#include "internal/RenderHostController.h"

#include <atomic>
#include <memory>
#include <mutex>

namespace ImGuiVRHelper::RenderHost
{
	namespace
	{
		using Result = API::RenderHostResult;
		std::mutex g_mutex;
		Internal::RenderHostController g_controller;
		std::atomic<std::uint32_t> g_renderThread{ 0 };
		std::atomic<bool> g_active{ false };
		std::shared_ptr<InSceneOverlay::HostedFrame> g_frame;

		template <class T>
		bool ValidStructure(const T* value) noexcept
		{
			return value && value->structSize == sizeof(T) && value->contractVersion == API::kRenderHostContractVersion;
		}

		template <class T>
		bool ResetOutput(T* value) noexcept
		{
			if (!ValidStructure(value))
				return false;
			*value = {};
			return true;
		}

		void SyncThread() noexcept
		{
			g_controller.ObserveRenderThread(g_renderThread.load(std::memory_order_acquire));
		}

		template <class Fn>
		Result Boundary(Fn&& fn) noexcept
		{
			try {
				return fn();
			} catch (...) {
				return Result::RenderFailure;
			}
		}
	}

	void ObserveRenderThread() noexcept
	{
		std::uint32_t unset = 0;
		g_renderThread.compare_exchange_strong(unset, GetCurrentThreadId(), std::memory_order_release);
	}

	bool IsActive() noexcept { return g_active.load(std::memory_order_acquire); }

	API::RenderHostResult QueryCapabilities(API::RenderHostCapabilities* out) noexcept
	{
		if (!ResetOutput(out))
			return Result::InvalidArgument;
		out->supportedLayers = API::RenderHostLayer_World | API::RenderHostLayer_HUD | API::RenderHostLayer_Panel | API::RenderHostLayer_Modal;
		out->maxAttemptsPerEye = Internal::RenderHostController::kMaxAttemptsPerEye;
		return Result::Success;
	}

	API::RenderHostResult Register(const API::RenderHostRegistration* registration, std::uint64_t* outHost) noexcept
	{
		if (outHost)
			*outHost = 0;
		if (!outHost || !ValidStructure(registration) || registration->reserved)
			return Result::InvalidArgument;
		return Boundary([&] {
			std::scoped_lock lock(g_mutex);
			SyncThread();
			return g_controller.Register(registration->kind, *outHost);
		});
	}

	API::RenderHostResult SetActive(std::uint64_t host, std::uint32_t active) noexcept
	{
		if (active > 1)
			return Result::InvalidArgument;
		return Boundary([&] {
			{
				std::scoped_lock lock(g_mutex);
				SyncThread();
				const auto check = g_controller.CheckHost(host, GetCurrentThreadId(), false);
				if (check != Result::Success)
					return check;
				if (g_controller.HasFrame())
					return Result::Busy;
			}
			if (active && (!Globals::IsReady() || InSceneOverlay::IsRenderPathDisabled() || !Hooks::EnsureHostedServices()))
				return Result::NotReady;
			std::scoped_lock lock(g_mutex);
			const auto result = g_controller.SetActive(host, GetCurrentThreadId(), active != 0);
			if (result == Result::Success)
				g_active.store(active != 0, std::memory_order_release);
			return result;
		});
	}

	API::RenderHostResult QueryContent(std::uint64_t host, API::RenderHostContent* out) noexcept
	{
		if (!ResetOutput(out))
			return Result::InvalidArgument;
		return Boundary([&] {
			{
				std::scoped_lock lock(g_mutex);
				SyncThread();
				const auto check = g_controller.CheckHost(host, GetCurrentThreadId(), false);
				if (check != Result::Success)
					return check;
				if (g_controller.HasFrame())
					return Result::Busy;
			}
			*out = InSceneOverlay::QueryHostedContent();
			return out->layers ? Result::Success : Result::NoContent;
		});
	}

	API::RenderHostResult Begin(std::uint64_t host, const API::HostedFrameInfo* frame, API::HostedFrameHandle* out) noexcept
	{
		if (!ResetOutput(out) || !ValidStructure(frame) || !frame->context || frame->worldLayerEnabled > 1 ||
			(frame->diagnostics & ~API::RenderHostDiagnostic_DisableWorldDepthTest))
			return Result::InvalidArgument;
		if (!Globals::IsReady() || frame->context != Globals::GetD3D().context ||
			frame->context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || InSceneOverlay::IsRenderPathDisabled())
			return Result::NotReady;
		return Boundary([&] {
			std::uint64_t cookie = 0;
			{
				std::scoped_lock lock(g_mutex);
				SyncThread();
				const auto result = g_controller.Begin(host, GetCurrentThreadId(), *frame, cookie);
				if (result != Result::Success)
					return result;
			}
			Result prepared = Result::RenderFailure;
			API::RenderHostContent content;
			std::shared_ptr<InSceneOverlay::HostedFrame> snapshot;
			try {
				snapshot = InSceneOverlay::PrepareHostedFrame(*frame, content, prepared);
			} catch (...) {
				prepared = Result::RenderFailure;
			}
			const bool ready = snapshot && prepared == Result::Success && content.layers;
			{
				std::scoped_lock lock(g_mutex);
				g_controller.FinishBegin(ready);
				if (ready)
					g_frame = std::move(snapshot);
			}
			if (!ready)
				return prepared == Result::Success ? Result::NoContent : prepared;
			out->cookie = cookie;
			out->layers = content.layers;
			out->worldQuadCount = content.worldQuadCount;
			return Result::Success;
		});
	}

	API::RenderHostResult RenderEye(std::uint64_t host, std::uint64_t cookie, const API::HostedEyeContext* eye, API::HostedEyeResult* out) noexcept
	{
		if (!ResetOutput(out) || !ValidStructure(eye) || !eye->target || eye->reserved)
			return Result::InvalidArgument;
		return Boundary([&] {
			winrt::com_ptr<ID3D11Resource> targetResource;
			eye->target->GetResource(targetResource.put());
			std::shared_ptr<InSceneOverlay::HostedFrame> snapshot;
			{
				std::scoped_lock lock(g_mutex);
				const auto result = g_controller.StartEye(host, cookie, GetCurrentThreadId(), *eye,
					reinterpret_cast<std::uintptr_t>(targetResource.get()));
				if (result != Result::Success)
					return result;
				snapshot = g_frame;
			}
			Result rendered = Result::RenderFailure;
			try {
				if (snapshot)
					rendered = InSceneOverlay::RenderHostedEye(*snapshot, *eye, *out);
			} catch (...) {
				out->targetWritten = 1;
				rendered = Result::PartialWrite;
			}
			if (out->targetWritten && rendered != Result::Success && rendered != Result::NoContent)
				rendered = Result::PartialWrite;
			{
				std::scoped_lock lock(g_mutex);
				g_controller.FinishEye(eye->eye, rendered);
			}
			return rendered;
		});
	}

	API::RenderHostResult End(std::uint64_t host, std::uint64_t cookie, bool abort) noexcept
	{
		return Boundary([&] {
			std::shared_ptr<InSceneOverlay::HostedFrame> released;
			Result result;
			{
				std::scoped_lock lock(g_mutex);
				result = g_controller.End(host, cookie, GetCurrentThreadId(), abort);
				if (result == Result::Success || result == Result::IncompletePair)
					released = std::move(g_frame);
			}
			return result;
		});
	}

	API::RenderHostResult Unregister(std::uint64_t host) noexcept
	{
		return Boundary([&] {
			std::scoped_lock lock(g_mutex);
			return g_controller.Unregister(host);
		});
	}
}
