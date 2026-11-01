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
#include "ScriptApiRouter.h"

using scriptapi::CommandRouter;
using scriptapi::CommandTracker;
using scriptapi::ConnectionRole;
using scriptapi::Json;

// -- roles in the router ---------------------------------------------------------------------------

namespace
{

CommandRouter makeRouter()
{
	CommandRouter router;
	router.add("look", [](const scriptapi::Request &request) {
		return Json{{"connection", request.connection}, {"role", scriptapi::roleName(request.role)}};
	});
	router.add("quit", [](const scriptapi::Request &) { return Json(); }, CommandRouter::Access::ControllerOnly);
	return router;
}

} // namespace

TEST(ScriptApiLive, RequestsCarryTheirConnectionAndRole)
{
	const Json reply = Json::parse(makeRouter().dispatch(R"({"id": 1, "cmd": "look"})", 7, ConnectionRole::Watcher));
	EXPECT_EQ(reply["connection"], 7);
	EXPECT_EQ(reply["role"], "watcher");
}

TEST(ScriptApiLive, ControllerOnlyCommandsRefuseWatchers)
{
	const CommandRouter router = makeRouter();
	const Json refused = Json::parse(router.dispatch(R"({"id": 1, "cmd": "quit"})", 7, ConnectionRole::Watcher));
	EXPECT_EQ(refused["ok"], false);
	EXPECT_EQ(refused["error"], "WRONG_ROLE");

	const Json accepted = Json::parse(router.dispatch(R"({"id": 2, "cmd": "quit"})", 3, ConnectionRole::Controller));
	EXPECT_EQ(accepted["ok"], true);
}

TEST(ScriptApiLive, AccessCanBeSetAfterRegistering)
{
	CommandRouter router = makeRouter();
	router.setAccess("look", CommandRouter::Access::ControllerOnly);
	router.setAccess("missing", CommandRouter::Access::ControllerOnly); // ignored
	const Json reply = Json::parse(router.dispatch(R"({"id": 1, "cmd": "look"})", 7, ConnectionRole::Watcher));
	EXPECT_EQ(reply["error"], "WRONG_ROLE");
	EXPECT_FALSE(router.has("missing"));
}

TEST(ScriptApiLive, DispatchDefaultsToTheController)
{
	const Json reply = Json::parse(makeRouter().dispatch(R"({"id": 1, "cmd": "quit"})"));
	EXPECT_EQ(reply["ok"], true);
}

// -- command tracking per connection ---------------------------------------------------------------

TEST(ScriptApiLive, CompletionsGoBackToTheSender)
{
	CommandTracker tracker;
	tracker.add(Json(1), "move", {100}, /*connection*/ 3);
	tracker.add(Json(1), "attack", {200}, /*connection*/ 5);  // same request id, another client

	CommandTracker::Completion done;
	ASSERT_TRUE(tracker.onExecuted(100, done));
	EXPECT_EQ(done.connection, 3u);
	EXPECT_EQ(done.cmd, "move");
	ASSERT_TRUE(tracker.onExecuted(200, done));
	EXPECT_EQ(done.connection, 5u);
}

TEST(ScriptApiLive, ClosingAConnectionOrphansOnlyItsOrders)
{
	CommandTracker tracker;
	tracker.add(Json(1), "move", {100}, 3);
	tracker.add(Json(2), "stop", {300}, 5);
	tracker.orphanConnection(3);

	CommandTracker::Completion done;
	EXPECT_FALSE(tracker.onExecuted(100, done));  // consumed, not reported
	ASSERT_TRUE(tracker.onExecuted(300, done));
	EXPECT_EQ(done.connection, 5u);
	EXPECT_EQ(tracker.pending(), 0u);
}

TEST(ScriptApiLive, WrongRoleHasAName)
{
	EXPECT_STREQ(scriptapi::errorCodeName(scriptapi::ErrorCode::WrongRole), "WRONG_ROLE");
	EXPECT_STREQ(scriptapi::roleName(ConnectionRole::Controller), "controller");
}
