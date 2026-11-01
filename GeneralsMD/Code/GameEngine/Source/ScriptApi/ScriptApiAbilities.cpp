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

// ScriptApiAbilities.cpp /////////////////////////////////////////////////////////////////////////
// Script API orders beyond the basics: upgrades, general's powers, command-button abilities
// (special powers, weapon modes, hacking...), containers, repair and docking, and group orders.
// As with ScriptApiActions.cpp, every order is checked like a click and sent as the network
// messages the UI would send (ControlBarCommandProcessing.cpp, CommandXlat.cpp).
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#if RTS_SCRIPT_API

#include "ScriptApi/ScriptApi.h"
#include "ScriptApiOrderHelpers.h"

#include "Common/ActionManager.h"
#include "Common/BuildAssistant.h"
#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Science.h"
#include "Common/SpecialPower.h"
#include "Common/ThingTemplate.h"
#include "Common/Upgrade.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/ContainModule.h"
#include "GameLogic/Module/DozerAIUpdate.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/Object.h"

#include <algorithm>
#include <functional>

using namespace scriptapi_orders;
using scriptapi::Json;
using scriptapi::Request;

namespace
{

const char *abilityTypeName(GUICommandType type)
{
	switch (type)
	{
	case GUI_COMMAND_SPECIAL_POWER:
	case GUI_COMMAND_SPECIAL_POWER_FROM_SHORTCUT: return "special_power";
	case GUI_COMMAND_SWITCH_WEAPON: return "switch_weapon";
	case GUI_COMMAND_HACK_INTERNET: return "hack_internet";
	case GUI_COMMAND_TOGGLE_OVERCHARGE: return "overcharge";
	case GUI_COMMAND_EVACUATE: return "evacuate";
	case GUI_COMMAND_PLAYER_UPGRADE:
	case GUI_COMMAND_OBJECT_UPGRADE: return "upgrade";
	default: return nullptr;
	}
}

const char *abilityTarget(const CommandButton *button)
{
	if (BitIsSet(button->getOptions(), NEED_TARGET_POS))
		return "position";
	if (BitIsSet(button->getOptions(), COMMAND_OPTION_NEED_OBJECT_TARGET))
		return "object";
	return "none";
}

const UpgradeTemplate *requireUpgrade(const Json &body)
{
	const auto name = body.find("upgrade");
	if (name == body.end() || !name->is_string())
		fail(scriptapi::ErrorCode::BadRequest, "upgrade must be a string");
	const UpgradeTemplate *upgrade = TheUpgradeCenter->findUpgrade(name->get<std::string>().c_str());
	if (upgrade == nullptr)
		fail(scriptapi::ErrorCode::InvalidArgument, "unknown upgrade \"" + name->get<std::string>() + "\"");
	return upgrade;
}

/// The upgrade button for this upgrade in the object's command set, if any.
const CommandButton *findUpgradeButton(const Object *obj, const UpgradeTemplate *upgrade)
{
	for (GUICommandType type : {GUI_COMMAND_PLAYER_UPGRADE, GUI_COMMAND_OBJECT_UPGRADE})
	{
		const CommandSet *commandSet = TheControlBar->findCommandSet(obj->getCommandSetString());
		for (Int i = 0; commandSet != nullptr && i < MAX_COMMANDS_PER_SET; ++i)
		{
			const CommandButton *button = commandSet->getCommandButton(i);
			if (button != nullptr && button->getCommandType() == type && button->getUpgradeTemplate() == upgrade)
				return button;
		}
	}
	return nullptr;
}

/// ControlBarCommandProcessing, GUI_COMMAND_PLAYER_UPGRADE / GUI_COMMAND_OBJECT_UPGRADE.
void requireCanQueueUpgrade(Object *obj, const CommandButton *button, const UpgradeTemplate *upgrade)
{
	if (button == nullptr)
		fail(scriptapi::ErrorCode::NotAvailable, std::string(obj->getTemplate()->getName().str()) + " does not offer that upgrade");
	if (!TheUpgradeCenter->canAffordUpgrade(obj->getControllingPlayer(), upgrade))
		fail(scriptapi::ErrorCode::InsufficientFunds, "not enough money");
	ProductionUpdateInterface *production = obj->getProductionUpdateInterface();
	if (production != nullptr)
	{
		switch (production->canQueueUpgrade(upgrade))
		{
		case CANMAKE_OK: break;
		case CANMAKE_QUEUE_FULL: fail(scriptapi::ErrorCode::QueueFull, "the production queue is full");
		default: fail(scriptapi::ErrorCode::NotAvailable, "that upgrade cannot be queued now");
		}
	}
	if (button->getCommandType() == GUI_COMMAND_OBJECT_UPGRADE && (obj->hasUpgrade(upgrade) || !obj->affectedByUpgrade(upgrade)))
		fail(scriptapi::ErrorCode::NotAvailable, "the object already has that upgrade");
	requireButtonAvailable(button, obj, "research that");
}

void appendQueueUpgrade(const Object *obj, const UpgradeTemplate *upgrade)
{
	GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_QUEUE_UPGRADE);
	msg->appendObjectIDArgument(obj->getID());
	msg->appendIntegerArgument(upgrade->getUpgradeNameKey());
}

} // namespace

