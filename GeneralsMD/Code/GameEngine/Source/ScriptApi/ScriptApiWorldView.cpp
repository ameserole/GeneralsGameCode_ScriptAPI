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

// ScriptApiWorldView.cpp /////////////////////////////////////////////////////////////////////////
// Script API read-only world view: match facts, the faction catalog, the terrain grid, fog-of-war
// limited snapshots and the local player's event hooks. Everything here only reads engine state,
// and nothing about other players is reported unless the local human could see it
// (scriptapi::isVisibleToPlayer).
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#if RTS_SCRIPT_API

#include "ScriptApi/ScriptApi.h"
#include "ScriptApiOrderHelpers.h"
#include "ScriptApiShellHelpers.h"

#include "Common/GameCommon.h"
#include "Common/GlobalData.h"
#include "Common/KindOf.h"
#include "Common/MessageStream.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/ScoreKeeper.h"
#include "Common/ProductionPrerequisite.h"
#include "Common/Science.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "Common/Upgrade.h"
#include "GameClient/ControlBar.h"
#include "GameClient/MapUtil.h"
#include "GameLogic/AI.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/Damage.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/ContainModule.h"
#include "GameLogic/Module/StealthUpdate.h"
#include "GameLogic/Module/SupplyTruckAIUpdate.h"
#include "GameLogic/Module/SupplyWarehouseDockUpdate.h"
#include "GameLogic/Object.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/VictoryConditions.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <set>

using scriptapi::CommandError;
using scriptapi::Json;
using scriptapi::Relation;
using scriptapi::Shroud;

namespace
{

constexpr int kDefaultMapCellSize = 20;
constexpr int kMinMapCellSize = 10;
constexpr int kMaxMapCellSize = 200;
constexpr size_t kMaxTemplateInfoNames = 500;
constexpr UnsignedInt kVisibilityScanInterval = 3;  ///< frames between enemy-visibility scans (0.1 s)
constexpr UnsignedInt kDamageWindow = 15;            ///< frames of damage grouped into one unit_damaged

std::string toUtf8(const UnicodeString &text)
{
	const int length = WideCharToMultiByte(CP_UTF8, 0, text.str(), -1, nullptr, 0, nullptr, nullptr);
	if (length <= 1)
		return std::string();
	std::string utf8(length - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.str(), -1, utf8.data(), length, nullptr, nullptr);
	return utf8;
}

Player *localPlayer()
{
	return ThePlayerList != nullptr ? ThePlayerList->getLocalPlayer() : nullptr;
}

Json position(const Coord3D &pos)
{
	return Json::array({pos.x, pos.y, pos.z});
}

Json kinds(const ThingTemplate *tmpl)
{
	Json result = Json::array();
	for (Int i = KINDOF_FIRST; i < KINDOF_COUNT; ++i)
	{
		if (tmpl->isKindOf(static_cast<KindOfType>(i)))
			result.push_back(KindOfMaskType::getNameFromSingleBit(i));
	}
	return result;
}

/// Objects that are never reported: effects, scenery and things the GUI ignores. Supply docks and
/// piles are IGNORED_IN_GUI (they can't be selected), but they are drawn and every player sees them.
/// Kinds of object a player thinks of as things in the world, rather than shots, debris or scenery.
Bool isReportableKind(const Object *obj)
{
	if (obj->isKindOf(KINDOF_SUPPLY_SOURCE))
		return TRUE;
	return !obj->isKindOf(KINDOF_PROJECTILE) && !obj->isKindOf(KINDOF_SHRUBBERY) && !obj->isKindOf(KINDOF_OPTIMIZED_TREE)
		&& !obj->isKindOf(KINDOF_INERT) && !obj->isKindOf(KINDOF_IGNORED_IN_GUI);
}

Bool isReportable(const Object *obj)
{
	if (obj->isEffectivelyDead() || obj->getControllingPlayer() == nullptr)
		return FALSE;
	return isReportableKind(obj);
}

Relation relationOf(const Player *me, const Player *owner)
{
	if (owner == me)
		return Relation::Self;
	switch (me->getRelationship(owner->getDefaultTeam()))
	{
	case ALLIES: return Relation::Ally;
	case ENEMIES: return Relation::Enemy;
	default: return Relation::Neutral;
	}
}

Relation relationOf(const Player *me, const Object *obj)
{
	if (obj->getControllingPlayer() == me)
		return Relation::Self;
	switch (me->getRelationship(obj->getTeam()))
	{
	case ALLIES: return Relation::Ally;
	case ENEMIES: return Relation::Enemy;
	default: return Relation::Neutral;
	}
}

Shroud shroudOf(const Object *obj, Int playerIndex)
{
	switch (obj->getShroudedStatus(playerIndex))
	{
	case OBJECTSHROUD_CLEAR: return Shroud::Clear;
	case OBJECTSHROUD_PARTIAL_CLEAR: return Shroud::PartialClear;
	case OBJECTSHROUD_FOGGED: return Shroud::Fogged;
	default: return Shroud::Shrouded; // unknown counts as hidden
	}
}

Bool isVisibleTo(const Player *me, const Object *obj)
{
	return scriptapi::isVisibleToPlayer(relationOf(me, obj), shroudOf(obj, me->getPlayerIndex()),
		obj->testStatus(OBJECT_STATUS_STEALTHED), obj->testStatus(OBJECT_STATUS_DETECTED));
}

Json statusFlags(const Object *obj)
{
	Json flags = Json::array();
	if (obj->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION)) flags.push_back("under_construction");
	if (obj->testStatus(OBJECT_STATUS_SOLD)) flags.push_back("sold");
	if (obj->testStatus(OBJECT_STATUS_STEALTHED)) flags.push_back("stealthed");
	if (obj->testStatus(OBJECT_STATUS_DETECTED)) flags.push_back("detected");
	if (obj->testStatus(OBJECT_STATUS_DISGUISED)) flags.push_back("disguised");
	return flags;
}

