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

// ScriptApiSkirmish.cpp //////////////////////////////////////////////////////////////////////////
// Script API commands for setting up, starting and leaving skirmish matches. These mirror what the
// skirmish menu (SkirmishGameOptionsMenu.cpp) and the quit menu (QuitMenu.cpp) do, without the UI.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#if RTS_SCRIPT_API

#include "ScriptApi/ScriptApi.h"
#include "ScriptApiShellHelpers.h"

#include "ScriptApiSkirmish.h"

#include "Common/GameCommon.h"
#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/MultiplayerSettings.h"
#include "Common/PlayerTemplate.h"
#include "Common/RandomValue.h"
#include "Common/Recorder.h"
#include "Common/SkirmishPreferences.h"
#include "GameClient/ChallengeGenerals.h"
#include "GameClient/InGameUI.h"
#include "GameClient/MapUtil.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/GameInfo.h"

#include <algorithm>
#include <set>
#include <vector>

using scriptapi::CommandError;
using scriptapi::Json;
using namespace scriptapi_shell;

namespace scriptapi_shell
{

std::string toUtf8(const UnicodeString &text)
{
	const WideChar *wide = text.str();
	const int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
	if (length <= 1)
		return std::string();
	std::string utf8(length - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), length, nullptr, nullptr);
	return utf8;
}

UnicodeString fromUtf8(const std::string &text)
{
	UnicodeString result;
	const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
	if (length > 1)
	{
		std::vector<WideChar> wide(length);
		MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), length);
		result.set(wide.data());
	}
	return result;
}

/// Player templates offered by the skirmish menu, using the filter of PopulatePlayerTemplateComboBox.
std::vector<Int> selectableFactions()
{
	std::vector<Int> factions;
	std::set<AsciiString> seenSides;
	for (Int i = 0; i < ThePlayerTemplateStore->getPlayerTemplateCount(); ++i)
	{
		const PlayerTemplate *faction = ThePlayerTemplateStore->getNthPlayerTemplate(i);
		if (faction == nullptr || faction->getStartingBuilding().isEmpty())
			continue;

		const GeneralPersona *general = TheChallengeGenerals->getGeneralByTemplateName(faction->getName());
		if (general != nullptr && !general->isStartingEnabled())
			continue;

		if (!seenSides.insert(faction->getSide()).second)
			continue;

		factions.push_back(i);
	}
	return factions;
}

Int findFaction(const std::string &name)
{
	for (Int index : selectableFactions())
	{
		if (ThePlayerTemplateStore->getNthPlayerTemplate(index)->getName().compareNoCase(name.c_str()) == 0)
			return index;
	}
	throw CommandError(scriptapi::ErrorCode::InvalidArgument, "unknown or unavailable faction \"" + name + "\"");
}

std::string factionName(Int playerTemplate)
{
	if (playerTemplate == PLAYERTEMPLATE_OBSERVER)
		return "observer";
	const PlayerTemplate *faction = playerTemplate >= 0 ? ThePlayerTemplateStore->getNthPlayerTemplate(playerTemplate) : nullptr;
	return faction != nullptr ? faction->getName().str() : "random";
}

} // namespace scriptapi_shell

namespace
{

SlotState slotStateFor(scriptapi::SlotType type)
{
	switch (type)
	{
	case scriptapi::SlotType::Script: return SLOT_PLAYER;
	case scriptapi::SlotType::EasyAi: return SLOT_EASY_AI;
	case scriptapi::SlotType::MediumAi: return SLOT_MED_AI;
	case scriptapi::SlotType::HardAi: return SLOT_BRUTAL_AI;
	}
	return SLOT_EASY_AI;
}

Json nullIfNegative(Int value, Int offset = 0)
{
	return value < 0 ? Json() : Json(value + offset);
}

} // namespace

