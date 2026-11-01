/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <gtest/gtest.h>

#include "ScriptApiCommands.h"

using scriptapi::ActionRateLimiter;
using scriptapi::CommandTracker;
using scriptapi::Json;

// -- CommandTracker::orphanAll ---------------------------------------------------------------------

TEST(ScriptApiHardening, OrphanedRequestsConsumeMessagesSilently)
{
	CommandTracker tracker;
	tracker.add(Json(1), "move", {100});
	tracker.add(Json(2), "train", {200, 200});
	tracker.orphanAll();
	tracker.add(Json(3), "stop", {300});

	CommandTracker::Completion done;
	EXPECT_FALSE(tracker.onExecuted(100, done));
	EXPECT_FALSE(tracker.onExecuted(200, done));
	EXPECT_FALSE(tracker.onExecuted(200, done));
	EXPECT_EQ(tracker.pending(), 1u);
	ASSERT_TRUE(tracker.onExecuted(300, done));
	EXPECT_EQ(done.requestId, Json(3));
	EXPECT_EQ(done.cmd, "stop");
	EXPECT_EQ(tracker.pending(), 0u);
}

// -- ActionRateLimiter -----------------------------------------------------------------------------

TEST(ScriptApiHardening, NoCapAllowsEverything)
{
	ActionRateLimiter limiter;
	for (uint32_t i = 0; i < 1000; ++i)
	{
		ASSERT_TRUE(limiter.allows(5));
		limiter.record(5);
	}
	EXPECT_EQ(limiter.framesUntilAvailable(5), 0u);
}

TEST(ScriptApiHardening, CapCountsActionsInTheWindow)
{
	ActionRateLimiter limiter(3);
	for (uint32_t frame : {10u, 10u, 20u})
	{
		ASSERT_TRUE(limiter.allows(frame));
		limiter.record(frame);
	}
	EXPECT_FALSE(limiter.allows(30));
	EXPECT_EQ(limiter.framesUntilAvailable(30), 10u + ActionRateLimiter::kDefaultWindowFrames - 30u);
}

TEST(ScriptApiHardening, WindowSlides)
{
	ActionRateLimiter limiter(2, 100);
	limiter.record(0);
	limiter.record(50);
	EXPECT_FALSE(limiter.allows(99));
	EXPECT_EQ(limiter.framesUntilAvailable(99), 1u);
	EXPECT_TRUE(limiter.allows(100)); // the action at frame 0 has left the window
	limiter.record(100);
	EXPECT_FALSE(limiter.allows(149));
	EXPECT_TRUE(limiter.allows(150));
}

TEST(ScriptApiHardening, SetLimitResetsAndZeroTurnsTheCapOff)
{
	ActionRateLimiter limiter(1);
	limiter.record(0);
	EXPECT_FALSE(limiter.allows(1));
	limiter.setLimit(1);
	EXPECT_TRUE(limiter.allows(1));
	limiter.setLimit(0);
	EXPECT_EQ(limiter.limit(), 0);
	limiter.record(1);
	EXPECT_TRUE(limiter.allows(1));
	limiter.setLimit(-5);
	EXPECT_EQ(limiter.limit(), 0);
}

// -- parseReplayName -------------------------------------------------------------------------------

TEST(ScriptApiHardening, ReplayNamesAccepted)
{
	EXPECT_EQ(scriptapi::parseReplayName("match"), "match");
	EXPECT_EQ(scriptapi::parseReplayName("20261031-120000_simple_vs_easy-ai"), "20261031-120000_simple_vs_easy-ai");
	EXPECT_EQ(scriptapi::parseReplayName("Bot (v2) game.1"), "Bot (v2) game.1");
	EXPECT_EQ(scriptapi::parseReplayName("match.rep"), "match");
	EXPECT_EQ(scriptapi::parseReplayName("match.REP"), "match");
	EXPECT_EQ(scriptapi::parseReplayName(std::string(64, 'a')), std::string(64, 'a'));
	EXPECT_EQ(scriptapi::parseReplayName(std::string(64, 'a') + ".rep"), std::string(64, 'a'));
}

TEST(ScriptApiHardening, ReplayNamesRejected)
{
	for (const std::string &name : {std::string(""), std::string(".rep"), std::string(65, 'a'), std::string("a/b"),
		std::string("a\\b"), std::string("..\\x"), std::string("c:x"), std::string(".hidden"), std::string("dot."),
		std::string(" lead"), std::string("trail "), std::string("a*b"), std::string("a?b"), std::string("a\"b"),
		std::string("caf\xc3\xa9"), std::string("tab\tname")})
	{
		try
		{
			scriptapi::parseReplayName(name);
			ADD_FAILURE() << "accepted \"" << name << "\"";
		}
		catch (const scriptapi::CommandError &error)
		{
			EXPECT_EQ(error.code(), scriptapi::ErrorCode::InvalidArgument) << name;
		}
	}
}

TEST(ScriptApiHardening, RateLimitedErrorName)
{
	EXPECT_STREQ(scriptapi::errorCodeName(scriptapi::ErrorCode::RateLimited), "RATE_LIMITED");
}
