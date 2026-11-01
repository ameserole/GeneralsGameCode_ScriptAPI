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

#include "ScriptApiRouter.h"

#include <stdexcept>

using scriptapi::Json;

namespace
{

scriptapi::CommandRouter makeRouter()
{
	scriptapi::CommandRouter router;
	router.add("echo", [](const scriptapi::Request &request) { return Json{{"value", request.body.value("value", 0)}}; });
	router.add("nothing", [](const scriptapi::Request &) { return Json(); });
	router.add("refuse", [](const scriptapi::Request &) -> Json {
		throw scriptapi::CommandError(scriptapi::ErrorCode::ProtocolMismatch, "wrong version");
	});
	router.add("explode", [](const scriptapi::Request &) -> Json { throw std::runtime_error("boom"); });
	router.add("hijack", [](const scriptapi::Request &) { return Json{{"ok", false}, {"reply_to", 123}}; });
	return router;
}

} // namespace

TEST(ScriptApiRouter, RoutesToHandlerAndMergesFields)
{
	const Json reply = Json::parse(makeRouter().dispatch(R"({"id": 1, "cmd": "echo", "value": 5})"));
	EXPECT_EQ(reply["ok"], true);
	EXPECT_EQ(reply["reply_to"], 1);
	EXPECT_EQ(reply["value"], 5);
}

TEST(ScriptApiRouter, NullFieldsGiveBareReply)
{
	const Json reply = Json::parse(makeRouter().dispatch(R"({"id": 2, "cmd": "nothing"})"));
	EXPECT_EQ(reply["ok"], true);
	EXPECT_EQ(reply.size(), 3u);
}

TEST(ScriptApiRouter, UnknownCommand)
{
	const Json reply = Json::parse(makeRouter().dispatch(R"({"id": 3, "cmd": "dance"})"));
	EXPECT_EQ(reply["ok"], false);
	EXPECT_EQ(reply["error"], "UNKNOWN_COMMAND");
	EXPECT_EQ(reply["reply_to"], 3);
}

TEST(ScriptApiRouter, CommandErrorUsesItsCode)
{
	const Json reply = Json::parse(makeRouter().dispatch(R"({"id": 4, "cmd": "refuse"})"));
	EXPECT_EQ(reply["error"], "PROTOCOL_MISMATCH");
	EXPECT_EQ(reply["message"], "wrong version");
}

TEST(ScriptApiRouter, OtherExceptionsAreInternalError)
{
	const Json reply = Json::parse(makeRouter().dispatch(R"({"id": 5, "cmd": "explode"})"));
	EXPECT_EQ(reply["error"], "INTERNAL_ERROR");
	EXPECT_EQ(reply["message"], "boom");
}

TEST(ScriptApiRouter, HandlerCannotOverwriteEnvelope)
{
	const Json reply = Json::parse(makeRouter().dispatch(R"({"id": 6, "cmd": "hijack"})"));
	EXPECT_EQ(reply["ok"], true);
	EXPECT_EQ(reply["reply_to"], 6);
}

TEST(ScriptApiRouter, ParseErrorsBecomeErrorReplies)
{
	const scriptapi::CommandRouter router = makeRouter();
	EXPECT_EQ(Json::parse(router.dispatch("garbage"))["error"], "BAD_JSON");
	EXPECT_EQ(Json::parse(router.dispatch(R"({"id": 7})"))["error"], "BAD_REQUEST");
	EXPECT_EQ(Json::parse(router.dispatch(R"({"id": 7})"))["reply_to"], 7);
}

TEST(ScriptApiRouter, Has)
{
	const scriptapi::CommandRouter router = makeRouter();
	EXPECT_TRUE(router.has("echo"));
	EXPECT_FALSE(router.has("dance"));
}
