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

// ScriptApiInput.cpp /////////////////////////////////////////////////////////////////////////////
// Input lock for script-controlled clients: a human at the bot's window may move the camera
// ("view", the default) or nothing at all ("locked"), but never play the bot's player.
//
// Two message stream translators do the work:
//  - ScriptApiInputGuard runs before every other translator and applies scriptapi::inputAction to
//    raw mouse and keyboard messages. "Camera only" input is handed straight to the look-at
//    (camera) translator and then destroyed, so selection, hotkeys, menus and orders never see it.
//  - ScriptApiCommandGuard runs last and drops any network message the script API did not send
//    itself, as a safety net for input paths the first guard does not know about.
// Both only drop or redirect local input before it becomes a command, so they cannot affect sync.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#if RTS_SCRIPT_API

#include "ScriptApi/ScriptApi.h"

#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/OptionPreferences.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/LookAtXlat.h"
#include "GameClient/Mouse.h"
#include "GameClient/View.h"

#include <optional>

using scriptapi::CommandError;
using scriptapi::InputAction;
using scriptapi::InputKind;
using scriptapi::Json;
using scriptapi::Request;

namespace
{

constexpr UnsignedInt kInputGuardPriority = 1;           ///< before WindowTranslator (10)
constexpr UnsignedInt kCommandGuardPriority = 999999998; ///< just before GameClientMessageDispatcher

std::optional<InputKind> inputKindOf(GameMessage::Type type)
{
	switch (type)
	{
	case GameMessage::MSG_RAW_MOUSE_POSITION:
		return InputKind::MousePosition;
	case GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_DOWN:
	case GameMessage::MSG_RAW_MOUSE_LEFT_DOUBLE_CLICK:
	case GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_UP:
	case GameMessage::MSG_RAW_MOUSE_LEFT_CLICK:
	case GameMessage::MSG_RAW_MOUSE_LEFT_DRAG:
		return InputKind::MouseLeft;
	case GameMessage::MSG_RAW_MOUSE_MIDDLE_BUTTON_DOWN:
	case GameMessage::MSG_RAW_MOUSE_MIDDLE_DOUBLE_CLICK:
	case GameMessage::MSG_RAW_MOUSE_MIDDLE_BUTTON_UP:
	case GameMessage::MSG_RAW_MOUSE_MIDDLE_DRAG:
		return InputKind::MouseMiddle;
	case GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_DOWN:
	case GameMessage::MSG_RAW_MOUSE_RIGHT_DOUBLE_CLICK:
	case GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_UP:
	case GameMessage::MSG_RAW_MOUSE_RIGHT_DRAG:
		return InputKind::MouseRight;
	case GameMessage::MSG_RAW_MOUSE_WHEEL:
		return InputKind::MouseWheel;
	case GameMessage::MSG_RAW_KEY_DOWN:
	case GameMessage::MSG_RAW_KEY_UP:
		return InputKind::Key;
	default:
		return std::nullopt;
	}
}

const char *inputKindName(InputKind kind)
{
	switch (kind)
	{
	case InputKind::MousePosition: return "mouse_position";
	case InputKind::MouseLeft: return "mouse_left";
	case InputKind::MouseRight: return "mouse_right";
	case InputKind::MouseMiddle: return "mouse_middle";
	case InputKind::MouseWheel: return "mouse_wheel";
	case InputKind::Key: return "key";
	}
	return "key";
}

/// System messages that are not player input and must reach the command list in every mode.
/// MSG_ENABLE_RETALIATION_MODE is posted by Player::update to sync the client's option to the logic.
Bool isSystemNetworkMessage(GameMessage::Type type)
{
	return type == GameMessage::MSG_LOGIC_CRC || type == GameMessage::MSG_SET_REPLAY_CAMERA
		|| type == GameMessage::MSG_SELF_DESTRUCT || type == GameMessage::MSG_BEGIN_NETWORK_MESSAGES
		|| type == GameMessage::MSG_ENABLE_RETALIATION_MODE;
}

class ScriptApiInputGuard : public GameMessageTranslator
{
public:
	virtual GameMessageDisposition translateGameMessage(const GameMessage *msg) override
	{
		const std::optional<InputKind> kind = inputKindOf(msg->getType());
		if (TheScriptApi == nullptr || !kind)
			return KEEP_MESSAGE;

		switch (scriptapi::inputAction(TheScriptApi->inputMode(), *kind))
		{
		case InputAction::Pass:
			TheScriptApi->countInput("passed", inputKindName(*kind));
			return KEEP_MESSAGE;
		case InputAction::CameraOnly:
			TheScriptApi->countInput("camera", inputKindName(*kind));
			if (TheLookAtTranslator != nullptr)
				TheLookAtTranslator->translateGameMessage(msg);
			return DESTROY_MESSAGE;
		case InputAction::Drop:
		default:
			TheScriptApi->countInput("dropped", inputKindName(*kind));
			return DESTROY_MESSAGE;
		}
	}
};

class ScriptApiCommandGuard : public GameMessageTranslator
{
public:
	virtual GameMessageDisposition translateGameMessage(const GameMessage *msg) override
	{
		const GameMessage::Type type = msg->getType();
		if (TheScriptApi == nullptr || type < GameMessage::MSG_BEGIN_NETWORK_MESSAGES || type > GameMessage::MSG_END_NETWORK_MESSAGES)
			return KEEP_MESSAGE;
		if (isSystemNetworkMessage(type) || TheScriptApi->isApiMessage(msg))
			return KEEP_MESSAGE;
		TheScriptApi->countInput("dropped", "foreign_command");
		DEBUG_LOG(("ScriptApi: dropped network message %d that the script did not send", (Int)type));
		return DESTROY_MESSAGE;
	}
};

struct NamedKey
{
	const char *name;
	Int key;
};

const NamedKey kInjectableKeys[] = {
	{"ESC", KEY_ESC}, {"P", KEY_P}, {"Q", KEY_Q}, {"A", KEY_A}, {"S", KEY_S},
	{"UP", KEY_UP}, {"DOWN", KEY_DOWN}, {"LEFT", KEY_LEFT}, {"RIGHT", KEY_RIGHT}, {"SPACE", KEY_SPACE},
};

Int requireKey(const Json &body)
{
	const auto key = body.find("key");
	if (key != body.end() && key->is_string())
	{
		for (const NamedKey &named : kInjectableKeys)
		{
			if (key->get<std::string>() == named.name)
				return named.key;
		}
	}
	throw CommandError(scriptapi::ErrorCode::BadRequest, "key must be one of ESC, P, Q, A, S, UP, DOWN, LEFT, RIGHT, SPACE");
}

ICoord2D pixelArgument(const Json &body)
{
	ICoord2D pixel;
	pixel.x = body.value("x", 0);
	pixel.y = body.value("y", 0);
	return pixel;
}

/// Appends raw input exactly the way Keyboard::createStreamMessages / Mouse::createStreamMessages do.
void injectInput(const Json &body)
{
	const std::string kind = body.value("kind", std::string());
	if (kind == "key" || kind == "key_down" || kind == "key_up")
	{
		const Int key = requireKey(body);
		const Int modifiers = body.value("shift", false) ? KEY_STATE_LSHIFT : 0;
		if (kind != "key_up")
		{
			GameMessage *down = TheMessageStream->appendMessage(GameMessage::MSG_RAW_KEY_DOWN);
			down->appendIntegerArgument(key);
			down->appendIntegerArgument(KEY_STATE_DOWN | modifiers);
		}
		if (kind != "key_down")
		{
			GameMessage *up = TheMessageStream->appendMessage(GameMessage::MSG_RAW_KEY_UP);
			up->appendIntegerArgument(key);
			up->appendIntegerArgument(KEY_STATE_UP | modifiers);
		}
		return;
	}
	if (kind == "click")
	{
		const std::string button = body.value("button", std::string("left"));
		GameMessage::Type downType;
		GameMessage::Type upType;
		if (button == "left") { downType = GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_DOWN; upType = GameMessage::MSG_RAW_MOUSE_LEFT_BUTTON_UP; }
		else if (button == "right") { downType = GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_DOWN; upType = GameMessage::MSG_RAW_MOUSE_RIGHT_BUTTON_UP; }
		else if (button == "middle") { downType = GameMessage::MSG_RAW_MOUSE_MIDDLE_BUTTON_DOWN; upType = GameMessage::MSG_RAW_MOUSE_MIDDLE_BUTTON_UP; }
		else throw CommandError(scriptapi::ErrorCode::BadRequest, "button must be left, right or middle");

		for (GameMessage::Type type : {downType, upType})
		{
			GameMessage *msg = TheMessageStream->appendMessage(type);
			msg->appendPixelArgument(pixelArgument(body));
			msg->appendIntegerArgument(0);
			msg->appendIntegerArgument(static_cast<Int>(timeGetTime()));
		}
		return;
	}
	if (kind == "wheel")
	{
		GameMessage *msg = TheMessageStream->appendMessage(GameMessage::MSG_RAW_MOUSE_WHEEL);
		msg->appendPixelArgument(pixelArgument(body));
		msg->appendRealArgument(body.value("spin", 1.0f));
		msg->appendIntegerArgument(0);
		return;
	}
	throw CommandError(scriptapi::ErrorCode::BadRequest, "kind must be key, key_down, key_up, click or wheel");
}

} // namespace

