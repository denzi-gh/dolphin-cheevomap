// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/CheevoMap/CheevoMapV2LocalDashboardServer.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <ranges>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "Common/Network.h"
#include "Common/SocketContext.h"
#include "Core/CheevoMap/V2/DashboardProtocol.h"

namespace CheevoMap::LocalDashboard
{
namespace
{
#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket INVALID_NATIVE_SOCKET = INVALID_SOCKET;
constexpr int WOULD_BLOCK_ERROR = WSAEWOULDBLOCK;

int LastSocketError()
{
  return WSAGetLastError();
}

bool SetNonBlocking(const NativeSocket socket)
{
  u_long mode = 1;
  return ioctlsocket(socket, FIONBIO, &mode) == 0;
}

void CloseNativeSocket(const NativeSocket socket)
{
  closesocket(socket);
}
#else
using NativeSocket = int;
constexpr NativeSocket INVALID_NATIVE_SOCKET = -1;
constexpr int WOULD_BLOCK_ERROR = EWOULDBLOCK;

int LastSocketError()
{
  return errno;
}

bool SetNonBlocking(const NativeSocket socket)
{
  const int flags = fcntl(socket, F_GETFL, 0);
  return flags != -1 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) != -1;
}

void CloseNativeSocket(const NativeSocket socket)
{
  close(socket);
}
#endif

bool IsWouldBlock(const int error)
{
#ifdef _WIN32
  return error == WOULD_BLOCK_ERROR;
#else
  return error == WOULD_BLOCK_ERROR || error == EAGAIN;
#endif
}

bool IsInterrupted(const int error)
{
#ifdef _WIN32
  return error == WSAEINTR;
#else
  return error == EINTR;
#endif
}

std::string SocketErrorString(const std::string_view operation)
{
  const int error = LastSocketError();
#ifdef _WIN32
  const char* decoded = Common::DecodeNetworkError(error);
#else
  const char* decoded = std::strerror(error);
#endif
  return std::string(operation) + " failed: " + decoded + " (" + std::to_string(error) + ")";
}

int SendFlags()
{
#ifdef MSG_NOSIGNAL
  return MSG_NOSIGNAL;
#else
  return 0;
#endif
}

bool ConfigureNoSigPipe(const NativeSocket socket)
{
#ifdef _WIN32
  return true;
#elif defined(SO_NOSIGPIPE)
  int value = 1;
  return setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &value, sizeof(value)) == 0;
#else
  return true;
#endif
}

bool IsSelectableSocket(const NativeSocket socket)
{
#ifdef _WIN32
  return true;
#else
  return socket >= 0 && socket < FD_SETSIZE;
#endif
}

bool IsInvalidSocket(const NativeSocket socket)
{
  return socket == INVALID_NATIVE_SOCKET;
}
}  // namespace

struct LocalDashboardServer::Impl
{
  struct Client
  {
    NativeSocket socket = INVALID_NATIVE_SOCKET;
    std::string request;
    std::string output;
    CheevoMap::V2::StateCursor last_sent_cursor;
    bool request_handled = false;
    bool sse = false;
    bool close_after_write = false;
  };

  struct PublicationState
  {
    SerializedSnapshot snapshot;
    std::vector<SerializedUpdate> pending_updates;
    size_t pending_update_bytes = 0;
    bool snapshot_resync_pending = false;
  };

