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

// Localhost WebSocket server for the script API.
//
// Network I/O happens on background threads. The owner (the game thread) exchanges messages
// through thread-safe queues: pollInbound() to receive and send() to transmit, neither of which
// blocks on the network. One controller connection is active at a time (a new controller supersedes
// the previous one), alongside up to kMaxWatchers watcher connections (URL path /watch). This header deliberately exposes no networking types so it can be included
// alongside the game's windows.h.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ScriptApiProtocol.h"

namespace scriptapi
{

constexpr int kCloseGoingAway = 1001;
constexpr int kCloseSuperseded = 4001;
constexpr int kCloseSlowConsumer = 4002;
constexpr int kCloseTooManyWatchers = 4003;
constexpr size_t kMaxWatchers = 3; ///< e.g. an agent's watch and order, plus a practice runner

/// The role a connection's URL asks for: the path "/watch" is a watcher, anything else the controller.
ConnectionRole roleForPath(const std::string &uri);

struct InboundMessage
{
	enum class Kind
	{
		Opened,
		Text,
		Closed,
	};

	Kind kind = Kind::Text;
	uint32_t connectionId = 0;
	ConnectionRole role = ConnectionRole::Controller;
	std::string text;
};

struct ServerOptions
{
	std::string host = "127.0.0.1";
	int port = 27100;
	size_t maxOutboundQueue = 10000;              ///< Queued messages before the client is dropped as a slow consumer.
	size_t maxBufferedBytes = 64 * 1024 * 1024;   ///< Unsent socket bytes before the client is dropped as a slow consumer.
};

class Server
{
public:
	Server();
	~Server();

	Server(const Server &) = delete;
	Server &operator=(const Server &) = delete;

	bool start(const ServerOptions &options, std::string *error = nullptr);

	/// Sends what is queued (up to flushTimeout), closes the client and joins all threads.
	void stop(std::chrono::milliseconds flushTimeout = std::chrono::milliseconds(1000));

	bool isRunning() const;
	int port() const;

	bool pollInbound(InboundMessage &out);

	/// Queues text for the given connection. Dropped if that connection has closed.
	void send(uint32_t connectionId, std::string text);

	/// Queues text for every live connection.
	void broadcast(const std::string &text);

	/// Id of the controller connection, or 0 when none is connected.
	uint32_t activeConnection() const;

	/// Ids of every live connection, controller and watchers.
	std::vector<uint32_t> connections() const;

	ConnectionRole roleOf(uint32_t connectionId) const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace scriptapi
