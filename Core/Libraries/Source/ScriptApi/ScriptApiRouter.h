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

// Maps script API command names to handlers and turns raw request text into reply text.

#pragma once

#include "ScriptApiProtocol.h"

#include <functional>
#include <map>
#include <string>

namespace scriptapi
{

class CommandRouter
{
public:
	/// Returns the reply fields (an object, or null for none). Throw CommandError to reply with an error.
	using Handler = std::function<Json(const Request &)>;

	/// Who may use a command: everyone, or only the controller (session and lifecycle commands).
	enum class Access
	{
		Any,
		ControllerOnly,
	};

	void add(const std::string &cmd, Handler handler, Access access = Access::Any);
	/// Changes who may use an already registered command; unknown commands are ignored.
	void setAccess(const std::string &cmd, Access access);
	bool has(const std::string &cmd) const;

	/// Parses, checks the role, routes and serializes. Never throws.
	std::string dispatch(const std::string &text, uint32_t connection = 0, ConnectionRole role = ConnectionRole::Controller) const;

private:
	struct Route
	{
		Handler handler;
		Access access;
	};
	std::map<std::string, Route> m_routes;
};

} // namespace scriptapi
