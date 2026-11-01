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

// Classifies the engine state the script API reports and detects transitions between states.

#pragma once

namespace scriptapi
{

enum class GameState
{
	Shell,    ///< Menus, including the animated shell map.
	Loading,  ///< A map is loading.
	InGame,   ///< A real (non-shell) game is running.
};

const char *gameStateName(GameState state);

GameState classifyGameState(bool loadingMap, bool inGame, bool inShellGame);

class GameStateTracker
{
public:
	struct Transition
	{
		GameState previous;
		GameState current;
	};

	/// Returns true and fills out when the state differs from the last update.
	/// The first update only establishes the state and reports no transition.
	bool update(GameState now, Transition &out);

	bool hasState() const { return m_hasState; }
	GameState current() const { return m_current; }

private:
	bool m_hasState = false;
	GameState m_current = GameState::Shell;
};

} // namespace scriptapi
