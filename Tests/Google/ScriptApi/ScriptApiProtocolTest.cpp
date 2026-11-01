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

#include "ScriptApiProtocol.h"

using scriptapi::ErrorCode;
using scriptapi::Json;

TEST(ScriptApiProtocol, ParsesRequestWithIntegerId)
{
	const scriptapi::ParseResult result = scriptapi::parseRequest(R"({"id": 7, "cmd": "ping", "x": 1})");
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.request.id, Json(7));
	EXPECT_EQ(result.request.cmd, "ping");
	EXPECT_EQ(result.request.body["x"], 1);
}

TEST(ScriptApiProtocol, ParsesRequestWithStringId)
{
	const scriptapi::ParseResult result = scriptapi::parseRequest(R"({"id": "abc", "cmd": "hello"})");
	ASSERT_TRUE(result.ok);
	EXPECT_EQ(result.request.id, Json("abc"));
}

TEST(ScriptApiProtocol, MissingIdIsNull)
{
	const scriptapi::ParseResult result = scriptapi::parseRequest(R"({"cmd": "ping"})");
	ASSERT_TRUE(result.ok);
	EXPECT_TRUE(result.request.id.is_null());
}

TEST(ScriptApiProtocol, InvalidJsonIsBadJson)
{
	const scriptapi::ParseResult result = scriptapi::parseRequest("{not json");
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.error, ErrorCode::BadJson);
	EXPECT_FALSE(result.message.empty());
}

TEST(ScriptApiProtocol, NonObjectIsBadRequest)
{
	const scriptapi::ParseResult result = scriptapi::parseRequest("[1, 2, 3]");
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.error, ErrorCode::BadRequest);
}

TEST(ScriptApiProtocol, MissingOrNonStringCmdIsBadRequest)
{
	EXPECT_EQ(scriptapi::parseRequest(R"({"id": 1})").error, ErrorCode::BadRequest);
	EXPECT_EQ(scriptapi::parseRequest(R"({"id": 1, "cmd": 5})").error, ErrorCode::BadRequest);
}

TEST(ScriptApiProtocol, BadRequestKeepsIdForReply)
{
	const scriptapi::ParseResult result = scriptapi::parseRequest(R"({"id": 9})");
	EXPECT_FALSE(result.ok);
	EXPECT_EQ(result.request.id, Json(9));
}

TEST(ScriptApiProtocol, ReplyHasEnvelopeAndFields)
{
	const Json reply = Json::parse(scriptapi::makeReply(Json(3), Json{{"frame", 42}}));
	EXPECT_EQ(reply["type"], "reply");
	EXPECT_EQ(reply["reply_to"], 3);
	EXPECT_EQ(reply["ok"], true);
	EXPECT_EQ(reply["frame"], 42);
}

TEST(ScriptApiProtocol, ReplyEnvelopeCannotBeOverwritten)
{
	const Json reply = Json::parse(scriptapi::makeReply(Json(3), Json{{"ok", false}, {"type", "x"}, {"reply_to", 99}}));
	EXPECT_EQ(reply["type"], "reply");
	EXPECT_EQ(reply["reply_to"], 3);
	EXPECT_EQ(reply["ok"], true);
}

TEST(ScriptApiProtocol, ReplyToNullWhenNoId)
{
	const Json reply = Json::parse(scriptapi::makeReply(Json()));
	ASSERT_TRUE(reply.contains("reply_to"));
	EXPECT_TRUE(reply["reply_to"].is_null());
}

TEST(ScriptApiProtocol, ErrorReply)
{
	const Json reply = Json::parse(scriptapi::makeError(Json("q"), ErrorCode::UnknownCommand, "nope"));
	EXPECT_EQ(reply["type"], "reply");
	EXPECT_EQ(reply["reply_to"], "q");
	EXPECT_EQ(reply["ok"], false);
	EXPECT_EQ(reply["error"], "UNKNOWN_COMMAND");
	EXPECT_EQ(reply["message"], "nope");
}

TEST(ScriptApiProtocol, EventHasEnvelopeAndFields)
{
	const Json event = Json::parse(scriptapi::makeEvent("game_state", 120, Json{{"state", "shell"}}));
	EXPECT_EQ(event["type"], "event");
	EXPECT_EQ(event["event"], "game_state");
	EXPECT_EQ(event["frame"], 120);
	EXPECT_EQ(event["state"], "shell");
}

TEST(ScriptApiProtocol, ErrorCodeNames)
{
	EXPECT_STREQ(scriptapi::errorCodeName(ErrorCode::BadJson), "BAD_JSON");
	EXPECT_STREQ(scriptapi::errorCodeName(ErrorCode::BadRequest), "BAD_REQUEST");
	EXPECT_STREQ(scriptapi::errorCodeName(ErrorCode::UnknownCommand), "UNKNOWN_COMMAND");
	EXPECT_STREQ(scriptapi::errorCodeName(ErrorCode::InternalError), "INTERNAL_ERROR");
	EXPECT_STREQ(scriptapi::errorCodeName(ErrorCode::ProtocolMismatch), "PROTOCOL_MISMATCH");
}
