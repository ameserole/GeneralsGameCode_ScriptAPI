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

#include "ScriptApiGameState.h"

namespace scriptapi
{

const char *gameStateName(GameState state)
{
	switch (state)
	{
	case GameState::Shell: return "shell";
	case GameState::Loading: return "loading";
	case GameState::InGame: return "in_game";
	}
	return "shell";
}

GameState classifyGameState(bool loadingMap, bool inGame, bool inShellGame)
{
	// The shell map is the animated menu background; loading or running it is still the shell.
	if (inShellGame)
		return GameState::Shell;
	if (loadingMap)
		return GameState::Loading;
	if (inGame)
		return GameState::InGame;
	return GameState::Shell;
}

bool GameStateTracker::update(GameState now, Transition &out)
{
	if (!m_hasState)
	{
		m_hasState = true;
		m_current = now;
		return false;
	}
	if (now == m_current)
		return false;

	out.previous = m_current;
	out.current = now;
	m_current = now;
	return true;
}

} // namespace scriptapi
