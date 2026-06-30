// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CheevoMap::LocalDashboard
{
constexpr std::string_view kBindAddress = "127.0.0.1";
constexpr int kPort = 32123;
constexpr size_t kMaxRequestHeaderSize = 16 * 1024;
constexpr int kMaxOpenClients = 16;
constexpr int kMaxSseClients = 8;
constexpr size_t kMaxSsePendingBytes = 256 * 1024;
constexpr size_t kMaxPendingUpdates = 256;
constexpr size_t kMaxPendingUpdateBytes = 1024 * 1024;

enum class Route
{
  Dashboard,
  JavaScript,
  Stylesheet,
  Health,
  Snapshot,
  Events,
};

enum class ParseStatus
{
  Complete,
  NeedMoreData,
  Malformed,
  HeaderTooLarge,
};

struct HttpRequest
{
  std::string method;
  std::string target;
  std::string version;
  std::optional<std::string> host;
  std::optional<Route> route;
  bool has_request_body = false;
  bool has_chunked_body = false;
};

struct ParseResult
{
  ParseStatus status = ParseStatus::NeedMoreData;
  HttpRequest request;
};

ParseResult ParseHttpRequest(std::string_view data, size_t max_header_size = kMaxRequestHeaderSize);
bool IsHostAllowed(const std::optional<std::string>& host, std::string_view version);
std::optional<Route> RouteForTarget(std::string_view target);

std::string HttpStatusText(int status_code);
std::string BuildHttpResponse(int status_code, std::string_view content_type, std::string_view body,
                              const std::vector<std::pair<std::string, std::string>>& headers = {});
std::string BuildSseHeaders();
std::string BuildSseEvent(std::string_view event_name, std::string_view json);
std::string BuildSseKeepalive();
std::vector<std::pair<std::string, std::string>> DashboardSecurityHeaders(bool html);
}  // namespace CheevoMap::LocalDashboard
