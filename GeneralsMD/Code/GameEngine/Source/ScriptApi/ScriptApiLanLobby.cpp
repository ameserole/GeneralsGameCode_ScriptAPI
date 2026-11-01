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

// ScriptApiLanLobby.cpp //////////////////////////////////////////////////////////////////////////
// Script API commands for LAN games. They drive the real LAN menu screens (LanLobbyMenu.cpp,
// LanGameOptionsMenu.cpp, LanMapSelectMenu.cpp) and TheLAN the way a player's clicks do, so the
// lobby protocol, the host's checks and the game start are the engine's own.
//
// Screen changes are deferred by the shell (it waits for the old screen's shutdown), so commands
// that open a screen reply at once and report the result with lan_lobby / lan_game events.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#if RTS_SCRIPT_API

#include "ScriptApi/ScriptApi.h"
#include "ScriptApiShellHelpers.h"

#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/MultiplayerSettings.h"
#include "Common/NameKeyGenerator.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/InGameUI.h"
#include "GameClient/MapUtil.h"
#include "GameClient/Shell.h"
#include "GameClient/WindowLayout.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/VictoryConditions.h"
#include "GameNetwork/GUIUtil.h"
#include "GameNetwork/IPEnumeration.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/LANGameInfo.h"
#include "GameNetwork/NetworkInterface.h"

#include <algorithm>
#include <set>

// Globals of the LAN menus (LanGameOptionsMenu.cpp, LanLobbyMenu.cpp) that have no header.
extern void StartPressed();
extern Bool LANbuttonPushed;

using scriptapi::CommandError;
using scriptapi::Json;
using scriptapi::Request;
using namespace scriptapi_shell;

namespace
{

const char *const kLobbyScreen = "Menus/LanLobbyMenu.wnd";
const char *const kOptionsScreen = "LanGameOptionsMenu.wnd";
constexpr UnsignedInt kLanStepTimeoutMs = 15000;

[[noreturn]] void fail(scriptapi::ErrorCode code, const std::string &message)
{
	throw CommandError(code, message);
}

Bool isTopScreen(const char *name)
{
	WindowLayout *top = TheShell != nullptr ? TheShell->top() : nullptr;
	AsciiString suffix(name);
	if (suffix.startsWith("Menus/"))
		suffix = suffix.str() + 6;
	return top != nullptr && top->getFilename().endsWithNoCase(suffix);
}

std::string topScreenName()
{
	WindowLayout *top = TheShell != nullptr ? TheShell->top() : nullptr;
	return top != nullptr ? top->getFilename().str() : "none";
}

GameWindow *findWindow(const AsciiString &name)
{
	return TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey(name));
}

std::string ipString(UnsignedInt ip)
{
	AsciiString text;
	text.format("%d.%d.%d.%d", PRINTF_IP_AS_4_INTS(ip));
	return text.str();
}

Json nullIfNegative(Int value, Int offset = 0)
{
	return value < 0 ? Json() : Json(value + offset);
}

const char *slotStateName(SlotState state)
{
	switch (state)
	{
	case SLOT_OPEN: return "open";
	case SLOT_CLOSED: return "closed";
	case SLOT_EASY_AI: return "easy_ai";
	case SLOT_MED_AI: return "medium_ai";
	case SLOT_BRUTAL_AI: return "hard_ai";
	case SLOT_PLAYER: return "player";
	default: return "unknown";
	}
}

SlotState slotStateFor(scriptapi::LanSlotType type)
{
	switch (type)
	{
	case scriptapi::LanSlotType::Closed: return SLOT_CLOSED;
	case scriptapi::LanSlotType::EasyAi: return SLOT_EASY_AI;
	case scriptapi::LanSlotType::MediumAi: return SLOT_MED_AI;
	case scriptapi::LanSlotType::HardAi: return SLOT_BRUTAL_AI;
	default: return SLOT_OPEN;
	}
}

LANGameInfo *lanGame()
{
	return TheLAN != nullptr ? TheLAN->GetMyGame() : nullptr;
}

/// In the options screen of a LAN game that has not started.
LANGameInfo *requireGameSetup()
{
	LANGameInfo *game = lanGame();
	if (game == nullptr || game->isGameInProgress() || !isTopScreen(kOptionsScreen))
		fail(scriptapi::ErrorCode::WrongState, "not in a LAN game setup (lan_host or lan_join first)");
	return game;
}