//-------------------------------------------------------------------------------------------------
Json scriptapi_orders::describeAbility(const CommandButton *button)
{
	const char *type = button ? abilityTypeName(button->getCommandType()) : nullptr;
	if (type == nullptr)
		return Json();
	Json ability{{"name", button->getName().str()}, {"type", type}, {"target", abilityTarget(button)}};
	if (button->getSpecialPowerTemplate() != nullptr)
		ability["special_power"] = button->getSpecialPowerTemplate()->getName().str();
	if (button->getUpgradeTemplate() != nullptr)
		ability["upgrade"] = button->getUpgradeTemplate()->getUpgradeName().str();
	const ScienceType science = requiredScience(button);
	ability["required_science"] = science != SCIENCE_INVALID
		? Json(TheScienceStore->getInternalNameForScience(science).str()) : Json();
	return ability;
}

//-------------------------------------------------------------------------------------------------
Json scriptapi_orders::abilitiesOf(const ThingTemplate *tmpl)
{
	Json abilities = Json::array();
	const CommandSet *commandSet = TheControlBar->findCommandSet(tmpl->friend_getCommandSetString());
	for (Int i = 0; commandSet != nullptr && i < MAX_COMMANDS_PER_SET; ++i)
	{
		Json ability = describeAbility(commandSet->getCommandButton(i));
		if (!ability.is_null())
			abilities.push_back(std::move(ability));
	}
	return abilities;
}

