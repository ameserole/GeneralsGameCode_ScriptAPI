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

// ScriptApiActions.cpp ///////////////////////////////////////////////////////////////////////////
// Script API orders. Each order is checked the way the UI checks a click (command button present
// and available, canMakeUnit, legal build location, visible target) and then sent as the exact
// network messages the UI would send: a selection followed by the order. Nothing here calls game
// logic to cause an effect, so orders travel through the command list, the network and replays
// like any human input.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#if RTS_SCRIPT_API

#include "ScriptApi/ScriptApi.h"
#include "ScriptApiOrderHelpers.h"

#include "Common/BuildAssistant.h"
#include "Common/MessageStream.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Science.h"
#include "Common/SpecialPower.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "GameClient/ControlBar.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/Object.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/WeaponSet.h"

#include <vector>

using scriptapi::CommandError;
using scriptapi::Json;
using scriptapi::Request;

namespace
{

constexpr Int kMaxTrainCount = 10;

} // namespace

namespace scriptapi_orders
{

void fail(scriptapi::ErrorCode code, const std::string &message)
{
	throw CommandError(code, message);
}

Coord3D requirePosition(const scriptapi::Position &pos)
{
	Region3D extent;
	TheTerrainLogic->getExtent(&extent);
	if (pos.x < extent.lo.x || pos.x > extent.hi.x || pos.y < extent.lo.y || pos.y > extent.hi.y)
		fail(scriptapi::ErrorCode::InvalidArgument, "position is outside the map");

	Coord3D world;
	world.x = pos.x;
	world.y = pos.y;
	world.z = TheTerrainLogic->getGroundHeight(pos.x, pos.y);
	return world;
}

const ThingTemplate *requireTemplate(const Json &body)
{
	const auto name = body.find("template");
	if (name == body.end() || !name->is_string())
		fail(scriptapi::ErrorCode::BadRequest, "template must be a string");
	const ThingTemplate *tmpl = TheThingFactory->findTemplate(AsciiString(name->get<std::string>().c_str()), FALSE);
	if (tmpl == nullptr)
		fail(scriptapi::ErrorCode::InvalidArgument, "unknown template \"" + name->get<std::string>() + "\"");
	return tmpl;
}

const CommandButton *findButton(const Object *obj, GUICommandType type, const ThingTemplate *product)
{
	const CommandSet *commandSet = TheControlBar->findCommandSet(obj->getCommandSetString());
	if (commandSet == nullptr)
		return nullptr;
	for (Int i = 0; i < MAX_COMMANDS_PER_SET; ++i)
	{
		const CommandButton *button = commandSet->getCommandButton(i);
		if (button == nullptr || button->getCommandType() != type)
			continue;
		const ThingTemplate *buttonProduct = button->getThingTemplate();
		if (product == nullptr || (buttonProduct != nullptr && buttonProduct->isEquivalentTo(product)))
			return button;
	}
	return nullptr;
}

const CommandButton *findButtonByName(const Object *obj, const std::string &name)
{
	const CommandSet *commandSet = TheControlBar->findCommandSet(obj->getCommandSetString());
	for (Int i = 0; commandSet != nullptr && i < MAX_COMMANDS_PER_SET; ++i)
	{
		const CommandButton *button = commandSet->getCommandButton(i);
		if (button != nullptr && button->getName().compareNoCase(name.c_str()) == 0)
			return button;
	}
	return nullptr;
}

ScienceType requiredScience(const CommandButton *button)
{
	// ControlBar::populateCommand: "commands that require sciences we don't have are hidden".
	const GUICommandType type = button->getCommandType();
	const SpecialPowerTemplate *power = button->getSpecialPowerTemplate();
	if (!BitIsSet(button->getOptions(), NEED_SPECIAL_POWER_SCIENCE) || power == nullptr
		|| type == GUI_COMMAND_PURCHASE_SCIENCE || type == GUI_COMMAND_PLAYER_UPGRADE || type == GUI_COMMAND_OBJECT_UPGRADE)
	{
		return SCIENCE_INVALID;
	}
	return power->getRequiredScience();
}

void requireButtonAvailable(const CommandButton *button, Object *obj, const char *what)
{
	if (button == nullptr)
		fail(scriptapi::ErrorCode::NotAvailable, std::string(obj->getTemplate()->getName().str()) + " cannot " + what);
	const ScienceType science = requiredScience(button);
	if (science != SCIENCE_INVALID && !obj->getControllingPlayer()->hasScience(science))
	{
		fail(scriptapi::ErrorCode::NotAvailable, std::string(what) + " requires the science "
			+ TheScienceStore->getInternalNameForScience(science).str());
	}
	// COMMAND_ACTIVE is a pressed-in toggle (e.g. the weapon mode in use): still clickable.
	const CommandAvailability availability = TheControlBar->getCommandAvailabilityForScriptApi(button, obj);
	if (availability != COMMAND_AVAILABLE && availability != COMMAND_ACTIVE)
		fail(scriptapi::ErrorCode::NotAvailable, std::string(what) + " is not available right now (prerequisites, power or state)");
}

void requireCanMake(Object *builder, const ThingTemplate *tmpl)
{
	switch (TheBuildAssistant->canMakeUnit(builder, tmpl))
	{
	case CANMAKE_OK: return;
	case CANMAKE_NO_MONEY: fail(scriptapi::ErrorCode::InsufficientFunds, "not enough money");
	case CANMAKE_QUEUE_FULL: fail(scriptapi::ErrorCode::QueueFull, "the production queue is full");
	case CANMAKE_PARKING_PLACES_FULL: fail(scriptapi::ErrorCode::ParkingFull, "no free parking places");
	case CANMAKE_MAXED_OUT_FOR_PLAYER: fail(scriptapi::ErrorCode::LimitReached, "the limit for this unit is reached");
	default: fail(scriptapi::ErrorCode::NotAvailable, "cannot be made right now (prerequisites or disabled factory)");
	}
}

void appendSelection(const std::vector<Object *> &objects)
{
	GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_CREATE_SELECTED_GROUP);
	msg->appendBooleanArgument(TRUE);
	for (const Object *obj : objects)
		msg->appendObjectIDArgument(obj->getID());
}

} // namespace scriptapi_orders

