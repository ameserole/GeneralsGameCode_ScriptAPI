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

// ScriptApiOrderHelpers.h ////////////////////////////////////////////////////////////////////////
// Private helpers shared by the script API order commands (ScriptApiActions.cpp,
// ScriptApiAbilities.cpp): the checks the UI makes before a click becomes a message, and building
// the selection message every order is preceded by. Defined in ScriptApiActions.cpp.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#if RTS_SCRIPT_API

#include "ScriptApiCommands.h"

#include "Common/GameType.h"
#include "GameClient/ControlBar.h"

#include <vector>

class Object;
class ThingTemplate;

namespace scriptapi_orders
{

[[noreturn]] void fail(scriptapi::ErrorCode code, const std::string &message);

/// A world position inside the map, with the ground height filled in.
Coord3D requirePosition(const scriptapi::Position &pos);

const ThingTemplate *requireTemplate(const scriptapi::Json &body);

/// The button the UI would show for this command type on this object, optionally for a product.
const CommandButton *findButton(const Object *obj, GUICommandType type, const ThingTemplate *product = nullptr);

/// The button with this name in the object's command set, if any.
const CommandButton *findButtonByName(const Object *obj, const std::string &name);

/// The general's science a button's special power needs before the control bar shows the button
/// at all, or SCIENCE_INVALID.
ScienceType requiredScience(const CommandButton *button);

/// The button must exist and be enabled, as it would have to be for a human to click it.
void requireButtonAvailable(const CommandButton *button, Object *obj, const char *what);

void requireCanMake(Object *builder, const ThingTemplate *tmpl);

/// What the UI sends when the player selects these objects.
void appendSelection(const std::vector<Object *> &objects);

/// The ability description used by the catalog, or null if the API does not support the button.
scriptapi::Json describeAbility(const CommandButton *button);

/// All API-supported abilities in a template's command set.
scriptapi::Json abilitiesOf(const ThingTemplate *tmpl);

} // namespace scriptapi_orders

#endif // RTS_SCRIPT_API
