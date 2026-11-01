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

#include "ScriptApiSkirmish.h"

using scriptapi::ErrorCode;
using scriptapi::Json;
using scriptapi::SlotType;

namespace
{

Json minimalRequest()
{
	return Json{
		{"cmd", "start_skirmish"},
		{"map", "maps\\test\\test.map"},
		{"players", Json::array({Json{{"type", "script"}}, Json{{"type", "easy_ai"}}})},
	};
}

// Expects parsing to fail with BAD_REQUEST.
void expectBadRequest(const Json &body)
{
	try
	{
		scriptapi::parseSkirmishRequest(body);
		ADD_FAILURE() << "expected BAD_REQUEST for " << body.dump();
	}
	catch (const scriptapi::CommandError &error)
	{
		EXPECT_EQ(error.code(), ErrorCode::BadRequest) << body.dump();
		EXPECT_STRNE(error.what(), "") << body.dump();
	}
}

} // namespace

TEST(ScriptApiSkirmish, MinimalRequestUsesDefaults)
{
	const scriptapi::SkirmishRequest request = scriptapi::parseSkirmishRequest(minimalRequest());
	EXPECT_EQ(request.map, "maps\\test\\test.map");
	ASSERT_EQ(request.players.size(), 2u);
	EXPECT_EQ(request.players[0].type, SlotType::Script);
	EXPECT_EQ(request.players[1].type, SlotType::EasyAi);
	for (const scriptapi::SkirmishPlayer &player : request.players)
	{
		EXPECT_TRUE(player.faction.empty());
		EXPECT_EQ(player.color, -1);
		EXPECT_EQ(player.team, -1);
		EXPECT_EQ(player.startPos, -1);
	}
	EXPECT_FALSE(request.hasSeed);
	EXPECT_FALSE(request.hasStartingCash);
	EXPECT_EQ(request.fpsLimit, scriptapi::kDefaultFpsLimit);
	EXPECT_FALSE(request.limitSuperweapons);
}

TEST(ScriptApiSkirmish, FullRequest)
{
	Json body = minimalRequest();
	body["players"] = Json::array({
		Json{{"type", "script"}, {"faction", "FactionAmerica"}, {"color", 2}, {"team", 1}, {"start_pos", 3}, {"name", "Bot"}},
		Json{{"type", "hard_ai"}, {"faction", "random"}, {"color", nullptr}, {"team", 4}, {"start_pos", 8}},
		Json{{"type", "medium_ai"}, {"faction", "FactionGLA"}},
	});
	body["seed"] = 1234;
	body["fps_limit"] = 60;
	body["starting_cash"] = 20000;
	body["limit_superweapons"] = true;

	const scriptapi::SkirmishRequest request = scriptapi::parseSkirmishRequest(body);
	ASSERT_EQ(request.players.size(), 3u);

	const scriptapi::SkirmishPlayer &me = request.players[0];
	EXPECT_EQ(me.faction, "FactionAmerica");
	EXPECT_EQ(me.color, 2);
	EXPECT_EQ(me.team, 0);      // 1-based on the wire, 0-based in the engine
	EXPECT_EQ(me.startPos, 2);  // 1-based on the wire, 0-based in the engine
	EXPECT_EQ(me.name, "Bot");

	const scriptapi::SkirmishPlayer &hard = request.players[1];
	EXPECT_EQ(hard.type, SlotType::HardAi);
	EXPECT_TRUE(hard.faction.empty()); // "random"
	EXPECT_EQ(hard.color, -1);
	EXPECT_EQ(hard.team, 3);
	EXPECT_EQ(hard.startPos, 7);

	EXPECT_EQ(request.players[2].type, SlotType::MediumAi);
	EXPECT_EQ(request.players[2].faction, "FactionGLA");

	EXPECT_TRUE(request.hasSeed);
	EXPECT_EQ(request.seed, 1234);
	EXPECT_EQ(request.fpsLimit, 60);
	EXPECT_TRUE(request.hasStartingCash);
	EXPECT_EQ(request.startingCash, 20000);
	EXPECT_TRUE(request.limitSuperweapons);
}

TEST(ScriptApiSkirmish, SinglePlayerIsAllowed)
{
	Json body = minimalRequest();
	body["players"] = Json::array({Json{{"type", "script"}}});
	EXPECT_EQ(scriptapi::parseSkirmishRequest(body).players.size(), 1u);
}

