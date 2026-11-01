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

#include "ScriptApiLan.h"
#include "ScriptApiSkirmish.h"

namespace scriptapi
{

namespace
{

constexpr size_t kMaxLanSlots = 8;

[[noreturn]] void badRequest(const std::string &message)
{
	throw CommandError(ErrorCode::BadRequest, message);
}

const Json *findOptional(const Json &object, const char *key)
{
	const auto it = object.find(key);
	if (it == object.end() || it->is_null())
		return nullptr;
	return &*it;
}

int requireInt(const Json &value, const std::string &what)
{
	if (!value.is_number_integer())
		badRequest(what + " must be an integer");
	return value.get<int>();
}

/// 1-based index on the wire, returned 0-based.
int oneBased(const Json &value, int max, const std::string &what)
{
	const int number = requireInt(value, what);
	if (number < 1 || number > max)
		badRequest(what + " must be between 1 and " + std::to_string(max));
	return number - 1;
}

std::string parseFaction(const Json &value, const std::string &what)
{
	if (!value.is_string())
		badRequest(what + " must be a string");
	const std::string name = value.get<std::string>();
	return name == "random" ? std::string() : name;
}

int parseColor(const Json &value, const std::string &what)
{
	const int color = requireInt(value, what);
	if (color < -1)
		badRequest(what + " must be -1 (random) or a color index");
	return color;
}

std::string requireName(const Json &body, const char *key, size_t maxLength)
{
	const auto it = body.find(key);
	if (it == body.end() || !it->is_string())
		badRequest(std::string(key) + " must be a string");
	const std::string name = it->get<std::string>();
	if (name.empty() || name.size() > maxLength)
		badRequest(std::string(key) + " must have 1 to " + std::to_string(maxLength) + " characters");
	return name;
}

LanSlotType parseLanSlotType(const Json &value, const std::string &what)
{
	if (!value.is_string())
		badRequest(what + " must be a string");
	const std::string type = value.get<std::string>();
	if (type == "host") return LanSlotType::Host;
	if (type == "open") return LanSlotType::Open;
	if (type == "closed") return LanSlotType::Closed;
	if (type == "easy_ai") return LanSlotType::EasyAi;
	if (type == "medium_ai") return LanSlotType::MediumAi;
	if (type == "hard_ai") return LanSlotType::HardAi;
	badRequest(what + " must be one of host, open, closed, easy_ai, medium_ai, hard_ai");
}

bool isAi(LanSlotType type)
{
	return type == LanSlotType::EasyAi || type == LanSlotType::MediumAi || type == LanSlotType::HardAi;
}

LanSlotSetup parseLanSlot(const Json &value, size_t index)
{
	const std::string what = "slots[" + std::to_string(index) + "]";
	if (!value.is_object())
		badRequest(what + " must be an object");

	LanSlotSetup slot;
	const auto type = value.find("type");
	if (type == value.end())
		badRequest(what + ".type is required");
	slot.type = parseLanSlotType(*type, what + ".type");

	const Json *faction = findOptional(value, "faction");
	const Json *color = findOptional(value, "color");
	const Json *team = findOptional(value, "team");
	const Json *startPos = findOptional(value, "start_pos");
	if (!isAi(slot.type) && (faction || color || team || startPos))
		badRequest(what + ": only AI slots take faction, color, team and start_pos (players choose their own)");

	if (faction)
		slot.faction = parseFaction(*faction, what + ".faction");
	if (color)
		slot.color = parseColor(*color, what + ".color");
	if (team)
		slot.team = oneBased(*team, kMaxSkirmishTeams, what + ".team");
	if (startPos)
		slot.startPos = oneBased(*startPos, kMaxSkirmishPlayers, what + ".start_pos");
	return slot;
}

} // namespace

const char *lanSlotTypeName(LanSlotType type)
{
	switch (type)
	{
	case LanSlotType::Host: return "host";
	case LanSlotType::Open: return "open";
	case LanSlotType::Closed: return "closed";
	case LanSlotType::EasyAi: return "easy_ai";
	case LanSlotType::MediumAi: return "medium_ai";
	case LanSlotType::HardAi: return "hard_ai";
	}
	return "open";
}

LanHostRequest parseLanHostRequest(const Json &body)
{
	LanHostRequest request;
	const auto map = body.find("map");
	if (map == body.end() || !map->is_string() || map->get<std::string>().empty())
		badRequest("map must be a non-empty string");
	request.map = map->get<std::string>();

	const Json *slots = findOptional(body, "slots");
	if (slots == nullptr)
	{
		// Default: the host and one open slot.
		request.slots.resize(2);
		request.slots[0].type = LanSlotType::Host;
	}
	else
	{
		if (!slots->is_array() || slots->empty())
			badRequest("slots must be a non-empty array");
		if (slots->size() > kMaxLanSlots)
			badRequest("slots can have at most " + std::to_string(kMaxLanSlots) + " entries");
		for (size_t i = 0; i < slots->size(); ++i)
		{
			LanSlotSetup slot = parseLanSlot((*slots)[i], i);
			if ((slot.type == LanSlotType::Host) != (i == 0))
				badRequest("slots[0] must be the host, and only slots[0]");
			request.slots.push_back(std::move(slot));
		}
	}

	if (const Json *cash = findOptional(body, "starting_cash"))
	{
		request.startingCash = requireInt(*cash, "starting_cash");
		if (request.startingCash < 0)
			badRequest("starting_cash must not be negative");
		request.hasStartingCash = true;
	}

	if (const Json *limit = findOptional(body, "limit_superweapons"))
	{
		if (!limit->is_boolean())
			badRequest("limit_superweapons must be a boolean");
		request.limitSuperweapons = limit->get<bool>();
	}

	return request;
}

LanSlotRequest parseLanSlotRequest(const Json &body)
{
	LanSlotRequest request;

	if (const Json *observer = findOptional(body, "observer"))
	{
		if (!observer->is_boolean())
			badRequest("observer must be a boolean");
		request.observer = observer->get<bool>();
	}

	// A present-but-null team or start_pos means "none" / "random".
	const auto faction = body.find("faction");
	const auto color = body.find("color");
	const auto team = body.find("team");
	const auto startPos = body.find("start_pos");

	if (request.observer && (faction != body.end() || team != body.end() || startPos != body.end()))
		badRequest("observer cannot be combined with faction, team or start_pos");

	if (faction != body.end())
	{
		request.hasFaction = true;
		if (!faction->is_null())
			request.faction = parseFaction(*faction, "faction");
	}
	if (color != body.end())
	{
		request.hasColor = true;
		if (!color->is_null())
			request.color = parseColor(*color, "color");
	}
	if (team != body.end())
	{
		request.hasTeam = true;
		if (!team->is_null())
			request.team = oneBased(*team, kMaxSkirmishTeams, "team");
	}
	if (startPos != body.end())
	{
		request.hasStartPos = true;
		if (!startPos->is_null())
			request.startPos = oneBased(*startPos, kMaxSkirmishPlayers, "start_pos");
	}

	if (body.find("observer") == body.end() && !request.hasFaction && !request.hasColor && !request.hasTeam && !request.hasStartPos)
		badRequest("lan_slot needs at least one of faction, color, team, start_pos, observer");
	return request;
}

std::string parseLanPlayerName(const Json &body, const char *key)
{
	return requireName(body, key, kMaxLanPlayerNameLength);
}

bool JsonChangeDetector::update(const Json &value)
{
	if (m_hasValue && value == m_value)
		return false;
	m_value = value;
	m_hasValue = true;
	return true;
}

} // namespace scriptapi