Real healthOf(const Object *obj, Real *maxHealth)
{
	const BodyModuleInterface *body = obj->getBodyModule();
	*maxHealth = body ? body->getMaxHealth() : 0.0f;
	return body ? body->getHealth() : 0.0f;
}

/// The template a player sees: enemies see disguised units as what they are disguised as.
const ThingTemplate *apparentTemplate(const Object *obj, Relation relation)
{
	if (relation == Relation::Enemy && obj->testStatus(OBJECT_STATUS_DISGUISED))
	{
		StealthUpdate *stealth = obj->getStealth();
		if (stealth != nullptr && stealth->isDisguised() && stealth->getDisguisedTemplate() != nullptr)
			return stealth->getDisguisedTemplate();
	}
	return obj->getTemplate();
}

Json ownObjectJson(const Object *obj)
{
	Real maxHealth;
	const Real health = healthOf(obj, &maxHealth);
	Json json{
		{"id", obj->getID()},
		{"template", obj->getTemplate()->getName().str()},
		{"pos", position(*obj->getPosition())},
		{"angle", obj->getOrientation()},
		{"health", health},
		{"max_health", maxHealth},
		{"status", statusFlags(obj)},
		{"veterancy", static_cast<Int>(obj->getVeterancyLevel())},
	};
	// What the player sees of their own units' activity: parked, or ferrying supplies.
	const AIUpdateInterface *ai = obj->getAI();
	if (ai != nullptr && ai->isIdle())
		json["status"].push_back("idle");
	const SupplyTruckAIInterface *truck = ai != nullptr ? ai->getSupplyTruckAIInterface() : nullptr;
	if (truck != nullptr && truck->isCurrentlyFerryingSupplies())
		json["status"].push_back("harvesting");
	if (obj->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION))
		json["construction_percent"] = obj->getConstructionPercent();
	const Object *container = obj->getContainedBy();
	json["contained_by"] = container ? Json(container->getID()) : Json();
	return json;
}

Json otherObjectJson(const Object *obj, Relation relation)
{
	Real maxHealth;
	const Real health = healthOf(obj, &maxHealth);
	Json json{
		{"id", obj->getID()},
		{"template", apparentTemplate(obj, relation)->getName().str()},
		{"owner", obj->getControllingPlayer()->getPlayerIndex()},
		{"pos", position(*obj->getPosition())},
		{"health_pct", maxHealth > 0.0f ? health / maxHealth : 0.0f},
		{"status", statusFlags(obj)},
	};
	// The dollar value the mouse-over tooltip shows for a supply source (InGameUI.cpp).
	static const NameKeyType warehouseModuleKey = TheNameKeyGenerator->nameToKey("SupplyWarehouseDockUpdate");
	const SupplyWarehouseDockUpdate *warehouse = static_cast<const SupplyWarehouseDockUpdate *>(obj->findUpdateModule(warehouseModuleKey));
	if (warehouse != nullptr)
		json["supply"] = warehouse->getBoxesStored() * TheGlobalData->m_baseValuePerSupplyBox;
	return json;
}

scriptapi::KnownStructure knownStructure(const Object *obj, Relation relation, UnsignedInt frame)
{
	scriptapi::KnownStructure entry;
	entry.id = obj->getID();
	entry.templateName = apparentTemplate(obj, relation)->getName().str();
	entry.owner = obj->getControllingPlayer()->getPlayerIndex();
	entry.x = obj->getPosition()->x;
	entry.y = obj->getPosition()->y;
	entry.z = obj->getPosition()->z;
	entry.lastSeenFrame = frame;
	return entry;
}

Json templateJson(const ThingTemplate *tmpl, const Player *me)
{
	const Int buildFrames = tmpl->calcTimeToBuild(me);
	return Json{
		{"name", tmpl->getName().str()},
		{"side", tmpl->getDefaultOwningSide().str()},
		{"kinds", kinds(tmpl)},
		{"cost", tmpl->calcCostToBuild(me)},
		{"build_time", static_cast<Real>(buildFrames) / LOGICFRAMES_PER_SECOND},
		{"energy", tmpl->getEnergyProduction()},
		{"radius", tmpl->getTemplateGeometryInfo().getBoundingCircleRadius()},
		{"buildable", tmpl->getBuildable() == BSTATUS_YES},
	};
}

/// Requirements as groups; any one template in a group satisfies it.
void addPrerequisites(Json &entry, const ThingTemplate *tmpl, const Player *me)
{
	Json groups = Json::array();
	Json sciences = Json::array();
	Bool available = TRUE;
	for (Int i = 0; i < tmpl->getPrereqCount(); ++i)
	{
		const ProductionPrerequisite *prereq = tmpl->getNthPrereq(i);
		available = available && prereq->isSatisfied(me);
		for (Int u = 0; u < prereq->getUnitPrereqCount(); ++u)
		{
			const ThingTemplate *unit = prereq->getUnitPrereq(u);
			if (unit == nullptr)
				continue;
			if (prereq->isUnitPrereqOrWithPrevious(u) && !groups.empty())
				groups.back().push_back(unit->getName().str());
			else
				groups.push_back(Json::array({unit->getName().str()}));
		}
		for (Int s = 0; s < prereq->getSciencePrereqCount(); ++s)
			sciences.push_back(TheScienceStore->getInternalNameForScience(prereq->getSciencePrereq(s)).str());
	}
	entry["prerequisites"] = groups;
	entry["science_prerequisites"] = sciences;
	entry["available_now"] = static_cast<bool>(available);
}

/// The players the API reports on: playable and not observing, as game_info lists them.
std::vector<Player *> reportedPlayers()
{
	std::vector<Player *> players;
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
	{
		Player *player = ThePlayerList->getNthPlayer(i);
		if (player == nullptr || player->isPlayerObserver())
			continue;
		const PlayerTemplate *tmpl = player->getPlayerTemplate();
		if (tmpl != nullptr && tmpl->isPlayableSide())
			players.push_back(player);
	}
	return players;
}

