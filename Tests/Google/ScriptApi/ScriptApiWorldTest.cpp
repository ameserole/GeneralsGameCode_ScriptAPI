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

#include "ScriptApiWorld.h"

using scriptapi::Json;
using scriptapi::KnownStructure;
using scriptapi::Relation;
using scriptapi::Shroud;

// -- ChangeDetector ------------------------------------------------------------------------------

TEST(ScriptApiChangeDetector, FirstValueIsBaseline)
{
	scriptapi::ChangeDetector<int> detector;
	EXPECT_FALSE(detector.hasValue());
	EXPECT_FALSE(detector.update(100));
	EXPECT_TRUE(detector.hasValue());
	EXPECT_EQ(detector.value(), 100);
}

TEST(ScriptApiChangeDetector, ReportsEachChangeOnceWithPrevious)
{
	scriptapi::ChangeDetector<int> detector;
	detector.update(100);
	int previous = 0;
	EXPECT_TRUE(detector.update(250, &previous));
	EXPECT_EQ(previous, 100);
	EXPECT_EQ(detector.value(), 250);
	EXPECT_FALSE(detector.update(250, &previous));
}

TEST(ScriptApiChangeDetector, ResetStartsOver)
{
	scriptapi::ChangeDetector<int> detector;
	detector.update(1);
	detector.reset();
	EXPECT_FALSE(detector.update(2));
}

// -- Visibility rule -----------------------------------------------------------------------------

TEST(ScriptApiVisibility, OwnObjectsAlwaysVisible)
{
	EXPECT_TRUE(scriptapi::isVisibleToPlayer(Relation::Self, Shroud::Shrouded, false, false));
	EXPECT_TRUE(scriptapi::isVisibleToPlayer(Relation::Self, Shroud::Fogged, true, false));
}

TEST(ScriptApiVisibility, OthersNeedClearOrPartialShroud)
{
	for (Relation relation : {Relation::Ally, Relation::Enemy, Relation::Neutral})
	{
		EXPECT_TRUE(scriptapi::isVisibleToPlayer(relation, Shroud::Clear, false, false));
		EXPECT_TRUE(scriptapi::isVisibleToPlayer(relation, Shroud::PartialClear, false, false));
		EXPECT_FALSE(scriptapi::isVisibleToPlayer(relation, Shroud::Fogged, false, false));
		EXPECT_FALSE(scriptapi::isVisibleToPlayer(relation, Shroud::Shrouded, false, false));
	}
}

TEST(ScriptApiVisibility, StealthedEnemiesNeedDetection)
{
	EXPECT_FALSE(scriptapi::isVisibleToPlayer(Relation::Enemy, Shroud::Clear, true, false));
	EXPECT_TRUE(scriptapi::isVisibleToPlayer(Relation::Enemy, Shroud::Clear, true, true));
	EXPECT_FALSE(scriptapi::isVisibleToPlayer(Relation::Enemy, Shroud::Fogged, true, true));
}

TEST(ScriptApiVisibility, StealthedAlliesAndNeutralsAreVisible)
{
	EXPECT_TRUE(scriptapi::isVisibleToPlayer(Relation::Ally, Shroud::Clear, true, false));
	EXPECT_TRUE(scriptapi::isVisibleToPlayer(Relation::Neutral, Shroud::Clear, true, false));
}

TEST(ScriptApiVisibility, Names)
{
	EXPECT_STREQ(scriptapi::relationName(Relation::Self), "self");
	EXPECT_STREQ(scriptapi::relationName(Relation::Ally), "ally");
	EXPECT_STREQ(scriptapi::relationName(Relation::Enemy), "enemy");
	EXPECT_STREQ(scriptapi::relationName(Relation::Neutral), "neutral");
	EXPECT_STREQ(scriptapi::shroudName(Shroud::Clear), "clear");
	EXPECT_STREQ(scriptapi::shroudName(Shroud::PartialClear), "partial");
	EXPECT_STREQ(scriptapi::shroudName(Shroud::Fogged), "fogged");
	EXPECT_STREQ(scriptapi::shroudName(Shroud::Shrouded), "shrouded");
}

// -- KnownStructureMemory ------------------------------------------------------------------------

