// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <locale>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "Common/SocketContext.h"
#include "DolphinQt/CheevoMap/CheevoMapV2LocalDashboardProtocol.h"
#include "DolphinQt/CheevoMap/CheevoMapV2LocalDashboardServer.h"

namespace
{
using CheevoMap::LocalDashboard::BuildHttpResponse;
using CheevoMap::LocalDashboard::BuildSseEvent;
using CheevoMap::LocalDashboard::BuildSseKeepalive;
using CheevoMap::LocalDashboard::DashboardSecurityHeaders;
using CheevoMap::LocalDashboard::IsHostAllowed;
using CheevoMap::LocalDashboard::LocalDashboardServer;
using CheevoMap::LocalDashboard::ParseHttpRequest;
using CheevoMap::LocalDashboard::ParseStatus;
using CheevoMap::LocalDashboard::Route;
using CheevoMap::V2::StateCursor;

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket INVALID_NATIVE_SOCKET = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket INVALID_NATIVE_SOCKET = -1;
#endif

std::string Request(std::string target, std::string host = "127.0.0.1:32123")
{
  return "GET " + target + " HTTP/1.1\r\nHost: " + host + "\r\n\r\n";
}

size_t CountSubstring(const std::string& text, const std::string& needle)
{
  size_t count = 0;
  size_t offset = 0;
  while ((offset = text.find(needle, offset)) != std::string::npos)
  {
    ++count;
    offset += needle.size();
  }
  return count;
}

void CloseSocket(NativeSocket socket)
{
#ifdef _WIN32
  closesocket(socket);
#else
  close(socket);
#endif
}

void SetReceiveTimeout(NativeSocket socket)
{
#ifdef _WIN32
  DWORD timeout_ms = 2000;
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms),
             sizeof(timeout_ms));
#else
  timeval timeout = {};
  timeout.tv_sec = 2;
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
}

NativeSocket ConnectToServer(const std::uint16_t port)
{
  NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == INVALID_NATIVE_SOCKET)
    return INVALID_NATIVE_SOCKET;

  SetReceiveTimeout(socket);

  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
  {
    CloseSocket(socket);
    return INVALID_NATIVE_SOCKET;
  }

  return socket;
}

bool SendAll(NativeSocket socket, std::string_view data)
{
  while (!data.empty())
  {
    const int sent = send(socket, data.data(), static_cast<int>(data.size()), 0);
    if (sent <= 0)
      return false;
    data.remove_prefix(static_cast<size_t>(sent));
  }
  return true;
}

std::string ReceiveUntilClosed(NativeSocket socket)
{
  std::string response;
  std::array<char, 4096> buffer;
  for (;;)
  {
    const int received = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
    if (received <= 0)
      return response;
    response.append(buffer.data(), static_cast<size_t>(received));
  }
}

std::string ReceiveUntilContains(NativeSocket socket, std::string_view needle)
{
  std::string response;
  std::array<char, 4096> buffer;
  while (response.find(needle) == std::string::npos)
  {
    const int received = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
    if (received <= 0)
      return response;
    response.append(buffer.data(), static_cast<size_t>(received));
  }
  return response;
}

std::string ReceiveFor(NativeSocket socket, std::chrono::milliseconds duration)
{
  const auto deadline = std::chrono::steady_clock::now() + duration;
  std::string response;
  std::array<char, 4096> buffer;
  while (std::chrono::steady_clock::now() < deadline)
  {
    const int received = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
    if (received <= 0)
      return response;
    response.append(buffer.data(), static_cast<size_t>(received));
  }
  return response;
}

std::string RequestFromServer(const std::uint16_t port, std::string_view request)
{
  Common::SocketContext socket_context;
  NativeSocket socket = ConnectToServer(port);
  EXPECT_NE(socket, INVALID_NATIVE_SOCKET);
  if (socket == INVALID_NATIVE_SOCKET)
    return {};

  EXPECT_TRUE(SendAll(socket, request));
  std::string response = ReceiveUntilClosed(socket);
  CloseSocket(socket);
  return response;
}

LocalDashboardServer::Assets TestAssets()
{
  return LocalDashboardServer::Assets{
      .html = "<!doctype html><title>CheevoMap</title>",
      .javascript = "'use strict';\n",
      .stylesheet = "body { color: #111; }\n",
  };
}

LocalDashboardServer::Options TestOptions()
{
  LocalDashboardServer::Options options;
  options.port = 0;
  options.keepalive_interval_ms = 100;
  options.select_timeout_ms = 10;
  return options;
}