//-------------------------------------------------------------------------------------------------
void ScriptApi::setInputMode(scriptapi::InputMode mode)
{
	m_inputMode = mode;
	if (mode == scriptapi::InputMode::Full)
	{
		detachInputGuards();
		if (TheMouse != nullptr)
			TheMouse->setCursorCaptureMode(OptionPreferences().getCursorCaptureMode());
	}
	else
	{
		attachInputGuards();
		// Several bot windows may be open; never trap the cursor in one of them.
		if (TheMouse != nullptr)
			TheMouse->setCursorCaptureMode(0);
	}
	if (TheLookAtTranslator != nullptr)
		TheLookAtTranslator->resetModes();
	DEBUG_LOG(("ScriptApi: input mode %s", scriptapi::inputModeName(mode)));
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::attachInputGuards()
{
	if (m_inputGuardId == 0)
		m_inputGuardId = TheMessageStream->attachTranslator(MSGNEW("ScriptApi") ScriptApiInputGuard, kInputGuardPriority);
	if (m_commandGuardId == 0)
		m_commandGuardId = TheMessageStream->attachTranslator(MSGNEW("ScriptApi") ScriptApiCommandGuard, kCommandGuardPriority);
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::detachInputGuards()
{
	// removeTranslator also deletes the translator.
	if (m_inputGuardId != 0 && TheMessageStream != nullptr)
		TheMessageStream->removeTranslator(m_inputGuardId);
	if (m_commandGuardId != 0 && TheMessageStream != nullptr)
		TheMessageStream->removeTranslator(m_commandGuardId);
	m_inputGuardId = 0;
	m_commandGuardId = 0;
}

//-------------------------------------------------------------------------------------------------
GameMessage *ScriptApi::lastStreamMessage() const
{
	GameMessage *last = nullptr;
	for (GameMessage *msg = TheMessageStream->getFirstMessage(); msg != nullptr; msg = msg->next())
		last = msg;
	return last;
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::markApiMessagesAfter(GameMessage *last)
{
	GameMessage *msg = last != nullptr ? last->next() : TheMessageStream->getFirstMessage();
	for (; msg != nullptr; msg = msg->next())
		m_apiMessages.insert(msg);
}

//-------------------------------------------------------------------------------------------------
Bool ScriptApi::isApiMessage(const GameMessage *msg) const
{
	return m_apiMessages.count(msg) != 0;
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::countInput(const char *outcome, const char *kind)
{
	++m_inputStats[outcome][kind];
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::registerInputCommands()
{
#ifdef RTS_DEBUG
	if (!TheGlobalData->m_scriptApiDebug)
		return;

	m_router.add("set_input_mode", [this](const Request &request) {
		setInputMode(scriptapi::parseInputMode(request.body.value("mode", std::string())));
		return Json{{"mode", scriptapi::inputModeName(m_inputMode)}};
	});

	m_router.add("debug_input_stats", [this](const Request &) {
		Json result{{"mode", scriptapi::inputModeName(m_inputMode)}};
		for (const char *outcome : {"passed", "dropped", "camera"})
		{
			Json counts = Json::object();
			for (InputKind kind : {InputKind::MousePosition, InputKind::MouseLeft, InputKind::MouseRight,
					InputKind::MouseMiddle, InputKind::MouseWheel, InputKind::Key})
				counts[inputKindName(kind)] = m_inputStats[outcome][inputKindName(kind)];
			result[outcome] = counts;
		}
		result["foreign_commands_dropped"] = m_inputStats["dropped"]["foreign_command"];
		return result;
	});

	m_router.add("debug_inject_input", [this](const Request &request) {
		requireInGame();
		injectInput(request.body);
		return Json();
	});

	m_router.add("debug_camera", [this](const Request &) {
		requireInGame();
		const Coord3D &pos = TheTacticalView->getPosition();
		return Json{
			{"pos", Json::array({pos.x, pos.y, pos.z})},
			{"zoom", TheTacticalView->getZoom()},
			{"angle", TheTacticalView->getAngle()},
		};
	});
#endif
}

#endif // RTS_SCRIPT_API
