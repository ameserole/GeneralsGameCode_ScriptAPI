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

// Game-independent parts of the script API order commands: argument parsing, the input lock
// policy and the tracking of which request a message executed by the game logic belongs to.

#pragma once

#include "ScriptApiProtocol.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace scriptapi
{

constexpr size_t kMaxUnitsPerCommand = 100;

/// An array of object ids, deduplicated in order. Throws CommandError(BadRequest).
std::vector<uint32_t> parseObjectIds(const Json &body, const char *key);

/// A single object id. Throws CommandError(BadRequest).
uint32_t parseObjectId(const Json &body, const char *key);

struct Position
{
	float x = 0.0f;
	float y = 0.0f;
};

/// [x, y] or [x, y, z] (z is ignored: the ground height is used). Throws CommandError(BadRequest).
Position parsePosition(const Json &body, const char *key);

constexpr size_t kMaxWaypoints = 20;

/// Optional list of positions; empty when the key is absent. Throws CommandError(BadRequest).
std::vector<Position> parseWaypoints(const Json &body, const char *key);

//-------------------------------------------------------------------------------------------------
// Input lock
//-------------------------------------------------------------------------------------------------
enum class InputMode
{
	Full,    ///< Human input works as normal (debugging only).
	View,    ///< Only camera control (scroll, zoom, rotate) reaches the game.
	Locked,  ///< No human input at all.
};

/// Throws CommandError(BadRequest) for unknown names.
InputMode parseInputMode(const std::string &name);
const char *inputModeName(InputMode mode);

enum class InputKind
{
	MousePosition,
	MouseLeft,
	MouseRight,
	MouseMiddle,
	MouseWheel,
	Key,
};

enum class InputAction
{
	Pass,        ///< Let every translator see it.
	Drop,        ///< Nobody sees it.
	CameraOnly,  ///< Only the camera (look-at) translator sees it.
};

InputAction inputAction(InputMode mode, InputKind kind);

//-------------------------------------------------------------------------------------------------
/// Matches network messages executed by the game logic to the requests that sent them. Lockstep
/// preserves the order in which the local player's messages were sent, so a FIFO is enough.
class CommandTracker
{
public:
	struct Completion
	{
		Json requestId;
		std::string cmd;
		uint32_t connection = 0; ///< The connection that sent the request
	};

	/// Records that a request sent these order messages, in order.
	void add(const Json &requestId, const std::string &cmd, const std::vector<int> &messageTypes, uint32_t connection = 0);

	/// Called for every network message of the local player the logic executes. Returns true when
	/// it was the last outstanding message of the oldest request that was not orphaned.
	bool onExecuted(int messageType, Completion &out);

	/// The client that sent the pending requests is gone: keep the entries so later messages still
	/// line up, but report no completion for them.
	void orphanAll();

	/// Like orphanAll, for the requests of one connection that closed.
	void orphanConnection(uint32_t connection);

	void clear() { m_pending.clear(); }
	size_t pending() const { return m_pending.size(); }

private:
	struct Entry
	{
		Json requestId;
		std::string cmd;
		std::deque<int> remaining;
		uint32_t connection = 0;
		bool orphaned = false;
	};

	std::deque<Entry> m_pending;
};

//-------------------------------------------------------------------------------------------------
/// The optional cap on accepted orders per minute of game time (-scriptApiApm).
class ActionRateLimiter
{
public:
	static constexpr uint32_t kDefaultWindowFrames = 30 * 60; ///< One minute at 30 logic frames per second

	explicit ActionRateLimiter(int limit = 0, uint32_t windowFrames = kDefaultWindowFrames);

	/// 0 or less turns the cap off. Forgets the actions recorded so far.
	void setLimit(int limit);
	int limit() const { return m_limit; }

	/// True when one more action fits into the window ending at this frame.
	bool allows(uint32_t frame);
	void record(uint32_t frame);
	/// Frames until allows() turns true; 0 when it already is.
	uint32_t framesUntilAvailable(uint32_t frame);
	void clear() { m_actions.clear(); }

private:
	void expire(uint32_t frame);

	int m_limit;
	uint32_t m_windowFrames;
	std::deque<uint32_t> m_actions; ///< Frames of the actions in the window, oldest first
};

//-------------------------------------------------------------------------------------------------
/// Money promised to spending orders (builds, units, upgrades) that the logic has not executed
/// yet. The engine only takes the money on execution, so orders checked against the bank alone
/// could overspend it between the request and the execution.
class PendingSpend
{
public:
	void add(int messageType, int cost);
	/// The logic executed a message of this type: the oldest entry of that type is settled.
	void onExecuted(int messageType);
	int committed() const;
	void clear() { m_entries.clear(); }
	size_t size() const { return m_entries.size(); }

private:
	struct Entry
	{
		int messageType;
		int cost;
	};
	std::deque<Entry> m_entries; ///< In the order the orders were sent
};

//-------------------------------------------------------------------------------------------------
constexpr size_t kMaxReplayNameLength = 64;

/// A replay file name without folder or extension: 1-64 of letters, digits, space and _-.(),
/// no leading or trailing space or dot. A trailing ".rep" is removed first. Throws
/// CommandError(InvalidArgument).
std::string parseReplayName(const std::string &name);

} // namespace scriptapi