namespace
{

KnownStructure structure(uint32_t id, float x, float y, unsigned int frame)
{
	KnownStructure s;
	s.id = id;
	s.templateName = "ChinaBarracks";
	s.owner = 3;
	s.x = x;
	s.y = y;
	s.lastSeenFrame = frame;
	return s;
}

const auto nothingVisible = [](float, float) { return false; };
const auto everythingVisible = [](float, float) { return true; };

} // namespace

TEST(ScriptApiKnownStructures, RememberedAfterSightingWhileFogged)
{
	scriptapi::KnownStructureMemory memory;
	memory.observe(structure(7, 100, 200, 30));

	memory.forgetUnseen(nothingVisible, {});
	const std::vector<KnownStructure> remembered = memory.remembered({});
	ASSERT_EQ(remembered.size(), 1u);
	EXPECT_EQ(remembered[0].id, 7u);
	EXPECT_EQ(remembered[0].lastSeenFrame, 30u);
	EXPECT_EQ(remembered[0].templateName, "ChinaBarracks");
}

TEST(ScriptApiKnownStructures, CurrentlyVisibleStructuresAreNotListed)
{
	scriptapi::KnownStructureMemory memory;
	memory.observe(structure(7, 100, 200, 30));
	EXPECT_TRUE(memory.remembered({7}).empty());
	EXPECT_EQ(memory.size(), 1u);
}

TEST(ScriptApiKnownStructures, ForgottenWhenSpotVisibleButStructureGone)
{
	scriptapi::KnownStructureMemory memory;
	memory.observe(structure(7, 100, 200, 30));
	memory.observe(structure(8, 900, 900, 30));

	// Only the spot of structure 7 is visible again, and 7 is not among the visible objects.
	memory.forgetUnseen([](float x, float) { return x < 500; }, {});
	const std::vector<KnownStructure> remembered = memory.remembered({});
	ASSERT_EQ(remembered.size(), 1u);
	EXPECT_EQ(remembered[0].id, 8u);
}

TEST(ScriptApiKnownStructures, KeptWhenSpotVisibleAndStructureVisible)
{
	scriptapi::KnownStructureMemory memory;
	memory.observe(structure(7, 100, 200, 30));
	memory.forgetUnseen(everythingVisible, {7});
	EXPECT_EQ(memory.size(), 1u);
}

TEST(ScriptApiKnownStructures, FoggedGhostAddedOnceAndFrozen)
{
	scriptapi::KnownStructureMemory memory;
	memory.observeFogged(structure(5, 100, 200, 10));
	memory.observeFogged(structure(5, 300, 400, 50)); // still fogged: the ghost does not change
	std::vector<KnownStructure> remembered = memory.remembered({});
	ASSERT_EQ(remembered.size(), 1u);
	EXPECT_EQ(remembered[0].lastSeenFrame, 10u);
	EXPECT_FLOAT_EQ(remembered[0].x, 100.0f);

	memory.observe(structure(5, 300, 400, 60)); // seen clearly again: updated
	remembered = memory.remembered({});
	EXPECT_EQ(remembered[0].lastSeenFrame, 60u);
}

TEST(ScriptApiKnownStructures, FoggedGhostDoesNotOverwriteClearSighting)
{
	scriptapi::KnownStructureMemory memory;
	memory.observe(structure(5, 100, 200, 40));
	memory.observeFogged(structure(5, 100, 200, 90));
	EXPECT_EQ(memory.remembered({})[0].lastSeenFrame, 40u);
}

TEST(ScriptApiKnownStructures, VersionChangesOnlyWithContent)
{
	scriptapi::KnownStructureMemory memory;
	uint64_t version = memory.version();
	const auto changed = [&]() { const bool c = memory.version() != version; version = memory.version(); return c; };

	memory.observe(structure(7, 100, 200, 30));
	EXPECT_TRUE(changed());
	memory.observe(structure(7, 100, 200, 45)); // same sighting, later frame
	EXPECT_FALSE(changed());
	memory.observe(structure(7, 120, 200, 60)); // moved
	EXPECT_TRUE(changed());
	memory.observeFogged(structure(7, 0, 0, 70)); // already known
	EXPECT_FALSE(changed());
	memory.observeFogged(structure(8, 0, 0, 70));
	EXPECT_TRUE(changed());
	memory.forgetUnseen(nothingVisible, {});
	EXPECT_FALSE(changed());
	memory.forgetUnseen(everythingVisible, {});
	EXPECT_TRUE(changed());
	memory.forget(7);
	EXPECT_FALSE(changed()); // already gone
}

