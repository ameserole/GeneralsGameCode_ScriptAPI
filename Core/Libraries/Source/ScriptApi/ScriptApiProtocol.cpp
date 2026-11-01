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

#include "ScriptApiProtocol.h"

namespace scriptapi
{

const char *errorCodeName(ErrorCode code)
{
	switch (code)
	{
	case ErrorCode::BadJson: return "BAD_JSON";
	case ErrorCode::BadRequest: return "BAD_REQUEST";
	case ErrorCode::UnknownCommand: return "UNKNOWN_COMMAND";
	case ErrorCode::InternalError: return "INTERNAL_ERROR";
	case ErrorCode::ProtocolMismatch: return "PROTOCOL_MISMATCH";
	case ErrorCode::WrongState: return "WRONG_STATE";
	case ErrorCode::InvalidArgument: return "INVALID_ARGUMENT";
	case ErrorCode::NotOwned: return "NOT_OWNED";
	case ErrorCode::NotVisible: return "NOT_VISIBLE";
	case ErrorCode::NotAvailable: return "NOT_AVAILABLE";
	case ErrorCode::InsufficientFunds: return "INSUFFICIENT_FUNDS";
	case ErrorCode::QueueFull: return "QUEUE_FULL";
	case ErrorCode::ParkingFull: return "PARKING_FULL";
	case ErrorCode::LimitReached: return "LIMIT_REACHED";
	case ErrorCode::IllegalLocation: return "ILLEGAL_LOCATION";
	case ErrorCode::CannotAttack: return "CANNOT_ATTACK";
	case ErrorCode::CannotEnter: return "CANNOT_ENTER";
	case ErrorCode::NotReady: return "NOT_READY";
	case ErrorCode::RateLimited: return "RATE_LIMITED";
	case ErrorCode::WrongRole: return "WRONG_ROLE";
	}
	return "INTERNAL_ERROR";
}

const char *roleName(ConnectionRole role)
{
	return role == ConnectionRole::Watcher ? "watcher" : "controller";
}

ParseResult parseRequest(const std::string &text)
{
	ParseResult result;

	Json body = Json::parse(text, nullptr, false);
	if (body.is_discarded())
	{
		result.error = ErrorCode::BadJson;
		result.message = "request is not valid JSON";
		return result;
	}
	if (!body.is_object())
	{
		result.error = ErrorCode::BadRequest;
		result.message = "request must be a JSON object";
		return result;
	}

	const auto id = body.find("id");
	if (id != body.end())
		result.request.id = *id;

	const auto cmd = body.find("cmd");
	if (cmd == body.end() || !cmd->is_string())
	{
		result.error = ErrorCode::BadRequest;
		result.message = "request needs a string \"cmd\"";
		return result;
	}

	result.request.cmd = cmd->get<std::string>();
	result.request.body = std::move(body);
	result.ok = true;
	return result;
}

std::string makeReply(const Json &replyTo, Json fields)
{
	Json reply = fields.is_object() ? std::move(fields) : Json::object();
	reply["type"] = "reply";
	reply["reply_to"] = replyTo;
	reply["ok"] = true;
	return reply.dump();
}

std::string makeError(const Json &replyTo, ErrorCode code, const std::string &message)
{
	Json reply = Json::object();
	reply["type"] = "reply";
	reply["reply_to"] = replyTo;
	reply["ok"] = false;
	reply["error"] = errorCodeName(code);
	reply["message"] = message;
	// Invalid UTF-8 in a message (e.g. from an exception) must not throw during serialization.
	return reply.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string makeEvent(const std::string &name, unsigned int frame, Json fields)
{
	Json event = fields.is_object() ? std::move(fields) : Json::object();
	event["type"] = "event";
	event["event"] = name;
	event["frame"] = frame;
	return event.dump();
}

} // namespace scriptapi
