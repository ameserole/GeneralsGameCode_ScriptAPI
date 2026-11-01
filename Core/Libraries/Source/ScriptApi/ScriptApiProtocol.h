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

// Script API wire protocol: request parsing and reply/event serialization.
// Game-independent so it can be unit tested without the engine.

#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace scriptapi
{

using Json = nlohmann::json;

constexpr int kProtocolVersion = 1;

enum class ErrorCode
{
	BadJson,
	BadRequest,
	UnknownCommand,
	InternalError,
	ProtocolMismatch,
	WrongState,       ///< The command is not valid in the current game state.
	InvalidArgument,  ///< Well-formed, but names something unknown or is out of range for this game.
	NotOwned,         ///< An object id is not one of the player's own live, selectable objects.
	NotVisible,       ///< A target is not visible to the player.
	NotAvailable,     ///< The object has no such command, or it is disabled (prerequisites, power...).
	InsufficientFunds,
	QueueFull,
	ParkingFull,
	LimitReached,     ///< The player already has as many of these as allowed.
	IllegalLocation,  ///< A structure cannot be placed there.
	CannotAttack,     ///< None of the units can attack the target.
	CannotEnter,      ///< None of the units can enter the target (full, wrong kind, enemy...).
	NotReady,         ///< The LAN game cannot start yet (players have not accepted).
	RateLimited,      ///< The order would go over the actions-per-minute cap (-scriptApiApm).
	WrongRole,        ///< A watcher connection asked for a command only the controller may use.
};

const char *errorCodeName(ErrorCode code);

/// A connection's role. The controller (the bot) has every command and is replaced by a new
/// controller; watchers query, receive events and give orders alongside it.
enum class ConnectionRole
{
	Controller,
	Watcher,
};

const char *roleName(ConnectionRole role);

struct Request
{
	Json id;          ///< Echoed back as "reply_to"; null when the request had no id.
	std::string cmd;
	Json body;        ///< The full request object, including "id" and "cmd".
	uint32_t connection = 0;                      ///< The connection the request came from
	ConnectionRole role = ConnectionRole::Controller;
};

struct ParseResult
{
	bool ok = false;
	Request request;
	ErrorCode error = ErrorCode::BadRequest;
	std::string message;
};

ParseResult parseRequest(const std::string &text);

/// Envelope fields ("type", "reply_to", "ok") always win over same-named entries in fields.
/// Fields are taken by value so large payloads (snapshots) can be moved in rather than copied.
std::string makeReply(const Json &replyTo, Json fields = Json::object());
std::string makeError(const Json &replyTo, ErrorCode code, const std::string &message);
std::string makeEvent(const std::string &name, unsigned int frame, Json fields = Json::object());

/// Thrown by command handlers to produce an error reply with a specific code.
class CommandError : public std::runtime_error
{
public:
	CommandError(ErrorCode code, const std::string &message) : std::runtime_error(message), m_code(code) {}
	ErrorCode code() const { return m_code; }

private:
	ErrorCode m_code;
};

} // namespace scriptapi