void requireLobby(const char *command)
{
	if (TheLAN == nullptr || lanGame() != nullptr || !isTopScreen(kLobbyScreen))
		fail(scriptapi::ErrorCode::WrongState, std::string(command) + " works in the LAN lobby (lan_open first)");
}

const MapMetaData *requireMap(const std::string &name)
{
	const MapMetaData *map = TheMapCache->findMap(AsciiString(name.c_str()));
	if (map == nullptr || !map->m_isMultiplayer)
		fail(scriptapi::ErrorCode::InvalidArgument, "unknown multiplayer map \"" + name + "\"");
	return map;
}

Money requireStartingCash(Int amount)
{
	const MultiplayerStartingMoneyList &allowed = TheMultiplayerSettings->getStartingMoneyList();
	const auto match = std::find_if(allowed.begin(), allowed.end(),
		[amount](const Money &money) { return money.countMoney() == static_cast<UnsignedInt>(amount); });
	if (match == allowed.end())
		fail(scriptapi::ErrorCode::InvalidArgument, "starting_cash " + std::to_string(amount) + " is not an allowed amount");
	return *match;
}

void requireColor(Int color)
{
	if (color >= TheMultiplayerSettings->getNumColors())
		fail(scriptapi::ErrorCode::InvalidArgument, "color " + std::to_string(color) + " does not exist");
}

Json gameSummary(LANGameInfo *game)
{
	Int players = 0;
	Int open = 0;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		if (slot->isOccupied())
			++players;
		else if (slot->isOpen())
			++open;
	}
	return Json{
		{"host", toUtf8(game->getPlayerName(0))},
		{"host_ip", ipString(game->getIP(0))},
		{"map", game->getMap().str()},
		{"players", players},
		{"open_slots", open},
		{"in_progress", static_cast<bool>(game->isGameInProgress())},
	};
}

} // namespace

//-------------------------------------------------------------------------------------------------
void ScriptApi::registerLanCommands()
{
	m_router.add("lan_open", [this](const Request &request) { return lanOpen(request); });
	m_router.add("lan_games", [this](const Request &) {
		if (TheLAN == nullptr)
			fail(scriptapi::ErrorCode::WrongState, "the LAN lobby is not open (lan_open first)");
		return Json{{"games", lanLobbyJson()["games"]}};
	});
	m_router.add("lan_host", [this](const Request &request) { return lanHost(request); });
	m_router.add("lan_join", [this](const Request &request) { return lanJoin(request); });
	m_router.add("lan_slot", [this](const Request &request) { return lanSlot(request); });
	m_router.add("lan_accept", [this](const Request &) {
		LANGameInfo *game = requireGameSetup();
		// The host has no accept button; starting the game is its accept.
		if (!game->amIHost())
			TheLAN->RequestAccept();
		return Json::object();
	});
	m_router.add("lan_game", [this](const Request &) {
		requireGameSetup();
		return lanGameJson();
	});
	m_router.add("lan_start", [this](const Request &) { return lanStart(); });
	m_router.add("lan_leave", [this](const Request &) { return lanLeave(); });
	m_router.add("surrender", [this](const Request &) {
		surrender();
		return Json::object();
	});
}

//-------------------------------------------------------------------------------------------------
Json ScriptApi::lanOpen(const Request &request)
{
	const std::string name = scriptapi::parseLanPlayerName(request.body, "name");

	if (currentGameState() != scriptapi::GameState::Shell || TheNetwork != nullptr || m_lanStep != LanStep::None)
		fail(scriptapi::ErrorCode::WrongState, "lan_open works from the main menu");
	if (!isTopScreen("MainMenu.wnd"))
		fail(scriptapi::ErrorCode::WrongState, "lan_open works from the main menu (current screen: " + topScreenName() + ")");

	// The IP the lobby binds to, as chosen in the options menu. Each multi-instance client has its own 127.x address.
	const auto ip = request.body.find("ip");
	if (ip != request.body.end() && !ip->is_null())
	{
		if (!ip->is_string())
			fail(scriptapi::ErrorCode::BadRequest, "ip must be a string");
		IPEnumeration ips;
		EnumeratedIP *entry = ips.getAddresses();
		while (entry != nullptr && entry->getIPstring().compareNoCase(ip->get<std::string>().c_str()) != 0)
			entry = entry->getNext();
		if (entry == nullptr)
			fail(scriptapi::ErrorCode::InvalidArgument, "ip " + ip->get<std::string>() + " is not an address of this computer");
		TheWritableGlobalData->m_defaultIP = entry->getIP();
	}

	// What the main menu's Network button does.
	TheShell->push(kLobbyScreen);

	m_lanName = name;
	m_lanStep = LanStep::OpeningLobby;
	m_lanStepStarted = timeGetTime();
	m_lanLobbyChanges.reset();
	m_lanGameChanges.reset();
	DEBUG_LOG(("ScriptApi: opening the LAN lobby as %s", name.c_str()));
	return Json::object();
}