LocalDashboardServer::SerializedSnapshot
TestSnapshot(std::uint64_t session_id, std::uint64_t sequence, std::string extra_json = "")
{
  if (!extra_json.empty() && extra_json.front() != ',')
    extra_json.insert(extra_json.begin(), ',');
  return {.cursor = StateCursor{session_id, sequence},
          .json = "{\"message_type\":\"snapshot\",\"session_id\":\"" + std::to_string(session_id) +
                  "\",\"sequence\":\"" + std::to_string(sequence) + "\"" + extra_json + "}"};
}

LocalDashboardServer::SerializedUpdate TestUpdate(std::uint64_t session_id, std::uint64_t sequence,
                                                  bool full = false, std::string extra_json = "")
{
  if (!extra_json.empty() && extra_json.front() != ',')
    extra_json.insert(extra_json.begin(), ',');
  return {.cursor = StateCursor{session_id, sequence},
          .json = "{\"message_type\":\"update\",\"session_id\":\"" + std::to_string(session_id) +
                  "\",\"sequence\":\"" + std::to_string(sequence) +
                  "\",\"full\":" + (full ? "true" : "false") + extra_json + "}"};
}

void ExpectSingleContentLengthResponse(const std::string& response)
{
  const size_t header_end = response.find("\r\n\r\n");
  ASSERT_NE(header_end, std::string::npos) << response;
  const std::string_view headers(response.data(), header_end);
  EXPECT_EQ(CountSubstring(std::string(headers), "Content-Length:"), 1u) << response;
  EXPECT_EQ(CountSubstring(std::string(headers), "Content-Type:"), 1u) << response;
  EXPECT_EQ(CountSubstring(std::string(headers), "Connection: close"), 1u) << response;

  const std::string_view marker = "Content-Length: ";
  const size_t length_start = headers.find(marker);
  ASSERT_NE(length_start, std::string_view::npos);
  const size_t number_start = length_start + marker.size();
  const size_t number_end = headers.find("\r\n", number_start);
  ASSERT_NE(number_end, std::string_view::npos);

  size_t expected_length = 0;
  const std::string_view number = headers.substr(number_start, number_end - number_start);
  EXPECT_FALSE(number.empty());
  for (const char c : number)
    EXPECT_GE(c, '0') << response;
  for (const char c : number)
    EXPECT_LE(c, '9') << response;
  const auto [ptr, error] =
      std::from_chars(number.data(), number.data() + number.size(), expected_length);
  ASSERT_EQ(error, std::errc{});
  ASSERT_EQ(ptr, number.data() + number.size());

  EXPECT_EQ(response.size() - header_end - 4, expected_length) << response;
}
}  // namespace

TEST(CheevoMapV2LocalDashboardProtocol, ParsesValidRoutes)
{
  auto parsed = ParseHttpRequest(Request("/"));
  ASSERT_EQ(parsed.status, ParseStatus::Complete);
  ASSERT_TRUE(parsed.request.route);
  EXPECT_EQ(*parsed.request.route, Route::Dashboard);

  parsed = ParseHttpRequest(Request("/api/v1/snapshot"));
  ASSERT_EQ(parsed.status, ParseStatus::Complete);
  ASSERT_TRUE(parsed.request.route);
  EXPECT_EQ(*parsed.request.route, Route::Snapshot);

  parsed = ParseHttpRequest(Request("/api/v1/events"));
  ASSERT_EQ(parsed.status, ParseStatus::Complete);
  ASSERT_TRUE(parsed.request.route);
  EXPECT_EQ(*parsed.request.route, Route::Events);
}

TEST(CheevoMapV2LocalDashboardProtocol, DistinguishesUnsupportedMethodAndUnknownRoute)
{
  auto parsed = ParseHttpRequest("POST / HTTP/1.1\r\nHost: 127.0.0.1:32123\r\n\r\n");
  ASSERT_EQ(parsed.status, ParseStatus::Complete);
  EXPECT_EQ(parsed.request.method, "POST");

  parsed = ParseHttpRequest(Request("/missing"));
  ASSERT_EQ(parsed.status, ParseStatus::Complete);
  EXPECT_FALSE(parsed.request.route);

  parsed = ParseHttpRequest(Request("/api/v1/snapshot?x=1"));
  ASSERT_EQ(parsed.status, ParseStatus::Complete);
  EXPECT_FALSE(parsed.request.route);
}

