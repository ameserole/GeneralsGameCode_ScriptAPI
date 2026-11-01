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

// ScriptApi.h ////////////////////////////////////////////////////////////////////////////////////
// Script API subsystem: lets an external script play this client's player over a localhost
// WebSocket. Requests are processed on the game thread once per frame, before the message stream
// is propagated, so anything a script does enters the game exactly like local player input.
// Never touches the simulation outside of that path.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#if RTS_SCRIPT_API

#include "Common/SubsystemInterface.h"

#include "ScriptApiCommands.h"
#include "ScriptApiEvents.h"
#include "ScriptApiGameState.h"
#include "ScriptApiLan.h"
#include "ScriptApiRouter.h"
#include "ScriptApiServer.h"
#include "ScriptApiWorld.h"

#include <optional>

class GameMessage;
class Object;
class Player;
class UpgradeTemplate;
class DamageInfo;

class ScriptApi : public SubsystemInterface
{
public:
	ScriptApi();
	virtual ~ScriptApi() override;

	virtual void init() override;
	virtual void reset() override;
	virtual void update() override;

	/// Queues an event for the connected script, if any.
	/// Sends an event to every connection (controller and watchers).
	void sendEvent(const std::string &name, scriptapi::Json fields = scriptapi::Json::object());
	/// Sends an event to one connection only, e.g. the one that sent the order it is about.
	void sendEventTo(uint32_t connection, const std::string &name, scriptapi::Json fields = scriptapi::Json::object());

	/// Called by GameLogic when a map starts loading. Loading blocks the game thread, so the state
	/// change is reported from here rather than from the next update().
	void onLoadingStarted();

	// Game event hooks. They only read engine state, and only report what concerns the local player.
	void onUnitCreated(const Player *player, const Object *factory, const Object *unit);
	void onStructureCreated(const Player *player, const Object *builder, const Object *structure);
	void onStructureConstructionComplete(const Player *player, const Object *builder, const Object *structure, Bool isRebuild);
	void onUpgradeCompleted(const Player *player, const UpgradeTemplate *upgrade);
	void onScienceAdded(const Player *player, Int science);
	void onObjectDied(const Object *object, const DamageInfo *damageInfo);
	void onEvaMessage(const char *message);
	void onCommandExecuted(Int messageType, const Player *player);
	void onObjectDamaged(const Object *object, const DamageInfo *damageInfo);
	void onObjectRemoved(const Object *object);

	// Input lock (ScriptApiInput.cpp), used by the message stream guards.
	scriptapi::InputMode inputMode() const { return m_inputMode; }
	Bool isApiMessage(const GameMessage *msg) const;
	void countInput(const char *outcome, const char *kind);

private:
	void registerCommands();
	void registerSkirmishCommands(); ///< ScriptApiSkirmish.cpp
	void processInbound();
	void trackGameState();
	scriptapi::GameState currentGameState() const;
	UnsignedInt currentFrame() const;

	scriptapi::Json skirmishOptions() const;
	scriptapi::Json startSkirmish(const scriptapi::Request &request);
	void quitGame();
	scriptapi::Json saveReplay(const scriptapi::Request &request);

	// LAN games (ScriptApiLanLobby.cpp)
	void registerLanCommands();
	void updateLan();
	scriptapi::Json lanOpen(const scriptapi::Request &request);
	scriptapi::Json lanHost(const scriptapi::Request &request);
	scriptapi::Json lanJoin(const scriptapi::Request &request);
	scriptapi::Json lanSlot(const scriptapi::Request &request);
	scriptapi::Json lanStart();
	scriptapi::Json lanLeave();
	void applyLanHostSetup(const scriptapi::LanHostRequest &setup);
	scriptapi::Json lanLobbyJson() const;
	scriptapi::Json lanGameJson() const;
	void surrender();

	// Orders (ScriptApiActions.cpp, ScriptApiAbilities.cpp)
	void registerActionCommands();
	void registerAbilityCommands();
	/// Registers an order command: it is refused with RATE_LIMITED before it adds any message when
	/// the actions-per-minute cap is used up, and counts once orderReplyFor accepts it.
	void addOrder(const std::string &cmd, scriptapi::CommandRouter::Handler handler);
	/// The local player's general's points, minus those of purchases still on their way.
	Int availableSciencePoints() const;
	/// Refuses with INSUFFICIENT_FUNDS unless the money not promised to spending orders still on
	/// their way covers the cost.
	void requireUncommittedMoney(Int cost) const;
	/// Sends build_failed for builds that executed without creating a structure.
	void reportFailedBuilds();
	Object *requireOwned(uint32_t id) const;
	std::vector<Object *> requireOwnedUnits(const scriptapi::Request &request) const;
	Object *requireVisibleTarget(const scriptapi::Json &body, const char *key) const;
	scriptapi::Json orderReplyFor(const scriptapi::Request &request, const std::vector<int> &orderTypes,
		scriptapi::Json extra = scriptapi::Json::object());
	Bool isVisibleToLocalPlayer(const Object *obj) const; ///< ScriptApiWorldView.cpp

