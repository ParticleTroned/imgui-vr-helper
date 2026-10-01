// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include <catch2/catch_test_macros.hpp>

#include "internal/RenderHostController.h"

#include <limits>

namespace
{
	namespace API = ImGuiVRHelperPluginAPI;
	using Controller = ImGuiVRHelper::Internal::RenderHostController;
	using Result = API::RenderHostResult;
	constexpr std::uint32_t kRenderThread = 42;

	API::HostedFrameInfo Frame(std::uint64_t pair = 7)
	{
		API::HostedFrameInfo frame;
		frame.pairToken = pair;
		frame.sceneFrame = 100;
		frame.resourceGeneration = 3;
		frame.compositorCycle = 9;
		return frame;
	}

	API::HostedEyeContext Eye(std::uint32_t index, std::uint64_t attempt = 1)
	{
		API::HostedEyeContext eye;
		eye.eye = index;
		eye.pairToken = 7;
		eye.resourceGeneration = 3;
		eye.attemptId = attempt;
		return eye;
	}

	struct ActiveHost
	{
		Controller controller;
		std::uint64_t token = 0;
		ActiveHost()
		{
			REQUIRE(controller.Register(API::RenderHostKind::CSX, token) == Result::Success);
			controller.ObserveRenderThread(kRenderThread);
			REQUIRE(controller.SetActive(token, kRenderThread, true) == Result::Success);
		}
		std::uint64_t Begin()
		{
			std::uint64_t cookie = 0;
			REQUIRE(controller.Begin(token, kRenderThread, Frame(), cookie) == Result::Success);
			controller.FinishBegin(true);
			return cookie;
		}
	};
}

TEST_CASE("Render host registration is explicit and does not suppress legacy rendering", "[render-host]")
{
	Controller controller;
	std::uint64_t token = 99;
	REQUIRE(controller.Register(static_cast<API::RenderHostKind>(0), token) == Result::Unsupported);
	REQUIRE(token == 0);
	REQUIRE_FALSE(controller.IsActive());
	REQUIRE(controller.Register(API::RenderHostKind::CSX, token) == Result::Success);
	REQUIRE(token != 0);
	REQUIRE_FALSE(controller.IsActive());
	std::uint64_t duplicate = 99;
	REQUIRE(controller.Register(API::RenderHostKind::CSX, duplicate) == Result::Busy);
	REQUIRE(duplicate == 0);
	REQUIRE(controller.SetActive(token, kRenderThread, true) == Result::NotReady);
	controller.ObserveRenderThread(kRenderThread);
	controller.ObserveRenderThread(99);
	REQUIRE(controller.SetActive(token, 99, true) == Result::WrongThread);
	REQUIRE(controller.SetActive(token + 1, kRenderThread, true) == Result::InvalidToken);
	REQUIRE_FALSE(controller.IsActive());
	REQUIRE(controller.SetActive(token, kRenderThread, true) == Result::Success);
	REQUIRE(controller.Unregister(token) == Result::Busy);
	REQUIRE(controller.SetActive(token, kRenderThread, false) == Result::Success);
	REQUIRE(controller.Unregister(token) == Result::Success);
	std::uint64_t replacement = 0;
	REQUIRE(controller.Register(API::RenderHostKind::CSX, replacement) == Result::Success);
	REQUIRE(replacement != token);
	REQUIRE(controller.SetActive(token, kRenderThread, true) == Result::InvalidToken);
}

