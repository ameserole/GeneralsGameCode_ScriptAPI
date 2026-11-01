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

// Parsing of the "start_skirmish" request. Only shape and ranges are checked here; names such as
// the map and factions are resolved against the game's data by the engine-side handler.

#pragma once

#include "ScriptApiProtocol.h"

#include <string>
#include <vector>

namespace scriptapi
{

constexpr int kMaxSkirmishPlayers = 8;
constexpr int kMaxSkirmishTeams = 4;
constexpr int kDefaultFpsLimit = 30;
constexpr int kMinFpsLimit = 15;
constexpr int kMaxFpsLimit = 1000;

enum class SlotType
{
	Script,   ///< The local player, controlled through the script API.
	EasyAi,
	MediumAi,
	HardAi,
};

const char *slotTypeName(SlotType type);

struct SkirmishPlayer
{
	SlotType type = SlotType::EasyAi;
	std::string faction;  ///< Player template name; empty for random.
	int color = -1;       ///< Multiplayer color index; -1 for random.
	int team = -1;        ///< Engine team number 0..3; -1 for no team.
	int startPos = -1;    ///< Engine start position 0..7; -1 for random.
	std::string name;     ///< Display name of the script player; empty for the default.
};

struct SkirmishRequest
{
	std::string map;
	std::vector<SkirmishPlayer> players;  ///< players[0] is always the script player.
	bool hasSeed = false;
	int seed = 0;
	int fpsLimit = kDefaultFpsLimit;
	bool hasStartingCash = false;
	int startingCash = 0;
	bool limitSuperweapons = false;
};

/// Throws CommandError(BadRequest) when the request is malformed.
SkirmishRequest parseSkirmishRequest(const Json &body);

} // namespace scriptapi