TEST(CheevoMapV2LocalDashboardProtocol, ValidatesHost)
{
  EXPECT_TRUE(IsHostAllowed(std::string("127.0.0.1"), "HTTP/1.1"));
  EXPECT_TRUE(IsHostAllowed(std::string("127.0.0.1:32123"), "HTTP/1.1"));
  EXPECT_TRUE(IsHostAllowed(std::string("localhost"), "HTTP/1.1"));
  EXPECT_TRUE(IsHostAllowed(std::string("localhost:32123"), "HTTP/1.1"));
  EXPECT_TRUE(IsHostAllowed(std::nullopt, "HTTP/1.0"));

  EXPECT_FALSE(IsHostAllowed(std::string("0.0.0.0:32123"), "HTTP/1.1"));
  EXPECT_FALSE(IsHostAllowed(std::string("192.168.0.2:32123"), "HTTP/1.1"));
  EXPECT_FALSE(IsHostAllowed(std::nullopt, "HTTP/1.1"));
}

TEST(CheevoMapV2LocalDashboardProtocol, RejectsOversizedAndMalformedHeaders)
{
  const std::string oversized(16 * 1024 + 1, 'x');
  EXPECT_EQ(ParseHttpRequest(oversized).status, ParseStatus::HeaderTooLarge);
  EXPECT_EQ(ParseHttpRequest("GET\r\n\r\n").status, ParseStatus::Malformed);
  EXPECT_EQ(ParseHttpRequest("GET / HTTP/2\r\nHost: 127.0.0.1\r\n\r\n").status,
            ParseStatus::Malformed);
}

TEST(CheevoMapV2LocalDashboardProtocol, DetectsUnsupportedRequestBodies)
{
  auto parsed =
      ParseHttpRequest("GET / HTTP/1.1\r\nHost: 127.0.0.1:32123\r\nContent-Length: 1\r\n\r\nx");
  ASSERT_EQ(parsed.status, ParseStatus::Complete);
  EXPECT_TRUE(parsed.request.has_request_body);

  parsed = ParseHttpRequest(
      "GET / HTTP/1.1\r\nHost: 127.0.0.1:32123\r\nTransfer-Encoding: chunked\r\n\r\n");
  ASSERT_EQ(parsed.status, ParseStatus::Complete);
  EXPECT_TRUE(parsed.request.has_chunked_body);
}

TEST(CheevoMapV2LocalDashboardProtocol, FramesSseEventsAndKeepalives)
{
  const std::string event = BuildSseEvent("snapshot", "{\"protocol_version\":1}");
  EXPECT_EQ(event, "event: snapshot\ndata: {\"protocol_version\":1}\n\n");
  EXPECT_EQ(event.find('\r'), std::string::npos);
  EXPECT_EQ(BuildSseKeepalive(), ": keepalive\n\n");
  EXPECT_EQ(CheevoMap::LocalDashboard::BuildSseHeaders().find("Content-Length"), std::string::npos);
}

TEST(CheevoMapV2LocalDashboardProtocol, BuildsSecurityAndApiHeaders)
{
  const std::string html = BuildHttpResponse(200, "text/html; charset=utf-8", "<!doctype html>",
                                             DashboardSecurityHeaders(true));
  EXPECT_NE(html.find("X-Content-Type-Options: nosniff\r\n"), std::string::npos);
  EXPECT_NE(html.find("Referrer-Policy: no-referrer\r\n"), std::string::npos);
  EXPECT_NE(html.find("Cache-Control: no-store\r\n"), std::string::npos);
  EXPECT_NE(html.find("Content-Security-Policy: default-src 'none';"), std::string::npos);
  EXPECT_EQ(html.find("Access-Control-Allow-Origin"), std::string::npos);

  const std::string json = BuildHttpResponse(200, "application/json; charset=utf-8", "{}",
                                             {{"Cache-Control", "no-store"}});
  EXPECT_NE(json.find("Content-Type: application/json; charset=utf-8\r\n"), std::string::npos);
  EXPECT_NE(json.find("Cache-Control: no-store\r\n"), std::string::npos);

  const std::string method = BuildHttpResponse(405, "text/plain; charset=utf-8",
                                               "Method not allowed.\n", {{"Allow", "GET"}});
  EXPECT_NE(method.find("HTTP/1.1 405 Method Not Allowed\r\n"), std::string::npos);
  EXPECT_NE(method.find("Allow: GET\r\n"), std::string::npos);
}

