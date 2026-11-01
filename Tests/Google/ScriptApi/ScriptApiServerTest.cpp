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

// Loopback tests for scriptapi::Server using an IXWebSocket client.

#include <gtest/gtest.h>

#include "ScriptApiServer.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using scriptapi::InboundMessage;

namespace
{

// Each test uses its own port so a lingering socket from one test cannot affect the next.
int nextPort()
{
	static int port = 37150;
	return port++;
}

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout = 5000ms)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (predicate())
			return true;
		std::this_thread::sleep_for(5ms);
	}
	return predicate();
}

bool waitForInbound(scriptapi::Server &server, InboundMessage &out, std::chrono::milliseconds timeout = 5000ms)
{
	return waitUntil([&] { return server.pollInbound(out); }, timeout);
}

class TestClient
{
public:
	explicit TestClient(int port, const std::string &path = "/")
	{
		m_socket.setUrl("ws://127.0.0.1:" + std::to_string(port) + path);
		m_socket.disableAutomaticReconnection();
		m_socket.disablePerMessageDeflate();
		m_socket.setOnMessageCallback([this](const ix::WebSocketMessagePtr &msg) {
			std::lock_guard<std::mutex> lock(m_mutex);
			switch (msg->type)
			{
			case ix::WebSocketMessageType::Open: m_open = true; break;
			case ix::WebSocketMessageType::Close:
				m_open = false;
				m_closed = true;
				m_closeCode = msg->closeInfo.code;
				break;
			case ix::WebSocketMessageType::Message: m_messages.push_back(msg->str); break;
			default: break;
			}
		});
	}

	~TestClient() { m_socket.stop(); }

	bool connect()
	{
		m_socket.start();
		return waitUntil([this] { std::lock_guard<std::mutex> lock(m_mutex); return m_open; });
	}

	void send(const std::string &text) { m_socket.sendText(text); }
	void close() { m_socket.close(); }

	bool waitForMessage(std::string &out)
	{
		return waitUntil([&] {
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_messages.empty())
				return false;
			out = m_messages.front();
			m_messages.pop_front();
			return true;
		});
	}

	bool waitForClose(int &code)
	{
		const bool closed = waitUntil([this] { std::lock_guard<std::mutex> lock(m_mutex); return m_closed; });
		std::lock_guard<std::mutex> lock(m_mutex);
		code = m_closeCode;
		return closed;
	}

	size_t messageCount()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_messages.size();
	}

private:
	ix::WebSocket m_socket;
	std::mutex m_mutex;
	bool m_open = false;
	bool m_closed = false;
	int m_closeCode = 0;
	std::deque<std::string> m_messages;
};

class ScriptApiServerTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		ix::initNetSystem();
		m_options.port = nextPort();
		std::string error;
		ASSERT_TRUE(m_server.start(m_options, &error)) << error;
	}

	void TearDown() override
	{
		m_server.stop();
		ix::uninitNetSystem();
	}

	// Connects a client and consumes the server-side Opened message.
	uint32_t connectClient(TestClient &client, scriptapi::ConnectionRole role = scriptapi::ConnectionRole::Controller)
	{
		EXPECT_TRUE(client.connect());
		InboundMessage msg;
		EXPECT_TRUE(waitForInbound(m_server, msg));
		EXPECT_EQ(msg.kind, InboundMessage::Kind::Opened);
		EXPECT_EQ(msg.role, role);
		return msg.connectionId;
	}

	scriptapi::ServerOptions m_options;
	scriptapi::Server m_server;
};

} // namespace

TEST_F(ScriptApiServerTest, ReportsOpenedWithActiveConnection)
{
	TestClient client(m_options.port);
	const uint32_t id = connectClient(client);
	EXPECT_NE(id, 0u);
	EXPECT_EQ(m_server.activeConnection(), id);
	EXPECT_TRUE(m_server.isRunning());
	EXPECT_EQ(m_server.port(), m_options.port);
}

TEST_F(ScriptApiServerTest, TextRoundTrip)
{
	TestClient client(m_options.port);
	const uint32_t id = connectClient(client);

	client.send("hello server");
	InboundMessage msg;
	ASSERT_TRUE(waitForInbound(m_server, msg));
	EXPECT_EQ(msg.kind, InboundMessage::Kind::Text);
	EXPECT_EQ(msg.connectionId, id);
	EXPECT_EQ(msg.text, "hello server");

	m_server.send(id, "hello client");
	std::string received;
	ASSERT_TRUE(client.waitForMessage(received));
	EXPECT_EQ(received, "hello client");
}

