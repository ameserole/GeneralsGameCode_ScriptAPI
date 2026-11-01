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

// Parsing of the script API LAN lobby requests, and change detection for the lobby events.
// Names are resolved against the game's data by the engine-side handlers.

#pragma once

#include "ScriptApiProtocol.h"

#include <string>
#include <vector>

namespace scriptapi
{

constexpr size_t kMaxLanPlayerNameLength = 12;  ///< g_lanPlayerNameLength

enum class LanSlotType
{
	Host,    ///< slot 0, the hosting player
	Open,
	Closed,
	EasyAi,
	MediumAi,
	HardAi,
};

const char *lanSlotTypeName(LanSlotType type);

struct LanSlotSetup
{
	LanSlotType type = LanSlotType::Open;
	std::string faction;  ///< AI slots only; empty for random
	int color = -1;
	int team = -1;        ///< engine numbering (0-based)
	int startPos = -1;    ///< engine numbering (0-based)
};

/// No game name: the engine names LAN games after the host's IP and seed (RequestGameCreate).
struct LanHostRequest
{
	std::string map;
	std::vector<LanSlotSetup> slots;  ///< slots[0] is the host
	bool hasStartingCash = false;
	int startingCash = 0;
	bool limitSuperweapons = false;
};

/// Throws CommandError(BadRequest).
LanHostRequest parseLanHostRequest(const Json &body);

struct LanSlotRequest
{
	bool hasFaction = false;
	std::string faction;  ///< empty for random
	bool hasColor = false;
	int color = -1;
	bool hasTeam = false;
	int team = -1;
	bool hasStartPos = false;
	int startPos = -1;
	bool observer = false;
};

/// Throws CommandError(BadRequest).
LanSlotRequest parseLanSlotRequest(const Json &body);

/// A LAN player name: 1..12 characters. Throws CommandError(BadRequest).
std::string parseLanPlayerName(const Json &body, const char *key);

/// Reports when a JSON value differs from the previous one (the first value counts as a change).
class JsonChangeDetector
{
public:
	bool update(const Json &value);
	void reset() { m_hasValue = false; }

private:
	bool m_hasValue = false;
	Json m_value;
};

} // namespace scriptapi