  bool Start(Options options_, Assets assets_, SerializedSnapshot initial_snapshot,
             std::string* error)
  {
    if (running.load())
      return true;

    socket_context.emplace();

    NativeSocket new_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (IsInvalidSocket(new_listener))
    {
      if (error)
        *error = SocketErrorString("socket");
      socket_context.reset();
      return false;
    }

    if (!IsSelectableSocket(new_listener))
    {
      if (error)
        *error = "Listener socket exceeds select() FD_SETSIZE.";
      CloseNativeSocket(new_listener);
      socket_context.reset();
      return false;
    }

    if (!SetNonBlocking(new_listener))
    {
      if (error)
        *error = SocketErrorString("ioctlsocket");
      CloseNativeSocket(new_listener);
      socket_context.reset();
      return false;
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(options_.port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(new_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
      if (error)
        *error = SocketErrorString("bind");
      CloseNativeSocket(new_listener);
      socket_context.reset();
      return false;
    }

    if (listen(new_listener, SOMAXCONN) != 0)
    {
      if (error)
        *error = SocketErrorString("listen");
      CloseNativeSocket(new_listener);
      socket_context.reset();
      return false;
    }

    sockaddr_in actual_address = {};
#ifdef _WIN32
    int actual_address_length = sizeof(actual_address);
#else
    socklen_t actual_address_length = sizeof(actual_address);
#endif
    if (getsockname(new_listener, reinterpret_cast<sockaddr*>(&actual_address),
                    &actual_address_length) != 0)
    {
      if (error)
        *error = SocketErrorString("getsockname");
      CloseNativeSocket(new_listener);
      socket_context.reset();
      return false;
    }

    options = std::move(options_);
    assets = std::move(assets_);
    bound_port = ntohs(actual_address.sin_port);
    bound_ipv4_address = actual_address.sin_addr.s_addr;
    listener = new_listener;

    {
      std::lock_guard lock(data_mutex);
      publication.snapshot = std::move(initial_snapshot);
      publication.pending_updates.clear();
      publication.pending_update_bytes = 0;
      publication.snapshot_resync_pending = false;
    }

    stop_requested.store(false);
    running.store(true);
    worker = std::thread([this] { Run(); });
    return true;
  }

  void Stop()
  {
    if (!running.load())
      return;

    stop_requested.store(true);
    if (worker.joinable())
      worker.join();

    running.store(false);
    bound_port = 0;
    bound_ipv4_address = 0;
    socket_context.reset();
  }

  void PublishSnapshotAndUpdate(SerializedSnapshot snapshot, std::optional<SerializedUpdate> update)
  {
    if (!running.load())
      return;

    std::lock_guard lock(data_mutex);
    publication.snapshot = std::move(snapshot);
    if (!update)
      return;

    const size_t event_bytes = BuildSseEvent("update", update->json).size();
    if (publication.pending_updates.size() + 1 > kMaxPendingUpdates ||
        publication.pending_update_bytes + event_bytes > kMaxPendingUpdateBytes)
    {
      publication.pending_updates.clear();
      publication.pending_update_bytes = 0;
      publication.snapshot_resync_pending = true;
      return;
    }

    publication.pending_update_bytes += event_bytes;
    publication.pending_updates.push_back(std::move(*update));
  }

  void PublishAuthoritativeSnapshot(SerializedSnapshot snapshot)
  {
    if (!running.load())
      return;

    std::lock_guard lock(data_mutex);
    if (CheevoMap::V2::IsCursorNewer(publication.snapshot.cursor, snapshot.cursor))
      return;

    publication.snapshot = std::move(snapshot);
    publication.pending_updates.clear();
    publication.pending_update_bytes = 0;
    // Equal-cursor resyncs are intentionally rebroadcast: the caller is asking
    // every existing SSE client to converge on this authoritative snapshot.
    publication.snapshot_resync_pending = true;
  }

  void Run()
  {
    auto last_keepalive = std::chrono::steady_clock::now();

    while (!stop_requested.load())
    {
      DrainPendingUpdates();

      const auto now = std::chrono::steady_clock::now();
      if (now - last_keepalive >= std::chrono::milliseconds(options.keepalive_interval_ms))
      {
        BroadcastSseBytes(BuildSseKeepalive());
        last_keepalive = now;
      }

      fd_set read_fds;
      fd_set write_fds;
      FD_ZERO(&read_fds);
      FD_ZERO(&write_fds);

      NativeSocket max_socket = listener;
      if (!IsInvalidSocket(listener))
        FD_SET(listener, &read_fds);

      for (const Client& client : clients)
      {
        if (!client.sse && !client.request_handled)
          FD_SET(client.socket, &read_fds);
        if (!client.output.empty())
          FD_SET(client.socket, &write_fds);
        max_socket = std::max(max_socket, client.socket);
      }

      timeval timeout = {};
      timeout.tv_sec = options.select_timeout_ms / 1000;
      timeout.tv_usec = (options.select_timeout_ms % 1000) * 1000;

      const int selected =
          select(static_cast<int>(max_socket + 1), &read_fds, &write_fds, nullptr, &timeout);
      if (selected < 0)
      {
        if (IsInterrupted(LastSocketError()))
          continue;
        continue;
      }

      if (selected > 0 && !IsInvalidSocket(listener) && FD_ISSET(listener, &read_fds))
        AcceptClients();

      for (size_t i = 0; i < clients.size();)
      {
        const NativeSocket socket = clients[i].socket;
        bool removed = false;

        if (!clients[i].sse && !clients[i].request_handled && FD_ISSET(socket, &read_fds))
          removed = ReadClient(i);

        if (!removed && i < clients.size() && !clients[i].output.empty() &&
            FD_ISSET(socket, &write_fds))
        {
          removed = WriteClient(i);
        }

        if (!removed)
          ++i;
      }
    }

    CloseAllSockets();
  }

  void AcceptClients()
  {
    for (;;)
    {
      sockaddr_in client_address = {};
#ifdef _WIN32
      int address_length = sizeof(client_address);
#else
      socklen_t address_length = sizeof(client_address);
#endif
      const NativeSocket accepted =
          accept(listener, reinterpret_cast<sockaddr*>(&client_address), &address_length);
      if (IsInvalidSocket(accepted))
      {
        if (IsWouldBlock(LastSocketError()))
          return;
        return;
      }

      if (!IsSelectableSocket(accepted))
      {
        CloseNativeSocket(accepted);
        continue;
      }

      if (static_cast<int>(clients.size()) >= kMaxOpenClients || !ConfigureNoSigPipe(accepted) ||
          !SetNonBlocking(accepted))
      {
        CloseNativeSocket(accepted);
        continue;
      }

      clients.push_back(Client{.socket = accepted});
    }
  }

  bool ReadClient(const size_t index)
  {
    Client& client = clients[index];
    const NativeSocket socket = client.socket;

    for (;;)
    {
      char buffer[4096];
      const int received = recv(client.socket, buffer, static_cast<int>(sizeof(buffer)), 0);
      if (received > 0)
      {
        client.request.append(buffer, received);
        if (client.request.size() > kMaxRequestHeaderSize)
          break;
        continue;
      }

      if (received == 0)
      {
        RemoveClient(index);
        return true;
      }

      if (IsWouldBlock(LastSocketError()))
        break;

      RemoveClient(index);
      return true;
    }

    ProcessRequest(index);
    return index >= clients.size() || clients[index].socket != socket;
  }

  bool WriteClient(const size_t index)
  {
    Client& client = clients[index];

    while (!client.output.empty())
    {
      const int bytes_to_send = std::min<int>(static_cast<int>(client.output.size()), 64 * 1024);
      const int sent = send(client.socket, client.output.data(), bytes_to_send, SendFlags());
      if (sent > 0)
      {
        client.output.erase(0, static_cast<size_t>(sent));
        continue;
      }

      if (sent == 0 || !IsWouldBlock(LastSocketError()))
      {
        RemoveClient(index);
        return true;
      }

      break;
    }

    if (client.output.empty() && client.close_after_write)
    {
      RemoveClient(index);
      return true;
    }

    return false;
  }

  void ProcessRequest(const size_t index)
  {
    Client& client = clients[index];
    if (client.request_handled)
      return;

    const ParseResult parsed = ParseHttpRequest(client.request);
    switch (parsed.status)
    {
    case ParseStatus::NeedMoreData:
      return;
    case ParseStatus::HeaderTooLarge:
      QueueHttpResponse(client, 431, "text/plain; charset=utf-8",
                        "Request headers are too large.\n");
      return;
    case ParseStatus::Malformed:
      RemoveClient(index);
      return;
    case ParseStatus::Complete:
      break;
    }

    if (parsed.request.method != "GET")
    {
      QueueHttpResponse(client, 405, "text/plain; charset=utf-8", "Method not allowed.\n",
                        {{"Allow", "GET"}});
      return;
    }

    if (parsed.request.has_request_body || parsed.request.has_chunked_body)
    {
      RemoveClient(index);
      return;
    }

    if (!IsHostAllowed(parsed.request.host, parsed.request.version))
    {
      QueueHttpResponse(client, 403, "text/plain; charset=utf-8", "Forbidden.\n");
      return;
    }

    if (!parsed.request.route)
    {
      QueueHttpResponse(client, 404, "text/plain; charset=utf-8", "Not found.\n");
      return;
    }

    switch (*parsed.request.route)
    {
    case Route::Dashboard:
      QueueHttpResponse(client, 200, "text/html; charset=utf-8", assets.html,
                        DashboardSecurityHeaders(true));
      break;
    case Route::JavaScript:
      QueueHttpResponse(client, 200, "application/javascript; charset=utf-8", assets.javascript,
                        DashboardSecurityHeaders(false));
      break;
    case Route::Stylesheet:
      QueueHttpResponse(client, 200, "text/css; charset=utf-8", assets.stylesheet,
                        DashboardSecurityHeaders(false));
      break;
    case Route::Health:
      QueueHttpResponse(client, 200, "application/json; charset=utf-8",
                        CheevoMap::V2::SerializeDashboardHealth(), {{"Cache-Control", "no-store"}});
      break;
    case Route::Snapshot:
      QueueHttpResponse(client, 200, "application/json; charset=utf-8", GetSnapshot().json,
                        {{"Cache-Control", "no-store"}});
      break;
    case Route::Events:
      OpenSseStream(index);
      break;
    }
  }

  void QueueHttpResponse(Client& client, const int status_code, const std::string_view content_type,
                         const std::string_view body,
                         const std::vector<std::pair<std::string, std::string>>& headers = {})
  {
    if (client.request_handled)
      return;

    client.request_handled = true;
    client.output = BuildHttpResponse(status_code, content_type, body, headers);
    client.close_after_write = true;
  }

  void OpenSseStream(const size_t index)
  {
    Client& client = clients[index];
    if (client.request_handled)
      return;

    if (SseClientCount() >= kMaxSseClients)
    {
      RemoveClient(index);
      return;
    }

    client.request_handled = true;
    client.sse = true;
    client.close_after_write = false;
    client.output = BuildSseHeaders();
    const SerializedSnapshot snapshot = GetSnapshot();
    if (!AppendSseSnapshot(client, snapshot))
    {
      RemoveClient(index);
      return;
    }
  }

  void DrainPendingUpdates()
  {
    std::optional<SerializedSnapshot> snapshot_resync;
    std::vector<SerializedUpdate> updates;
    {
      std::lock_guard lock(data_mutex);
      if (publication.snapshot_resync_pending)
      {
        snapshot_resync = publication.snapshot;
        publication.snapshot_resync_pending = false;
      }
      updates.swap(publication.pending_updates);
      publication.pending_update_bytes = 0;
    }

    if (snapshot_resync)
      BroadcastSseSnapshot(*snapshot_resync);

    for (const SerializedUpdate& update : updates)
      BroadcastSseUpdate(update);
  }

  void BroadcastSseBytes(const std::string& bytes)
  {
    for (size_t i = 0; i < clients.size();)
    {
      if (!clients[i].sse)
      {
        ++i;
        continue;
      }

      if (!AppendSseBytes(clients[i], bytes))
      {
        RemoveClient(i);
        continue;
      }

      ++i;
    }
  }

  void BroadcastSseSnapshot(const SerializedSnapshot& snapshot)
  {
    const std::string bytes = BuildSseEvent("snapshot", snapshot.json);
    for (size_t i = 0; i < clients.size();)
    {
      if (!clients[i].sse)
      {
        ++i;
        continue;
      }

      if (!AppendSseBytes(clients[i], bytes))
      {
        RemoveClient(i);
        continue;
      }

      clients[i].last_sent_cursor = snapshot.cursor;
      ++i;
    }
  }

  void BroadcastSseUpdate(const SerializedUpdate& update)
  {
    const std::string bytes = BuildSseEvent("update", update.json);
    for (size_t i = 0; i < clients.size();)
    {
      if (!clients[i].sse ||
          !CheevoMap::V2::IsCursorNewer(update.cursor, clients[i].last_sent_cursor))
      {
        ++i;
        continue;
      }

      if (!AppendSseBytes(clients[i], bytes))
      {
        RemoveClient(i);
        continue;
      }

      clients[i].last_sent_cursor = update.cursor;
      ++i;
    }
  }

  bool AppendSseSnapshot(Client& client, const SerializedSnapshot& snapshot)
  {
    if (!AppendSseBytes(client, BuildSseEvent("snapshot", snapshot.json)))
      return false;

    client.last_sent_cursor = snapshot.cursor;
    return true;
  }

  bool AppendSseBytes(Client& client, const std::string& bytes)
  {
    if (client.output.size() + bytes.size() > kMaxSsePendingBytes)
      return false;

    client.output.append(bytes);
    return true;
  }

  SerializedSnapshot GetSnapshot() const
  {
    std::lock_guard lock(data_mutex);
    return publication.snapshot;
  }

  int SseClientCount() const
  {
    return static_cast<int>(
        std::ranges::count_if(clients, [](const Client& client) { return client.sse; }));
  }

  void RemoveClient(const size_t index)
  {
    CloseNativeSocket(clients[index].socket);
    clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(index));
  }

  void CloseAllSockets()
  {
    for (const Client& client : clients)
      CloseNativeSocket(client.socket);
    clients.clear();

    if (!IsInvalidSocket(listener))
    {
      CloseNativeSocket(listener);
      listener = INVALID_NATIVE_SOCKET;
    }
  }

  std::optional<Common::SocketContext> socket_context;
  Options options;
  Assets assets;
  NativeSocket listener = INVALID_NATIVE_SOCKET;
  std::atomic_bool stop_requested = false;
  std::atomic_bool running = false;
  std::thread worker;
  std::uint16_t bound_port = 0;
  std::uint32_t bound_ipv4_address = 0;

  mutable std::mutex data_mutex;
  PublicationState publication;
  std::vector<Client> clients;
};

LocalDashboardServer::LocalDashboardServer() : m_impl(std::make_unique<Impl>())
{
}

LocalDashboardServer::~LocalDashboardServer()
{
  Stop();
}

bool LocalDashboardServer::Start(Options options, Assets assets,
                                 SerializedSnapshot initial_snapshot, std::string* error)
{
  return m_impl->Start(std::move(options), std::move(assets), std::move(initial_snapshot), error);
}

void LocalDashboardServer::Stop()
{
  m_impl->Stop();
}

bool LocalDashboardServer::IsRunning() const
{
  return m_impl->running.load();
}

std::uint16_t LocalDashboardServer::GetBoundPort() const
{
  return m_impl->bound_port;
}

std::uint32_t LocalDashboardServer::GetBoundIPv4AddressForTesting() const
{
  return m_impl->bound_ipv4_address;
}

void LocalDashboardServer::PublishSnapshotAndUpdate(SerializedSnapshot snapshot,
                                                    std::optional<SerializedUpdate> update)
{
  m_impl->PublishSnapshotAndUpdate(std::move(snapshot), std::move(update));
}

void LocalDashboardServer::PublishAuthoritativeSnapshot(SerializedSnapshot snapshot)
{
  m_impl->PublishAuthoritativeSnapshot(std::move(snapshot));
}
}  // namespace CheevoMap::LocalDashboard
