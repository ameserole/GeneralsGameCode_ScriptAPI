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

#include "ScriptApiRouter.h"

#include <exception>

namespace scriptapi
{

void CommandRouter::add(const std::string &cmd, Handler handler, Access access)
{
	m_routes[cmd] = Route{std::move(handler), access};
}

void CommandRouter::setAccess(const std::string &cmd, Access access)
{
	const auto it = m_routes.find(cmd);
	if (it != m_routes.end())
		it->second.access = access;
}

bool CommandRouter::has(const std::string &cmd) const
{
	return m_routes.find(cmd) != m_routes.end();
}

std::string CommandRouter::dispatch(const std::string &text, uint32_t connection, ConnectionRole role) const
{
	ParseResult parsed = parseRequest(text);
	if (!parsed.ok)
		return makeError(parsed.request.id, parsed.error, parsed.message);

	Request &request = parsed.request;
	request.connection = connection;
	request.role = role;
	const auto it = m_routes.find(request.cmd);
	if (it == m_routes.end())
		return makeError(request.id, ErrorCode::UnknownCommand, "unknown command \"" + request.cmd + "\"");
	if (it->second.access == Access::ControllerOnly && role != ConnectionRole::Controller)
		return makeError(request.id, ErrorCode::WrongRole, "only the controller connection may use \"" + request.cmd + "\"");

	try
	{
		return makeReply(request.id, it->second.handler(request));
	}
	catch (const CommandError &error)
	{
		return makeError(request.id, error.code(), error.what());
	}
	catch (const std::exception &error)
	{
		return makeError(request.id, ErrorCode::InternalError, error.what());
	}
	catch (...)
	{
		return makeError(request.id, ErrorCode::InternalError, "unknown error");
	}
}

} // namespace scriptapi