/// What the score screen shows for a player.
Json playerStatsJson(Player *player)
{
	ScoreKeeper *score = player->getScoreKeeper();
	return Json{
		{"index", player->getPlayerIndex()},
		{"side", player->getSide().str()},
		{"score", score->calculateScore()},
		{"units_built", score->getTotalUnitsBuilt()},
		{"units_lost", score->getTotalUnitsLost()},
		{"units_destroyed", score->getTotalUnitsDestroyed()},
		{"buildings_built", score->getTotalBuildingsBuilt()},
		{"buildings_lost", score->getTotalBuildingsLost()},
		{"buildings_destroyed", score->getTotalBuildingsDestroyed()},
		{"money_earned", score->getTotalMoneyEarned()},
		{"money_spent", score->getTotalMoneySpent()},
	};
}

Json allPlayerStats()
{
	Json stats = Json::array();
	for (Player *player : reportedPlayers())
		stats.push_back(playerStatsJson(player));
	return stats;
}

/// The game is decided (one alliance left), whether or not the local player is still in it.
Bool isGameDecided()
{
	return TheVictoryConditions != nullptr && TheVictoryConditions->getEndFrame() != 0;
}

/// What a faction can build, found by walking the command sets reachable from its starting
/// objects, like the UI would.
struct CommandSetWalk
{
	std::map<AsciiString, std::set<std::string>> thingBuilders;
	std::map<AsciiString, std::set<std::string>> upgradeBuilders;
	std::map<AsciiString, const ThingTemplate *> things;
	std::map<AsciiString, const UpgradeTemplate *> upgrades;
};

CommandSetWalk walkCommandSets(const PlayerTemplate *faction)
{
	CommandSetWalk walk;
	std::deque<const ThingTemplate *> queue;

	const auto discover = [&](const ThingTemplate *tmpl) {
		if (tmpl != nullptr && walk.things.emplace(tmpl->getName(), tmpl).second)
			queue.push_back(tmpl);
	};

	if (faction != nullptr)
	{
		discover(TheThingFactory->findTemplate(faction->getStartingBuilding(), FALSE));
		for (Int i = 0; i < MAX_MP_STARTING_UNITS; ++i)
		{
			const AsciiString unit = faction->getStartingUnit(i);
			if (unit.isNotEmpty())
				discover(TheThingFactory->findTemplate(unit, FALSE));
		}
	}

	while (!queue.empty())
	{
		const ThingTemplate *builder = queue.front();
		queue.pop_front();

		const CommandSet *commandSet = TheControlBar->findCommandSet(builder->friend_getCommandSetString());
		if (commandSet == nullptr)
			continue;

		for (Int i = 0; i < MAX_COMMANDS_PER_SET; ++i)
		{
			const CommandButton *button = commandSet->getCommandButton(i);
			if (button == nullptr)
				continue;

			switch (button->getCommandType())
			{
			case GUI_COMMAND_DOZER_CONSTRUCT:
			case GUI_COMMAND_UNIT_BUILD:
			{
				const ThingTemplate *product = button->getThingTemplate();
				if (product != nullptr)
				{
					walk.thingBuilders[product->getName()].insert(builder->getName().str());
					discover(product);
				}
				break;
			}
			case GUI_COMMAND_PLAYER_UPGRADE:
			case GUI_COMMAND_OBJECT_UPGRADE:
			{
				const UpgradeTemplate *upgrade = button->getUpgradeTemplate();
				if (upgrade != nullptr)
				{
					walk.upgrades[upgrade->getUpgradeName()] = upgrade;
					walk.upgradeBuilders[upgrade->getUpgradeName()].insert(builder->getName().str());
				}
				break;
			}
			default:
				break;
			}
		}
	}
	return walk;
}

/// The catalog's structures, units and upgrades for a walk; costs and times as `player` pays them.
void addWalkedTemplates(Json &result, CommandSetWalk &walk, Player *player)
{
	Json structures = Json::array();
	Json units = Json::array();
	for (const auto &thing : walk.things)
	{
		Json entry = templateJson(thing.second, player);
		Json builtBy = Json::array();
		for (const std::string &name : walk.thingBuilders[thing.first])
			builtBy.push_back(name);
		entry["built_by"] = builtBy;
		entry["abilities"] = scriptapi_orders::abilitiesOf(thing.second);
		addPrerequisites(entry, thing.second, player);
		(thing.second->isKindOf(KINDOF_STRUCTURE) ? structures : units).push_back(entry);
	}

	Json upgradeList = Json::array();
	for (const auto &upgrade : walk.upgrades)
	{
		Json builtBy = Json::array();
		for (const std::string &name : walk.upgradeBuilders[upgrade.first])
			builtBy.push_back(name);
		upgradeList.push_back(Json{
			{"name", upgrade.first.str()},
			{"type", upgrade.second->getUpgradeType() == UPGRADE_TYPE_PLAYER ? "player" : "object"},
			{"cost", upgrade.second->calcCostToBuild(player)},
			{"build_time", static_cast<Real>(upgrade.second->calcTimeToBuild(player)) / LOGICFRAMES_PER_SECOND},
			{"built_by", builtBy},
		});
	}
	result["structures"] = std::move(structures);
	result["units"] = std::move(units);
	result["upgrades"] = std::move(upgradeList);
}

Bool isDebugEnabled()
{
#ifdef RTS_DEBUG
	return TheGlobalData->m_scriptApiDebug;
#else
	return FALSE;
#endif
}

} // namespace

