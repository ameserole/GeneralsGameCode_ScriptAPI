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

using scriptapi::mayReportPlayerStats;

// -- player_stats visibility -----------------------------------------------------------------------

TEST(ScriptApiTournament, PlayersSeeOnlyTheirOwnStatsDuringTheGame)
{
	EXPECT_TRUE(mayReportPlayerStats(/*isSelf*/ true, /*isObserver*/ false, /*gameOver*/ false));
	EXPECT_FALSE(mayReportPlayerStats(false, false, false));
}

TEST(ScriptApiTournament, ObserversSeeEveryonesStats)
{
	EXPECT_TRUE(mayReportPlayerStats(false, true, false));
}

TEST(ScriptApiTournament, EveryoneSeesAllStatsOnceTheGameIsOver)
{
	EXPECT_TRUE(mayReportPlayerStats(false, false, true));
	EXPECT_TRUE(mayReportPlayerStats(true, false, true));
}