TEST(ScriptApiKnownStructures, ForgetById)
{
	scriptapi::KnownStructureMemory memory;
	memory.observe(structure(7, 100, 200, 30));
	EXPECT_TRUE(memory.knows(7));
	memory.forget(7);
	EXPECT_EQ(memory.size(), 0u);
	EXPECT_FALSE(memory.knows(7));
	memory.forget(12345); // unknown ids are ignored
}

TEST(ScriptApiKnownStructures, ResightingUpdatesEntry)
{
	scriptapi::KnownStructureMemory memory;
	memory.observe(structure(7, 100, 200, 30));
	memory.observe(structure(7, 110, 210, 90));
	const std::vector<KnownStructure> remembered = memory.remembered({});
	ASSERT_EQ(remembered.size(), 1u);
	EXPECT_EQ(remembered[0].lastSeenFrame, 90u);
	EXPECT_FLOAT_EQ(remembered[0].x, 110.0f);
}

// -- SnapshotTimer -------------------------------------------------------------------------------

TEST(ScriptApiSnapshotTimer, FiresImmediatelyThenEveryInterval)
{
	scriptapi::SnapshotTimer timer(15);
	EXPECT_TRUE(timer.due(3));
	EXPECT_FALSE(timer.due(4));
	EXPECT_FALSE(timer.due(17));
	EXPECT_TRUE(timer.due(18));
	EXPECT_FALSE(timer.due(19));
	EXPECT_TRUE(timer.due(40)); // late frames still fire once
	EXPECT_FALSE(timer.due(41));
}

TEST(ScriptApiSnapshotTimer, ZeroDisables)
{
	scriptapi::SnapshotTimer timer(0);
	EXPECT_FALSE(timer.due(0));
	EXPECT_FALSE(timer.due(1000));
}

TEST(ScriptApiSnapshotTimer, IntervalChangeFiresNext)
{
	scriptapi::SnapshotTimer timer(15);
	EXPECT_TRUE(timer.due(10));
	timer.setInterval(30);
	EXPECT_EQ(timer.interval(), 30u);
	EXPECT_TRUE(timer.due(11));
	EXPECT_FALSE(timer.due(40));
	EXPECT_TRUE(timer.due(41));
}

TEST(ScriptApiSnapshotTimer, FrameRestartFires)
{
	scriptapi::SnapshotTimer timer(15);
	EXPECT_TRUE(timer.due(500));
	EXPECT_TRUE(timer.due(2)); // new game: frame counter went backwards
	EXPECT_FALSE(timer.due(3));
}

// -- Terrain grid --------------------------------------------------------------------------------

TEST(ScriptApiTerrainGrid, ShapeKindsAndRounding)
{
	const Json grid = scriptapi::encodeTerrainGrid(3, 2, [](int column, int row) {
		scriptapi::TerrainSample sample;
		sample.height = column * 10.4f + row * 0.6f;
		const char kinds[] = {'.', 'w', 'c', 'x', '.', '.'};
		sample.kind = kinds[row * 3 + column];
		return sample;
	});

	ASSERT_EQ(grid["heights"].size(), 2u);
	ASSERT_EQ(grid["terrain"].size(), 2u);
	EXPECT_EQ(grid["heights"][0].size(), 3u);
	EXPECT_EQ(grid["terrain"][0], ".wc");
	EXPECT_EQ(grid["terrain"][1], "x..");
	EXPECT_EQ(grid["heights"][0][0], 0);
	EXPECT_EQ(grid["heights"][0][1], 10);  // 10.4 rounds down
	EXPECT_EQ(grid["heights"][1][2], 21);  // 20.8 + 0.6 = 21.4
	EXPECT_EQ(grid["heights"][1][0], 1);   // 0.6 rounds up
}

TEST(ScriptApiTerrainGrid, EmptyGrid)
{
	const Json grid = scriptapi::encodeTerrainGrid(0, 0, [](int, int) { return scriptapi::TerrainSample(); });
	EXPECT_TRUE(grid["heights"].empty());
	EXPECT_TRUE(grid["terrain"].empty());
}
