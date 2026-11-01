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

// Game-independent building blocks of the script API world view: the visibility rule, the memory
// of structures seen through the fog, change detection, snapshot timing and terrain grid encoding.

#pragma once

#include "ScriptApiProtocol.h"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace scriptapi
{

//-------------------------------------------------------------------------------------------------
/// Reports when a polled value changes. The first value only establishes the baseline.
template <typename T>
class ChangeDetector
{
public:
	bool update(const T &value, T *previous = nullptr)
	{
		if (!m_hasValue)
		{
			m_hasValue = true;
			m_value = value;
			return false;
		}
		if (value == m_value)
			return false;
		if (previous != nullptr)
			*previous = m_value;
		m_value = value;
		return true;
	}

	void reset() { m_hasValue = false; }
	bool hasValue() const { return m_hasValue; }
	const T &value() const { return m_value; }

private:
	bool m_hasValue = false;
	T m_value{};
};

//-------------------------------------------------------------------------------------------------
enum class Relation
{
	Self,
	Ally,
	Enemy,
	Neutral,
};

enum class Shroud
{
	Clear,
	PartialClear,
	Fogged,
	Shrouded,
};

const char *relationName(Relation relation);
const char *shroudName(Shroud shroud);

/// The single rule deciding whether the script may learn about an object right now: exactly what
/// the local human player would see on screen.
bool isVisibleToPlayer(Relation relation, Shroud shroud, bool stealthed, bool detected);

/// Whether a player's score-screen statistics may be reported: a player's own at any time; everyone's
/// to an observer, or to anyone once the game is over (the score screen shows them all).
bool mayReportPlayerStats(bool isSelf, bool isObserver, bool gameOver);

//-------------------------------------------------------------------------------------------------
struct KnownStructure
{
	uint32_t id = 0;
	std::string templateName;
	int owner = -1;
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	unsigned int lastSeenFrame = 0;
};

/// What the player remembers about other players' structures seen earlier: the last sighting,
/// kept until the player can see the spot again and the structure is not there.
class KnownStructureMemory
{
public:
	using CellVisible = std::function<bool(float x, float y)>;

	/// Records a structure that is visible now. Returns true if it was not known before.
	bool observe(const KnownStructure &seen);

	/// Records a structure the player can only see as a fog "ghost" (the engine reports it as fogged).
	/// Ghosts are frozen while fogged, so an existing entry is left unchanged. Returns true if new.
	bool observeFogged(const KnownStructure &ghost);

	/// Forgets structures whose last seen position is visible again but that are not visible.
	void forgetUnseen(const CellVisible &cellVisible, const std::set<uint32_t> &visibleIds);

	/// Forgets a structure, e.g. one seen being destroyed.
	void forget(uint32_t id);

	/// Remembered structures that are not currently visible (those are reported as visible instead).
	std::vector<KnownStructure> remembered(const std::set<uint32_t> &visibleIds) const;

	bool knows(uint32_t id) const { return m_entries.count(id) != 0; }
	void clear() { m_entries.clear(); ++m_version; }
	size_t size() const { return m_entries.size(); }

	/// Increases whenever an entry is added, changed or removed, so callers can cache derived data.
	uint64_t version() const { return m_version; }

private:
	std::map<uint32_t, KnownStructure> m_entries;
	uint64_t m_version = 0;
};

//-------------------------------------------------------------------------------------------------
/// Decides on which frames a periodic snapshot is due.
class SnapshotTimer
{
public:
	explicit SnapshotTimer(unsigned int interval = 15) : m_interval(interval) {}

	/// 0 disables snapshots. Changing the interval makes the next check fire.
	void setInterval(unsigned int interval);
	unsigned int interval() const { return m_interval; }

	/// True when a snapshot is due on this frame. Also fires when the frame counter restarts (new game).
	bool due(unsigned int frame);

	void reset() { m_hasFired = false; }

private:
	unsigned int m_interval;
	unsigned int m_lastFrame = 0;
	bool m_hasFired = false;
};

//-------------------------------------------------------------------------------------------------
struct TerrainSample
{
	float height = 0.0f;
	char kind = '.'; ///< '.' passable, 'w' water, 'c' cliff, 'x' impassable
};

/// Builds {"heights": [[int]], "terrain": ["..."]} with one row per y, sampling each cell once.
Json encodeTerrainGrid(int columns, int rows, const std::function<TerrainSample(int column, int row)> &sample);

} // namespace scriptapi