//-------------------------------------------------------------------------------------------------
Json ScriptApi::lanHost(const Request &request)
{
	const scriptapi::LanHostRequest setup = scriptapi::parseLanHostRequest(request.body);
	if (m_lanStep != LanStep::None)
		fail(scriptapi::ErrorCode::WrongState, "another LAN action is in progress");
	requireLobby("lan_host");

	// Check everything before creating the game.
	const MapMetaData *map = requireMap(setup.map);
	std::set<Int> colors;
	std::set<Int> startPositions;
	for (size_t i = 1; i < setup.slots.size(); ++i)
	{
		const scriptapi::LanSlotSetup &slot = setup.slots[i];
		if (!slot.faction.empty())
			findFaction(slot.faction);
		if (slot.color >= 0)
		{
			requireColor(slot.color);
			if (!colors.insert(slot.color).second)
				fail(scriptapi::ErrorCode::InvalidArgument, "color " + std::to_string(slot.color) + " is used twice");
		}
		if (slot.startPos >= 0)
		{
			if (slot.startPos >= map->m_numPlayers)
				fail(scriptapi::ErrorCode::InvalidArgument, "start_pos " + std::to_string(slot.startPos + 1) + " is not on this map");
			if (!startPositions.insert(slot.startPos).second)
				fail(scriptapi::ErrorCode::InvalidArgument, "start_pos " + std::to_string(slot.startPos + 1) + " is used twice");
		}
	}
	if (setup.hasStartingCash)
		requireStartingCash(setup.startingCash);

	// The lobby's Host button. The game options screen opens on a later frame; updateLan() applies the setup then.
	m_lanHostSetup = setup;
	m_lanStep = LanStep::Hosting;
	m_lanStepStarted = timeGetTime();
	TheLAN->RequestGameCreate(UnicodeString::TheEmptyString, FALSE);
	if (lanGame() == nullptr)
	{
		m_lanStep = LanStep::None;
		m_lanHostSetup.reset();
		fail(scriptapi::ErrorCode::NotAvailable, "the LAN lobby refused to create a game");
	}

	DEBUG_LOG(("ScriptApi: hosting a LAN game on %s", map->m_fileName.str()));
	return Json{{"local_ip", ipString(TheLAN->GetLocalIP())}};
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::applyLanHostSetup(const scriptapi::LanHostRequest &setup)
{
	LANGameInfo *game = lanGame();
	const MapMetaData *map = TheMapCache->findMap(AsciiString(setup.map.c_str()));
	if (game == nullptr || map == nullptr)
		return;

	// What LanMapSelectMenu does when the host picks a map.
	game->setMap(map->m_fileName.isEmpty() ? AsciiString(setup.map.c_str()) : map->m_fileName);
	game->getSlot(0)->setMapAvailability(true);
	game->setMapCRC(map->m_CRC);
	game->setMapSize(map->m_filesize);
	game->resetStartSpots();
	game->adjustSlotsForMap();

	if (setup.hasStartingCash)
		game->setStartingCash(requireStartingCash(setup.startingCash));
	game->setSuperweaponRestriction(setup.limitSuperweapons ? 1 : 0);

	// What the host's player combo boxes do. Slots not in the request are closed.
	GameWindow *templateCombos[MAX_SLOTS];
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		AsciiString name;
		name.format("LanGameOptionsMenu.wnd:ComboBoxPlayerTemplate%d", i);
		templateCombos[i] = findWindow(name);
	}
	for (Int i = 1; i < MAX_SLOTS; ++i)
	{
		const scriptapi::LanSlotSetup *wanted = i < static_cast<Int>(setup.slots.size()) ? &setup.slots[i] : nullptr;
		LANGameSlot *slot = game->getLANSlot(i);
		const Bool wasAI = slot->isAI();
		slot->setState(wanted != nullptr ? slotStateFor(wanted->type) : SLOT_CLOSED);
		const Bool isAI = slot->isAI();
		if ((wasAI != isAI) && templateCombos[i] != nullptr)
			PopulatePlayerTemplateComboBox(i, templateCombos, game, wasAI);

		if (isAI && wanted != nullptr)
		{
			slot->setPlayerTemplate(wanted->faction.empty() ? PLAYERTEMPLATE_RANDOM : findFaction(wanted->faction));
			slot->setColor(wanted->color);
			slot->setTeamNumber(wanted->team);
			slot->setStartPos(wanted->startPos);
		}
	}

	game->resetAccepted();
	TheLAN->RequestGameOptions(GenerateGameOptionsString(), true);
	lanUpdateSlotList();
	updateGameOptions();
	TheLAN->RequestGameAnnounce();
}

