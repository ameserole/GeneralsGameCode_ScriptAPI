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

#include "ScriptApiSkirmish.h"

namespace scriptapi
{

namespace
{

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

/// Optional 1-based index on the wire, returned 0-based; -1 when absent.
int optionalOneBased(const Json &object, const char *key, int max, const std::string &what)
{
	const Json *value = findOptional(object, key);
	if (value == nullptr)
		return -1;
	const int number = requireInt(*value, what);
	if (number < 1 || number > max)
		badRequest(what + " must be between 1 and " + std::to_string(max));
	return number - 1;
}

SlotType parseSlotType(const Json &value, const std::string &what)
{
	if (!value.is_string())
		badRequest(what + " must be a string");
	const std::string type = value.get<std::string>();
	if (type == "script") return SlotType::Script;
	if (type == "easy_ai") return SlotType::EasyAi;
	if (type == "medium_ai") return SlotType::MediumAi;
	if (type == "hard_ai") return SlotType::HardAi;
	badRequest(what + " must be one of script, easy_ai, medium_ai, hard_ai");
}

SkirmishPlayer parsePlayer(const Json &value, size_t index)
{
	const std::string what = "players[" + std::to_string(index) + "]";
	if (!value.is_object())
		badRequest(what + " must be an object");

	SkirmishPlayer player;
	const auto type = value.find("type");
	if (type == value.end())
		badRequest(what + ".type is required");
	player.type = parseSlotType(*type, what + ".type");

	if (const Json *faction = findOptional(value, "faction"))
	{
		if (!faction->is_string())
			badRequest(what + ".faction must be a string");
		const std::string name = faction->get<std::string>();
		if (name != "random")
			player.faction = name;
	}

	if (const Json *color = findOptional(value, "color"))
	{
		player.color = requireInt(*color, what + ".color");
		if (player.color < 0)
			badRequest(what + ".color must not be negative");
	}

	player.team = optionalOneBased(value, "team", kMaxSkirmishTeams, what + ".team");
	player.startPos = optionalOneBased(value, "start_pos", kMaxSkirmishPlayers, what + ".start_pos");

	if (const Json *name = findOptional(value, "name"))
	{
		if (player.type != SlotType::Script)
			badRequest(what + ".name is only allowed for the script player");
		if (!name->is_string())
			badRequest(what + ".name must be a string");
		player.name = name->get<std::string>();
	}

	return player;
}

} // namespace

const char *slotTypeName(SlotType type)
{
	switch (type)
	{
	case SlotType::Script: return "script";
	case SlotType::EasyAi: return "easy_ai";
	case SlotType::MediumAi: return "medium_ai";
	case SlotType::HardAi: return "hard_ai";
	}
	return "easy_ai";
}

SkirmishRequest parseSkirmishRequest(const Json &body)
{
	SkirmishRequest request;

	const auto map = body.find("map");
	if (map == body.end() || !map->is_string() || map->get<std::string>().empty())
		badRequest("map must be a non-empty string");
	request.map = map->get<std::string>();

	const auto players = body.find("players");
	if (players == body.end() || !players->is_array() || players->empty())
		badRequest("players must be a non-empty array");
	if (players->size() > static_cast<size_t>(kMaxSkirmishPlayers))
		badRequest("players can have at most " + std::to_string(kMaxSkirmishPlayers) + " entries");

	for (size_t i = 0; i < players->size(); ++i)
	{
		SkirmishPlayer player = parsePlayer((*players)[i], i);
		if (player.type == SlotType::Script && i != 0)
			badRequest("the script player must be players[0] and appear only once");
		request.players.push_back(std::move(player));
	}
	if (request.players[0].type != SlotType::Script)
		badRequest("players[0] must be the script player");

	if (const Json *seed = findOptional(body, "seed"))
	{
		request.seed = requireInt(*seed, "seed");
		request.hasSeed = true;
	}

	if (const Json *fps = findOptional(body, "fps_limit"))
	{
		request.fpsLimit = requireInt(*fps, "fps_limit");
		if (request.fpsLimit < kMinFpsLimit || request.fpsLimit > kMaxFpsLimit)
			badRequest("fps_limit must be between " + std::to_string(kMinFpsLimit) + " and " + std::to_string(kMaxFpsLimit));
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

} // namespace scriptapi
