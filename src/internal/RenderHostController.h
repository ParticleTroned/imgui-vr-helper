// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include "ImGuiVRHelperRenderHost.h"

#include <array>
#include <cstdint>
#include <limits>

namespace ImGuiVRHelper::Internal
{
	/// Serialized ownership policy; the DLL adapter supplies synchronization and the observed render thread.
	class RenderHostController
	{
	public:
		using Result = ImGuiVRHelperPluginAPI::RenderHostResult;
		static constexpr std::uint32_t kMaxAttemptsPerEye = 4;

		/// Follow the game's Present thread only between frames; an open pair keeps its owner.
		void ObserveRenderThread(std::uint32_t thread) noexcept
		{
			if (thread && !HasFrame())
				renderThread_ = thread;
		}

		[[nodiscard]] bool IsActive() const noexcept { return active_; }
		[[nodiscard]] bool HasFrame() const noexcept { return phase_ != Phase::Idle; }
		[[nodiscard]] std::uint64_t Cookie() const noexcept { return cookie_; }

		Result Register(ImGuiVRHelperPluginAPI::RenderHostKind kind, std::uint64_t& token) noexcept
		{
			token = 0;
			if (kind != ImGuiVRHelperPluginAPI::RenderHostKind::CSX)
				return Result::Unsupported;
			if (host_)
				return Result::Busy;
			host_ = Next(nextHost_);
			token = host_;
			return Result::Success;
		}

		Result CheckHost(std::uint64_t token, std::uint32_t thread, bool requireActive = true) const noexcept
		{
			if (!token || token != host_)
				return Result::InvalidToken;
			if (!renderThread_)
				return Result::NotReady;
			if (thread != renderThread_)
				return Result::WrongThread;
			if (requireActive && !active_)
				return Result::Inactive;
			return Result::Success;
		}

		Result SetActive(std::uint64_t token, std::uint32_t thread, bool active) noexcept
		{
			const auto check = CheckHost(token, thread, false);
			if (check != Result::Success)
				return check;
			if (HasFrame())
				return Result::Busy;
			active_ = active;
			return Result::Success;
		}

		Result Unregister(std::uint64_t token) noexcept
		{
			if (!token || token != host_)
				return Result::InvalidToken;
			if (active_ || HasFrame())
				return Result::Busy;
			host_ = 0;
			lastPair_ = {};
			return Result::Success;
		}

		Result Begin(std::uint64_t token, std::uint32_t thread,
			const ImGuiVRHelperPluginAPI::HostedFrameInfo& frame, std::uint64_t& cookie) noexcept
		{
			cookie = 0;
			const auto check = CheckHost(token, thread);
			if (check != Result::Success)
				return check;
			if (HasFrame())
				return Result::Busy;
			if (!frame.pairToken || !frame.sceneFrame || !frame.resourceGeneration || !frame.compositorCycle ||
				frame.sceneFrame == std::numeric_limits<std::uint32_t>::max() ||
				frame.sceneFrame == std::numeric_limits<std::uint64_t>::max())
				return Result::InvalidArgument;
			const Pair pair{ frame.pairToken, frame.sceneFrame, frame.resourceGeneration, frame.compositorCycle };
			if (pair.token <= lastPair_.token)
				return Result::StaleFrame;
			pair_ = pair;
			cookie_ = Next(nextCookie_);
			cookie = cookie_;
			phase_ = Phase::Preparing;
			completedEyes_ = 0;
			attemptCounts_ = {};
			attempts_ = {};
			return Result::Success;
		}

		void FinishBegin(bool prepared) noexcept
		{
			if (prepared)
				phase_ = Phase::Ready;
			else
				Close();
		}

		Result CheckFrame(std::uint64_t token, std::uint64_t cookie, std::uint32_t thread) const noexcept
		{
			const auto check = CheckHost(token, thread);
			if (check != Result::Success)
				return check;
			if (!cookie || cookie != cookie_ || !HasFrame())
				return Result::StaleFrame;
			return Result::Success;
		}

		Result StartEye(std::uint64_t token, std::uint64_t cookie, std::uint32_t thread,
			const ImGuiVRHelperPluginAPI::HostedEyeContext& eye, std::uintptr_t resourceIdentity) noexcept
		{
			const auto check = CheckFrame(token, cookie, thread);
			if (check != Result::Success)
				return check;
			if (phase_ != Phase::Ready)
				return Result::Busy;
			if (eye.eye > 1 || !eye.attemptId || !resourceIdentity)
				return Result::InvalidArgument;
			if (eye.pairToken != pair_.token || eye.resourceGeneration != pair_.generation)
				return Result::StaleFrame;
			if (completedEyes_ & (1u << eye.eye))
				return Result::DuplicateAttempt;
			for (std::uint32_t index = 0; index < attemptCounts_[eye.eye]; ++index) {
				const auto& attempt = attempts_[eye.eye][index];
				if (attempt.id == eye.attemptId || attempt.resource == resourceIdentity)
					return Result::DuplicateAttempt;
			}
			if (attemptCounts_[eye.eye] == kMaxAttemptsPerEye)
				return Result::Busy;
			attempts_[eye.eye][attemptCounts_[eye.eye]++] = { eye.attemptId, resourceIdentity };
			phase_ = Phase::Rendering;
			return Result::Success;
		}

		void FinishEye(std::uint32_t eye, Result result) noexcept
		{
			if (result == Result::Success || result == Result::NoContent)
				completedEyes_ |= 1u << eye;
			phase_ = Phase::Ready;
		}

		Result End(std::uint64_t token, std::uint64_t cookie, std::uint32_t thread, bool abort) noexcept
		{
			const auto check = CheckFrame(token, cookie, thread);
			if (check != Result::Success)
				return check;
			if (phase_ != Phase::Ready)
				return Result::Busy;
			const auto result = abort || completedEyes_ == 3u ? Result::Success : Result::IncompletePair;
			Close();
			return result;
		}

	private:
		enum class Phase
		{
			Idle,
			Preparing,
			Ready,
			Rendering
		};
		struct Pair
		{
			std::uint64_t token = 0, frame = 0, generation = 0, cycle = 0;
			bool operator==(const Pair&) const = default;
		};
		struct Attempt
		{
			std::uint64_t id = 0;
			std::uintptr_t resource = 0;
		};
		static std::uint64_t Next(std::uint64_t& counter) noexcept
		{
			if (++counter == 0)
				++counter;
			return counter;
		}
		void Close() noexcept
		{
			lastPair_ = pair_;
			pair_ = {};
			cookie_ = 0;
			phase_ = Phase::Idle;
		}
		std::uint64_t host_ = 0, nextHost_ = 0, cookie_ = 0, nextCookie_ = 0;
		std::uint32_t renderThread_ = 0, completedEyes_ = 0;
		bool active_ = false;
		Phase phase_ = Phase::Idle;
		Pair pair_{}, lastPair_{};
		std::array<std::uint32_t, 2> attemptCounts_{};
		std::array<std::array<Attempt, kMaxAttemptsPerEye>, 2> attempts_{};
	};
}
