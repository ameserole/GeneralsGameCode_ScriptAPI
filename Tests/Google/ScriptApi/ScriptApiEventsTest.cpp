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
#include "ScriptApiEvents.h"
#include "ScriptApiWorld.h"

using scriptapi::Json;
using scriptapi::VisibleInfo;

namespace
{

VisibleInfo seen(uint32_t id, float x = 0.0f)
{
	VisibleInfo info;
	info.id = id;
	info.templateName = "ChinaTankBattleMaster";
	info.owner = 3;
	info.x = x;
	return info;
}

std::map<uint32_t, VisibleInfo> visible(std::initializer_list<VisibleInfo> infos)
{
	std::map<uint32_t, VisibleInfo> result;
	for (const VisibleInfo &info : infos)
		result[info.id] = info;
	return result;
}

} // namespace

// -- VisibilityTracker ---------------------------------------------------------------------------

TEST(ScriptApiVisibilityTracker, SpottedOnceThenLostWithLastPosition)
{
	scriptapi::VisibilityTracker tracker;
	scriptapi::VisibilityTracker::Changes changes = tracker.update(visible({seen(1, 10)}));
	ASSERT_EQ(changes.spotted.size(), 1u);
	EXPECT_EQ(changes.spotted[0].id, 1u);
	EXPECT_TRUE(changes.lost.empty());
	EXPECT_TRUE(tracker.isVisible(1));

	changes = tracker.update(visible({seen(1, 20)}));
	EXPECT_TRUE(changes.spotted.empty());
	EXPECT_TRUE(changes.lost.empty());

	changes = tracker.update(visible({}));
	ASSERT_EQ(changes.lost.size(), 1u);
	EXPECT_FLOAT_EQ(changes.lost[0].x, 20.0f); // the last position seen
	EXPECT_FALSE(tracker.isVisible(1));
}

TEST(ScriptApiVisibilityTracker, ForgottenObjectsAreNotLost)
{
	scriptapi::VisibilityTracker tracker;
	tracker.update(visible({seen(1), seen(2)}));
	tracker.forget(1); // seen dying
	const scriptapi::VisibilityTracker::Changes changes = tracker.update(visible({}));
	ASSERT_EQ(changes.lost.size(), 1u);
	EXPECT_EQ(changes.lost[0].id, 2u);
}

TEST(ScriptApiVisibilityTracker, ReappearingIsSpottedAgain)
{
	scriptapi::VisibilityTracker tracker;
	tracker.update(visible({seen(1)}));
	tracker.update(visible({}));
	EXPECT_EQ(tracker.update(visible({seen(1)})).spotted.size(), 1u);
}

TEST(ScriptApiVisibilityTracker, ClearStartsOver)
{
	scriptapi::VisibilityTracker tracker;
	tracker.update(visible({seen(1)}));
	tracker.clear();
	EXPECT_EQ(tracker.update(visible({seen(1)})).spotted.size(), 1u);
}

// -- DamageAggregator ----------------------------------------------------------------------------

TEST(ScriptApiDamageAggregator, HitsWithinWindowAreSummed)
{
	scriptapi::DamageAggregator damage;
	damage.add(5, 10.0f, 0, 100);
	damage.add(5, 15.0f, 0, 105);
	EXPECT_TRUE(damage.due(110, 15).empty()); // window not over yet

	const std::vector<scriptapi::DamageAggregator::Entry> due = damage.due(115, 15);
	ASSERT_EQ(due.size(), 1u);
	EXPECT_EQ(due[0].id, 5u);
	EXPECT_FLOAT_EQ(due[0].damage, 25.0f);
	EXPECT_EQ(damage.size(), 0u);
}

TEST(ScriptApiDamageAggregator, ObjectsAreSeparate)
{
	scriptapi::DamageAggregator damage;
	damage.add(5, 10.0f, 0, 100);
	damage.add(6, 7.0f, 0, 108);
	std::vector<scriptapi::DamageAggregator::Entry> due = damage.due(115, 15);
	ASSERT_EQ(due.size(), 1u);
	EXPECT_EQ(due[0].id, 5u);
	due = damage.due(123, 15);
	ASSERT_EQ(due.size(), 1u);
	EXPECT_EQ(due[0].id, 6u);
}

TEST(ScriptApiDamageAggregator, KnownAttackerIsKept)
{
	scriptapi::DamageAggregator damage;
	damage.add(5, 10.0f, 77, 100);
	damage.add(5, 10.0f, 0, 101);  // later hit from an unseen attacker
	damage.add(6, 10.0f, 0, 100);
	damage.add(6, 10.0f, 88, 102);
	std::vector<scriptapi::DamageAggregator::Entry> due = damage.due(200, 15);
	ASSERT_EQ(due.size(), 2u);
	EXPECT_EQ(due[0].attacker, 77u);
	EXPECT_EQ(due[1].attacker, 88u);
}

TEST(ScriptApiDamageAggregator, ForgetDropsEntry)
{
	scriptapi::DamageAggregator damage;
	damage.add(5, 10.0f, 0, 100);
	damage.forget(5);
	EXPECT_TRUE(damage.due(500, 15).empty());
}

// -- KnownStructureMemory return values ----------------------------------------------------------

TEST(ScriptApiKnownStructures, ObserveReportsNewEntries)
{
	scriptapi::KnownStructureMemory memory;
	scriptapi::KnownStructure s;
	s.id = 9;
	s.templateName = "ChinaBarracks";
	EXPECT_TRUE(memory.observe(s));
	EXPECT_FALSE(memory.observe(s));
	s.id = 10;
	EXPECT_TRUE(memory.observeFogged(s));
	EXPECT_FALSE(memory.observeFogged(s));
}

// -- Waypoints -----------------------------------------------------------------------------------

TEST(ScriptApiCommands, ParseWaypoints)
{
	EXPECT_TRUE(scriptapi::parseWaypoints(Json::object(), "waypoints").empty());
	const std::vector<scriptapi::Position> points =
		scriptapi::parseWaypoints(Json{{"waypoints", Json::array({Json::array({1, 2}), Json::array({3, 4})})}}, "waypoints");
	ASSERT_EQ(points.size(), 2u);
	EXPECT_FLOAT_EQ(points[1].x, 3.0f);

	Json tooMany = Json::array();
	for (size_t i = 0; i <= scriptapi::kMaxWaypoints; ++i)
		tooMany.push_back(Json::array({1, 1}));
	for (const Json &bad : {Json(5), Json::array({Json::array({1})}), Json::array({"x"}), tooMany})
	{
		try
		{
			scriptapi::parseWaypoints(Json{{"waypoints", bad}}, "waypoints");
			ADD_FAILURE() << bad.dump();
		}
		catch (const scriptapi::CommandError &error)
		{
			EXPECT_EQ(error.code(), scriptapi::ErrorCode::BadRequest);
		}
	}
}

TEST(ScriptApiCommands, CannotEnterErrorName)
{
	EXPECT_STREQ(scriptapi::errorCodeName(scriptapi::ErrorCode::CannotEnter), "CANNOT_ENTER");
}
