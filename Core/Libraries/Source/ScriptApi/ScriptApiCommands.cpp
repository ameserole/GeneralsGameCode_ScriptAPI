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

#include "ScriptApiCommands.h"

#include <algorithm>
#include <cctype>

namespace scriptapi
{

namespace
{

[[noreturn]] void badRequest(const std::string &message)
{
	throw CommandError(ErrorCode::BadRequest, message);
}

uint32_t toObjectId(const Json &value, const std::string &what)
{
	// Object id 0 is INVALID_ID in the engine.
	if (!value.is_number_integer() || value.get<long long>() <= 0 || value.get<long long>() > 0xFFFFFFFFll)
		badRequest(what + " must be a positive integer object id");
	return static_cast<uint32_t>(value.get<long long>());
}

} // namespace

std::vector<uint32_t> parseObjectIds(const Json &body, const char *key)
{
	const auto it = body.find(key);
	if (it == body.end() || !it->is_array() || it->empty())
		badRequest(std::string(key) + " must be a non-empty array of object ids");
	if (it->size() > kMaxUnitsPerCommand)
		badRequest(std::string(key) + " can have at most " + std::to_string(kMaxUnitsPerCommand) + " ids");

	std::vector<uint32_t> ids;
	for (const Json &value : *it)
	{
		const uint32_t id = toObjectId(value, key);
		if (std::find(ids.begin(), ids.end(), id) == ids.end())
			ids.push_back(id);
	}
	return ids;
}

uint32_t parseObjectId(const Json &body, const char *key)
{
	const auto it = body.find(key);
	if (it == body.end())
		badRequest(std::string(key) + " is required");
	return toObjectId(*it, key);
}

Position parsePosition(const Json &body, const char *key)
{
	const auto it = body.find(key);
	if (it == body.end() || !it->is_array() || it->size() < 2 || it->size() > 3)
		badRequest(std::string(key) + " must be [x, y]");
	for (const Json &value : *it)
	{
		if (!value.is_number())
			badRequest(std::string(key) + " must contain numbers");
	}
	Position pos;
	pos.x = (*it)[0].get<float>();
	pos.y = (*it)[1].get<float>();
	return pos;
}

std::vector<Position> parseWaypoints(const Json &body, const char *key)
{
	std::vector<Position> points;
	const auto it = body.find(key);
	if (it == body.end() || it->is_null())
		return points;
	if (!it->is_array() || it->size() > kMaxWaypoints)
		badRequest(std::string(key) + " must be an array of at most " + std::to_string(kMaxWaypoints) + " [x, y] points");
	for (const Json &point : *it)
		points.push_back(parsePosition(Json{{"point", point}}, "point"));
	return points;
}

//-------------------------------------------------------------------------------------------------
InputMode parseInputMode(const std::string &name)
{
	if (name == "full") return InputMode::Full;
	if (name == "view") return InputMode::View;
	if (name == "locked") return InputMode::Locked;
	badRequest("input mode must be one of view, locked, full");
}

const char *inputModeName(InputMode mode)
{
	switch (mode)
	{
	case InputMode::Full: return "full";
	case InputMode::View: return "view";
	case InputMode::Locked: return "locked";
	}
	return "view";
}

InputAction inputAction(InputMode mode, InputKind kind)
{
	switch (mode)
	{
	case InputMode::Full:
		return InputAction::Pass;
	case InputMode::Locked:
		return InputAction::Drop;
	case InputMode::View:
		switch (kind)
		{
		case InputKind::MousePosition: return InputAction::Pass;       // cursor and screen-edge scrolling
		case InputKind::MouseLeft: return InputAction::Drop;           // selection, buttons, placement
		case InputKind::MouseRight: return InputAction::CameraOnly;    // right-drag scrolling, never orders
		case InputKind::MouseMiddle: return InputAction::CameraOnly;   // rotation
		case InputKind::MouseWheel: return InputAction::CameraOnly;    // zoom
		case InputKind::Key: return InputAction::CameraOnly;           // arrow-key scrolling, never hotkeys
		}
		break;
	}
	return InputAction::Drop;
}

//-------------------------------------------------------------------------------------------------
void CommandTracker::add(const Json &requestId, const std::string &cmd, const std::vector<int> &messageTypes, uint32_t connection)
{
	if (messageTypes.empty())
		return;
	Entry entry;
	entry.requestId = requestId;
	entry.cmd = cmd;
	entry.connection = connection;
	entry.remaining.assign(messageTypes.begin(), messageTypes.end());
	m_pending.push_back(std::move(entry));
}

bool CommandTracker::onExecuted(int messageType, Completion &out)
{
	if (m_pending.empty() || m_pending.front().remaining.front() != messageType)
		return false;

	Entry &head = m_pending.front();
	head.remaining.pop_front();
	if (!head.remaining.empty())
		return false;

	const bool report = !head.orphaned;
	out.requestId = std::move(head.requestId);
	out.cmd = std::move(head.cmd);
	out.connection = head.connection;
	m_pending.pop_front();
	return report;
}

void CommandTracker::orphanAll()
{
	for (Entry &entry : m_pending)
		entry.orphaned = true;
}

void CommandTracker::orphanConnection(uint32_t connection)
{
	for (Entry &entry : m_pending)
	{
		if (entry.connection == connection)
			entry.orphaned = true;
	}
}

//-------------------------------------------------------------------------------------------------
ActionRateLimiter::ActionRateLimiter(int limit, uint32_t windowFrames)
	: m_limit(limit > 0 ? limit : 0)
	, m_windowFrames(windowFrames)
{
}

void ActionRateLimiter::setLimit(int limit)
{
	m_limit = limit > 0 ? limit : 0;
	m_actions.clear();
}

void ActionRateLimiter::expire(uint32_t frame)
{
	while (!m_actions.empty() && frame - m_actions.front() >= m_windowFrames)
		m_actions.pop_front();
}

bool ActionRateLimiter::allows(uint32_t frame)
{
	if (m_limit == 0)
		return true;
	expire(frame);
	return m_actions.size() < static_cast<size_t>(m_limit);
}

void ActionRateLimiter::record(uint32_t frame)
{
	if (m_limit != 0)
		m_actions.push_back(frame);
}

uint32_t ActionRateLimiter::framesUntilAvailable(uint32_t frame)
{
	if (allows(frame))
		return 0;
	// The slot frees up when the oldest action counting against the cap leaves the window.
	const uint32_t oldest = m_actions[m_actions.size() - static_cast<size_t>(m_limit)];
	return oldest + m_windowFrames - frame;
}

//-------------------------------------------------------------------------------------------------
void PendingSpend::add(int messageType, int cost)
{
	m_entries.push_back(Entry{messageType, cost});
}

void PendingSpend::onExecuted(int messageType)
{
	for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
	{
		if (it->messageType == messageType)
		{
			m_entries.erase(it);
			return;
		}
	}
}

int PendingSpend::committed() const
{
	int total = 0;
	for (const Entry &entry : m_entries)
		total += entry.cost;
	return total;
}

//-------------------------------------------------------------------------------------------------
std::string parseReplayName(const std::string &name)
{
	std::string result = name;
	const std::string extension = ".rep";
	if (result.size() > extension.size())
	{
		std::string tail = result.substr(result.size() - extension.size());
		std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (tail == extension)
			result.resize(result.size() - extension.size());
	}

	if (result.empty() || result.size() > kMaxReplayNameLength)
		throw CommandError(ErrorCode::InvalidArgument, "replay name must be 1-" + std::to_string(kMaxReplayNameLength) + " characters");
	for (const char c : result)
	{
		const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
			|| c == ' ' || c == '_' || c == '-' || c == '.' || c == '(' || c == ')';
		if (!allowed)
			throw CommandError(ErrorCode::InvalidArgument, "replay name may only use letters, digits, space and _-.()");
	}
	const char first = result.front();
	const char last = result.back();
	if (first == ' ' || first == '.' || last == ' ' || last == '.')
		throw CommandError(ErrorCode::InvalidArgument, "replay name must not start or end with a space or dot");
	return result;
}

} // namespace scriptapi