//-------------------------------------------------------------------------------------------------
Int ScriptApi::availableSciencePoints() const
{
	Int points = ThePlayerList->getLocalPlayer()->getSciencePurchasePoints();
	for (const Int pending : m_pendingSciences)
		points -= TheScienceStore->getSciencePurchaseCost(static_cast<ScienceType>(pending));
	return points;
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::registerAbilityCommands()
{
	// Orders on a group that take a visible target object and a capability check per unit.
	const auto targetOrder = [this](GameMessage::Type type, scriptapi::ErrorCode refusal, const char *what,
		std::function<Bool(const Object *, const Object *)> canDo) {
		return [this, type, refusal, what, canDo](const Request &request) {
			const std::vector<Object *> units = requireOwnedUnits(request);
			Object *target = requireVisibleTarget(request.body, "target");
			Bool anyCan = FALSE;
			for (const Object *unit : units)
				anyCan = anyCan || canDo(unit, target);
			if (!anyCan)
				fail(refusal, std::string("none of the units can ") + what);
			appendSelection(units);
			GameMessage *msg = TheMessageStream->appendMessage(type);
			if (type == GameMessage::MSG_ENTER)
				msg->appendObjectIDArgument(INVALID_ID); // the current selection enters (CommandXlat)
			msg->appendObjectIDArgument(target->getID());
			if (type == GameMessage::MSG_DO_GUARD_OBJECT)
				msg->appendIntegerArgument(GUARDMODE_NORMAL);
			return orderReplyFor(request, {static_cast<int>(type)});
		};
	};

	addOrder("enter", targetOrder(GameMessage::MSG_ENTER, scriptapi::ErrorCode::CannotEnter, "enter that",
		[](const Object *unit, const Object *target) {
			return TheActionManager->canEnterObject(unit, target, CMD_FROM_PLAYER, CHECK_CAPACITY);
		}));
	addOrder("get_repaired", targetOrder(GameMessage::MSG_GET_REPAIRED, scriptapi::ErrorCode::NotAvailable, "get repaired there",
		[](const Object *unit, const Object *target) { return TheActionManager->canGetRepairedAt(unit, target, CMD_FROM_PLAYER); }));
	addOrder("dock", targetOrder(GameMessage::MSG_DOCK, scriptapi::ErrorCode::NotAvailable, "dock there",
		[](const Object *unit, const Object *target) { return TheActionManager->canDockAt(unit, target, CMD_FROM_PLAYER); }));
	addOrder("guard_object", targetOrder(GameMessage::MSG_DO_GUARD_OBJECT, scriptapi::ErrorCode::NotAvailable, "guard",
		[](const Object *, const Object *) { return TRUE; }));
	addOrder("force_attack_object", targetOrder(GameMessage::MSG_DO_FORCE_ATTACK_OBJECT, scriptapi::ErrorCode::CannotAttack, "attack that",
		[](const Object *unit, const Object *target) {
			// CommandXlat canObjectForceAttack: the forced check also allows own and allied targets.
			return unit->getAbleToAttackSpecificObject(ATTACK_NEW_TARGET_FORCED, target, CMD_FROM_PLAYER) != ATTACKRESULT_NOT_POSSIBLE;
		}));

	// Dozer orders: a single dozer and a visible target.
	const auto dozerOrder = [this](GameMessage::Type type, const char *what, std::function<Bool(const Object *, const Object *)> canDo) {
		return [this, type, what, canDo](const Request &request) {
			requireInGame();
			Object *dozer = requireOwned(scriptapi::parseObjectId(request.body, "dozer"));
			Object *target = requireVisibleTarget(request.body, "target");
			if (!canDo(dozer, target))
				fail(scriptapi::ErrorCode::NotAvailable, std::string("that dozer cannot ") + what);
			appendSelection({dozer});
			GameMessage *msg = TheMessageStream->appendMessage(type);
			msg->appendObjectIDArgument(target->getID());
			return orderReplyFor(request, {static_cast<int>(type)});
		};
	};
	addOrder("repair", dozerOrder(GameMessage::MSG_DO_REPAIR, "repair that",
		[](const Object *dozer, const Object *target) { return TheActionManager->canRepairObject(dozer, target, CMD_FROM_PLAYER); }));
	addOrder("resume_construction", dozerOrder(GameMessage::MSG_RESUME_CONSTRUCTION, "resume that",
		[](const Object *dozer, const Object *target) {
			if (TheActionManager->canResumeConstructionOf(dozer, target, CMD_FROM_PLAYER))
				return TRUE;
			// The common reasons, in ActionManager's terms; anything else gets the generic message.
			if (!target->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION))
				fail(scriptapi::ErrorCode::NotAvailable, "that structure is not under construction");
			Object *builder = TheGameLogic->findObjectByID(target->getBuilderID());
			DozerAIInterface *builderAI = builder != nullptr && builder->getAI() != nullptr
				? builder->getAI()->getDozerAIInterface() : nullptr;
			if (builderAI != nullptr && builderAI->getCurrentTask() == DOZER_TASK_BUILD
				&& builderAI->getTaskTarget(DOZER_TASK_BUILD) == target->getID())
			{
				fail(scriptapi::ErrorCode::NotAvailable, builder == dozer
					? "that dozer is already building it" : "another dozer is building it");
			}
			return FALSE;
		}));

	// Group orders without arguments.
	const auto groupOrder = [this](GameMessage::Type type) {
		return [this, type](const Request &request) {
			appendSelection(requireOwnedUnits(request));
			TheMessageStream->appendMessage(type);
			return orderReplyFor(request, {static_cast<int>(type)});
		};
	};
	addOrder("scatter", groupOrder(GameMessage::MSG_DO_SCATTER));
	addOrder("formation", groupOrder(GameMessage::MSG_CREATE_FORMATION));

	addOrder("exit_container", [this](const Request &request) {
		const std::vector<Object *> units = requireOwnedUnits(request);
		std::vector<int> types;
		for (const Object *unit : units)
		{
			if (unit->getContainedBy() == nullptr)
				fail(scriptapi::ErrorCode::NotAvailable, "unit " + std::to_string(unit->getID()) + " is not inside anything");
		}
		// ControlBarCommandProcessing GUI_COMMAND_EXIT_CONTAINER: one MSG_EXIT per unit leaving.
		for (const Object *unit : units)
		{
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_EXIT);
			msg->appendObjectIDArgument(unit->getID());
			types.push_back(GameMessage::MSG_EXIT);
		}
		return orderReplyFor(request, types);
	});

	addOrder("evacuate", [this](const Request &request) {
		requireInGame();
		Object *container = requireOwned(scriptapi::parseObjectId(request.body, "object"));
		ContainModuleInterface *contain = container->getContain();
		if (contain == nullptr || contain->getContainCount() == 0)
			fail(scriptapi::ErrorCode::NotAvailable, "nothing to evacuate");
		appendSelection({container});
		TheMessageStream->appendMessage(GameMessage::MSG_EVACUATE);
		return orderReplyFor(request, {GameMessage::MSG_EVACUATE});
	});

	addOrder("cancel_build", [this](const Request &request) {
		requireInGame();
		Object *building = requireOwned(scriptapi::parseObjectId(request.body, "building"));
		if (!building->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION))
			fail(scriptapi::ErrorCode::NotAvailable, "that building is not under construction");
		appendSelection({building});
		TheMessageStream->appendMessage(GameMessage::MSG_DOZER_CANCEL_CONSTRUCT);
		return orderReplyFor(request, {GameMessage::MSG_DOZER_CANCEL_CONSTRUCT});
	});

	addOrder("upgrade", [this](const Request &request) {
		requireInGame();
		Object *obj = requireOwned(scriptapi::parseObjectId(request.body, "object"));
		const UpgradeTemplate *upgrade = requireUpgrade(request.body);
		requireCanQueueUpgrade(obj, findUpgradeButton(obj, upgrade), upgrade);
		const Int cost = upgrade->calcCostToBuild(obj->getControllingPlayer());
		requireUncommittedMoney(cost);
		appendSelection({obj});
		appendQueueUpgrade(obj, upgrade);
		m_pendingSpend.add(GameMessage::MSG_QUEUE_UPGRADE, cost);
		return orderReplyFor(request, {GameMessage::MSG_QUEUE_UPGRADE});
	});

	addOrder("cancel_upgrade", [this](const Request &request) {
		requireInGame();
		Object *obj = requireOwned(scriptapi::parseObjectId(request.body, "object"));
		const UpgradeTemplate *upgrade = requireUpgrade(request.body);
		ProductionUpdateInterface *production = obj->getProductionUpdateInterface();
		if (production == nullptr || !production->isUpgradeInQueue(upgrade))
			fail(scriptapi::ErrorCode::InvalidArgument, "that upgrade is not being researched there");
		appendSelection({obj});
		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_CANCEL_UPGRADE);
		msg->appendIntegerArgument(upgrade->getUpgradeNameKey());
		return orderReplyFor(request, {GameMessage::MSG_CANCEL_UPGRADE});
	});

	addOrder("purchase_science", [this](const Request &request) {
		requireInGame();
		const auto name = request.body.find("science");
		if (name == request.body.end() || !name->is_string())
			fail(scriptapi::ErrorCode::BadRequest, "science must be a string");
		const ScienceType science = TheScienceStore->getScienceFromInternalName(AsciiString(name->get<std::string>().c_str()));
		// getScienceFromInternalName only hashes the name, so check that such a science exists.
		if (science == SCIENCE_INVALID || !TheScienceStore->isValidScience(science))
			fail(scriptapi::ErrorCode::InvalidArgument, "unknown science \"" + name->get<std::string>() + "\"");

		// getPurchasableSciences only checks ownership and prerequisites. Ownership changes when the
		// logic executes the purchase, so purchases still on their way count as owned and spent.
		Player *me = ThePlayerList->getLocalPlayer();
		if (std::find(m_pendingSciences.begin(), m_pendingSciences.end(), science) != m_pendingSciences.end())
			fail(scriptapi::ErrorCode::NotAvailable, "that science is already being purchased");
		ScienceVec now;
		ScienceVec later;
		TheScienceStore->getPurchasableSciences(me, now, later);
		if (std::find(now.begin(), now.end(), science) == now.end())
			fail(scriptapi::ErrorCode::NotAvailable, "that science cannot be purchased now (owned, or prerequisites missing)");
		const Int points = availableSciencePoints();
		const Int cost = TheScienceStore->getSciencePurchaseCost(science);
		if (points < cost)
			fail(scriptapi::ErrorCode::InsufficientFunds, "that science costs " + std::to_string(cost) + " points, "
				+ std::to_string(points > 0 ? points : 0) + " available");

		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_PURCHASE_SCIENCE);
		msg->appendIntegerArgument(science);
		m_pendingSciences.push_back(science);
		return orderReplyFor(request, {GameMessage::MSG_PURCHASE_SCIENCE});
	});

	addOrder("use_ability", [this](const Request &request) {
		const std::vector<Object *> units = requireOwnedUnits(request);
		const auto name = request.body.find("ability");
		if (name == request.body.end() || !name->is_string())
			fail(scriptapi::ErrorCode::BadRequest, "ability must be a command button name (see the catalog)");
		const CommandButton *button = findButtonByName(units.front(), name->get<std::string>());
		if (button == nullptr || describeAbility(button).is_null())
			fail(scriptapi::ErrorCode::NotAvailable, "no supported ability \"" + name->get<std::string>() + "\" on that unit");

		const GUICommandType type = button->getCommandType();
		if (type == GUI_COMMAND_PLAYER_UPGRADE || type == GUI_COMMAND_OBJECT_UPGRADE)
		{
			requireCanQueueUpgrade(units.front(), button, button->getUpgradeTemplate());
			const Int cost = button->getUpgradeTemplate()->calcCostToBuild(units.front()->getControllingPlayer());
			requireUncommittedMoney(cost);
			appendSelection({units.front()});
			appendQueueUpgrade(units.front(), button->getUpgradeTemplate());
			m_pendingSpend.add(GameMessage::MSG_QUEUE_UPGRADE, cost);
			return orderReplyFor(request, {GameMessage::MSG_QUEUE_UPGRADE});
		}

		requireButtonAvailable(button, units.front(), "use that");

		if (type == GUI_COMMAND_SPECIAL_POWER || type == GUI_COMMAND_SPECIAL_POWER_FROM_SHORTCUT)
		{
			const SpecialPowerTemplate *power = button->getSpecialPowerTemplate();
			const UnsignedInt options = button->getOptions();
			const char *target = abilityTarget(button);
			if (std::string(target) == "position")
			{
				const Coord3D pos = requirePosition(scriptapi::parsePosition(request.body, "pos"));
				Bool anyCan = FALSE;
				for (const Object *unit : units)
					anyCan = anyCan || TheActionManager->canDoSpecialPowerAtLocation(unit, &pos, CMD_FROM_PLAYER, power, nullptr, options);
				if (!anyCan)
					fail(scriptapi::ErrorCode::NotAvailable, "that ability cannot be used there now");
				appendSelection(units);
				GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_SPECIAL_POWER_AT_LOCATION);
				msg->appendIntegerArgument(power->getID());
				msg->appendLocationArgument(pos);
				msg->appendRealArgument(INVALID_ANGLE);
				msg->appendObjectIDArgument(INVALID_ID);
				msg->appendIntegerArgument(options);
				msg->appendObjectIDArgument(INVALID_ID);
				return orderReplyFor(request, {GameMessage::MSG_DO_SPECIAL_POWER_AT_LOCATION});
			}
			if (std::string(target) == "object")
			{
				Object *targetObj = requireVisibleTarget(request.body, "target");
				Bool anyCan = FALSE;
				for (const Object *unit : units)
				{
					anyCan = anyCan || (button->isValidObjectTarget(unit, targetObj)
						&& TheActionManager->canDoSpecialPowerAtObject(unit, targetObj, CMD_FROM_PLAYER, power, options));
				}
				if (!anyCan)
					fail(scriptapi::ErrorCode::NotAvailable, "that ability cannot be used on that target now");
				appendSelection(units);
				GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_SPECIAL_POWER_AT_OBJECT);
				msg->appendIntegerArgument(power->getID());
				msg->appendObjectIDArgument(targetObj->getID());
				msg->appendIntegerArgument(options);
				msg->appendObjectIDArgument(INVALID_ID);
				return orderReplyFor(request, {GameMessage::MSG_DO_SPECIAL_POWER_AT_OBJECT});
			}
			Bool anyCan = FALSE;
			for (const Object *unit : units)
				anyCan = anyCan || TheActionManager->canDoSpecialPower(unit, power, CMD_FROM_PLAYER, options);
			if (!anyCan)
				fail(scriptapi::ErrorCode::NotAvailable, "that ability is not ready");
			appendSelection(units);
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_SPECIAL_POWER);
			msg->appendIntegerArgument(power->getID());
			msg->appendIntegerArgument(options);
			msg->appendObjectIDArgument(INVALID_ID);
			return orderReplyFor(request, {GameMessage::MSG_DO_SPECIAL_POWER});
		}

		appendSelection(units);
		GameMessage::Type messageType = GameMessage::MSG_INVALID;
		switch (type)
		{
		case GUI_COMMAND_SWITCH_WEAPON:
		{
			messageType = GameMessage::MSG_SWITCH_WEAPONS;
			GameMessage *msg = TheMessageStream->appendMessage(messageType);
			msg->appendIntegerArgument(button->getWeaponSlot());
			break;
		}
		case GUI_COMMAND_HACK_INTERNET: messageType = GameMessage::MSG_INTERNET_HACK; break;
		case GUI_COMMAND_TOGGLE_OVERCHARGE: messageType = GameMessage::MSG_TOGGLE_OVERCHARGE; break;
		case GUI_COMMAND_EVACUATE: messageType = GameMessage::MSG_EVACUATE; break;
		default: fail(scriptapi::ErrorCode::NotAvailable, "that ability is not supported by the script API yet");
		}
		if (type != GUI_COMMAND_SWITCH_WEAPON)
			TheMessageStream->appendMessage(messageType);
		return orderReplyFor(request, {static_cast<int>(messageType)});
	});

#ifdef RTS_DEBUG
	if (TheGlobalData->m_scriptApiDebug)
	{
		m_router.add("debug_hurt", [this](const Request &request) {
			requireInGame();
			// The debug-build "hurt" message, as the debug selection tool sends it (10% of max health).
			GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_DEBUG_HURT_OBJECT);
			msg->appendObjectIDArgument(static_cast<ObjectID>(scriptapi::parseObjectId(request.body, "object")));
			return Json();
		});
		m_router.add("debug_give_science_points", [this](const Request &) {
			requireInGame();
			TheMessageStream->appendMessage(GameMessage::MSG_META_DEMO_GIVE_SCIENCEPURCHASEPOINTS);
			return Json();
		});
	}
#endif
}

#endif // RTS_SCRIPT_API
