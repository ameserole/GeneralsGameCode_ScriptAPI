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

#include "ScriptApiGameState.h"

using scriptapi::GameState;

TEST(ScriptApiGameState, Classify)
{
	EXPECT_EQ(scriptapi::classifyGameState(false, false, false), GameState::Shell);
	EXPECT_EQ(scriptapi::classifyGameState(false, true, true), GameState::Shell); // shell map
	EXPECT_EQ(scriptapi::classifyGameState(false, true, false), GameState::InGame);
	EXPECT_EQ(scriptapi::classifyGameState(true, true, false), GameState::Loading);
	EXPECT_EQ(scriptapi::classifyGameState(true, false, false), GameState::Loading);
	EXPECT_EQ(scriptapi::classifyGameState(true, true, true), GameState::Shell); // loading the shell map
}

TEST(ScriptApiGameState, Names)
{
	EXPECT_STREQ(scriptapi::gameStateName(GameState::Shell), "shell");
	EXPECT_STREQ(scriptapi::gameStateName(GameState::Loading), "loading");
	EXPECT_STREQ(scriptapi::gameStateName(GameState::InGame), "in_game");
}

TEST(ScriptApiGameState, FirstUpdateEstablishesStateWithoutTransition)
{
	scriptapi::GameStateTracker tracker;
	scriptapi::GameStateTracker::Transition transition;
	EXPECT_FALSE(tracker.hasState());
	EXPECT_FALSE(tracker.update(GameState::Shell, transition));
	EXPECT_TRUE(tracker.hasState());
	EXPECT_EQ(tracker.current(), GameState::Shell);
}

TEST(ScriptApiGameState, ReportsEachChangeOnce)
{
	scriptapi::GameStateTracker tracker;
	scriptapi::GameStateTracker::Transition transition;
	tracker.update(GameState::Shell, transition);

	ASSERT_TRUE(tracker.update(GameState::InGame, transition));
	EXPECT_EQ(transition.previous, GameState::Shell);
	EXPECT_EQ(transition.current, GameState::InGame);
	EXPECT_FALSE(tracker.update(GameState::InGame, transition));

	ASSERT_TRUE(tracker.update(GameState::Shell, transition));
	EXPECT_EQ(transition.previous, GameState::InGame);
	EXPECT_EQ(transition.current, GameState::Shell);
	EXPECT_FALSE(tracker.update(GameState::Shell, transition));
}