//-------------------------------------------------------------------------------------------------
void ScriptApi::registerSkirmishCommands()
{
	using scriptapi::Request;

	m_router.add("skirmish_options", [this](const Request &) { return skirmishOptions(); });
	m_router.add("start_skirmish", [this](const Request &request) { return startSkirmish(request); });
	m_router.add("quit_game", [this](const Request &) {
		quitGame();
		return Json();
	});
	m_router.add("save_replay", [this](const Request &request) { return saveReplay(request); });
}

//-------------------------------------------------------------------------------------------------
Json ScriptApi::saveReplay(const scriptapi::Request &request)
{
	// The recorder writes the replay until the game ends, so it can only be copied from the menus.
	if (currentGameState() != scriptapi::GameState::Shell)
		throw CommandError(scriptapi::ErrorCode::WrongState, "replays can be saved from the menus, after the game");
	const auto nameIt = request.body.find("name");
	if (nameIt == request.body.end() || !nameIt->is_string())
		throw CommandError(scriptapi::ErrorCode::BadRequest, "name must be a string");
	const std::string name = scriptapi::parseReplayName(nameIt->get<std::string>());
	const auto overwriteIt = request.body.find("overwrite");
	if (overwriteIt != request.body.end() && !overwriteIt->is_boolean())
		throw CommandError(scriptapi::ErrorCode::BadRequest, "overwrite must be a boolean");
	const bool overwrite = overwriteIt != request.body.end() && overwriteIt->get<bool>();
	if (!m_playedGame)
		throw CommandError(scriptapi::ErrorCode::NotAvailable, "no game has been played since the game started");

	const AsciiString extension = RecorderClass::getReplayExtention();
	AsciiString source = RecorderClass::getReplayDir();
	source.concat(RecorderClass::getLastReplayFileName());
	source.concat(extension);
	const std::string file = name + extension.str();
	AsciiString destination = RecorderClass::getReplayDir();
	destination.concat(file.c_str());

	if (destination.compareNoCase(source) == 0)
		throw CommandError(scriptapi::ErrorCode::InvalidArgument, "that is the name of the last-replay file");
	if (!overwrite && GetFileAttributes(destination.str()) != INVALID_FILE_ATTRIBUTES)
		throw CommandError(scriptapi::ErrorCode::InvalidArgument, "replay \"" + file + "\" exists; pass overwrite to replace it");
	if (!CopyFile(source.str(), destination.str(), FALSE))
		throw CommandError(scriptapi::ErrorCode::NotAvailable, "could not copy the last replay (error " + std::to_string(GetLastError()) + ")");
	return Json{{"file", file}};
}

//-------------------------------------------------------------------------------------------------
Json ScriptApi::skirmishOptions() const
{
	Json maps = Json::array();
	for (MapCache::const_iterator it = TheMapCache->begin(); it != TheMapCache->end(); ++it)
	{
		const MapMetaData &map = it->second;
		if (!map.m_isMultiplayer || !map.m_doesExist)
			continue;
		maps.push_back(Json{
			{"name", it->first.str()},
			{"display_name", toUtf8(map.m_displayName)},
			{"players", map.m_numPlayers},
			{"official", static_cast<bool>(map.m_isOfficial)},
		});
	}

	Json factions = Json::array();
	for (Int index : selectableFactions())
	{
		const PlayerTemplate *faction = ThePlayerTemplateStore->getNthPlayerTemplate(index);
		factions.push_back(Json{{"name", faction->getName().str()}, {"side", faction->getSide().str()}});
	}

	Json colors = Json::array();
	for (Int i = 0; i < TheMultiplayerSettings->getNumColors(); ++i)
	{
		const MultiplayerColorDefinition *color = TheMultiplayerSettings->getColor(i);
		colors.push_back(Json{{"index", i}, {"name", color ? color->getTooltipName().str() : ""}});
	}

	Json cashOptions = Json::array();
	for (const Money &money : TheMultiplayerSettings->getStartingMoneyList())
		cashOptions.push_back(money.countMoney());

	return Json{
		{"maps", maps},
		{"factions", factions},
		{"colors", colors},
		{"starting_cash", Json{
			{"default", TheMultiplayerSettings->getDefaultStartingMoney().countMoney()},
			{"options", cashOptions},
		}},
		{"ai_types", Json::array({"easy_ai", "medium_ai", "hard_ai"})},
	};
}

