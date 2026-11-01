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

#include "ScriptApiWorld.h"

#include <cmath>

namespace scriptapi
{

const char *relationName(Relation relation)
{
	switch (relation)
	{
	case Relation::Self: return "self";
	case Relation::Ally: return "ally";
	case Relation::Enemy: return "enemy";
	case Relation::Neutral: return "neutral";
	}
	return "neutral";
}

const char *shroudName(Shroud shroud)
{
	switch (shroud)
	{
	case Shroud::Clear: return "clear";
	case Shroud::PartialClear: return "partial";
	case Shroud::Fogged: return "fogged";
	case Shroud::Shrouded: return "shrouded";
	}
	return "shrouded";
}

bool isVisibleToPlayer(Relation relation, Shroud shroud, bool stealthed, bool detected)
{
	if (relation == Relation::Self)
		return true;
	if (shroud != Shroud::Clear && shroud != Shroud::PartialClear)
		return false;
	// Undetected stealth only hides units from their enemies.
	if (relation == Relation::Enemy && stealthed && !detected)
		return false;
	return true;
}

bool mayReportPlayerStats(bool isSelf, bool isObserver, bool gameOver)
{
	return isSelf || isObserver || gameOver;
}

//-------------------------------------------------------------------------------------------------
namespace
{

bool sameSighting(const KnownStructure &a, const KnownStructure &b)
{
	return a.templateName == b.templateName && a.owner == b.owner && a.x == b.x && a.y == b.y && a.z == b.z;
}

} // namespace

bool KnownStructureMemory::observe(const KnownStructure &seen)
{
	const auto it = m_entries.find(seen.id);
	if (it == m_entries.end())
	{
		m_entries.emplace(seen.id, seen);
		++m_version;
		return true;
	}
	else if (!sameSighting(it->second, seen))
	{
		it->second = seen;
		++m_version;
	}
	else
	{
		// Unchanged apart from the frame; not worth invalidating caches of the reported data for.
		it->second.lastSeenFrame = seen.lastSeenFrame;
	}
	return false;
}

bool KnownStructureMemory::observeFogged(const KnownStructure &ghost)
{
	if (!m_entries.emplace(ghost.id, ghost).second)
		return false;
	++m_version;
	return true;
}

void KnownStructureMemory::forgetUnseen(const CellVisible &cellVisible, const std::set<uint32_t> &visibleIds)
{
	for (auto it = m_entries.begin(); it != m_entries.end();)
	{
		const KnownStructure &entry = it->second;
		if (visibleIds.count(entry.id) == 0 && cellVisible(entry.x, entry.y))
		{
			it = m_entries.erase(it);
			++m_version;
		}
		else
			++it;
	}
}

void KnownStructureMemory::forget(uint32_t id)
{
	if (m_entries.erase(id) != 0)
		++m_version;
}

std::vector<KnownStructure> KnownStructureMemory::remembered(const std::set<uint32_t> &visibleIds) const
{
	std::vector<KnownStructure> result;
	for (const auto &entry : m_entries)
	{
		if (visibleIds.count(entry.first) == 0)
			result.push_back(entry.second);
	}
	return result;
}

//-------------------------------------------------------------------------------------------------
void SnapshotTimer::setInterval(unsigned int interval)
{
	m_interval = interval;
	m_hasFired = false;
}

bool SnapshotTimer::due(unsigned int frame)
{
	if (m_interval == 0)
		return false;
	if (!m_hasFired || frame < m_lastFrame || frame >= m_lastFrame + m_interval)
	{
		m_hasFired = true;
		m_lastFrame = frame;
		return true;
	}
	return false;
}

//-------------------------------------------------------------------------------------------------
Json encodeTerrainGrid(int columns, int rows, const std::function<TerrainSample(int column, int row)> &sample)
{
	Json heights = Json::array();
	Json terrain = Json::array();
	for (int row = 0; row < rows; ++row)
	{
		Json heightRow = Json::array();
		std::string terrainRow;
		terrainRow.reserve(columns);
		for (int column = 0; column < columns; ++column)
		{
			const TerrainSample cell = sample(column, row);
			heightRow.push_back(static_cast<int>(std::lround(cell.height)));
			terrainRow.push_back(cell.kind);
		}
		heights.push_back(std::move(heightRow));
		terrain.push_back(std::move(terrainRow));
	}
	return Json{{"heights", std::move(heights)}, {"terrain", std::move(terrain)}};
}

} // namespace scriptapi
