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

using scriptapi::PendingSpend;

// -- PendingSpend ----------------------------------------------------------------------------------

TEST(ScriptApiAgentFixes, PendingSpendSumsUnexecutedOrders)
{
	PendingSpend spend;
	EXPECT_EQ(spend.committed(), 0);
	spend.add(10, 500);
	spend.add(20, 800);
	spend.add(10, 300);
	EXPECT_EQ(spend.committed(), 1600);
	EXPECT_EQ(spend.size(), 3u);
}

TEST(ScriptApiAgentFixes, ExecutionSettlesTheOldestEntryOfItsType)
{
	PendingSpend spend;
	spend.add(10, 500);
	spend.add(20, 800);
	spend.add(10, 300);

	spend.onExecuted(10);
	EXPECT_EQ(spend.committed(), 1100); // the 500 build went, the unit and the second build stay
	spend.onExecuted(30);
	EXPECT_EQ(spend.committed(), 1100); // a type with nothing pending changes nothing
	spend.onExecuted(20);
	EXPECT_EQ(spend.committed(), 300);
	spend.onExecuted(10);
	spend.onExecuted(10);
	EXPECT_EQ(spend.committed(), 0);
	EXPECT_EQ(spend.size(), 0u);
}

TEST(ScriptApiAgentFixes, ClearForgetsEverything)
{
	PendingSpend spend;
	spend.add(10, 500);
	spend.clear();
	EXPECT_EQ(spend.committed(), 0);
	EXPECT_EQ(spend.size(), 0u);
}