TEST(CheevoMapV2LocalDashboardProtocol, ResponseBuilderOwnsFramingHeaders)
{
  const std::string response = BuildHttpResponse(200, "text/plain; charset=utf-8", "ok",
                                                 {{"Content-Length", "999"},
                                                  {"content-length", "999"},
                                                  {"Connection", "keep-alive"},
                                                  {"Content-Type", "text/html"},
                                                  {"Transfer-Encoding", "chunked"},
                                                  {"Cache-Control", "no-store"}});

  EXPECT_EQ(CountSubstring(response, "Content-Length:"), 1u);
  EXPECT_EQ(CountSubstring(response, "Connection:"), 1u);
  EXPECT_EQ(CountSubstring(response, "Content-Type:"), 1u);
  EXPECT_EQ(response.find("Transfer-Encoding:"), std::string::npos);
  EXPECT_NE(response.find("Content-Length: 2\r\n"), std::string::npos);
  EXPECT_NE(response.find("Connection: close\r\n"), std::string::npos);
  EXPECT_NE(response.find("Cache-Control: no-store\r\n"), std::string::npos);
}

TEST(CheevoMapV2LocalDashboardProtocol, ResponseBuilderFormatsContentLengthWithoutLocale)
{
  const std::locale previous_locale;
  try
  {
    std::locale::global(std::locale(""));
  }
  catch (const std::runtime_error&)
  {
  }

  const std::string response =
      BuildHttpResponse(200, "text/plain; charset=utf-8", std::string(1391, 'x'), {});
  std::locale::global(previous_locale);

  EXPECT_NE(response.find("Content-Length: 1391\r\n"), std::string::npos);
  EXPECT_EQ(response.find("Content-Length: 1,391\r\n"), std::string::npos);
  EXPECT_EQ(response.find("Content-Length: 1.391\r\n"), std::string::npos);
  ExpectSingleContentLengthResponse(response);
}