TEST_F(ScriptApiServerTest, PreservesMessageOrder)
{
	TestClient client(m_options.port);
	const uint32_t id = connectClient(client);

	for (int i = 0; i < 100; ++i)
		m_server.send(id, std::to_string(i));

	for (int i = 0; i < 100; ++i)
	{
		std::string received;
		ASSERT_TRUE(client.waitForMessage(received));
		EXPECT_EQ(received, std::to_string(i));
	}
}

TEST_F(ScriptApiServerTest, ReportsClosed)
{
	TestClient client(m_options.port);
	const uint32_t id = connectClient(client);

	client.close();
	InboundMessage msg;
	ASSERT_TRUE(waitForInbound(m_server, msg));
	EXPECT_EQ(msg.kind, InboundMessage::Kind::Closed);
	EXPECT_EQ(msg.connectionId, id);
	EXPECT_TRUE(waitUntil([&] { return m_server.activeConnection() == 0; }));
}

TEST_F(ScriptApiServerTest, NewClientSupersedesOld)
{
	TestClient first(m_options.port);
	const uint32_t firstId = connectClient(first);

	TestClient second(m_options.port);
	ASSERT_TRUE(second.connect());

	int code = 0;
	ASSERT_TRUE(first.waitForClose(code));
	EXPECT_EQ(code, scriptapi::kCloseSuperseded);

	// Expect Opened for the new connection and Closed for the old one, in either order.
	uint32_t secondId = 0;
	bool sawClosed = false;
	for (int i = 0; i < 2; ++i)
	{
		InboundMessage msg;
		ASSERT_TRUE(waitForInbound(m_server, msg));
		if (msg.kind == InboundMessage::Kind::Opened)
			secondId = msg.connectionId;
		else if (msg.kind == InboundMessage::Kind::Closed && msg.connectionId == firstId)
			sawClosed = true;
	}
	EXPECT_TRUE(sawClosed);
	ASSERT_NE(secondId, 0u);
	EXPECT_NE(secondId, firstId);
	EXPECT_EQ(m_server.activeConnection(), secondId);

	// Messages for the superseded connection are dropped; the new one still works.
	m_server.send(firstId, "stale");
	m_server.send(secondId, "fresh");
	std::string received;
	ASSERT_TRUE(second.waitForMessage(received));
	EXPECT_EQ(received, "fresh");
}

TEST_F(ScriptApiServerTest, StopFlushesPendingMessages)
{
	TestClient client(m_options.port);
	const uint32_t id = connectClient(client);

	for (int i = 0; i < 50; ++i)
		m_server.send(id, "bye" + std::to_string(i));
	m_server.stop();

	EXPECT_FALSE(m_server.isRunning());
	EXPECT_TRUE(waitUntil([&] { return client.messageCount() == 50; }));
	int code = 0;
	ASSERT_TRUE(client.waitForClose(code));
	EXPECT_EQ(code, scriptapi::kCloseGoingAway);
}

TEST_F(ScriptApiServerTest, SlowConsumerIsDropped)
{
	m_server.stop();
	// Any backlog at all counts as too slow.
	m_options.maxOutboundQueue = 0;
	m_options.maxBufferedBytes = 0;
	m_options.port = nextPort();
	ASSERT_TRUE(m_server.start(m_options));

	TestClient client(m_options.port);
	const uint32_t id = connectClient(client);

	// Queue far more than can be sent instantly; the server must disconnect rather than buffer forever.
	const std::string big(64 * 1024, 'x');
	for (int i = 0; i < 2000 && m_server.activeConnection() == id; ++i)
		m_server.send(id, big);

	int code = 0;
	ASSERT_TRUE(client.waitForClose(code));
	EXPECT_EQ(code, scriptapi::kCloseSlowConsumer);
}

TEST_F(ScriptApiServerTest, PortInUseFailsToStart)
{
	scriptapi::Server other;
	std::string error;
	EXPECT_FALSE(other.start(m_options, &error));
	EXPECT_FALSE(error.empty());
	EXPECT_FALSE(other.isRunning());
}