//-------------------------------------------------------------------------------------------------
void ScriptApi::registerWorldCommands()
{
	using scriptapi::Request;

	m_router.add("game_info", [this](const Request &) { requireInGame(); return gameInfo(); });
	m_router.add("catalog", [this](const Request &) { requireInGame(); return catalog(); });
	m_router.add("template_info", [this](const Request &request) { requireInGame(); return templateInfo(request); });
	m_router.add("map", [this](const Request &request) { requireInGame(); return mapGrid(request); });
	m_router.add("state", [this](const Request &) { requireInGame(); return snapshot(); });
	m_router.add("player_stats", [this](const Request &) { requireInGame(); return playerStats(); });
	// Static game data, like the faction descriptions in the menus: allowed outside a game too.
	m_router.add("faction_catalog", [this](const Request &request) { return factionCatalog(request); });

	m_router.add("set_snapshot_interval", [this](const Request &request) {
		const auto frames = request.body.find("frames");
		if (frames == request.body.end() || !frames->is_number_integer() || frames->get<Int>() < 0)
			throw CommandError(scriptapi::ErrorCode::BadRequest, "frames must be an integer >= 0");
		scriptapi::SnapshotTimer &timer = m_snapshotTimers[request.connection];
		timer.setInterval(frames->get<UnsignedInt>());
		return Json{{"frames", timer.interval()}};
	});

	if (isDebugEnabled())
	{
		m_router.add("debug_world", [this](const Request &request) {
			requireInGame();
			// "all": also shots, debris and scenery, which snapshots never report.
			const auto all = request.body.find("all");
			return debugWorld(all != request.body.end() && all->is_boolean() && all->get<bool>());
		});
#ifdef RTS_DEBUG
		m_router.add("debug_add_cash", [this](const Request &) {
			requireInGame();
			TheMessageStream->appendMessage(GameMessage::MSG_META_DEMO_ADD_CASH);
			return Json();
		});
#endif
	}
}