TEST_CASE("Open hosted frames reject nested work and lifecycle transitions", "[render-host]")
{
	ActiveHost host;
	std::uint64_t cookie = 0, nested = 0;
	REQUIRE(host.controller.Begin(host.token, kRenderThread, Frame(), cookie) == Result::Success);
	REQUIRE(host.controller.StartEye(host.token, cookie, kRenderThread, Eye(0), 100) == Result::Busy);
	REQUIRE(host.controller.End(host.token, cookie, kRenderThread, true) == Result::Busy);
	REQUIRE(host.controller.Begin(host.token, kRenderThread, Frame(8), nested) == Result::Busy);
	REQUIRE(nested == 0);
	REQUIRE(host.controller.SetActive(host.token, kRenderThread, false) == Result::Busy);
	REQUIRE(host.controller.Unregister(host.token) == Result::Busy);
	host.controller.FinishBegin(true);
	REQUIRE(host.controller.StartEye(host.token, cookie, 99, Eye(0), 100) == Result::WrongThread);
	REQUIRE(host.controller.StartEye(host.token, cookie + 1, kRenderThread, Eye(0), 100) == Result::StaleFrame);
	REQUIRE(host.controller.StartEye(host.token, cookie, kRenderThread, Eye(0), 100) == Result::Success);
	REQUIRE(host.controller.StartEye(host.token, cookie, kRenderThread, Eye(1), 200) == Result::Busy);
	REQUIRE(host.controller.End(host.token, cookie, kRenderThread, true) == Result::Busy);
	host.controller.FinishEye(0, Result::Success);
	REQUIRE(host.controller.End(host.token, cookie, kRenderThread, false) == Result::IncompletePair);
	REQUIRE_FALSE(host.controller.HasFrame());
	REQUIRE(host.controller.End(host.token, cookie, kRenderThread, true) == Result::StaleFrame);
	REQUIRE(host.controller.Begin(host.token, kRenderThread, Frame(), nested) == Result::StaleFrame);
}

TEST_CASE("Successful stereo composition closes without allowing duplicate alpha blending", "[render-host]")
{
	ActiveHost host;
	const auto cookie = host.Begin();
	REQUIRE(host.controller.StartEye(host.token, cookie, kRenderThread, Eye(1), 200) == Result::Success);
	host.controller.FinishEye(1, Result::Success);
	REQUIRE(host.controller.StartEye(host.token, cookie, kRenderThread, Eye(1, 2), 300) == Result::DuplicateAttempt);
	REQUIRE(host.controller.StartEye(host.token, cookie, kRenderThread, Eye(0), 100) == Result::Success);
	host.controller.FinishEye(0, Result::Success);
	REQUIRE(host.controller.End(host.token, cookie, kRenderThread, false) == Result::Success);
	REQUIRE_FALSE(host.controller.HasFrame());
	REQUIRE(host.controller.IsActive());
}

TEST_CASE("Closed pair tokens cannot be replayed with changed metadata or after reactivation", "[render-host]")
{
	ActiveHost host;
	const auto cookie = host.Begin();
	REQUIRE(host.controller.End(host.token, cookie, kRenderThread, true) == Result::Success);
	std::uint64_t replacement = 0;
	auto relabeled = Frame();
	++relabeled.sceneFrame;
	++relabeled.resourceGeneration;
	++relabeled.compositorCycle;
	REQUIRE(host.controller.Begin(host.token, kRenderThread, relabeled, replacement) == Result::StaleFrame);
	REQUIRE(replacement == 0);
	REQUIRE(host.controller.Begin(host.token, kRenderThread, Frame(8), replacement) == Result::Success);
	host.controller.FinishBegin(false);
	REQUIRE(host.controller.Begin(host.token, kRenderThread, Frame(), replacement) == Result::StaleFrame);
	REQUIRE(host.controller.SetActive(host.token, kRenderThread, false) == Result::Success);
	REQUIRE(host.controller.SetActive(host.token, kRenderThread, true) == Result::Success);
	REQUIRE(host.controller.Begin(host.token, kRenderThread, Frame(8), replacement) == Result::StaleFrame);
	REQUIRE(host.controller.Begin(host.token, kRenderThread, Frame(9), replacement) == Result::Success);
	host.controller.FinishBegin(false);
}

