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

// Game-independent event bookkeeping for the script API: which enemy objects came into or went
// out of view, and per-object grouping of damage so a script is not flooded with one event per hit.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace scriptapi
{

struct VisibleInfo
{
	uint32_t id = 0;
	std::string templateName;
	int owner = -1;
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

class VisibilityTracker
{
public:
	struct Changes
	{
		std::vector<VisibleInfo> spotted;  ///< Visible now, not visible at the previous update.
		std::vector<VisibleInfo> lost;     ///< Visible at the previous update, not now (last known info).
	};

	/// Takes the complete set of currently visible objects.
	Changes update(const std::map<uint32_t, VisibleInfo> &visibleNow);

	/// Drops an object (e.g. seen dying), so it is not later reported as lost.
	void forget(uint32_t id) { m_visible.erase(id); }

	bool isVisible(uint32_t id) const { return m_visible.count(id) != 0; }
	void clear() { m_visible.clear(); }

private:
	std::map<uint32_t, VisibleInfo> m_visible;
};

//-------------------------------------------------------------------------------------------------
class DamageAggregator
{
public:
	struct Entry
	{
		uint32_t id = 0;
		float damage = 0.0f;
		uint32_t attacker = 0;      ///< Last attacker the player could see; 0 if none.
		unsigned int firstFrame = 0;
	};

	/// attacker 0 means "unknown or not visible" and never replaces a known attacker.
	void add(uint32_t id, float amount, uint32_t attacker, unsigned int frame);

	/// Entries whose window (frames since their first hit) has passed. They are removed.
	std::vector<Entry> due(unsigned int frame, unsigned int window);

	void forget(uint32_t id) { m_entries.erase(id); }
	void clear() { m_entries.clear(); }
	size_t size() const { return m_entries.size(); }

private:
	std::map<uint32_t, Entry> m_entries;
};

} // namespace scriptapi
