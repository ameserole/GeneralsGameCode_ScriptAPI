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

using scriptapi::ErrorCode;
using scriptapi::InputAction;
using scriptapi::InputKind;
using scriptapi::InputMode;
using scriptapi::Json;

namespace
{

template <typename Fn>
void expectBadRequest(Fn fn)
{
	try
	{
		fn();
		ADD_FAILURE() << "expected BAD_REQUEST";
	}
	catch (const scriptapi::CommandError &error)
	{
		EXPECT_EQ(error.code(), ErrorCode::BadRequest);
	}
}

} // namespace

// -- Arguments -----------------------------------------------------------------------------------

TEST(ScriptApiCommands, ParseObjectIds)
{
	const Json body{{"units", Json::array({5, 3, 5, 9})}};
	EXPECT_EQ(scriptapi::parseObjectIds(body, "units"), (std::vector<uint32_t>{5, 3, 9}));
}

TEST(ScriptApiCommands, ParseObjectIdsRejectsBadInput)
{
	expectBadRequest([] { scriptapi::parseObjectIds(Json::object(), "units"); });
	expectBadRequest([] { scriptapi::parseObjectIds(Json{{"units", Json::array()}}, "units"); });
	expectBadRequest([] { scriptapi::parseObjectIds(Json{{"units", 5}}, "units"); });
	expectBadRequest([] { scriptapi::parseObjectIds(Json{{"units", Json::array({1, "two"})}}, "units"); });
	expectBadRequest([] { scriptapi::parseObjectIds(Json{{"units", Json::array({-1})}}, "units"); });

	Json tooMany = Json::array();
	for (size_t i = 0; i <= scriptapi::kMaxUnitsPerCommand; ++i)
		tooMany.push_back(i + 1);
	expectBadRequest([&] { scriptapi::parseObjectIds(Json{{"units", tooMany}}, "units"); });
}

TEST(ScriptApiCommands, ParseObjectId)
{
	EXPECT_EQ(scriptapi::parseObjectId(Json{{"target", 42}}, "target"), 42u);
	expectBadRequest([] { scriptapi::parseObjectId(Json::object(), "target"); });
	expectBadRequest([] { scriptapi::parseObjectId(Json{{"target", "x"}}, "target"); });
	expectBadRequest([] { scriptapi::parseObjectId(Json{{"target", 0}}, "target"); });
}

TEST(ScriptApiCommands, ParsePosition)
{
	scriptapi::Position pos = scriptapi::parsePosition(Json{{"pos", Json::array({10.5, 20})}}, "pos");
	EXPECT_FLOAT_EQ(pos.x, 10.5f);
	EXPECT_FLOAT_EQ(pos.y, 20.0f);
	pos = scriptapi::parsePosition(Json{{"pos", Json::array({1, 2, 3})}}, "pos");
	EXPECT_FLOAT_EQ(pos.y, 2.0f);

	expectBadRequest([] { scriptapi::parsePosition(Json::object(), "pos"); });
	expectBadRequest([] { scriptapi::parsePosition(Json{{"pos", Json::array({1})}}, "pos"); });
	expectBadRequest([] { scriptapi::parsePosition(Json{{"pos", Json::array({1, 2, 3, 4})}}, "pos"); });
	expectBadRequest([] { scriptapi::parsePosition(Json{{"pos", Json::array({1, "y"})}}, "pos"); });
}

// -- Input lock ----------------------------------------------------------------------------------

TEST(ScriptApiInput, ModeNames)
{
	EXPECT_EQ(scriptapi::parseInputMode("full"), InputMode::Full);
	EXPECT_EQ(scriptapi::parseInputMode("view"), InputMode::View);
	EXPECT_EQ(scriptapi::parseInputMode("locked"), InputMode::Locked);
	expectBadRequest([] { scriptapi::parseInputMode("open"); });
	EXPECT_STREQ(scriptapi::inputModeName(InputMode::View), "view");
	EXPECT_STREQ(scriptapi::inputModeName(InputMode::Locked), "locked");
	EXPECT_STREQ(scriptapi::inputModeName(InputMode::Full), "full");
}

TEST(ScriptApiInput, FullPassesEverything)
{
	for (InputKind kind : {InputKind::MousePosition, InputKind::MouseLeft, InputKind::MouseRight,
			InputKind::MouseMiddle, InputKind::MouseWheel, InputKind::Key})
		EXPECT_EQ(scriptapi::inputAction(InputMode::Full, kind), InputAction::Pass);
}

TEST(ScriptApiInput, LockedDropsEverything)
{
	for (InputKind kind : {InputKind::MousePosition, InputKind::MouseLeft, InputKind::MouseRight,
			InputKind::MouseMiddle, InputKind::MouseWheel, InputKind::Key})
		EXPECT_EQ(scriptapi::inputAction(InputMode::Locked, kind), InputAction::Drop);
}