TEST(ScriptApiSkirmish, MapIsRequired)
{
	Json body = minimalRequest();
	body.erase("map");
	expectBadRequest(body);
	body["map"] = 5;
	expectBadRequest(body);
	body["map"] = "";
	expectBadRequest(body);
}

TEST(ScriptApiSkirmish, PlayersMustBeNonEmptyArrayOfAtMostEight)
{
	Json body = minimalRequest();
	body.erase("players");
	expectBadRequest(body);
	body["players"] = Json::array();
	expectBadRequest(body);
	body["players"] = "script";
	expectBadRequest(body);

	Json players = Json::array({Json{{"type", "script"}}});
	for (int i = 0; i < scriptapi::kMaxSkirmishPlayers; ++i)
		players.push_back(Json{{"type", "easy_ai"}});
	body["players"] = players; // 9 players
	expectBadRequest(body);
}

TEST(ScriptApiSkirmish, ExactlyOneScriptPlayerInFirstSlot)
{
	Json body = minimalRequest();
	body["players"] = Json::array({Json{{"type", "easy_ai"}}, Json{{"type", "easy_ai"}}});
	expectBadRequest(body); // none
	body["players"] = Json::array({Json{{"type", "script"}}, Json{{"type", "script"}}});
	expectBadRequest(body); // two
	body["players"] = Json::array({Json{{"type", "easy_ai"}}, Json{{"type", "script"}}});
	expectBadRequest(body); // not first
}

TEST(ScriptApiSkirmish, PlayerFieldValidation)
{
	const auto withPlayer = [](const Json &player) {
		Json body = minimalRequest();
		body["players"][1] = player;
		return body;
	};
	expectBadRequest(withPlayer(Json{{"type", "brutal"}}));
	expectBadRequest(withPlayer(Json{{"faction", "FactionChina"}})); // type missing
	expectBadRequest(withPlayer(Json("easy_ai")));                   // not an object
	expectBadRequest(withPlayer(Json{{"type", "easy_ai"}, {"faction", 3}}));
	expectBadRequest(withPlayer(Json{{"type", "easy_ai"}, {"color", "red"}}));
	expectBadRequest(withPlayer(Json{{"type", "easy_ai"}, {"color", -2}}));
	expectBadRequest(withPlayer(Json{{"type", "easy_ai"}, {"team", 0}}));
	expectBadRequest(withPlayer(Json{{"type", "easy_ai"}, {"team", 5}}));
	expectBadRequest(withPlayer(Json{{"type", "easy_ai"}, {"start_pos", 0}}));
	expectBadRequest(withPlayer(Json{{"type", "easy_ai"}, {"start_pos", 9}}));
	expectBadRequest(withPlayer(Json{{"type", "easy_ai"}, {"name", "AI"}})); // only the script player has a name
}

TEST(ScriptApiSkirmish, MatchSettingValidation)
{
	Json body = minimalRequest();
	body["fps_limit"] = scriptapi::kMinFpsLimit - 1;
	expectBadRequest(body);
	body["fps_limit"] = scriptapi::kMaxFpsLimit + 1;
	expectBadRequest(body);
	body["fps_limit"] = "fast";
	expectBadRequest(body);

	body = minimalRequest();
	body["seed"] = "abc";
	expectBadRequest(body);

	body = minimalRequest();
	body["starting_cash"] = -5;
	expectBadRequest(body);

	body = minimalRequest();
	body["limit_superweapons"] = "yes";
	expectBadRequest(body);
}

TEST(ScriptApiSkirmish, SlotTypeNames)
{
	EXPECT_STREQ(scriptapi::slotTypeName(SlotType::Script), "script");
	EXPECT_STREQ(scriptapi::slotTypeName(SlotType::EasyAi), "easy_ai");
	EXPECT_STREQ(scriptapi::slotTypeName(SlotType::MediumAi), "medium_ai");
	EXPECT_STREQ(scriptapi::slotTypeName(SlotType::HardAi), "hard_ai");
}

TEST(ScriptApiSkirmish, NewErrorCodeNames)
{
	EXPECT_STREQ(scriptapi::errorCodeName(ErrorCode::WrongState), "WRONG_STATE");
	EXPECT_STREQ(scriptapi::errorCodeName(ErrorCode::InvalidArgument), "INVALID_ARGUMENT");
}