//-------------------------------------------------------------------------------------------------
Json ScriptApi::lanJoin(const Request &request)
{
	const auto hostIp = request.body.find("host_ip");
	const auto host = request.body.find("host");
	const Bool byIp = hostIp != request.body.end() && hostIp->is_string();
	const Bool byName = host != request.body.end() && host->is_string();
	if (byIp == byName)
		fail(scriptapi::ErrorCode::BadRequest, "lan_join needs exactly one of host_ip or host (strings)");
	if (m_lanStep != LanStep::None)
		fail(scriptapi::ErrorCode::WrongState, "another LAN action is in progress");
	requireLobby("lan_join");

	if (byIp)
	{
		// The direct connect screen's join: ask that address for its game, then join it.
		const UnsignedInt ip = ResolveIP(AsciiString(hostIp->get<std::string>().c_str()));
		if (ip == 0)
			fail(scriptapi::ErrorCode::InvalidArgument, "host_ip " + hostIp->get<std::string>() + " is not a valid address");
		TheLAN->RequestGameJoinDirectConnect(ip);
	}
	else
	{
		// The lobby's Join button, with the game picked from the game list.
		const UnicodeString wanted = fromUtf8(host->get<std::string>());
		LANGameInfo *found = nullptr;
		for (Int i = 0; LANGameInfo *game = TheLAN->LookupGameByListOffset(i); ++i)
		{
			if (game->getPlayerName(0).compareNoCase(wanted) == 0 && !game->isGameInProgress())
			{
				found = game;
				break;
			}
		}
		if (found == nullptr)
			fail(scriptapi::ErrorCode::InvalidArgument, "no open game hosted by \"" + host->get<std::string>() + "\" in the lobby");
		TheLAN->RequestGameJoin(found);
	}

	m_lanStep = LanStep::Joining;
	m_lanStepStarted = timeGetTime();
	return Json::object();
}