TEST(CheevoMapV2LocalDashboardProtocol, NativeServerServesOrdinaryRoutesWithSingleLength)
{
  LocalDashboardServer server;
  ASSERT_TRUE(server.Start(TestOptions(), TestAssets(), TestSnapshot(1, 1), nullptr));

  const std::vector<std::pair<std::string, std::string>> routes = {
      {"/", "<!doctype html>"},
      {"/app.js", "'use strict';"},
      {"/style.css", "body { color:"},
      {"/api/v1/health", "\"status\":\"ok\""},
      {"/api/v1/snapshot", "\"message_type\":\"snapshot\""},
  };

  for (const auto& [route, expected_body] : routes)
  {
    const std::string response = RequestFromServer(
        server.GetBoundPort(), "GET " + route + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << route << response;
    ExpectSingleContentLengthResponse(response);
    EXPECT_NE(response.find(expected_body), std::string::npos) << route << response;
  }
}

TEST(CheevoMapV2LocalDashboardProtocol, NativeServerReturnsErrorsWithoutDuplicateLength)
{
  LocalDashboardServer server;
  ASSERT_TRUE(server.Start(TestOptions(), TestAssets(), TestSnapshot(1, 1), nullptr));

  std::string response =
      RequestFromServer(server.GetBoundPort(), "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  EXPECT_NE(response.find("HTTP/1.1 405 Method Not Allowed\r\n"), std::string::npos);
  EXPECT_NE(response.find("Allow: GET\r\n"), std::string::npos);
  ExpectSingleContentLengthResponse(response);

  response =
      RequestFromServer(server.GetBoundPort(), "GET / HTTP/1.1\r\nHost: 192.168.0.2:32123\r\n\r\n");
  EXPECT_NE(response.find("HTTP/1.1 403 Forbidden\r\n"), std::string::npos);
  ExpectSingleContentLengthResponse(response);

  response = RequestFromServer(server.GetBoundPort(),
                               "GET /missing?x=1 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  EXPECT_NE(response.find("HTTP/1.1 404 Not Found\r\n"), std::string::npos);
  ExpectSingleContentLengthResponse(response);
}

TEST(CheevoMapV2LocalDashboardProtocol, NativeServerHandlesRepeatedBrowserLikeRequests)
{
  LocalDashboardServer server;
  ASSERT_TRUE(server.Start(TestOptions(), TestAssets(), TestSnapshot(1, 1), nullptr));

  for (int i = 0; i < 100; ++i)
  {
    const std::string response = RequestFromServer(
        server.GetBoundPort(),
        "GET / HTTP/1.1\r\nHost: localhost:32123\r\nUser-Agent: test\r\nAccept: */*\r\n\r\n");
    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
    ExpectSingleContentLengthResponse(response);
  }
}

TEST(CheevoMapV2LocalDashboardProtocol, NativeServerBindsIPv4Loopback)
{
  LocalDashboardServer server;
  ASSERT_TRUE(server.Start(TestOptions(), TestAssets(), TestSnapshot(1, 1), nullptr));

  EXPECT_EQ(server.GetBoundIPv4AddressForTesting(), htonl(INADDR_LOOPBACK));
}

TEST(CheevoMapV2LocalDashboardProtocol, NativeServerStartStopRestartAndBindConflict)
{
  LocalDashboardServer server;
  ASSERT_TRUE(server.Start(TestOptions(), TestAssets(), TestSnapshot(1, 1), nullptr));
  const std::uint16_t port = server.GetBoundPort();
  EXPECT_NE(port, 0);
  server.Stop();

  LocalDashboardServer::Options restart_options = TestOptions();
  restart_options.port = port;
  ASSERT_TRUE(server.Start(restart_options, TestAssets(), TestSnapshot(1, 2), nullptr));
  server.Stop();

  LocalDashboardServer first;
  ASSERT_TRUE(first.Start(restart_options, TestAssets(), TestSnapshot(1, 3), nullptr));

  LocalDashboardServer second;
  std::string error;
  EXPECT_FALSE(second.Start(restart_options, TestAssets(), TestSnapshot(1, 4), &error));
  EXPECT_NE(error.find("bind failed:"), std::string::npos) << error;

  first.Stop();

  LocalDashboardServer third;
  EXPECT_TRUE(third.Start(restart_options, TestAssets(), TestSnapshot(1, 5), nullptr));
}

TEST(CheevoMapV2LocalDashboardProtocol, NativeServerStreamsSseSnapshotUpdatesAndKeepalive)
{
  Common::SocketContext socket_context;
  LocalDashboardServer server;
  ASSERT_TRUE(server.Start(TestOptions(), TestAssets(), TestSnapshot(1, 1), nullptr));

  NativeSocket socket = ConnectToServer(server.GetBoundPort());
  ASSERT_NE(socket, INVALID_NATIVE_SOCKET);
  ASSERT_TRUE(SendAll(socket, "GET /api/v1/events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));

  std::string response = ReceiveUntilContains(socket, "event: snapshot\n");
  ASSERT_NE(response.find("\r\n\r\n"), std::string::npos) << response;
  const std::string headers = response.substr(0, response.find("\r\n\r\n"));
  EXPECT_EQ(headers.find("Content-Length"), std::string::npos) << response;
  EXPECT_NE(response.find("Content-Type: text/event-stream; charset=utf-8\r\n"), std::string::npos)
      << response;
  EXPECT_NE(response.find("event: snapshot\n"), std::string::npos) << response;
  EXPECT_NE(response.find("\"session_id\":\"1\""), std::string::npos) << response;
  EXPECT_NE(response.find("\"sequence\":\"1\""), std::string::npos) << response;

  server.PublishSnapshotAndUpdate(TestSnapshot(1, 2), TestUpdate(1, 2));
  response += ReceiveUntilContains(socket, "event: update\n");
  EXPECT_NE(response.find("event: update\n"), std::string::npos) << response;
  EXPECT_NE(response.find("\"sequence\":\"2\""), std::string::npos) << response;

  response += ReceiveUntilContains(socket, ": keepalive\n\n");
  EXPECT_NE(response.find(": keepalive\n\n"), std::string::npos) << response;

  CloseSocket(socket);
}

TEST(CheevoMapV2LocalDashboardProtocol, NativeServerDoesNotSendStaleOrDuplicateSseUpdates)
{
  Common::SocketContext socket_context;
  LocalDashboardServer server;
  ASSERT_TRUE(server.Start(TestOptions(), TestAssets(), TestSnapshot(1, 10), nullptr));

  NativeSocket socket = ConnectToServer(server.GetBoundPort());
  ASSERT_NE(socket, INVALID_NATIVE_SOCKET);
  ASSERT_TRUE(SendAll(socket, "GET /api/v1/events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
  std::string response = ReceiveUntilContains(socket, "\"sequence\":\"10\"");
  ASSERT_NE(response.find("event: snapshot\n"), std::string::npos) << response;

  server.PublishSnapshotAndUpdate(TestSnapshot(1, 10), TestUpdate(1, 9));
  server.PublishSnapshotAndUpdate(TestSnapshot(1, 10), TestUpdate(1, 10));
  response = ReceiveFor(socket, std::chrono::milliseconds(250));
  EXPECT_EQ(response.find("event: update\n"), std::string::npos) << response;

  CloseSocket(socket);
}

TEST(CheevoMapV2LocalDashboardProtocol, NativeServerSendsNewerUpdatesToExistingClients)
{
  Common::SocketContext socket_context;
  LocalDashboardServer server;
  ASSERT_TRUE(server.Start(TestOptions(), TestAssets(), TestSnapshot(1, 8), nullptr));

  NativeSocket socket = ConnectToServer(server.GetBoundPort());
  ASSERT_NE(socket, INVALID_NATIVE_SOCKET);
  ASSERT_TRUE(SendAll(socket, "GET /api/v1/events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
  ASSERT_NE(ReceiveUntilContains(socket, "\"sequence\":\"8\"").find("event: snapshot\n"),
            std::string::npos);

  server.PublishSnapshotAndUpdate(TestSnapshot(1, 10), TestUpdate(1, 9));
  const std::string response = ReceiveUntilContains(socket, "\"sequence\":\"9\"");
  EXPECT_NE(response.find("event: update\n"), std::string::npos) << response;

  CloseSocket(socket);
}

TEST(CheevoMapV2LocalDashboardProtocol, NativeServerSendsNewSessionFullUpdate)
{
  Common::SocketContext socket_context;
  LocalDashboardServer server;
  ASSERT_TRUE(server.Start(TestOptions(), TestAssets(), TestSnapshot(1, 50), nullptr));

  NativeSocket socket = ConnectToServer(server.GetBoundPort());
  ASSERT_NE(socket, INVALID_NATIVE_SOCKET);
  ASSERT_TRUE(SendAll(socket, "GET /api/v1/events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
  ASSERT_NE(ReceiveUntilContains(socket, "\"sequence\":\"50\"").find("event: snapshot\n"),
            std::string::npos);

  server.PublishSnapshotAndUpdate(TestSnapshot(2, 1), TestUpdate(2, 1, true));
  const std::string response = ReceiveUntilContains(socket, "\"session_id\":\"2\"");
  EXPECT_NE(response.find("event: update\n"), std::string::npos) << response;

  CloseSocket(socket);
}

TEST(CheevoMapV2LocalDashboardProtocol,
     NativeServerBroadcastsAuthoritativeSnapshotToExistingClients)
{
  Common::SocketContext socket_context;
  LocalDashboardServer server;
  ASSERT_TRUE(server.Start(TestOptions(), TestAssets(), TestSnapshot(1, 10), nullptr));

  NativeSocket socket = ConnectToServer(server.GetBoundPort());
  ASSERT_NE(socket, INVALID_NATIVE_SOCKET);
  ASSERT_TRUE(SendAll(socket, "GET /api/v1/events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
  ASSERT_NE(ReceiveUntilContains(socket, "\"sequence\":\"10\"").find("event: snapshot\n"),
            std::string::npos);

  server.PublishAuthoritativeSnapshot(TestSnapshot(2, 1));
  const std::string response = ReceiveUntilContains(socket, "\"sequence\":\"1\"");
  EXPECT_NE(response.find("event: snapshot\n"), std::string::npos) << response;
  EXPECT_EQ(response.find("event: update\n"), std::string::npos) << response;
  EXPECT_NE(response.find("\"session_id\":\"2\""), std::string::npos) << response;
  EXPECT_NE(response.find("\"sequence\":\"1\""), std::string::npos) << response;

  CloseSocket(socket);
}

TEST(CheevoMapV2LocalDashboardProtocol, NativeServerDiscardsPendingDeltasDuringAuthoritativeResync)
{
  Common::SocketContext socket_context;
  LocalDashboardServer::Options options = TestOptions();
  options.select_timeout_ms = 1000;
  options.keepalive_interval_ms = 5000;

  LocalDashboardServer server;
  ASSERT_TRUE(server.Start(options, TestAssets(), TestSnapshot(1, 10), nullptr));

  NativeSocket socket = ConnectToServer(server.GetBoundPort());
  ASSERT_NE(socket, INVALID_NATIVE_SOCKET);
  ASSERT_TRUE(SendAll(socket, "GET /api/v1/events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
  ASSERT_NE(ReceiveUntilContains(socket, "\"sequence\":\"10\"").find("event: snapshot\n"),
            std::string::npos);

  server.PublishSnapshotAndUpdate(TestSnapshot(1, 11), TestUpdate(1, 11));
  server.PublishSnapshotAndUpdate(TestSnapshot(1, 12), TestUpdate(1, 12));
  server.PublishAuthoritativeSnapshot(TestSnapshot(2, 1));

  std::string response = ReceiveUntilContains(socket, "\"sequence\":\"1\"");
  EXPECT_EQ(CountSubstring(response, "event: snapshot\n"), 1u) << response;
  EXPECT_EQ(response.find("event: update\n"), std::string::npos) << response;
  EXPECT_NE(response.find("\"session_id\":\"2\""), std::string::npos) << response;
  EXPECT_EQ(response.find("\"sequence\":\"11\""), std::string::npos) << response;
  EXPECT_EQ(response.find("\"sequence\":\"12\""), std::string::npos) << response;

  server.PublishSnapshotAndUpdate(TestSnapshot(2, 2), TestUpdate(2, 2));
  response = ReceiveUntilContains(socket, "\"sequence\":\"2\"");
  EXPECT_NE(response.find("event: update\n"), std::string::npos) << response;

  CloseSocket(socket);
}

TEST(CheevoMapV2LocalDashboardProtocol, NativeServerRejectsOlderAuthoritativeSnapshot)
{
  Common::SocketContext socket_context;
  LocalDashboardServer server;
  ASSERT_TRUE(server.Start(TestOptions(), TestAssets(), TestSnapshot(2, 5), nullptr));

  NativeSocket socket = ConnectToServer(server.GetBoundPort());
  ASSERT_NE(socket, INVALID_NATIVE_SOCKET);
  ASSERT_TRUE(SendAll(socket, "GET /api/v1/events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
  ASSERT_NE(ReceiveUntilContains(socket, "\"sequence\":\"5\"").find("event: snapshot\n"),
            std::string::npos);

  server.PublishAuthoritativeSnapshot(TestSnapshot(1, 999));
  std::string response = ReceiveFor(socket, std::chrono::milliseconds(250));
  EXPECT_EQ(response.find("\"session_id\":\"1\""), std::string::npos) << response;
  EXPECT_EQ(response.find("\"sequence\":\"999\""), std::string::npos) << response;

  response = RequestFromServer(server.GetBoundPort(),
                               "GET /api/v1/snapshot HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  EXPECT_NE(response.find("\"session_id\":\"2\""), std::string::npos) << response;
  EXPECT_NE(response.find("\"sequence\":\"5\""), std::string::npos) << response;
  EXPECT_EQ(response.find("\"session_id\":\"1\""), std::string::npos) << response;
  EXPECT_EQ(response.find("\"sequence\":\"999\""), std::string::npos) << response;

  CloseSocket(socket);
}

TEST(CheevoMapV2LocalDashboardProtocol, NativeServerRebroadcastsEqualCursorAuthoritativeSnapshot)
{
  Common::SocketContext socket_context;
  LocalDashboardServer server;
  ASSERT_TRUE(server.Start(TestOptions(), TestAssets(), TestSnapshot(2, 5), nullptr));

  NativeSocket socket = ConnectToServer(server.GetBoundPort());
  ASSERT_NE(socket, INVALID_NATIVE_SOCKET);
  ASSERT_TRUE(SendAll(socket, "GET /api/v1/events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
  ASSERT_NE(ReceiveUntilContains(socket, "\"sequence\":\"5\"").find("event: snapshot\n"),
            std::string::npos);

  server.PublishAuthoritativeSnapshot(TestSnapshot(2, 5, R"(,"resync":"equal")"));
  const std::string response = ReceiveUntilContains(socket, "\"resync\":\"equal\"");
  EXPECT_NE(response.find("event: snapshot\n"), std::string::npos) << response;
  EXPECT_EQ(response.find("event: update\n"), std::string::npos) << response;
  EXPECT_NE(response.find("\"session_id\":\"2\""), std::string::npos) << response;
  EXPECT_NE(response.find("\"sequence\":\"5\""), std::string::npos) << response;

  CloseSocket(socket);
}

TEST(CheevoMapV2LocalDashboardProtocol, NativeServerSendsResynchronizedSnapshotToNewClients)
{
  Common::SocketContext socket_context;
  LocalDashboardServer server;
  ASSERT_TRUE(server.Start(TestOptions(), TestAssets(), TestSnapshot(1, 10), nullptr));

  server.PublishAuthoritativeSnapshot(TestSnapshot(2, 1, R"(,"resync":"new-client")"));

  NativeSocket socket = ConnectToServer(server.GetBoundPort());
  ASSERT_NE(socket, INVALID_NATIVE_SOCKET);
  ASSERT_TRUE(SendAll(socket, "GET /api/v1/events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));

  const std::string response = ReceiveUntilContains(socket, "\"resync\":\"new-client\"");
  EXPECT_NE(response.find("event: snapshot\n"), std::string::npos) << response;
  EXPECT_NE(response.find("\"session_id\":\"2\""), std::string::npos) << response;
  EXPECT_NE(response.find("\"sequence\":\"1\""), std::string::npos) << response;
  EXPECT_EQ(response.find("event: update\n"), std::string::npos) << response;

  CloseSocket(socket);
}

TEST(CheevoMapV2LocalDashboardProtocol, NativeServerCountOverflowResynchronizesSseClients)
{
  Common::SocketContext socket_context;
  LocalDashboardServer::Options options = TestOptions();
  options.select_timeout_ms = 1000;
  options.keepalive_interval_ms = 5000;

  LocalDashboardServer server;
  ASSERT_TRUE(server.Start(options, TestAssets(), TestSnapshot(1, 1), nullptr));

  NativeSocket socket = ConnectToServer(server.GetBoundPort());
  ASSERT_NE(socket, INVALID_NATIVE_SOCKET);
  ASSERT_TRUE(SendAll(socket, "GET /api/v1/events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
  ASSERT_NE(ReceiveUntilContains(socket, "\"sequence\":\"1\"").find("event: snapshot\n"),
            std::string::npos);

  for (size_t i = 0; i < CheevoMap::LocalDashboard::kMaxPendingUpdates; ++i)
    server.PublishSnapshotAndUpdate(TestSnapshot(1, 2 + i), TestUpdate(1, 2 + i));
  server.PublishSnapshotAndUpdate(TestSnapshot(1, 300), TestUpdate(1, 300));

  std::string response = ReceiveUntilContains(socket, "\"sequence\":\"300\"");
  EXPECT_NE(response.find("event: snapshot\n"), std::string::npos) << response;
  EXPECT_EQ(response.find("event: update\n"), std::string::npos) << response;

  server.PublishSnapshotAndUpdate(TestSnapshot(1, 301), TestUpdate(1, 301));
  response = ReceiveUntilContains(socket, "\"sequence\":\"301\"");
  EXPECT_NE(response.find("event: update\n"), std::string::npos) << response;

  CloseSocket(socket);
}

TEST(CheevoMapV2LocalDashboardProtocol, NativeServerByteOverflowResynchronizesSseClients)
{
  Common::SocketContext socket_context;
  LocalDashboardServer::Options options = TestOptions();
  options.select_timeout_ms = 1000;
  options.keepalive_interval_ms = 5000;

  LocalDashboardServer server;
  ASSERT_TRUE(server.Start(options, TestAssets(), TestSnapshot(1, 1), nullptr));

  NativeSocket socket = ConnectToServer(server.GetBoundPort());
  ASSERT_NE(socket, INVALID_NATIVE_SOCKET);
  ASSERT_TRUE(SendAll(socket, "GET /api/v1/events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"));
  ASSERT_NE(ReceiveUntilContains(socket, "\"sequence\":\"1\"").find("event: snapshot\n"),
            std::string::npos);

  server.PublishSnapshotAndUpdate(TestSnapshot(1, 2), TestUpdate(1, 2));
  server.PublishSnapshotAndUpdate(
      TestSnapshot(1, 3),
      TestUpdate(1, 3, false,
                 ",\"payload\":\"" +
                     std::string(CheevoMap::LocalDashboard::kMaxPendingUpdateBytes, 'x') + "\""));

  std::string response = ReceiveUntilContains(socket, "\"sequence\":\"3\"");
  EXPECT_NE(response.find("event: snapshot\n"), std::string::npos) << response;
  EXPECT_EQ(response.find("event: update\n"), std::string::npos) << response;

  server.PublishSnapshotAndUpdate(TestSnapshot(1, 4), TestUpdate(1, 4));
  response = ReceiveUntilContains(socket, "\"sequence\":\"4\"");
  EXPECT_NE(response.find("event: update\n"), std::string::npos) << response;

  CloseSocket(socket);
}
