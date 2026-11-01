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

// ScriptApi.cpp //////////////////////////////////////////////////////////////////////////////////
// Script API subsystem. See ScriptApi.h.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#if RTS_SCRIPT_API

#include "ScriptApi/ScriptApi.h"

#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "GameClient/ClientInstance.h"
#include "GameLogic/GameLogic.h"

ScriptApi *TheScriptApi = nullptr;

namespace
{

constexpr int kDefaultPort = 27100;
constexpr int kMaxInboundPerFrame = 64; ///< Bounds the work a flooding script can cause in one frame.

} // namespace

//-------------------------------------------------------------------------------------------------
ScriptApi::ScriptApi() : m_exitRequested(FALSE), m_transitionPending(FALSE), m_gameOverSent(FALSE)
{
}

//-------------------------------------------------------------------------------------------------
ScriptApi::~ScriptApi()
{
	detachInputGuards();
	m_server.stop();
	DEBUG_LOG(("ScriptApi: server stopped"));
	TheScriptApi = nullptr;
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::init()
{
	registerCommands();

	scriptapi::ServerOptions options;
	options.port = TheGlobalData->m_scriptApiPort > 0
		? TheGlobalData->m_scriptApiPort
		: kDefaultPort + static_cast<int>(rts::ClientInstance::getInstanceIndex());

	scriptapi::InputMode mode = scriptapi::InputMode::View;
	if (TheGlobalData->m_scriptApiInput.isNotEmpty())
	{
		try
		{
			mode = scriptapi::parseInputMode(TheGlobalData->m_scriptApiInput.str());
		}
		catch (const scriptapi::CommandError &)
		{
			DEBUG_LOG(("ScriptApi: unknown -scriptApiInput '%s', using view", TheGlobalData->m_scriptApiInput.str()));
		}
	}
	setInputMode(mode);
	m_actionLimiter.setLimit(TheGlobalData->m_scriptApiApm);

	std::string error;
	if (m_server.start(options, &error))
		DEBUG_LOG(("ScriptApi: listening on %s:%d", options.host.c_str(), options.port));
	else
		DEBUG_LOG(("ScriptApi: failed to listen on %s:%d: %s", options.host.c_str(), options.port, error.c_str()));
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::reset()
{
	// The connection deliberately survives engine resets between games.
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::update()
{
	if (m_exitRequested)
	{
		// Deferred by one frame so the reply to "exit" is queued before shutdown begins;
		// the destructor flushes it.
		TheGameEngine->setQuitting(TRUE);
		return;
	}

	// Remember which messages this update adds, so the command guard can tell the script's own
	// network messages from anything else.
	m_apiMessages.clear();
	GameMessage *last = lastStreamMessage();

	trackGameState();
	processInbound();

	if (currentGameState() == scriptapi::GameState::InGame)
		updateWorldView();
	else if (currentGameState() == scriptapi::GameState::Shell)
		updateLan();

	markApiMessagesAfter(last);
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::sendEvent(const std::string &name, scriptapi::Json fields)
{
	if (!m_server.connections().empty())
		m_server.broadcast(scriptapi::makeEvent(name, currentFrame(), std::move(fields)));
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::sendEventTo(uint32_t connection, const std::string &name, scriptapi::Json fields)
{
	if (connection != 0)
		m_server.send(connection, scriptapi::makeEvent(name, currentFrame(), std::move(fields)));
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::onLoadingStarted()
{
	trackGameState();
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::registerCommands()
{
	using scriptapi::Json;
	using scriptapi::Request;

	m_router.add("hello", [this](const Request &request) {
		const auto version = request.body.find("protocol_version");
		if (version != request.body.end() && *version != scriptapi::kProtocolVersion)
		{
			throw scriptapi::CommandError(scriptapi::ErrorCode::ProtocolMismatch,
				"server speaks protocol version " + std::to_string(scriptapi::kProtocolVersion));
		}
		return Json{
			{"protocol_version", scriptapi::kProtocolVersion},
			{"game", "zh"},
#ifdef RTS_DEBUG
			{"build", "debug"},
#else
			{"build", "release"},
#endif
			{"game_state", scriptapi::gameStateName(currentGameState())},
			{"frame", currentFrame()},
			{"apm_limit", m_actionLimiter.limit()},
			{"role", scriptapi::roleName(request.role)},
			// A client that connects after game_over (e.g. between two watch calls) still learns the result.
			{"game_result", m_gameResult.empty() ? Json() : Json(m_gameResult)},
		};
	});

	m_router.add("ping", [this](const Request &) {
		return Json{{"frame", currentFrame()}};
	});

	m_router.add("exit", [this](const Request &) {
		m_exitRequested = TRUE;
		return Json();
	});

	registerSkirmishCommands();
	registerLanCommands();
	registerWorldCommands();
	registerActionCommands();
	registerAbilityCommands();
	registerInputCommands();

	// Session and lifecycle commands belong to the controller; watchers query, listen and give orders.
	for (const char *cmd : {"exit", "start_skirmish", "quit_game", "save_replay", "surrender", "set_input_mode",
		"lan_open", "lan_host", "lan_join", "lan_slot", "lan_accept", "lan_start", "lan_leave",
		"debug_world", "debug_add_cash", "debug_hurt", "debug_give_science_points", "debug_input_stats",
		"debug_inject_input", "debug_camera"})
	{
		m_router.setAccess(cmd, scriptapi::CommandRouter::Access::ControllerOnly);
	}
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::processInbound()
{
	scriptapi::InboundMessage message;
	for (int i = 0; i < kMaxInboundPerFrame && m_server.pollInbound(message); ++i)
	{
		switch (message.kind)
		{
		case scriptapi::InboundMessage::Kind::Opened:
			DEBUG_LOG(("ScriptApi: %s %u connected", scriptapi::roleName(message.role), message.connectionId));
			// Every connection starts a clean session in the same match, with the default snapshot
			// interval; results of orders go only to the connection that sent them.
			m_snapshotTimers[message.connectionId] = scriptapi::SnapshotTimer();
			break;
		case scriptapi::InboundMessage::Kind::Closed:
			DEBUG_LOG(("ScriptApi: client %u disconnected", message.connectionId));
			m_commandTracker.orphanConnection(message.connectionId);
			m_snapshotTimers.erase(message.connectionId);
			break;
		case scriptapi::InboundMessage::Kind::Text:
			m_server.send(message.connectionId, m_router.dispatch(message.text, message.connectionId, message.role));
			break;
		}
	}
}

//-------------------------------------------------------------------------------------------------
void ScriptApi::trackGameState()
{
	scriptapi::GameStateTracker::Transition transition;
	if (m_stateTracker.update(currentGameState(), transition))
	{
		m_transitionPending = FALSE;
		if (transition.current == scriptapi::GameState::InGame)
		{
			resetWorldView();
			m_playedGame = TRUE;
		}
		sendEvent("game_state", scriptapi::Json{
			{"state", scriptapi::gameStateName(transition.current)},
			{"previous", scriptapi::gameStateName(transition.previous)},
		});
	}
}

//-------------------------------------------------------------------------------------------------
scriptapi::GameState ScriptApi::currentGameState() const
{
	if (TheGameLogic == nullptr)
		return scriptapi::GameState::Shell;
	return scriptapi::classifyGameState(TheGameLogic->isLoadingMap(), TheGameLogic->isInGame(), TheGameLogic->isInShellGame());
}

//-------------------------------------------------------------------------------------------------
UnsignedInt ScriptApi::currentFrame() const
{
	return TheGameLogic != nullptr ? TheGameLogic->getFrame() : 0;
}

#endif // RTS_SCRIPT_API
