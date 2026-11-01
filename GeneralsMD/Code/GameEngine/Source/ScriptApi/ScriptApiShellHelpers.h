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

// ScriptApiShellHelpers.h ////////////////////////////////////////////////////////////////////////
// Private helpers shared by the script API menu commands (ScriptApiSkirmish.cpp,
// ScriptApiLanLobby.cpp). Defined in ScriptApiSkirmish.cpp.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#if RTS_SCRIPT_API

#include <string>
#include <vector>

namespace scriptapi_shell
{

std::string toUtf8(const UnicodeString &text);
UnicodeString fromUtf8(const std::string &text);

/// Player templates offered by the game setup menus, using the filter of PopulatePlayerTemplateComboBox.
std::vector<Int> selectableFactions();

/// A selectable faction by name. Throws CommandError(InvalidArgument).
Int findFaction(const std::string &name);

/// A faction name for the API: the template name, "random" or "observer".
std::string factionName(Int playerTemplate);

} // namespace scriptapi_shell

#endif // RTS_SCRIPT_API