TEST_F(ScriptApiServerTest, StopIsIdempotent)
{
	m_server.stop();
	m_server.stop();
	EXPECT_FALSE(m_server.isRunning());
}

// -- watchers --------------------------------------------------------------------------------------

TEST(ScriptApiServerRoles, RoleComesFromThePath)
{
	using scriptapi::ConnectionRole;
	EXPECT_EQ(scriptapi::roleForPath("/watch"), ConnectionRole::Watcher);
	EXPECT_EQ(scriptapi::roleForPath("/watch?name=x"), ConnectionRole::Watcher);
	EXPECT_EQ(scriptapi::roleForPath("/"), ConnectionRole::Controller);
	EXPECT_EQ(scriptapi::roleForPath(""), ConnectionRole::Controller);
	EXPECT_EQ(scriptapi::roleForPath("/watcher"), ConnectionRole::Controller);
	EXPECT_EQ(scriptapi::roleForPath("ws://127.0.0.1:5000/watch"), ConnectionRole::Watcher);
}

TEST_F(ScriptApiServerTest, WatcherDoesNotSupersedeTheController)
{
	TestClient controller(m_options.port);
	const uint32_t controllerId = connectClient(controller);
	TestClient watcher(m_options.port, "/watch");
	const uint32_t watcherId = connectClient(watcher, scriptapi::ConnectionRole::Watcher);

	EXPECT_EQ(m_server.activeConnection(), controllerId);
	EXPECT_EQ(m_server.roleOf(watcherId), scriptapi::ConnectionRole::Watcher);

	watcher.send("from watcher");
	InboundMessage msg;
	ASSERT_TRUE(waitForInbound(m_server, msg));
	EXPECT_EQ(msg.connectionId, watcherId);
	EXPECT_EQ(msg.role, scriptapi::ConnectionRole::Watcher);

	m_server.send(controllerId, "to controller");
	m_server.send(watcherId, "to watcher");
	std::string received;
	ASSERT_TRUE(controller.waitForMessage(received));
	EXPECT_EQ(received, "to controller");
	ASSERT_TRUE(watcher.waitForMessage(received));
	EXPECT_EQ(received, "to watcher");
}

TEST_F(ScriptApiServerTest, NewControllerKeepsTheWatchers)
{
	TestClient first(m_options.port);
	connectClient(first);
	TestClient watcher(m_options.port, "/watch");
	const uint32_t watcherId = connectClient(watcher, scriptapi::ConnectionRole::Watcher);

	TestClient second(m_options.port);
	ASSERT_TRUE(second.connect());
	int code = 0;
	ASSERT_TRUE(first.waitForClose(code));
	EXPECT_EQ(code, scriptapi::kCloseSuperseded);

	m_server.send(watcherId, "still here");
	std::string received;
	ASSERT_TRUE(watcher.waitForMessage(received));
	EXPECT_EQ(received, "still here");
}

TEST_F(ScriptApiServerTest, TooManyWatchersAreRefused)
{
	TestClient one(m_options.port, "/watch");
	connectClient(one, scriptapi::ConnectionRole::Watcher);
	TestClient two(m_options.port, "/watch");
	connectClient(two, scriptapi::ConnectionRole::Watcher);
	TestClient three(m_options.port, "/watch");
	connectClient(three, scriptapi::ConnectionRole::Watcher);
	static_assert(scriptapi::kMaxWatchers == 3, "this test connects one watcher more than the limit");

	TestClient four(m_options.port, "/watch");
	four.connect();
	int code = 0;
	ASSERT_TRUE(four.waitForClose(code));
	EXPECT_EQ(code, scriptapi::kCloseTooManyWatchers);
}

TEST_F(ScriptApiServerTest, BroadcastReachesEveryConnection)
{
	TestClient controller(m_options.port);
	connectClient(controller);
	TestClient watcher(m_options.port, "/watch");
	connectClient(watcher, scriptapi::ConnectionRole::Watcher);

	m_server.broadcast("event");
	std::string received;
	ASSERT_TRUE(controller.waitForMessage(received));
	EXPECT_EQ(received, "event");
	ASSERT_TRUE(watcher.waitForMessage(received));
	EXPECT_EQ(received, "event");
	EXPECT_EQ(m_server.connections().size(), 2u);
}