using namespace scriptapi_orders;

namespace
{

const char *legalBuildCodeName(LegalBuildCode code)
{
	switch (code)
	{
	case LBC_RESTRICTED_TERRAIN: return "restricted_terrain";
	case LBC_NOT_FLAT_ENOUGH: return "not_flat";
	case LBC_OBJECTS_IN_THE_WAY: return "objects_in_the_way";
	case LBC_NO_CLEAR_PATH: return "no_clear_path";
	case LBC_SHROUD: return "shroud";
	case LBC_TOO_CLOSE_TO_SUPPLIES: return "too_close_to_supplies";
	default: return "generic_failure";
	}
}

} // namespace

//-------------------------------------------------------------------------------------------------
Object *ScriptApi::requireOwned(uint32_t id) const
{
	const Player *me = ThePlayerList->getLocalPlayer();
	if (me != nullptr && me->isPlayerObserver())
		fail(scriptapi::ErrorCode::NotAvailable, "observers cannot give orders");

	// The same answer whether or not the id exists, so it reveals nothing about other objects.
	Object *obj = TheGameLogic->findObjectByID(static_cast<ObjectID>(id));
	if (obj == nullptr || obj->isEffectivelyDead() || obj->getControllingPlayer() != ThePlayerList->getLocalPlayer()
		|| !obj->isSelectable())
	{
		fail(scriptapi::ErrorCode::NotOwned, "object " + std::to_string(id) + " is not one of your units");
	}
	return obj;
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::addOrder(const std::string &cmd, scriptapi::CommandRouter::Handler handler)
{
	m_router.add(cmd, [this, handler = std::move(handler)](const Request &request) {
		const UnsignedInt frame = currentFrame();
		if (!m_actionLimiter.allows(frame))
		{
			fail(scriptapi::ErrorCode::RateLimited, "actions-per-minute cap of " + std::to_string(m_actionLimiter.limit())
				+ " reached; the next order is possible in " + std::to_string(m_actionLimiter.framesUntilAvailable(frame)) + " frames");
		}
		return handler(request);
	});
}

//-------------------------------------------------------------------------------------------------
Json ScriptApi::orderReplyFor(const Request &request, const std::vector<int> &orderTypes, Json extra)
{
	m_actionLimiter.record(currentFrame());
	// Remember the order message types so command_executed can be reported when they run.
	m_commandTracker.add(request.id, request.cmd, orderTypes, request.connection);
	extra["client_frame"] = currentFrame();
	extra["messages"] = orderTypes.size();
	return extra;
}

//-------------------------------------------------------------------------------------------------
std::vector<Object *> ScriptApi::requireOwnedUnits(const Request &request) const
{
	requireInGame();
	std::vector<Object *> units;
	for (uint32_t id : scriptapi::parseObjectIds(request.body, "units"))
		units.push_back(requireOwned(id));
	if (units.size() > 1)
	{
		for (Object *unit : units)
		{
			if (!unit->isMassSelectable())
				fail(scriptapi::ErrorCode::NotAvailable, std::string(unit->getTemplate()->getName().str()) + " can only be ordered on its own");
		}
	}
	return units;
}

//-------------------------------------------------------------------------------------------------
Object *ScriptApi::requireVisibleTarget(const Json &body, const char *key) const
{
	// Like a click: only something the player can see can be targeted. Same answer whether the
	// id exists or not.
	const uint32_t id = scriptapi::parseObjectId(body, key);
	Object *target = TheGameLogic->findObjectByID(static_cast<ObjectID>(id));
	if (target == nullptr || target->isEffectivelyDead() || !isVisibleToLocalPlayer(target))
		fail(scriptapi::ErrorCode::NotVisible, std::string(key) + " " + std::to_string(id) + " is not visible");
	return target;
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::registerActionCommands()
{
	const auto orderReply = [this](const Request &request, const std::vector<int> &orderTypes, Json extra = Json::object()) {
		return orderReplyFor(request, orderTypes, std::move(extra));
	};
	const auto requireUnits = [this](const Request &request) { return requireOwnedUnits(request); };

	const auto positionOrder = [this, requireUnits, orderReply](GameMessage::Type type) {
		return [this, requireUnits, orderReply, type](const Request &request) {
			const std::vector<Object *> units = requireUnits(request);
			const Coord3D pos = requirePosition(scriptapi::parsePosition(request.body, "pos"));

			// A move may continue along waypoints, like shift-clicking a path (MSG_ADD_WAYPOINT).
			std::vector<Coord3D> waypoints;
			if (type == GameMessage::MSG_DO_MOVETO)
			{
				for (const scriptapi::Position &point : scriptapi::parseWaypoints(request.body, "waypoints"))
					waypoints.push_back(requirePosition(point));
			}

			appendSelection(units);
			GameMessage *msg = TheMessageStream->appendMessage(type);
			msg->appendLocationArgument(pos);
			if (type == GameMessage::MSG_DO_GUARD_POSITION)
				msg->appendIntegerArgument(GUARDMODE_NORMAL);

			std::vector<int> types{static_cast<int>(type)};
			for (const Coord3D &waypoint : waypoints)
			{
				GameMessage *waypointMsg = TheMessageStream->appendMessage(GameMessage::MSG_ADD_WAYPOINT);
				waypointMsg->appendLocationArgument(waypoint);
				types.push_back(GameMessage::MSG_ADD_WAYPOINT);
			}
			return orderReply(request, types);
		};
	};

	addOrder("move", positionOrder(GameMessage::MSG_DO_MOVETO));
	addOrder("attack_move", positionOrder(GameMessage::MSG_DO_ATTACKMOVETO));
	addOrder("guard", positionOrder(GameMessage::MSG_DO_GUARD_POSITION));
	addOrder("force_move", positionOrder(GameMessage::MSG_DO_FORCEMOVETO));

	addOrder("force_attack", [this, requireUnits, orderReply](const Request &request) {
		const std::vector<Object *> units = requireUnits(request);
		const Coord3D pos = requirePosition(scriptapi::parsePosition(request.body, "pos"));
		Bool anyCanAttack = FALSE;
		for (const Object *unit : units)
			anyCanAttack = anyCanAttack || unit->isAbleToAttack();
		if (!anyCanAttack)
			fail(scriptapi::ErrorCode::CannotAttack, "none of the units can attack");
		appendSelection(units);
		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_FORCE_ATTACK_GROUND);
		msg->appendLocationArgument(pos);
		return orderReply(request, {GameMessage::MSG_DO_FORCE_ATTACK_GROUND});
	});

	addOrder("attack", [this, requireUnits, orderReply](const Request &request) {
		const std::vector<Object *> units = requireUnits(request);
		Object *target = requireVisibleTarget(request.body, "target");

		Bool anyCanAttack = FALSE;
		for (const Object *unit : units)
		{
			anyCanAttack = anyCanAttack
				|| unit->getAbleToAttackSpecificObject(ATTACK_NEW_TARGET, target, CMD_FROM_PLAYER) != ATTACKRESULT_NOT_POSSIBLE;
		}
		if (!anyCanAttack)
			fail(scriptapi::ErrorCode::CannotAttack, "none of the units can attack that target");

		appendSelection(units);
		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_ATTACK_OBJECT);
		msg->appendObjectIDArgument(target->getID());
		return orderReply(request, {GameMessage::MSG_DO_ATTACK_OBJECT});
	});

	addOrder("stop", [this, requireUnits, orderReply](const Request &request) {
		appendSelection(requireUnits(request));
		TheMessageStream->appendMessage(GameMessage::MSG_DO_STOP);
		return orderReply(request, {GameMessage::MSG_DO_STOP});
	});

	addOrder("train", [this, orderReply](const Request &request) {
		requireInGame();
		Object *factory = requireOwned(scriptapi::parseObjectId(request.body, "factory"));
		const ThingTemplate *tmpl = requireTemplate(request.body);
		Int count = 1;
		const auto countIt = request.body.find("count");
		if (countIt != request.body.end())
		{
			if (!countIt->is_number_integer() || countIt->get<Int>() < 1 || countIt->get<Int>() > kMaxTrainCount)
				fail(scriptapi::ErrorCode::BadRequest, "count must be between 1 and " + std::to_string(kMaxTrainCount));
			count = countIt->get<Int>();
		}

		// ControlBarCommandProcessing, GUI_COMMAND_UNIT_BUILD.
		// canMakeUnit first: the UI also greys the button out for money and queue space, and the
		// specific reason is more useful than "not available". Both must pass either way.
		const CommandButton *button = findButton(factory, GUI_COMMAND_UNIT_BUILD, tmpl);
		if (button == nullptr)
			fail(scriptapi::ErrorCode::NotAvailable, std::string(factory->getTemplate()->getName().str()) + " cannot train that");
		requireCanMake(factory, tmpl);
		requireButtonAvailable(button, factory, "train that");
		const Int cost = tmpl->calcCostToBuild(factory->getControllingPlayer());
		requireUncommittedMoney(count * cost);

		ProductionUpdateInterface *production = factory->getProductionUpdateInterface();
		if (production == nullptr)
			fail(scriptapi::ErrorCode::NotAvailable, "that object cannot produce units");

		appendSelection({factory});
		Json productionIds = Json::array();
		std::vector<int> types;
		for (Int i = 0; i < count; ++i)
		{
			const ProductionID productionId = production->requestUniqueUnitID();
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_QUEUE_UNIT_CREATE);
			msg->appendIntegerArgument(tmpl->getTemplateID());
			msg->appendIntegerArgument(productionId);
			productionIds.push_back(static_cast<Int>(productionId));
			types.push_back(GameMessage::MSG_QUEUE_UNIT_CREATE);
			m_pendingSpend.add(GameMessage::MSG_QUEUE_UNIT_CREATE, cost);
		}
		return orderReply(request, types, Json{{"production_ids", productionIds}});
	});

	addOrder("cancel_train", [this, orderReply](const Request &request) {
		requireInGame();
		Object *factory = requireOwned(scriptapi::parseObjectId(request.body, "factory"));
		const auto idIt = request.body.find("production_id");
		if (idIt == request.body.end() || !idIt->is_number_integer())
			fail(scriptapi::ErrorCode::BadRequest, "production_id must be an integer");
		const ProductionID productionId = static_cast<ProductionID>(idIt->get<Int>());

		ProductionUpdateInterface *production = factory->getProductionUpdateInterface();
		Bool queued = FALSE;
		for (const ProductionEntry *entry = production ? production->firstProduction() : nullptr; entry != nullptr;
			entry = production->nextProduction(entry))
		{
			queued = queued || entry->getProductionID() == productionId;
		}
		if (!queued)
			fail(scriptapi::ErrorCode::InvalidArgument, "production " + std::to_string(idIt->get<Int>()) + " is not queued there");

		appendSelection({factory});
		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_CANCEL_UNIT_CREATE);
		msg->appendIntegerArgument(productionId);
		return orderReply(request, {GameMessage::MSG_CANCEL_UNIT_CREATE});
	});

	addOrder("build", [this, orderReply](const Request &request) {
		requireInGame();
		Object *dozer = requireOwned(scriptapi::parseObjectId(request.body, "dozer"));
		const ThingTemplate *tmpl = requireTemplate(request.body);
		const Coord3D pos = requirePosition(scriptapi::parsePosition(request.body, "pos"));
		Real angle = 0.0f;
		const auto angleIt = request.body.find("angle");
		if (angleIt != request.body.end())
		{
			if (!angleIt->is_number())
				fail(scriptapi::ErrorCode::BadRequest, "angle must be a number (radians)");
			angle = angleIt->get<Real>();
		}

		// ControlBarCommandProcessing GUI_COMMAND_DOZER_CONSTRUCT, then PlaceEventTranslator.
		const CommandButton *button = findButton(dozer, GUI_COMMAND_DOZER_CONSTRUCT, tmpl);
		if (button == nullptr)
			fail(scriptapi::ErrorCode::NotAvailable, std::string(dozer->getTemplate()->getName().str()) + " cannot build that");
		requireCanMake(dozer, tmpl);
		const Int cost = tmpl->calcCostToBuild(dozer->getControllingPlayer());
		requireUncommittedMoney(cost);
		requireButtonAvailable(button, dozer, "build that");
		const LegalBuildCode legal = TheBuildAssistant->isLocationLegalToBuild(&pos, tmpl, angle,
			BuildAssistant::USE_QUICK_PATHFIND | BuildAssistant::TERRAIN_RESTRICTIONS | BuildAssistant::CLEAR_PATH |
			BuildAssistant::NO_OBJECT_OVERLAP | BuildAssistant::SHROUD_REVEALED | BuildAssistant::IGNORE_STEALTHED |
			BuildAssistant::FAIL_STEALTHED_WITHOUT_FEEDBACK,
			dozer, dozer->getControllingPlayer());
		if (legal != LBC_OK)
			fail(scriptapi::ErrorCode::IllegalLocation, legalBuildCodeName(legal));

		appendSelection({dozer});
		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_DOZER_CONSTRUCT);
		msg->appendIntegerArgument(tmpl->getTemplateID());
		msg->appendLocationArgument(pos);
		msg->appendRealArgument(angle);
		m_pendingSpend.add(GameMessage::MSG_DOZER_CONSTRUCT, cost);
		PendingBuild build;
		build.requestId = request.id;
		build.connection = request.connection;
		build.dozer = dozer->getID();
		build.templateName = tmpl->getName().str();
		build.pos = pos;
		m_pendingBuilds.push_back(std::move(build));
		return orderReply(request, {GameMessage::MSG_DOZER_CONSTRUCT});
	});

	addOrder("set_rally_point", [this, orderReply](const Request &request) {
		requireInGame();
		Object *building = requireOwned(scriptapi::parseObjectId(request.body, "building"));
		if (building->getProductionUpdateInterface() == nullptr)
			fail(scriptapi::ErrorCode::NotAvailable, "that object has no rally point");
		const Coord3D pos = requirePosition(scriptapi::parsePosition(request.body, "pos"));

		// CommandTranslator::handleSetRallyPointCommand.
		appendSelection({building});
		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_SET_RALLY_POINT);
		msg->appendObjectIDArgument(building->getID());
		msg->appendLocationArgument(pos);
		return orderReply(request, {GameMessage::MSG_SET_RALLY_POINT});
	});

	addOrder("sell", [this, orderReply](const Request &request) {
		requireInGame();
		Object *building = requireOwned(scriptapi::parseObjectId(request.body, "building"));
		requireButtonAvailable(findButton(building, GUI_COMMAND_SELL), building, "be sold");
		appendSelection({building});
		TheMessageStream->appendMessage(GameMessage::MSG_SELL);
		return orderReply(request, {GameMessage::MSG_SELL});
	});
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::requireUncommittedMoney(Int cost) const
{
	const Player *me = ThePlayerList->getLocalPlayer();
	const Int committed = m_pendingSpend.committed();
	if (static_cast<Int>(me->getMoney()->countMoney()) - committed < cost)
	{
		fail(scriptapi::ErrorCode::InsufficientFunds, "not enough money once the " + std::to_string(committed)
			+ " for orders still on their way is spent");
	}
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::reportFailedBuilds()
{
	for (const PendingBuild &build : m_executedBuilds)
	{
		sendEventTo(build.connection, "build_failed", Json{
			{"reply_to", build.requestId},
			{"dozer", build.dozer},
			{"template", build.templateName},
			{"pos", Json::array({build.pos.x, build.pos.y, build.pos.z})},
		});
	}
	m_executedBuilds.clear();
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::onCommandExecuted(Int messageType, const Player *player)
{
	if (player == nullptr || player != ThePlayerList->getLocalPlayer())
		return;
	if (messageType == GameMessage::MSG_PURCHASE_SCIENCE && !m_pendingSciences.empty())
		m_pendingSciences.pop_front();
	m_pendingSpend.onExecuted(messageType);
	// The engine places the structure while it executes this message, or silently drops it
	// (BuildAssistant::buildObjectNow); onStructureCreated tells which before the next update().
	if (messageType == GameMessage::MSG_DOZER_CONSTRUCT && !m_pendingBuilds.empty())
	{
		m_executedBuilds.push_back(std::move(m_pendingBuilds.front()));
		m_pendingBuilds.pop_front();
	}
	scriptapi::CommandTracker::Completion done;
	if (m_commandTracker.onExecuted(messageType, done))
		sendEventTo(done.connection, "command_executed", Json{{"reply_to", done.requestId}, {"cmd", done.cmd}});
}

#endif // RTS_SCRIPT_API
