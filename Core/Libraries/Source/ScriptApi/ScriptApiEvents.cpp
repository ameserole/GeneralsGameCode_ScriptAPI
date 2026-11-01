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

#include "ScriptApiEvents.h"

namespace scriptapi
{

VisibilityTracker::Changes VisibilityTracker::update(const std::map<uint32_t, VisibleInfo> &visibleNow)
{
	Changes changes;
	for (const auto &entry : visibleNow)
	{
		if (m_visible.count(entry.first) == 0)
			changes.spotted.push_back(entry.second);
	}
	for (const auto &entry : m_visible)
	{
		if (visibleNow.count(entry.first) == 0)
			changes.lost.push_back(entry.second);
	}
	m_visible = visibleNow;
	return changes;
}

//-------------------------------------------------------------------------------------------------
void DamageAggregator::add(uint32_t id, float amount, uint32_t attacker, unsigned int frame)
{
	auto it = m_entries.find(id);
	if (it == m_entries.end())
	{
		Entry entry;
		entry.id = id;
		entry.firstFrame = frame;
		it = m_entries.emplace(id, entry).first;
	}
	it->second.damage += amount;
	if (attacker != 0)
		it->second.attacker = attacker;
}

std::vector<DamageAggregator::Entry> DamageAggregator::due(unsigned int frame, unsigned int window)
{
	std::vector<Entry> result;
	for (auto it = m_entries.begin(); it != m_entries.end();)
	{
		if (frame >= it->second.firstFrame + window)
		{
			result.push_back(it->second);
			it = m_entries.erase(it);
		}
		else
		{
			++it;
		}
	}
	return result;
}

} // namespace scriptapi