	// Input lock (ScriptApiInput.cpp)
	void registerInputCommands();
	void setInputMode(scriptapi::InputMode mode);
	void attachInputGuards();
	void detachInputGuards();
	GameMessage *lastStreamMessage() const;
	void markApiMessagesAfter(GameMessage *last);

	// World view (ScriptApiWorldView.cpp)
	void registerWorldCommands();
	void resetWorldView();
	void updateWorldView();
	void scanVisibility();
	void flushDamage();
	Bool reportsFor(const Player *player) const; ///< In a game and player is the local player.
	scriptapi::Json gameInfo() const;
	scriptapi::Json catalog() const;
	scriptapi::Json factionCatalog(const scriptapi::Request &request) const;
	scriptapi::Json playerStats() const;
	scriptapi::Json templateInfo(const scriptapi::Request &request) const;
	scriptapi::Json mapGrid(const scriptapi::Request &request) const;
	scriptapi::Json snapshot();
	scriptapi::Json debugWorld(Bool all) const;
	void requireInGame() const;

	scriptapi::Server m_server;
	scriptapi::CommandRouter m_router;
	scriptapi::GameStateTracker m_stateTracker;
	Bool m_exitRequested;
	Bool m_transitionPending; ///< A start or quit was issued and the game state has not changed yet.
	Bool m_playedGame = FALSE; ///< A game has been played since startup, so there is a last replay to save.

	std::map<uint32_t, scriptapi::SnapshotTimer> m_snapshotTimers; ///< Per connection: each picks its own interval
	scriptapi::KnownStructureMemory m_knownStructures;
	scriptapi::Json m_knownJson;                 ///< Cached "known_structures" array
	uint64_t m_knownJsonVersion = ~0ull;         ///< m_knownStructures version m_knownJson was built from
	std::set<uint32_t> m_knownJsonVisibleIds;    ///< visible ids m_knownJson was built for
	scriptapi::ChangeDetector<UnsignedInt> m_money;
	scriptapi::ChangeDetector<std::pair<Int, Int>> m_power;
	scriptapi::ChangeDetector<std::pair<Int, Int>> m_rank;
	Bool m_gameOverSent;
	std::string m_gameResult; ///< victory, defeat or ended once game_over was sent; hello reports it

	scriptapi::CommandTracker m_commandTracker;
	scriptapi::ActionRateLimiter m_actionLimiter;
	std::deque<Int> m_pendingSciences; ///< Sciences purchased whose MSG_PURCHASE_SCIENCE has not executed yet
	scriptapi::PendingSpend m_pendingSpend; ///< Costs of builds, units and upgrades sent but not executed yet
	struct PendingBuild
	{
		scriptapi::Json requestId;
		uint32_t dozer = 0;
		std::string templateName;
		Coord3D pos;
		uint32_t connection = 0; ///< build_failed goes to this connection only
	};
	std::deque<PendingBuild> m_pendingBuilds;   ///< Builds whose MSG_DOZER_CONSTRUCT has not executed yet
	std::vector<PendingBuild> m_executedBuilds; ///< Executed since the last update() without a structure so far
	scriptapi::VisibilityTracker m_enemyTracker;
	scriptapi::DamageAggregator m_damage;
	std::set<uint32_t> m_diedIds; ///< Own objects reported by unit_destroyed, so destroyObject does not report them again

	enum class LanStep { None, OpeningLobby, Hosting, Joining };
	LanStep m_lanStep = LanStep::None;          ///< A lobby screen change the script asked for and is waiting on
	UnsignedInt m_lanStepStarted = 0;           ///< timeGetTime() when m_lanStep began
	std::string m_lanName;                      ///< Player name for lan_open
	std::optional<scriptapi::LanHostRequest> m_lanHostSetup; ///< Applied once the game options screen is up
	scriptapi::JsonChangeDetector m_lanLobbyChanges;
	scriptapi::JsonChangeDetector m_lanGameChanges;
	Bool m_lanInGameSetup = FALSE;              ///< Last poll saw us in a game setup that had not started
	Bool m_lanLeaving = FALSE;                  ///< lan_leave was requested

	scriptapi::InputMode m_inputMode = scriptapi::InputMode::View;
	UnsignedInt m_inputGuardId = 0;
	UnsignedInt m_commandGuardId = 0;
	std::set<const GameMessage *> m_apiMessages;  ///< Messages this frame's update() added to the stream
	std::map<std::string, std::map<std::string, UnsignedInt>> m_inputStats; ///< outcome -> kind -> count
};

extern ScriptApi *TheScriptApi;

#endif // RTS_SCRIPT_API
