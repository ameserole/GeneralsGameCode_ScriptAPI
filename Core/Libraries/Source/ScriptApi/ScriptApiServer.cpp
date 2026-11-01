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

#include "ScriptApiServer.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace scriptapi
{

namespace
{

// IXWebSocket listens with SO_REUSEADDR, which on Windows lets a second server bind a port that is
// already in use. Probe with a plain bind first so an occupied port is reported instead of shared.
bool isPortFree(const std::string &host, int port)
{
	SOCKET probe = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (probe == INVALID_SOCKET)
		return false;

	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_port = htons(static_cast<u_short>(port));
	inet_pton(AF_INET, host.c_str(), &address.sin_addr);

	const bool free = ::bind(probe, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
	::closesocket(probe);
	return free;
}

} // namespace

struct Server::Impl
{
	struct Outbound
	{
		uint32_t connectionId;
		std::string text;
	};

	struct Connection
	{
		std::shared_ptr<ix::WebSocket> socket;
		ConnectionRole role = ConnectionRole::Controller;
	};

	ServerOptions options;
	std::unique_ptr<ix::WebSocketServer> server;
	bool running = false;

	// Connection bookkeeping, written by IX connection threads.
	mutable std::mutex connectionMutex;
	std::map<std::string, uint32_t> connectionIds; ///< IX connection state id -> our id
	std::map<uint32_t, Connection> connections;    ///< Live connections by our id
	uint32_t nextConnectionId = 1;
	uint32_t controllerId = 0;

	std::mutex inboundMutex;
	std::deque<InboundMessage> inbound;

	std::mutex outboundMutex;
	std::condition_variable outboundReady;
	std::deque<Outbound> outbound;
	bool senderBusy = false;
	bool stopSender = false;
	std::thread sender;

	void pushInbound(InboundMessage::Kind kind, uint32_t connectionId, ConnectionRole role, std::string text = std::string())
	{
		InboundMessage message;
		message.kind = kind;
		message.connectionId = connectionId;
		message.role = role;
		message.text = std::move(text);
		std::lock_guard<std::mutex> lock(inboundMutex);
		inbound.push_back(std::move(message));
	}

	void onClientMessage(const std::shared_ptr<ix::ConnectionState> &state, ix::WebSocket &socket, const ix::WebSocketMessagePtr &msg)
	{
		switch (msg->type)
		{
		case ix::WebSocketMessageType::Open: onOpen(state, socket, msg->openInfo.uri); break;
		case ix::WebSocketMessageType::Close: onClose(state); break;
		case ix::WebSocketMessageType::Message:
		{
			uint32_t id = 0;
			ConnectionRole role = ConnectionRole::Controller;
			{
				std::lock_guard<std::mutex> lock(connectionMutex);
				const auto it = connectionIds.find(state->getId());
				if (it != connectionIds.end())
				{
					const auto live = connections.find(it->second);
					if (live != connections.end())
					{
						id = it->second;
						role = live->second.role;
					}
				}
			}
			if (id != 0)
				pushInbound(InboundMessage::Kind::Text, id, role, msg->str);
			break;
		}
		default: break;
		}
	}

	void onOpen(const std::shared_ptr<ix::ConnectionState> &state, ix::WebSocket &socket, const std::string &uri)
	{
		// Hold a shared_ptr so the sender thread can keep using the socket after IX drops it.
		std::shared_ptr<ix::WebSocket> shared;
		for (const std::shared_ptr<ix::WebSocket> &client : server->getClients())
		{
			if (client.get() == &socket)
			{
				shared = client;
				break;
			}
		}
		if (!shared)
			return;

		const ConnectionRole role = roleForPath(uri);
		std::shared_ptr<ix::WebSocket> superseded;
		uint32_t supersededId = 0;
		uint32_t id = 0;
		bool refused = false;
		{
			std::lock_guard<std::mutex> lock(connectionMutex);
			if (role == ConnectionRole::Watcher)
			{
				size_t watchers = 0;
				for (const auto &connection : connections)
					watchers += connection.second.role == ConnectionRole::Watcher ? 1 : 0;
				refused = watchers >= kMaxWatchers;
			}
			if (!refused)
				id = nextConnectionId++;
		}
		if (refused)
		{
			// Never registered, so no Opened or Closed reaches the game. Closed outside the lock:
			// closing can call back into onClose.
			shared->close(kCloseTooManyWatchers, "too many watchers");
			return;
		}
		{
			std::lock_guard<std::mutex> lock(connectionMutex);
			connectionIds[state->getId()] = id;
			connections[id] = Connection{shared, role};
			if (role == ConnectionRole::Controller)
			{
				// A new controller replaces the old one; watchers stay.
				const auto old = connections.find(controllerId);
				if (old != connections.end())
				{
					supersededId = old->first;
					superseded = std::move(old->second.socket);
					connections.erase(old);
				}
				controllerId = id;
			}
		}
		if (supersededId != 0)
			dropQueued(supersededId); // anything still queued was meant for the previous controller
		pushInbound(InboundMessage::Kind::Opened, id, role);

		if (superseded)
			superseded->close(kCloseSuperseded, "superseded");
	}

	void onClose(const std::shared_ptr<ix::ConnectionState> &state)
	{
		uint32_t id = 0;
		ConnectionRole role = ConnectionRole::Controller;
		{
			std::lock_guard<std::mutex> lock(connectionMutex);
			const auto it = connectionIds.find(state->getId());
			if (it == connectionIds.end())
				return;
			id = it->second;
			connectionIds.erase(it);
			const auto live = connections.find(id);
			if (live != connections.end())
			{
				role = live->second.role;
				connections.erase(live);
			}
			if (controllerId == id)
				controllerId = 0;
		}
		pushInbound(InboundMessage::Kind::Closed, id, role);
	}

	std::shared_ptr<ix::WebSocket> socketFor(uint32_t connectionId) const
	{
		std::lock_guard<std::mutex> lock(connectionMutex);
		const auto it = connections.find(connectionId);
		return it != connections.end() ? it->second.socket : nullptr;
	}

	void dropQueued(uint32_t connectionId)
	{
		std::lock_guard<std::mutex> lock(outboundMutex);
		for (auto it = outbound.begin(); it != outbound.end();)
			it = it->connectionId == connectionId ? outbound.erase(it) : it + 1;
	}

	void dropSlowConsumer(uint32_t connectionId)
	{
		std::shared_ptr<ix::WebSocket> socket;
		{
			std::lock_guard<std::mutex> lock(connectionMutex);
			const auto it = connections.find(connectionId);
			if (it == connections.end())
				return;
			socket = std::move(it->second.socket);
			connections.erase(it);
			if (controllerId == connectionId)
				controllerId = 0;
		}
		dropQueued(connectionId);
		if (socket)
			socket->close(kCloseSlowConsumer, "slow consumer");
	}

	void senderLoop()
	{
		std::unique_lock<std::mutex> lock(outboundMutex);
		for (;;)
		{
			outboundReady.wait(lock, [this] { return stopSender || !outbound.empty(); });
			if (outbound.empty())
				return; // stopSender with nothing left to send

			Outbound next = std::move(outbound.front());
			outbound.pop_front();
			senderBusy = true;
			lock.unlock();

			if (std::shared_ptr<ix::WebSocket> socket = socketFor(next.connectionId))
			{
				socket->sendText(next.text);
				if (socket->bufferedAmount() > options.maxBufferedBytes)
					dropSlowConsumer(next.connectionId);
			}

			lock.lock();
			senderBusy = false;
		}
	}

	bool waitUntil(std::chrono::steady_clock::time_point deadline, const std::function<bool()> &done)
	{
		while (!done())
		{
			if (std::chrono::steady_clock::now() >= deadline)
				return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		return true;
	}
};

Server::Server() : m_impl(std::make_unique<Impl>())
{
}

Server::~Server()
{
	stop();
}

bool Server::start(const ServerOptions &options, std::string *error)
{
	Impl &impl = *m_impl;
	if (impl.running)
	{
		if (error)
			*error = "server is already running";
		return false;
	}

	ix::initNetSystem();

	if (!isPortFree(options.host, options.port))
	{
		if (error)
			*error = "port " + std::to_string(options.port) + " is already in use";
		ix::uninitNetSystem();
		return false;
	}

	impl.options = options;
	impl.server = std::make_unique<ix::WebSocketServer>(options.port, options.host);
	impl.server->disablePerMessageDeflate();
	impl.server->setOnClientMessageCallback(
		[&impl](std::shared_ptr<ix::ConnectionState> state, ix::WebSocket &socket, const ix::WebSocketMessagePtr &msg) {
			impl.onClientMessage(state, socket, msg);
		});

	const std::pair<bool, std::string> listening = impl.server->listen();
	if (!listening.first)
	{
		if (error)
			*error = listening.second;
		impl.server.reset();
		ix::uninitNetSystem();
		return false;
	}

	impl.stopSender = false;
	impl.sender = std::thread([&impl] { impl.senderLoop(); });
	impl.server->start();
	impl.running = true;
	return true;
}

void Server::stop(std::chrono::milliseconds flushTimeout)
{
	Impl &impl = *m_impl;
	if (!impl.running)
		return;

	const auto deadline = std::chrono::steady_clock::now() + flushTimeout;

	// Let the sender drain what the game already queued, then stop it.
	impl.waitUntil(deadline, [&impl] {
		std::lock_guard<std::mutex> lock(impl.outboundMutex);
		return impl.outbound.empty() && !impl.senderBusy;
	});
	{
		std::lock_guard<std::mutex> lock(impl.outboundMutex);
		impl.stopSender = true;
		impl.outbound.clear();
	}
	impl.outboundReady.notify_all();
	impl.sender.join();

	std::vector<std::shared_ptr<ix::WebSocket>> sockets;
	{
		std::lock_guard<std::mutex> lock(impl.connectionMutex);
		for (auto &connection : impl.connections)
			sockets.push_back(std::move(connection.second.socket));
		impl.connections.clear();
		impl.controllerId = 0;
	}
	for (std::shared_ptr<ix::WebSocket> &socket : sockets)
	{
		if (!socket)
			continue;
		impl.waitUntil(deadline, [&socket] { return socket->bufferedAmount() == 0; });
		socket->close(kCloseGoingAway, "server shutting down");
	}
	for (std::shared_ptr<ix::WebSocket> &socket : sockets)
	{
		if (socket)
			impl.waitUntil(deadline, [&socket] { return socket->getReadyState() == ix::ReadyState::Closed; });
	}
	sockets.clear();

	impl.server->stop();
	impl.server.reset();

	{
		std::lock_guard<std::mutex> lock(impl.connectionMutex);
		impl.connectionIds.clear();
	}
	{
		std::lock_guard<std::mutex> lock(impl.inboundMutex);
		impl.inbound.clear();
	}

	impl.running = false;
	ix::uninitNetSystem();
}

bool Server::isRunning() const
{
	return m_impl->running;
}

int Server::port() const
{
	return m_impl->running ? m_impl->options.port : 0;
}

bool Server::pollInbound(InboundMessage &out)
{
	std::lock_guard<std::mutex> lock(m_impl->inboundMutex);
	if (m_impl->inbound.empty())
		return false;
	out = std::move(m_impl->inbound.front());
	m_impl->inbound.pop_front();
	return true;
}

void Server::send(uint32_t connectionId, std::string text)
{
	Impl &impl = *m_impl;
	if (!impl.running || connectionId == 0)
		return;

	bool tooSlow = false;
	{
		std::lock_guard<std::mutex> lock(impl.outboundMutex);
		if (impl.outbound.size() > impl.options.maxOutboundQueue)
			tooSlow = true;
		else
			impl.outbound.push_back(Impl::Outbound{connectionId, std::move(text)});
	}
	if (tooSlow)
		impl.dropSlowConsumer(connectionId);
	else
		impl.outboundReady.notify_one();
}

void Server::broadcast(const std::string &text)
{
	for (uint32_t connectionId : connections())
		send(connectionId, text);
}

uint32_t Server::activeConnection() const
{
	std::lock_guard<std::mutex> lock(m_impl->connectionMutex);
	return m_impl->controllerId;
}

std::vector<uint32_t> Server::connections() const
{
	std::lock_guard<std::mutex> lock(m_impl->connectionMutex);
	std::vector<uint32_t> ids;
	for (const auto &connection : m_impl->connections)
		ids.push_back(connection.first);
	return ids;
}

ConnectionRole Server::roleOf(uint32_t connectionId) const
{
	std::lock_guard<std::mutex> lock(m_impl->connectionMutex);
	const auto it = m_impl->connections.find(connectionId);
	return it != m_impl->connections.end() ? it->second.role : ConnectionRole::Controller;
}

//-------------------------------------------------------------------------------------------------
ConnectionRole roleForPath(const std::string &uri)
{
	// The path may come alone ("/watch?x") or inside a full URL ("ws://host:port/watch").
	std::string path = uri;
	const size_t scheme = path.find("://");
	if (scheme != std::string::npos)
	{
		const size_t slash = path.find('/', scheme + 3);
		path = slash == std::string::npos ? std::string() : path.substr(slash);
	}
	path = path.substr(0, path.find_first_of("?#"));
	return path == "/watch" ? ConnectionRole::Watcher : ConnectionRole::Controller;
}

} // namespace scriptapi