//-------------------------------------------------------------------------------------------------
Json ScriptApi::lanSlot(const Request &request)
{
	const scriptapi::LanSlotRequest wanted = scriptapi::parseLanSlotRequest(request.body);
	LANGameInfo *game = requireGameSetup();
	const Int local = game->getLocalSlotNum();
	if (local < 0)
		fail(scriptapi::ErrorCode::WrongState, "not in a slot of this game");
	LANGameSlot *slot = game->getLANSlot(local);
	const MapMetaData *map = TheMapCache->findMap(game->getMap());

	const Int oldTemplate = slot->getPlayerTemplate();
	Int playerTemplate = oldTemplate;
	Int color = slot->getColor();
	Int team = slot->getTeamNumber();
	Int startPos = slot->getStartPos();

	if (wanted.observer)
		playerTemplate = PLAYERTEMPLATE_OBSERVER;
	else if (wanted.hasFaction)
		playerTemplate = wanted.faction.empty() ? PLAYERTEMPLATE_RANDOM : findFaction(wanted.faction);
	else if (request.body.contains("observer") && oldTemplate == PLAYERTEMPLATE_OBSERVER)
		playerTemplate = PLAYERTEMPLATE_RANDOM;

	// Becoming or leaving an observer resets color, team and start position (handlePlayerTemplateSelection).
	if ((playerTemplate == PLAYERTEMPLATE_OBSERVER) != (oldTemplate == PLAYERTEMPLATE_OBSERVER))
	{
		color = -1;
		team = -1;
		startPos = -1;
	}

	if (wanted.hasColor)
	{
		if (playerTemplate == PLAYERTEMPLATE_OBSERVER && wanted.color >= 0)
			fail(scriptapi::ErrorCode::BadRequest, "observers have no color");
		color = wanted.color;
		if (color >= 0)
		{
			requireColor(color);
			for (Int i = 0; i < MAX_SLOTS; ++i)
			{
				if (i != local && game->getConstSlot(i)->getColor() == color)
					fail(scriptapi::ErrorCode::InvalidArgument, "color " + std::to_string(color) + " is taken");
			}
		}
	}
	if (wanted.hasTeam)
		team = wanted.team;
	if (wanted.hasStartPos)
	{
		startPos = wanted.startPos;
		if (startPos >= 0)
		{
			if (map == nullptr || startPos >= map->m_numPlayers)
				fail(scriptapi::ErrorCode::InvalidArgument, "start_pos " + std::to_string(startPos + 1) + " is not on this map");
			for (Int i = 0; i < MAX_SLOTS; ++i)
			{
				if (i != local && game->getConstSlot(i)->getStartPos() == startPos)
					fail(scriptapi::ErrorCode::InvalidArgument, "start_pos " + std::to_string(startPos + 1) + " is taken");
			}
		}
	}

	const Bool templateChanged = playerTemplate != oldTemplate;
	const Bool colorChanged = color != slot->getColor();
	const Bool teamChanged = team != slot->getTeamNumber();
	const Bool startPosChanged = startPos != slot->getStartPos();

	// What the slot's combo boxes and the map's start position buttons do.
	slot->setPlayerTemplate(playerTemplate);
	slot->setColor(color);
	slot->setTeamNumber(team);
	slot->setStartPos(startPos);
	if (templateChanged || teamChanged || startPosChanged)
		game->resetAccepted();

	if (game->amIHost())
	{
		TheLAN->RequestGameOptions(GenerateGameOptionsString(), true);
		lanUpdateSlotList();
	}
	else
	{
		// A joiner asks the host, which sends the new slot list to everyone.
		AsciiString option;
		if (templateChanged)
		{
			option.format("PlayerTemplate=%d", playerTemplate);
			TheLAN->RequestGameOptions(option, true);
		}
		if (colorChanged)
		{
			option.format("Color=%d", color);
			TheLAN->RequestGameOptions(option, true);
		}
		if (teamChanged)
		{
			option.format("Team=%d", team);
			TheLAN->RequestGameOptions(option, true);
		}
		if (startPosChanged)
		{
			option.format("StartPos=%d", startPos);
			TheLAN->RequestGameOptions(option, true);
		}
		lanUpdateSlotList();
	}

	return lanGameJson();
}

//-------------------------------------------------------------------------------------------------
Json ScriptApi::lanStart()
{
	LANGameInfo *game = requireGameSetup();
	if (!game->amIHost())
		fail(scriptapi::ErrorCode::NotAvailable, "only the host can start the game");

	// StartPressed() reports these as lobby chat; check them first to give the script a reason.
	Int numUsers = 0;
	Int numHumans = 0;
	Int numRandomTeams = 0;
	std::set<Int> teams;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		if (!slot->isOccupied() || slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER)
			continue;
		++numUsers;
		if (slot->isHuman())
			++numHumans;
		if (slot->getTeamNumber() >= 0)
			teams.insert(slot->getTeamNumber());
		else
			++numRandomTeams;
	}

	const MapMetaData *map = TheMapCache->findMap(game->getMap());
	if (map == nullptr || map->m_numPlayers < numUsers)
	{
		fail(scriptapi::ErrorCode::InvalidArgument, "the map supports " + std::to_string(map ? map->m_numPlayers : 0)
			+ " players and " + std::to_string(numUsers) + " are set up");
	}
	const Int minPlayers = TheGlobalData->m_netMinPlayers;
	if (minPlayers && numHumans == 0)
		fail(scriptapi::ErrorCode::InvalidArgument, "the game needs a human (non-observer) player");
	if (numUsers < minPlayers || numRandomTeams + static_cast<Int>(teams.size()) < minPlayers)
		fail(scriptapi::ErrorCode::InvalidArgument, "not enough players or teams");

	std::string waiting;
	for (Int i = 1; i < MAX_SLOTS; ++i)
	{
		LANGameSlot *slot = game->getLANSlot(i);
		if (slot->isHuman() && !slot->isAccepted())
			waiting += (waiting.empty() ? "" : ", ") + toUtf8(slot->getName());
	}
	if (!waiting.empty())
		fail(scriptapi::ErrorCode::NotReady, "waiting for players to accept: " + waiting);

	// The host's Start button.
	StartPressed();
	DEBUG_LOG(("ScriptApi: starting the LAN game"));
	return Json::object();
}

