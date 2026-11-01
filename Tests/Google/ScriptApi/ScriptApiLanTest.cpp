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

#include "ScriptApiLan.h"

using scriptapi::Json;
using scriptapi::LanSlotType;

namespace
{

Json hostRequest()
{
	return Json{
		{"map", "maps\\test\\test.map"},
		{"slots", Json::array({Json{{"type", "host"}}, Json{{"type", "open"}}})},
	};
}

template <typename Fn>
void expectBadRequest(Fn fn, const std::string &what)
{
	try
	{
		fn();
		ADD_FAILURE() << "expected BAD_REQUEST: " << what;
	}
	catch (const scriptapi::CommandError &error)
	{
		EXPECT_EQ(error.code(), scriptapi::ErrorCode::BadRequest) << what;
	}
}

void expectBadHost(const Json &body)
{
	expectBadRequest([&] { scriptapi::parseLanHostRequest(body); }, body.dump());
}

void expectBadSlot(const Json &body)
{
	expectBadRequest([&] { scriptapi::parseLanSlotRequest(body); }, body.dump());
}

} // namespace

// -- lan_host ------------------------------------------------------------------------------------

TEST(ScriptApiLan, MinimalHostRequest)
{
	const scriptapi::LanHostRequest request = scriptapi::parseLanHostRequest(hostRequest());
	ASSERT_EQ(request.slots.size(), 2u);
	EXPECT_EQ(request.slots[0].type, LanSlotType::Host);
	EXPECT_EQ(request.slots[1].type, LanSlotType::Open);
	EXPECT_FALSE(request.hasStartingCash);
	EXPECT_FALSE(request.limitSuperweapons);

	Json noSlots = hostRequest();
	noSlots.erase("slots");
	const scriptapi::LanHostRequest defaulted = scriptapi::parseLanHostRequest(noSlots);
	ASSERT_EQ(defaulted.slots.size(), 2u);
	EXPECT_EQ(defaulted.slots[0].type, LanSlotType::Host);
	EXPECT_EQ(defaulted.slots[1].type, LanSlotType::Open);
}

TEST(ScriptApiLan, AiSlotsTakeSettings)
{
	Json body = hostRequest();
	body["slots"] = Json::array({
		Json{{"type", "host"}},
		Json{{"type", "easy_ai"}, {"faction", "FactionChina"}, {"color", 2}, {"team", 2}, {"start_pos", 2}},
		Json{{"type", "hard_ai"}, {"faction", "random"}},
		Json{{"type", "closed"}},
	});
	body["starting_cash"] = 20000;
	body["limit_superweapons"] = true;

	const scriptapi::LanHostRequest request = scriptapi::parseLanHostRequest(body);
	ASSERT_EQ(request.slots.size(), 4u);
	EXPECT_EQ(request.slots[1].type, LanSlotType::EasyAi);
	EXPECT_EQ(request.slots[1].faction, "FactionChina");
	EXPECT_EQ(request.slots[1].color, 2);
	EXPECT_EQ(request.slots[1].team, 1);
	EXPECT_EQ(request.slots[1].startPos, 1);
	EXPECT_EQ(request.slots[2].type, LanSlotType::HardAi);
	EXPECT_TRUE(request.slots[2].faction.empty());
	EXPECT_EQ(request.slots[3].type, LanSlotType::Closed);
	EXPECT_TRUE(request.hasStartingCash);
	EXPECT_EQ(request.startingCash, 20000);
	EXPECT_TRUE(request.limitSuperweapons);
}

TEST(ScriptApiLan, HostRequestValidation)
{
	Json body = hostRequest();
	body.erase("map");
	expectBadHost(body);

	body = hostRequest();
	body["slots"] = Json::array();
	expectBadHost(body);
	body["slots"] = Json::array({Json{{"type", "open"}}});
	expectBadHost(body); // slot 0 must be the host
	body["slots"] = Json::array({Json{{"type", "host"}}, Json{{"type", "host"}}});
	expectBadHost(body); // only one host
	body["slots"] = Json::array({Json{{"type", "host"}}, Json{{"type", "human"}}});
	expectBadHost(body); // unknown type
	body["slots"] = Json::array({Json{{"type", "host"}}, Json{{"type", "open"}, {"faction", "FactionChina"}}});
	expectBadHost(body); // only AI slots carry settings

	Json nine = Json::array({Json{{"type", "host"}}});
	for (int i = 0; i < 8; ++i)
		nine.push_back(Json{{"type", "open"}});
	body["slots"] = nine;
	expectBadHost(body);

	body = hostRequest();
	body["slots"][1] = Json{{"type", "easy_ai"}, {"team", 5}};
	expectBadHost(body);
}