TEST(ScriptApiInput, ViewAllowsOnlyCameraControl)
{
	EXPECT_EQ(scriptapi::inputAction(InputMode::View, InputKind::MousePosition), InputAction::Pass);
	EXPECT_EQ(scriptapi::inputAction(InputMode::View, InputKind::MouseLeft), InputAction::Drop);
	EXPECT_EQ(scriptapi::inputAction(InputMode::View, InputKind::MouseRight), InputAction::CameraOnly);
	EXPECT_EQ(scriptapi::inputAction(InputMode::View, InputKind::MouseMiddle), InputAction::CameraOnly);
	EXPECT_EQ(scriptapi::inputAction(InputMode::View, InputKind::MouseWheel), InputAction::CameraOnly);
	EXPECT_EQ(scriptapi::inputAction(InputMode::View, InputKind::Key), InputAction::CameraOnly);
}

// -- CommandTracker ------------------------------------------------------------------------------

TEST(ScriptApiCommandTracker, SingleMessageRequest)
{
	scriptapi::CommandTracker tracker;
	tracker.add(Json(1), "move", {100});
	scriptapi::CommandTracker::Completion done;
	ASSERT_TRUE(tracker.onExecuted(100, done));
	EXPECT_EQ(done.requestId, Json(1));
	EXPECT_EQ(done.cmd, "move");
	EXPECT_EQ(tracker.pending(), 0u);
}

TEST(ScriptApiCommandTracker, MultiMessageRequestCompletesOnLast)
{
	scriptapi::CommandTracker tracker;
	tracker.add(Json("t"), "train", {200, 200, 200});
	scriptapi::CommandTracker::Completion done;
	EXPECT_FALSE(tracker.onExecuted(200, done));
	EXPECT_FALSE(tracker.onExecuted(200, done));
	ASSERT_TRUE(tracker.onExecuted(200, done));
	EXPECT_EQ(done.requestId, Json("t"));
}

TEST(ScriptApiCommandTracker, UnrelatedMessagesAreIgnored)
{
	scriptapi::CommandTracker tracker;
	tracker.add(Json(1), "move", {100});
	scriptapi::CommandTracker::Completion done;
	EXPECT_FALSE(tracker.onExecuted(999, done)); // e.g. a selection or camera message
	EXPECT_EQ(tracker.pending(), 1u);
	EXPECT_TRUE(tracker.onExecuted(100, done));
}

TEST(ScriptApiCommandTracker, RequestsCompleteInOrder)
{
	scriptapi::CommandTracker tracker;
	tracker.add(Json(1), "move", {100});
	tracker.add(Json(2), "stop", {101});
	scriptapi::CommandTracker::Completion done;
	EXPECT_FALSE(tracker.onExecuted(101, done)); // not the head of the queue
	ASSERT_TRUE(tracker.onExecuted(100, done));
	EXPECT_EQ(done.requestId, Json(1));
	ASSERT_TRUE(tracker.onExecuted(101, done));
	EXPECT_EQ(done.requestId, Json(2));
}

TEST(ScriptApiCommandTracker, ClearDropsPending)
{
	scriptapi::CommandTracker tracker;
	tracker.add(Json(1), "move", {100});
	tracker.clear();
	scriptapi::CommandTracker::Completion done;
	EXPECT_FALSE(tracker.onExecuted(100, done));
	EXPECT_EQ(tracker.pending(), 0u);
}

TEST(ScriptApiCommands, NewErrorCodeNames)
{
	EXPECT_STREQ(scriptapi::errorCodeName(ErrorCode::NotOwned), "NOT_OWNED");
	EXPECT_STREQ(scriptapi::errorCodeName(ErrorCode::NotVisible), "NOT_VISIBLE");
	EXPECT_STREQ(scriptapi::errorCodeName(ErrorCode::NotAvailable), "NOT_AVAILABLE");
	EXPECT_STREQ(scriptapi::errorCodeName(ErrorCode::InsufficientFunds), "INSUFFICIENT_FUNDS");
	EXPECT_STREQ(scriptapi::errorCodeName(ErrorCode::QueueFull), "QUEUE_FULL");
	EXPECT_STREQ(scriptapi::errorCodeName(ErrorCode::ParkingFull), "PARKING_FULL");
	EXPECT_STREQ(scriptapi::errorCodeName(ErrorCode::LimitReached), "LIMIT_REACHED");
	EXPECT_STREQ(scriptapi::errorCodeName(ErrorCode::IllegalLocation), "ILLEGAL_LOCATION");
	EXPECT_STREQ(scriptapi::errorCodeName(ErrorCode::CannotAttack), "CANNOT_ATTACK");
}