//-------------------------------------------------------------------------------------------------
Json ScriptApi::lanLeave()
{
	if (TheLAN == nullptr || m_lanStep != LanStep::None)
		fail(scriptapi::ErrorCode::WrongState, "not in the LAN lobby or a LAN game setup");

	LANGameInfo *game = lanGame();
	if (game != nullptr && !game->isGameInProgress() && isTopScreen(kOptionsScreen))
	{
		// The game options screen's Back button.
		m_lanLeaving = TRUE;
		TheLAN->RequestGameLeave();
		return Json{{"left", "game"}};
	}
	if (game == nullptr && isTopScreen(kLobbyScreen))
	{
		// The lobby's Back button.
		LANbuttonPushed = true;
		TheShell->pop();
		delete TheLAN;
		TheLAN = nullptr;
		return Json{{"left", "lobby"}};
	}
	fail(scriptapi::ErrorCode::WrongState, "not in the LAN lobby or a LAN game setup");
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::surrender()
{
	requireInGame();
	if (TheNetwork == nullptr)
		fail(scriptapi::ErrorCode::WrongState, "surrender is for network games; use quit_game in a skirmish");
	const Player *me = ThePlayerList->getLocalPlayer();
	if (me == nullptr || !me->isPlayerActive())
		fail(scriptapi::ErrorCode::NotAvailable, "observers and defeated players cannot surrender");
	if (TheVictoryConditions->isLocalAlliedVictory())
		fail(scriptapi::ErrorCode::WrongState, "the game is already won");

	// The quit menu's Surrender button.
	GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_SELF_DESTRUCT);
	msg->appendBooleanArgument(TRUE);
	TheInGameUI->setClientQuiet(TRUE);
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::updateLan()
{
	// Finish a screen change the script asked for once the new screen is up.
	switch (m_lanStep)
	{
	case LanStep::OpeningLobby:
		if (TheLAN != nullptr && isTopScreen(kLobbyScreen) && TheShell->isAnimFinished())
		{
			// What typing a name into the lobby's name box does.
			const UnicodeString name = fromUtf8(m_lanName);
			if (GameWindow *box = findWindow("LanLobbyMenu.wnd:TextEntryPlayerName"))
				GadgetTextEntrySetText(box, name);
			TheLAN->RequestSetName(name);
			m_lanStep = LanStep::None;
		}
		break;
	case LanStep::Hosting:
		if (lanGame() != nullptr && isTopScreen(kOptionsScreen))
		{
			if (m_lanHostSetup)
				applyLanHostSetup(*m_lanHostSetup);
			m_lanHostSetup.reset();
			m_lanStep = LanStep::None;
		}
		break;
	case LanStep::Joining:
		if (lanGame() != nullptr && isTopScreen(kOptionsScreen))
			m_lanStep = LanStep::None;
		break;
	case LanStep::None:
		break;
	}
	if (m_lanStep != LanStep::None && timeGetTime() - m_lanStepStarted > kLanStepTimeoutMs)
	{
		const char *action = m_lanStep == LanStep::Joining ? "join" : m_lanStep == LanStep::Hosting ? "host" : "open";
		sendEvent("lan_failed", Json{{"action", action}, {"reason", "timeout"}, {"screen", topScreenName()}});
		m_lanStep = LanStep::None;
		m_lanHostSetup.reset();
	}

	if (TheLAN == nullptr)
	{
		m_lanInGameSetup = FALSE;
		return;
	}

	// Report lobby and game setup changes.
	LANGameInfo *game = lanGame();
	const Bool inSetup = game != nullptr && !game->isGameInProgress() && isTopScreen(kOptionsScreen);
	if (inSetup && m_lanStep == LanStep::None)
	{
		m_lanInGameSetup = TRUE;
		Json json = lanGameJson();
		if (m_lanGameChanges.update(json))
			sendEvent("lan_game", std::move(json));
		return;
	}
	if (m_lanInGameSetup && (game == nullptr || game->isGameInProgress()))
	{
		if (game == nullptr)
			sendEvent("lan_left", Json{{"reason", m_lanLeaving ? "left" : "host_left"}});
		m_lanInGameSetup = FALSE;
		m_lanLeaving = FALSE;
		m_lanGameChanges.reset();
	}
	if (game == nullptr && m_lanStep == LanStep::None && isTopScreen(kLobbyScreen))
	{
		Json json = lanLobbyJson();
		if (m_lanLobbyChanges.update(json))
			sendEvent("lan_lobby", std::move(json));
	}
}

//-------------------------------------------------------------------------------------------------
Json ScriptApi::lanLobbyJson() const
{
	// The players as the lobby's player list shows them.
	Json players = Json::array();
	if (GameWindow *list = findWindow("LanLobbyMenu.wnd:ListboxPlayers"))
	{
		for (Int row = 0; row < GadgetListBoxGetNumEntries(list); ++row)
		{
			players.push_back(Json{
				{"name", toUtf8(GadgetListBoxGetText(list, row))},
				{"ip", ipString(static_cast<UnsignedInt>(reinterpret_cast<uintptr_t>(GadgetListBoxGetItemData(list, row))))},
			});
		}
	}

	Json games = Json::array();
	for (Int i = 0; LANGameInfo *game = TheLAN->LookupGameByListOffset(i); ++i)
		games.push_back(gameSummary(game));

	return Json{
		{"name", toUtf8(TheLAN->GetMyName())},
		{"local_ip", ipString(TheLAN->GetLocalIP())},
		{"players", std::move(players)},
		{"games", std::move(games)},
	};
}

//-------------------------------------------------------------------------------------------------
Json ScriptApi::lanGameJson() const
{
	LANGameInfo *game = lanGame();
	if (game == nullptr)
		return Json();

	Json slots = Json::array();
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		LANGameSlot *slot = game->getLANSlot(i);
		Json entry{{"index", i}, {"state", slotStateName(slot->getState())}};
		if (slot->isOccupied())
		{
			const Int playerTemplate = slot->getPlayerTemplate();
			entry["name"] = toUtf8(slot->getName());
			entry["faction"] = factionName(playerTemplate);
			entry["observer"] = playerTemplate == PLAYERTEMPLATE_OBSERVER;
			entry["color"] = nullIfNegative(slot->getColor());
			entry["team"] = nullIfNegative(slot->getTeamNumber(), 1);
			entry["start_pos"] = nullIfNegative(slot->getStartPos(), 1);
			if (slot->isHuman())
			{
				entry["ip"] = ipString(slot->getIP());
				entry["accepted"] = static_cast<bool>(slot->isAccepted());
				entry["has_map"] = static_cast<bool>(slot->hasMap());
			}
		}
		slots.push_back(std::move(entry));
	}

	const MapMetaData *map = TheMapCache->findMap(game->getMap());
	return Json{
		{"host", toUtf8(game->getPlayerName(0))},
		{"host_ip", ipString(game->getIP(0))},
		{"is_host", static_cast<bool>(game->amIHost())},
		{"local_slot", game->getLocalSlotNum()},
		{"map", game->getMap().str()},
		{"map_players", map != nullptr ? Json(map->m_numPlayers) : Json()},
		{"starting_cash", game->getStartingCash().countMoney()},
		{"limit_superweapons", game->getSuperweaponRestriction() > 0},
		{"slots", std::move(slots)},
	};
}

#endif // RTS_SCRIPT_API