// -- lan_slot ------------------------------------------------------------------------------------

TEST(ScriptApiLan, SlotRequestFields)
{
	const scriptapi::LanSlotRequest request = scriptapi::parseLanSlotRequest(
		Json{{"faction", "FactionChina"}, {"color", 3}, {"team", 1}, {"start_pos", 2}});
	EXPECT_TRUE(request.hasFaction);
	EXPECT_EQ(request.faction, "FactionChina");
	EXPECT_TRUE(request.hasColor);
	EXPECT_EQ(request.color, 3);
	EXPECT_TRUE(request.hasTeam);
	EXPECT_EQ(request.team, 0);
	EXPECT_TRUE(request.hasStartPos);
	EXPECT_EQ(request.startPos, 1);
	EXPECT_FALSE(request.observer);

	const scriptapi::LanSlotRequest random = scriptapi::parseLanSlotRequest(Json{{"faction", "random"}, {"team", nullptr}});
	EXPECT_TRUE(random.hasFaction);
	EXPECT_TRUE(random.faction.empty());
	EXPECT_TRUE(random.hasTeam);
	EXPECT_EQ(random.team, -1);
}

TEST(ScriptApiLan, ObserverSlot)
{
	const scriptapi::LanSlotRequest request = scriptapi::parseLanSlotRequest(Json{{"observer", true}});
	EXPECT_TRUE(request.observer);
	EXPECT_FALSE(request.hasFaction);

	expectBadSlot(Json{{"observer", true}, {"faction", "FactionChina"}});
	expectBadSlot(Json{{"observer", true}, {"team", 1}});
	expectBadSlot(Json{{"observer", true}, {"start_pos", 1}});
	expectBadSlot(Json{{"observer", "yes"}});
}

TEST(ScriptApiLan, SlotRequestValidation)
{
	expectBadSlot(Json::object()); // nothing to change
	expectBadSlot(Json{{"faction", 3}});
	expectBadSlot(Json{{"color", -2}});
	expectBadSlot(Json{{"team", 0}});
	expectBadSlot(Json{{"start_pos", 9}});
}

TEST(ScriptApiLan, PlayerName)
{
	EXPECT_EQ(scriptapi::parseLanPlayerName(Json{{"name", "Bot1"}}, "name"), "Bot1");
	expectBadRequest([] { scriptapi::parseLanPlayerName(Json{{"name", ""}}, "name"); }, "empty");
	expectBadRequest([] { scriptapi::parseLanPlayerName(Json{{"name", "ThirteenChars"}}, "name"); }, "too long");
	expectBadRequest([] { scriptapi::parseLanPlayerName(Json::object(), "name"); }, "missing");
}

// -- JsonChangeDetector --------------------------------------------------------------------------

TEST(ScriptApiLan, JsonChangeDetector)
{
	scriptapi::JsonChangeDetector detector;
	EXPECT_TRUE(detector.update(Json{{"a", 1}}));  // first value is reported
	EXPECT_FALSE(detector.update(Json{{"a", 1}}));
	EXPECT_TRUE(detector.update(Json{{"a", 2}}));
	detector.reset();
	EXPECT_TRUE(detector.update(Json{{"a", 2}}));
}

TEST(ScriptApiLan, SlotTypeNames)
{
	EXPECT_STREQ(scriptapi::lanSlotTypeName(LanSlotType::Host), "host");
	EXPECT_STREQ(scriptapi::lanSlotTypeName(LanSlotType::Open), "open");
	EXPECT_STREQ(scriptapi::lanSlotTypeName(LanSlotType::Closed), "closed");
	EXPECT_STREQ(scriptapi::lanSlotTypeName(LanSlotType::EasyAi), "easy_ai");
	EXPECT_STREQ(scriptapi::lanSlotTypeName(LanSlotType::MediumAi), "medium_ai");
	EXPECT_STREQ(scriptapi::lanSlotTypeName(LanSlotType::HardAi), "hard_ai");
	EXPECT_STREQ(scriptapi::errorCodeName(scriptapi::ErrorCode::NotReady), "NOT_READY");
}