//-------------------------------------------------------------------------------------------------
Bool ScriptApi::isVisibleToLocalPlayer(const Object *obj) const
{
	const Player *me = localPlayer();
	return me != nullptr && obj != nullptr && isVisibleTo(me, obj);
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::requireInGame() const
{
	if (currentGameState() != scriptapi::GameState::InGame)
		throw CommandError(scriptapi::ErrorCode::WrongState, "only available in a game");
}

//-------------------------------------------------------------------------------------------------
Bool ScriptApi::reportsFor(const Player *player) const
{
	return player != nullptr && player == localPlayer() && currentGameState() == scriptapi::GameState::InGame;
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::resetWorldView()
{
	m_knownStructures.clear();
	m_knownJson = Json::array();
	m_knownJsonVisibleIds.clear();
	m_money.reset();
	m_power.reset();
	m_rank.reset();
	for (auto &timer : m_snapshotTimers)
		timer.second.reset();
	m_gameOverSent = FALSE;
	m_gameResult.clear();
	m_commandTracker.clear();
	m_actionLimiter.clear();
	m_pendingSciences.clear();
	m_pendingSpend.clear();
	m_pendingBuilds.clear();
	m_executedBuilds.clear();
	m_enemyTracker.clear();
	m_damage.clear();
	m_diedIds.clear();
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::updateWorldView()
{
	Player *me = localPlayer();
	if (me == nullptr)
		return;

	reportFailedBuilds();

	UnsignedInt previousMoney = 0;
	const UnsignedInt money = me->getMoney()->countMoney();
	if (m_money.update(money, &previousMoney))
	{
		sendEvent("money_changed", Json{
			{"money", money},
			{"previous", previousMoney},
			{"delta", static_cast<Int>(money) - static_cast<Int>(previousMoney)},
		});
	}

	std::pair<Int, Int> previousPower;
	const std::pair<Int, Int> power(me->getEnergy()->getProduction(), me->getEnergy()->getConsumption());
	if (m_power.update(power, &previousPower))
	{
		sendEvent("power_changed", Json{
			{"produced", power.first},
			{"consumed", power.second},
			{"previous", Json{{"produced", previousPower.first}, {"consumed", previousPower.second}}},
		});
	}

	const std::pair<Int, Int> rank(me->getRankLevel(), me->getSciencePurchasePoints());
	if (m_rank.update(rank))
		sendEvent("rank_changed", Json{{"rank", rank.first}, {"science_points", rank.second}});

	if (!m_gameOverSent && TheVictoryConditions != nullptr)
	{
		const char *result = nullptr;
		if (TheVictoryConditions->isLocalAlliedVictory())
			result = "victory";
		else if (TheVictoryConditions->isLocalDefeat())
			result = "defeat";
		else if (TheVictoryConditions->amIObserver() && isGameDecided())
			result = "ended"; // an observer neither wins nor loses
		if (result != nullptr)
		{
			m_gameOverSent = TRUE;
			m_gameResult = result;
			Json winners = Json::array();
			for (Player *player : reportedPlayers())
			{
				if (TheVictoryConditions->hasAchievedVictory(player))
					winners.push_back(player->getPlayerIndex());
			}
			sendEvent("game_over", Json{{"result", result}, {"winners", std::move(winners)}, {"stats", allPlayerStats()}});
		}
	}

	if (currentFrame() % kVisibilityScanInterval == 0)
		scanVisibility();
	flushDamage();

	// One snapshot serves every connection whose interval is due this frame.
	std::vector<uint32_t> due;
	for (auto &timer : m_snapshotTimers)
	{
		if (timer.second.due(currentFrame()))
			due.push_back(timer.first);
	}
	if (!due.empty())
	{
		Json state = snapshot();
		state.erase("frame"); // the event envelope carries the frame
		const std::string event = scriptapi::makeEvent("snapshot", currentFrame(), std::move(state));
		for (uint32_t connection : due)
			m_server.send(connection, event);
	}
}

//-------------------------------------------------------------------------------------------------
Json ScriptApi::snapshot()
{
	Player *me = localPlayer();
	const Int myIndex = me->getPlayerIndex();
	const UnsignedInt frame = currentFrame();
	const auto started = std::chrono::steady_clock::now();

	Json units = Json::array();
	Json visible = Json::array();
	std::set<uint32_t> visibleIds;
	Int scanned = 0;

	for (Object *obj = TheGameLogic->getFirstObject(); obj != nullptr; obj = obj->getNextObject())
	{
		++scanned;
		if (!isReportable(obj))
			continue;

		const Relation relation = relationOf(me, obj);
		if (relation == Relation::Self)
		{
			units.push_back(ownObjectJson(obj));
			continue;
		}
		if (!scriptapi::isVisibleToPlayer(relation, shroudOf(obj, myIndex), obj->testStatus(OBJECT_STATUS_STEALTHED), obj->testStatus(OBJECT_STATUS_DETECTED)))
			continue;

		visible.push_back(otherObjectJson(obj, relation));
		visibleIds.insert(obj->getID());
	}

	// Remembered structures rarely change, so their JSON is rebuilt only when the memory or the
	// set of visible objects changes. Visible ones are listed too (they are also under "visible"),
	// so a script finds every structure it knows of in one place.
	if (m_knownJsonVersion != m_knownStructures.version() || m_knownJsonVisibleIds != visibleIds)
	{
		m_knownJson = Json::array();
		for (const scriptapi::KnownStructure &entry : m_knownStructures.remembered({}))
		{
			m_knownJson.push_back(Json{
				{"id", entry.id},
				{"template", entry.templateName},
				{"owner", entry.owner},
				{"pos", Json::array({entry.x, entry.y, entry.z})},
				{"last_seen_frame", entry.lastSeenFrame},
				{"visible", visibleIds.count(entry.id) != 0},
			});
		}
		m_knownJsonVersion = m_knownStructures.version();
		m_knownJsonVisibleIds = std::move(visibleIds);
	}
	Json known = m_knownJson;
	for (Json &entry : known)
	{
		if (entry["visible"].get<bool>())
			entry["last_seen_frame"] = frame;
	}

	Json self{
		{"index", myIndex},
		{"money", me->getMoney()->countMoney()},
		{"power", Json{{"produced", me->getEnergy()->getProduction()}, {"consumed", me->getEnergy()->getConsumption()}}},
		{"rank", me->getRankLevel()},
		{"science_points", me->getSciencePurchasePoints()},
	};

	// Log the cost now and then, so it can be watched as matches grow.
	static UnsignedInt s_snapshotCount = 0;
	if (++s_snapshotCount % 60 == 0)
	{
		const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
		DEBUG_LOG(("ScriptApi: snapshot scanned %d objects (%d own, %d visible, %d known) in %lld us",
			scanned, (Int)units.size(), (Int)visible.size(), (Int)known.size(), (long long)micros));
	}

	// Assembled by moving: initializer lists would deep-copy every array.
	Json result = Json::object();
	result["frame"] = frame;
	result["self"] = std::move(self);
	result["units"] = std::move(units);
	result["visible"] = std::move(visible);
	result["known_structures"] = std::move(known);
	return result;
}

//-------------------------------------------------------------------------------------------------
Json ScriptApi::gameInfo() const
{
	Player *me = localPlayer();

	Region3D extent;
	TheTerrainLogic->getExtent(&extent);

	const AsciiString mapName = TheGlobalData->m_mapName;
	const MapMetaData *map = TheMapCache->findMap(mapName);

	// Start positions are public map information (they are shown in the map preview).
	std::map<Int, Json> starts;
	if (map != nullptr)
	{
		for (WaypointMap::const_iterator it = map->m_waypoints.begin(); it != map->m_waypoints.end(); ++it)
		{
			Int number = 0;
			if (sscanf(it->first.str(), "Player_%d_Start", &number) == 1)
				starts[number] = Json::array({it->second.x, it->second.y});
		}
	}
	Json startPositions = Json::array();
	for (const auto &start : starts)
		startPositions.push_back(start.second);

	Json players = Json::array();
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
	{
		Player *player = ThePlayerList->getNthPlayer(i);
		if (player == nullptr || player->isPlayerObserver())
			continue;
		const PlayerTemplate *tmpl = player->getPlayerTemplate();
		if (tmpl == nullptr || !tmpl->isPlayableSide())
			continue;
		players.push_back(Json{
			{"index", player->getPlayerIndex()},
			{"name", toUtf8(player->getPlayerDisplayName())},
			{"side", player->getSide().str()},
			{"type", player->getPlayerType() == PLAYER_HUMAN ? "human" : "computer"},
			{"relation", scriptapi::relationName(relationOf(me, player))},
			{"active", static_cast<bool>(player->isPlayerActive())},
		});
	}

	return Json{
		{"map", Json{
			{"name", mapName.str()},
			{"display_name", map != nullptr ? toUtf8(map->m_displayName) : std::string()},
			{"extent", Json{{"min", Json::array({extent.lo.x, extent.lo.y})}, {"max", Json::array({extent.hi.x, extent.hi.y})}}},
		}},
		{"local_player", me->getPlayerIndex()},
		{"start_positions", startPositions},
		{"players", players},
		{"logic_frames_per_second", LOGICFRAMES_PER_SECOND},
	};
}

//-------------------------------------------------------------------------------------------------
Json ScriptApi::catalog() const
{
	Player *me = localPlayer();
	CommandSetWalk walk = walkCommandSets(me->getPlayerTemplate());
	Json result = Json::object();
	result["faction"] = me->getSide().str();
	addWalkedTemplates(result, walk, me);

	ScienceVec purchasableNow;
	ScienceVec purchasableLater;
	TheScienceStore->getPurchasableSciences(me, purchasableNow, purchasableLater);
	Json sciences = Json::array();
	// Like the general's promotion screen: a science is only purchasable with enough points.
	const Int points = availableSciencePoints();
	const auto addSciences = [&](const ScienceVec &list, bool now) {
		for (ScienceType science : list)
		{
			const Int cost = TheScienceStore->getSciencePurchaseCost(science);
			sciences.push_back(Json{
				{"name", TheScienceStore->getInternalNameForScience(science).str()},
				{"cost", cost},
				{"prerequisites_met", now},
				{"purchasable_now", now && cost <= points},
			});
		}
	};
	addSciences(purchasableNow, true);
	addSciences(purchasableLater, false);
	result["sciences"] = std::move(sciences);
	return result;
}

//-------------------------------------------------------------------------------------------------
Json ScriptApi::factionCatalog(const scriptapi::Request &request) const
{
	const auto name = request.body.find("faction");
	if (name == request.body.end() || !name->is_string())
		throw CommandError(scriptapi::ErrorCode::BadRequest, "faction must be a faction name (see skirmish_options)");
	const PlayerTemplate *faction = ThePlayerTemplateStore->getNthPlayerTemplate(scriptapi_shell::findFaction(name->get<std::string>()));

	// Base costs and times: the neutral player has no handicap and no faction modifiers.
	Player *neutral = ThePlayerList->getNeutralPlayer();
	CommandSetWalk walk = walkCommandSets(faction);
	Json result = Json::object();
	result["faction"] = faction->getName().str();
	result["side"] = faction->getSide().str();
	addWalkedTemplates(result, walk, neutral);
	for (const char *group : {"structures", "units"})
	{
		for (Json &entry : result[group])
			entry.erase("available_now"); // about a player, not the faction
	}

	// The general's promotion screen: one command set per tier.
	Json sciences = Json::array();
	const AsciiString tiers[] = {
		faction->getPurchaseScienceCommandSetRank1(),
		faction->getPurchaseScienceCommandSetRank3(),
		faction->getPurchaseScienceCommandSetRank8(),
	};
	for (Int tier = 0; tier < 3; ++tier)
	{
		const CommandSet *commandSet = TheControlBar->findCommandSet(tiers[tier]);
		for (Int i = 0; commandSet != nullptr && i < MAX_COMMANDS_PER_SET; ++i)
		{
			const CommandButton *button = commandSet->getCommandButton(i);
			if (button == nullptr || button->getCommandType() != GUI_COMMAND_PURCHASE_SCIENCE || button->getScienceVec().empty())
				continue;
			const ScienceType science = button->getScienceVec()[0];
			sciences.push_back(Json{
				{"name", TheScienceStore->getInternalNameForScience(science).str()},
				{"cost", TheScienceStore->getSciencePurchaseCost(science)},
				{"tier", tier + 1},
			});
		}
	}
	result["sciences"] = std::move(sciences);
	return result;
}

//-------------------------------------------------------------------------------------------------
Json ScriptApi::playerStats() const
{
	Player *me = localPlayer();
	const Bool observer = TheVictoryConditions != nullptr && TheVictoryConditions->amIObserver();
	Json players = Json::array();
	Bool all = TRUE;
	for (Player *player : reportedPlayers())
	{
		if (scriptapi::mayReportPlayerStats(player == me, observer, isGameDecided()))
			players.push_back(playerStatsJson(player));
		else
			all = FALSE;
	}
	return Json{{"players", std::move(players)}, {"all", static_cast<bool>(all)}};
}

//-------------------------------------------------------------------------------------------------
Json ScriptApi::templateInfo(const scriptapi::Request &request) const
{
	const auto names = request.body.find("names");
	if (names == request.body.end() || !names->is_array() || names->size() > kMaxTemplateInfoNames)
		throw CommandError(scriptapi::ErrorCode::BadRequest, "names must be an array of at most " + std::to_string(kMaxTemplateInfoNames) + " strings");

	Player *me = localPlayer();
	Json templates = Json::object();
	Json unknown = Json::array();
	for (const Json &name : *names)
	{
		if (!name.is_string())
			throw CommandError(scriptapi::ErrorCode::BadRequest, "names must be strings");
		const std::string text = name.get<std::string>();
		const ThingTemplate *tmpl = TheThingFactory->findTemplate(AsciiString(text.c_str()), FALSE);
		if (tmpl == nullptr)
			unknown.push_back(text);
		else
			templates[text] = templateJson(tmpl, me);
	}
	return Json{{"templates", templates}, {"unknown", unknown}};
}

//-------------------------------------------------------------------------------------------------
Json ScriptApi::mapGrid(const scriptapi::Request &request) const
{
	Int cellSize = kDefaultMapCellSize;
	const auto size = request.body.find("cell_size");
	if (size != request.body.end() && !size->is_null())
	{
		if (!size->is_number_integer() || size->get<Int>() < kMinMapCellSize || size->get<Int>() > kMaxMapCellSize)
		{
			throw CommandError(scriptapi::ErrorCode::BadRequest,
				"cell_size must be an integer between " + std::to_string(kMinMapCellSize) + " and " + std::to_string(kMaxMapCellSize));
		}
		cellSize = size->get<Int>();
	}

	Region3D extent;
	TheTerrainLogic->getExtent(&extent);
	const Int columns = static_cast<Int>(std::ceil((extent.hi.x - extent.lo.x) / cellSize));
	const Int rows = static_cast<Int>(std::ceil((extent.hi.y - extent.lo.y) / cellSize));

	// Static terrain only: buildings and other obstacles are left out, since they would reveal
	// structures hidden in the fog.
	Json grid = scriptapi::encodeTerrainGrid(columns, rows, [&](int column, int row) {
		Coord3D pos;
		pos.x = extent.lo.x + (column + 0.5f) * cellSize;
		pos.y = extent.lo.y + (row + 0.5f) * cellSize;
		pos.z = 0.0f;

		scriptapi::TerrainSample sample;
		sample.height = TheTerrainLogic->getGroundHeight(pos.x, pos.y);
		if (TheTerrainLogic->isUnderwater(pos.x, pos.y))
		{
			sample.kind = 'w';
			return sample;
		}
		const PathfindCell *cell = TheAI->pathfinder()->getCell(LAYER_GROUND, &pos);
		if (cell != nullptr)
		{
			switch (cell->getType())
			{
			case PathfindCell::CELL_WATER: sample.kind = 'w'; break;
			case PathfindCell::CELL_CLIFF: sample.kind = 'c'; break;
			case PathfindCell::CELL_IMPASSABLE:
			case PathfindCell::CELL_BRIDGE_IMPASSABLE: sample.kind = 'x'; break;
			default: sample.kind = '.'; break;
			}
		}
		return sample;
	});

	grid["cell_size"] = cellSize;
	grid["origin"] = Json::array({extent.lo.x, extent.lo.y});
	grid["columns"] = columns;
	grid["rows"] = rows;
	return grid;
}

//-------------------------------------------------------------------------------------------------
Json ScriptApi::debugWorld(Bool all) const
{
	Player *me = localPlayer();
	Json objects = Json::array();
	for (Object *obj = TheGameLogic->getFirstObject(); obj != nullptr; obj = obj->getNextObject())
	{
		if (all ? obj->getControllingPlayer() == nullptr : !isReportable(obj))
			continue;
		objects.push_back(Json{
			{"id", obj->getID()},
			{"template", obj->getTemplate()->getName().str()},
			{"owner", obj->getControllingPlayer()->getPlayerIndex()},
			{"kinds", kinds(obj->getTemplate())},
			{"pos", position(*obj->getPosition())},
			{"relation", scriptapi::relationName(relationOf(me, obj))},
			{"shroud", scriptapi::shroudName(shroudOf(obj, me->getPlayerIndex()))},
			{"visible", static_cast<bool>(isVisibleTo(me, obj))},
		});
	}
	return Json{{"objects", objects}};
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::scanVisibility()
{
	Player *me = localPlayer();
	if (me == nullptr)
		return;
	const Int myIndex = me->getPlayerIndex();
	const UnsignedInt frame = currentFrame();
	const auto started = std::chrono::steady_clock::now();

	std::map<uint32_t, scriptapi::VisibleInfo> visibleEnemies;
	std::set<uint32_t> visibleIds;
	Int scanned = 0;

	const auto discovered = [this](const scriptapi::KnownStructure &entry, bool fogged) {
		sendEvent("structure_discovered", Json{
			{"id", entry.id},
			{"template", entry.templateName},
			{"owner", entry.owner},
			{"pos", Json::array({entry.x, entry.y, entry.z})},
			{"fogged", fogged},
		});
	};

	for (Object *obj = TheGameLogic->getFirstObject(); obj != nullptr; obj = obj->getNextObject())
	{
		++scanned;
		if (!isReportable(obj) || obj->getControllingPlayer() == me)
			continue;

		const Relation relation = relationOf(me, obj);
		const Shroud shroud = shroudOf(obj, myIndex);
		if (!scriptapi::isVisibleToPlayer(relation, shroud, obj->testStatus(OBJECT_STATUS_STEALTHED), obj->testStatus(OBJECT_STATUS_DETECTED)))
		{
			// The engine only reports an object as fogged when the player's client draws it as a ghost:
			// immobile neutral objects, and immobile buildings the player has seen before.
			if (shroud == Shroud::Fogged && obj->isKindOf(KINDOF_IMMOBILE) && !m_knownStructures.knows(obj->getID()))
			{
				const scriptapi::KnownStructure ghost = knownStructure(obj, relation, frame);
				if (m_knownStructures.observeFogged(ghost))
					discovered(ghost, true);
			}
			continue;
		}

		visibleIds.insert(obj->getID());
		if (obj->isKindOf(KINDOF_IMMOBILE))
		{
			const scriptapi::KnownStructure seen = knownStructure(obj, relation, frame);
			if (m_knownStructures.observe(seen))
				discovered(seen, false);
		}
		if (relation == Relation::Enemy)
		{
			scriptapi::VisibleInfo info;
			info.id = obj->getID();
			info.templateName = apparentTemplate(obj, relation)->getName().str();
			info.owner = obj->getControllingPlayer()->getPlayerIndex();
			info.x = obj->getPosition()->x;
			info.y = obj->getPosition()->y;
			info.z = obj->getPosition()->z;
			visibleEnemies[info.id] = info;
		}
	}

	m_knownStructures.forgetUnseen([myIndex](float x, float y) {
		const Coord3D pos = {x, y, 0.0f};
		return ThePartitionManager->getShroudStatusForPlayer(myIndex, &pos) == CELLSHROUD_CLEAR;
	}, visibleIds);

	const scriptapi::VisibilityTracker::Changes changes = m_enemyTracker.update(visibleEnemies);
	for (const scriptapi::VisibleInfo &info : changes.spotted)
	{
		sendEvent("enemy_spotted", Json{
			{"id", info.id}, {"template", info.templateName}, {"owner", info.owner},
			{"pos", Json::array({info.x, info.y, info.z})},
		});
	}
	for (const scriptapi::VisibleInfo &info : changes.lost)
	{
		sendEvent("enemy_lost_from_view", Json{
			{"id", info.id}, {"template", info.templateName}, {"owner", info.owner},
			{"last_pos", Json::array({info.x, info.y, info.z})},
		});
	}

	static UnsignedInt s_scanCount = 0;
	if (++s_scanCount % 300 == 0)
	{
		const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
		DEBUG_LOG(("ScriptApi: visibility scan of %d objects (%d visible, %d enemies) took %lld us",
			scanned, (Int)visibleIds.size(), (Int)visibleEnemies.size(), (long long)micros));
	}
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::flushDamage()
{
	Player *me = localPlayer();
	for (const scriptapi::DamageAggregator::Entry &entry : m_damage.due(currentFrame(), kDamageWindow))
	{
		const Object *obj = TheGameLogic->findObjectByID(static_cast<ObjectID>(entry.id));
		if (obj == nullptr || obj->isEffectivelyDead())
			continue; // unit_destroyed covers it
		Real maxHealth;
		const Real health = healthOf(obj, &maxHealth);
		Json event{
			{"id", entry.id},
			{"template", obj->getTemplate()->getName().str()},
			{"damage", entry.damage},
			{"health", health},
			{"max_health", maxHealth},
			{"attacker_id", nullptr},
			{"attacker_template", nullptr},
		};
		const Object *attacker = entry.attacker ? TheGameLogic->findObjectByID(static_cast<ObjectID>(entry.attacker)) : nullptr;
		if (entry.attacker != 0)
			event["attacker_id"] = entry.attacker;
		if (attacker != nullptr && me != nullptr)
			event["attacker_template"] = apparentTemplate(attacker, relationOf(me, attacker))->getName().str();
		sendEvent("unit_damaged", std::move(event));
	}
}

//-------------------------------------------------------------------------------------------------
// Hooks
//-------------------------------------------------------------------------------------------------
void ScriptApi::onObjectDamaged(const Object *object, const DamageInfo *damageInfo)
{
	Player *me = localPlayer();
	if (object == nullptr || damageInfo == nullptr || object->getControllingPlayer() != me
		|| currentGameState() != scriptapi::GameState::InGame)
	{
		return;
	}
	// The attacker is only remembered if the player can see it at the moment of the hit.
	uint32_t attacker = 0;
	const Object *source = TheGameLogic->findObjectByID(damageInfo->in.m_sourceID);
	if (source != nullptr && source != object && isVisibleTo(me, source))
		attacker = source->getID();
	m_damage.add(object->getID(), damageInfo->out.m_actualDamageDealt, attacker, currentFrame());
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::onObjectRemoved(const Object *object)
{
	Player *me = localPlayer();
	if (object == nullptr || me == nullptr || object->getControllingPlayer() != me
		|| currentGameState() != scriptapi::GameState::InGame || TheGameLogic->isClearingGameData() || !isReportableKind(object))
	{
		return;
	}
	// Deaths were already reported as unit_destroyed; this covers selling and script removals.
	if (m_diedIds.erase(object->getID()) != 0)
		return;
	m_damage.forget(object->getID());
	sendEvent("unit_removed", Json{
		{"id", object->getID()},
		{"template", object->getTemplate()->getName().str()},
		{"reason", object->testStatus(OBJECT_STATUS_SOLD) ? "sold" : "removed"},
	});
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::onUnitCreated(const Player *player, const Object *factory, const Object *unit)
{
	if (!reportsFor(player) || unit == nullptr)
		return;
	sendEvent("unit_created", Json{
		{"id", unit->getID()},
		{"template", unit->getTemplate()->getName().str()},
		{"factory_id", factory ? Json(factory->getID()) : Json()},
		{"pos", position(*unit->getPosition())},
	});
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::onStructureCreated(const Player *player, const Object *builder, const Object *structure)
{
	if (!reportsFor(player) || structure == nullptr)
		return;
	if (builder != nullptr)
	{
		m_executedBuilds.erase(std::remove_if(m_executedBuilds.begin(), m_executedBuilds.end(),
			[builder](const PendingBuild &build) { return build.dozer == builder->getID(); }), m_executedBuilds.end());
	}
	sendEvent("construction_started", Json{
		{"id", structure->getID()},
		{"template", structure->getTemplate()->getName().str()},
		{"builder_id", builder ? Json(builder->getID()) : Json()},
		{"pos", position(*structure->getPosition())},
	});
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::onStructureConstructionComplete(const Player *player, const Object *builder, const Object *structure, Bool isRebuild)
{
	if (!reportsFor(player) || structure == nullptr)
		return;
	sendEvent("construction_complete", Json{
		{"id", structure->getID()},
		{"template", structure->getTemplate()->getName().str()},
		{"builder_id", builder ? Json(builder->getID()) : Json()},
		{"rebuild", static_cast<bool>(isRebuild)},
	});
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::onUpgradeCompleted(const Player *player, const UpgradeTemplate *upgrade)
{
	if (!reportsFor(player) || upgrade == nullptr)
		return;
	sendEvent("upgrade_complete", Json{{"upgrade", upgrade->getUpgradeName().str()}});
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::onScienceAdded(const Player *player, Int science)
{
	if (!reportsFor(player))
		return;
	sendEvent("science_purchased", Json{
		{"science", TheScienceStore->getInternalNameForScience(static_cast<ScienceType>(science)).str()},
	});
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::onObjectDied(const Object *object, const DamageInfo *damageInfo)
{
	Player *me = localPlayer();
	// Shots and debris die all the time; snapshots never list them, so neither do the events.
	if (object == nullptr || me == nullptr || currentGameState() != scriptapi::GameState::InGame || !isReportableKind(object))
		return;

	if (object->getControllingPlayer() != me)
	{
		if (!isVisibleTo(me, object))
			return;
		// A structure seen being destroyed is no longer remembered.
		if (object->isKindOf(KINDOF_IMMOBILE))
			m_knownStructures.forget(object->getID());
		if (relationOf(me, object) == Relation::Enemy)
		{
			m_enemyTracker.forget(object->getID());
			Json event{
				{"id", object->getID()},
				{"template", apparentTemplate(object, Relation::Enemy)->getName().str()},
				{"owner", object->getControllingPlayer()->getPlayerIndex()},
				{"pos", position(*object->getPosition())},
				{"attacker_id", nullptr},
			};
			const Object *attacker = damageInfo ? TheGameLogic->findObjectByID(damageInfo->in.m_sourceID) : nullptr;
			if (attacker != nullptr && attacker != object && isVisibleTo(me, attacker))
				event["attacker_id"] = attacker->getID();
			sendEvent("enemy_destroyed", std::move(event));
		}
		return;
	}

	m_diedIds.insert(object->getID());
	m_damage.forget(object->getID());

	Json event{
		{"id", object->getID()},
		{"template", object->getTemplate()->getName().str()},
		{"pos", position(*object->getPosition())},
		{"attacker_id", nullptr},
		{"attacker_template", nullptr},
	};

	// The attacker is only named if the player can see it.
	const Object *attacker = damageInfo ? TheGameLogic->findObjectByID(damageInfo->in.m_sourceID) : nullptr;
	if (attacker != nullptr && attacker != object && isVisibleTo(me, attacker))
	{
		event["attacker_id"] = attacker->getID();
		event["attacker_template"] = apparentTemplate(attacker, relationOf(me, attacker))->getName().str();
	}
	sendEvent("unit_destroyed", event);
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::onEvaMessage(const char *message)
{
	// EVA only raises alerts for the local player.
	if (currentGameState() != scriptapi::GameState::InGame || message == nullptr)
		return;
	sendEvent("eva", Json{{"message", message}});
}

#endif // RTS_SCRIPT_API