//-------------------------------------------------------------------------------------------------
Json ScriptApi::startSkirmish(const scriptapi::Request &request)
{
	const scriptapi::SkirmishRequest setup = scriptapi::parseSkirmishRequest(request.body);

	if (currentGameState() != scriptapi::GameState::Shell || m_transitionPending
		|| TheGameLogic->isClearingGameData() || TheNetwork != nullptr)
	{
		throw CommandError(scriptapi::ErrorCode::WrongState, "a skirmish can only be started from the menus");
	}

	// Resolve and check everything before touching TheSkirmishGameInfo.
	const MapMetaData *map = TheMapCache->findMap(AsciiString(setup.map.c_str()));
	if (map == nullptr || !map->m_isMultiplayer)
		throw CommandError(scriptapi::ErrorCode::InvalidArgument, "unknown skirmish map \"" + setup.map + "\"");
	if (static_cast<Int>(setup.players.size()) > map->m_numPlayers)
	{
		throw CommandError(scriptapi::ErrorCode::InvalidArgument,
			"map \"" + setup.map + "\" supports at most " + std::to_string(map->m_numPlayers) + " players");
	}

	std::vector<Int> templates;
	std::set<Int> usedStartPositions;
	for (const scriptapi::SkirmishPlayer &player : setup.players)
	{
		templates.push_back(player.faction.empty() ? PLAYERTEMPLATE_RANDOM : findFaction(player.faction));

		if (player.color >= TheMultiplayerSettings->getNumColors())
			throw CommandError(scriptapi::ErrorCode::InvalidArgument, "color " + std::to_string(player.color) + " does not exist");

		if (player.startPos >= 0)
		{
			if (player.startPos >= map->m_numPlayers)
				throw CommandError(scriptapi::ErrorCode::InvalidArgument, "start_pos " + std::to_string(player.startPos + 1) + " is not on this map");
			if (!usedStartPositions.insert(player.startPos).second)
				throw CommandError(scriptapi::ErrorCode::InvalidArgument, "start_pos " + std::to_string(player.startPos + 1) + " is used twice");
		}
	}

	Money startingCash = TheMultiplayerSettings->getDefaultStartingMoney();
	if (setup.hasStartingCash)
	{
		const MultiplayerStartingMoneyList &allowed = TheMultiplayerSettings->getStartingMoneyList();
		const auto match = std::find_if(allowed.begin(), allowed.end(),
			[&setup](const Money &money) { return money.countMoney() == static_cast<UnsignedInt>(setup.startingCash); });
		if (match == allowed.end())
			throw CommandError(scriptapi::ErrorCode::InvalidArgument, "starting_cash " + std::to_string(setup.startingCash) + " is not an allowed amount");
		startingCash = *match;
	}

	// Same preparation as SkirmishGameOptionsMenuInit.
	if (TheSkirmishGameInfo == nullptr)
		TheSkirmishGameInfo = NEW SkirmishGameInfo;
	else if (TheSkirmishGameInfo->isInGame())
		TheSkirmishGameInfo->endGame();

	TheSkirmishGameInfo->init();
	TheSkirmishGameInfo->clearSlotList();
	TheSkirmishGameInfo->reset();
	TheSkirmishGameInfo->setLocalIP(TheSkirmishGameInfo->getSlot(0)->getIP());
	TheSkirmishGameInfo->enterGame();

	SkirmishPreferences prefs;
	Json players = Json::array();
	for (size_t i = 0; i < setup.players.size(); ++i)
	{
		const scriptapi::SkirmishPlayer &player = setup.players[i];
		GameSlot slot;
		if (player.type == scriptapi::SlotType::Script)
			slot.setState(SLOT_PLAYER, player.name.empty() ? prefs.getUserName() : fromUtf8(player.name));
		else
			slot.setState(slotStateFor(player.type));
		slot.setPlayerTemplate(templates[i]);
		slot.setColor(player.color);
		slot.setStartPos(player.startPos);
		slot.setTeamNumber(player.team);
		TheSkirmishGameInfo->setSlot(static_cast<Int>(i), slot);

		players.push_back(Json{
			{"slot", i},
			{"type", scriptapi::slotTypeName(player.type)},
			{"faction", player.faction.empty() ? "random" : player.faction},
			{"color", nullIfNegative(player.color)},
			{"team", nullIfNegative(player.team, 1)},
			{"start_pos", nullIfNegative(player.startPos, 1)},
		});
	}

	TheSkirmishGameInfo->setStartingCash(startingCash);
	TheSkirmishGameInfo->setSuperweaponRestriction(setup.limitSuperweapons ? 1 : 0);
	TheSkirmishGameInfo->setMap(map->m_fileName.isEmpty() ? AsciiString(setup.map.c_str()) : map->m_fileName);
	TheSkirmishGameInfo->setMapCRC(map->m_CRC);
	TheSkirmishGameInfo->setMapSize(map->m_filesize);
	TheSkirmishGameInfo->setSeed(setup.hasSeed ? setup.seed : static_cast<Int>(GetTickCount()));

	// Same as reallyDoStart, with the frame rate from the request instead of the menu slider.
	if (TheGameLogic->isInGame())
		TheGameLogic->clearGameData(FALSE);

	TheWritableGlobalData->m_mapName = TheSkirmishGameInfo->getMap();
	TheSkirmishGameInfo->startGame(0);
	InitRandom(TheSkirmishGameInfo->getSeed());

	GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_NEW_GAME);
	msg->appendIntegerArgument(GAME_SKIRMISH);
	msg->appendIntegerArgument(DIFFICULTY_NORMAL);
	msg->appendIntegerArgument(0);
	msg->appendIntegerArgument(setup.fpsLimit);

	m_transitionPending = TRUE;
	DEBUG_LOG(("ScriptApi: starting skirmish on %s with %d players", TheSkirmishGameInfo->getMap().str(), (Int)setup.players.size()));

	return Json{
		{"map", TheSkirmishGameInfo->getMap().str()},
		{"seed", TheSkirmishGameInfo->getSeed()},
		{"fps_limit", setup.fpsLimit},
		{"starting_cash", startingCash.countMoney()},
		{"limit_superweapons", setup.limitSuperweapons},
		{"players", players},
	};
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::quitGame()
{
	if (currentGameState() != scriptapi::GameState::InGame || m_transitionPending)
		throw CommandError(scriptapi::ErrorCode::WrongState, "not in a game");
	if (!TheGameLogic->isInSkirmishGame() && TheNetwork == nullptr)
		throw CommandError(scriptapi::ErrorCode::WrongState, "quit_game supports skirmish and LAN games");

	// What GameLogic::quit(FALSE) does once the quit menu is confirmed. Leaving a network game
	// surrenders first, so the other players see a defeat rather than a disconnect.
	if (TheNetwork != nullptr && TheGameInfo != nullptr && !TheGameInfo->isSandbox())
	{
		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_SELF_DESTRUCT);
		msg->appendBooleanArgument(TRUE);
	}
	if (TheRecorder != nullptr && TheRecorder->getMode() == RECORDERMODETYPE_RECORD)
		TheRecorder->stopRecording();

	TheGameLogic->exitGame();

	if (TheInGameUI != nullptr)
		TheInGameUI->setClientQuiet(TRUE);

	m_transitionPending = TRUE;
	DEBUG_LOG(("ScriptApi: quitting the game"));
}

#endif // RTS_SCRIPT_API