TEST_CASE("Partial targets require a fresh attempt and surface and bounded retries", "[render-host]")
{
	ActiveHost host;
	const auto cookie = host.Begin();
	REQUIRE(host.controller.StartEye(host.token, cookie, kRenderThread, Eye(0), 100) == Result::Success);
	host.controller.FinishEye(0, Result::PartialWrite);
	REQUIRE(host.controller.StartEye(host.token, cookie, kRenderThread, Eye(0), 101) == Result::DuplicateAttempt);
	REQUIRE(host.controller.StartEye(host.token, cookie, kRenderThread, Eye(0, 2), 100) == Result::DuplicateAttempt);
	for (std::uint32_t index = 1; index < Controller::kMaxAttemptsPerEye; ++index) {
		REQUIRE(host.controller.StartEye(host.token, cookie, kRenderThread, Eye(0, index + 1), 100 + index) == Result::Success);
		host.controller.FinishEye(0, Result::RenderFailure);
	}
	REQUIRE(host.controller.StartEye(host.token, cookie, kRenderThread, Eye(0, 99), 999) == Result::Busy);
	REQUIRE(host.controller.End(host.token, cookie, kRenderThread, true) == Result::Success);
	REQUIRE_FALSE(host.controller.HasFrame());
}

TEST_CASE("Stale generations and malformed eyes do not consume an attempt", "[render-host]")
{
	ActiveHost host;
	const auto cookie = host.Begin();
	auto eye = Eye(0);
	++eye.resourceGeneration;
	REQUIRE(host.controller.StartEye(host.token, cookie, kRenderThread, eye, 100) == Result::StaleFrame);
	eye = Eye(0);
	++eye.pairToken;
	REQUIRE(host.controller.StartEye(host.token, cookie, kRenderThread, eye, 100) == Result::StaleFrame);
	REQUIRE(host.controller.StartEye(host.token, cookie, kRenderThread, Eye(2), 100) == Result::InvalidArgument);
	REQUIRE(host.controller.StartEye(host.token, cookie, kRenderThread, Eye(0, 0), 100) == Result::InvalidArgument);
	REQUIRE(host.controller.StartEye(host.token, cookie, kRenderThread, Eye(0), 0) == Result::InvalidArgument);
	REQUIRE(host.controller.StartEye(host.token, cookie, kRenderThread, Eye(0), 100) == Result::Success);
	host.controller.FinishEye(0, Result::NoContent);
	REQUIRE(host.controller.End(host.token, cookie, kRenderThread, true) == Result::Success);
}

TEST_CASE("Failed preparation closes its frame and identity sentinels are rejected", "[render-host]")
{
	ActiveHost host;
	std::uint64_t cookie = 0;
	for (std::uint64_t invalidFrame : { 0ull, static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()), std::numeric_limits<std::uint64_t>::max() }) {
		auto frame = Frame();
		frame.sceneFrame = invalidFrame;
		REQUIRE(host.controller.Begin(host.token, kRenderThread, frame, cookie) == Result::InvalidArgument);
		REQUIRE(cookie == 0);
	}
	REQUIRE(host.controller.Begin(host.token, kRenderThread, Frame(), cookie) == Result::Success);
	host.controller.FinishBegin(false);
	REQUIRE_FALSE(host.controller.HasFrame());
	REQUIRE(host.controller.CheckFrame(host.token, cookie, kRenderThread) == Result::StaleFrame);
	REQUIRE(host.controller.SetActive(host.token, kRenderThread, false) == Result::Success);
	REQUIRE(host.controller.Begin(host.token, kRenderThread, Frame(8), cookie) == Result::Inactive);
}

TEST_CASE("Renderer host POD layout is fixed for both DLLs", "[render-host][abi]")
{
	STATIC_REQUIRE(sizeof(API::HostedFrameInfo) == 1120);
	STATIC_REQUIRE(sizeof(API::HostedEyeScene) == 392);
	STATIC_REQUIRE(sizeof(API::HostedEyeContext) == 48);
	STATIC_REQUIRE(offsetof(API::HostedFrameInfo, eyes) == 336);
	STATIC_REQUIRE(offsetof(API::HostedEyeScene, trackingToColorClip) == 104);
	STATIC_REQUIRE(std::is_standard_layout_v<API::HostedFrameInfo>);
	STATIC_REQUIRE(std::is_trivially_copyable_v<API::HostedEyeScene>);
}
