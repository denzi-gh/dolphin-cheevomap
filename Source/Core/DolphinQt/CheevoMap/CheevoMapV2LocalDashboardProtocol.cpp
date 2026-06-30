// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/CheevoMap/CheevoMapV2LocalDashboardProtocol.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <sstream>
#include <utility>

namespace CheevoMap::LocalDashboard
{
namespace
{
constexpr std::string_view CRLF = "\r\n";

std::string ToLower(std::string_view text)
{
  std::string lower;
  lower.reserve(text.size());
  for (const unsigned char c : text)
    lower.push_back(static_cast<char>(std::tolower(c)));
  return lower;
}

std::string Trim(std::string_view text)
{
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
    text.remove_suffix(1);
  return std::string(text);
}

bool IsValidToken(std::string_view token)
{
  return !token.empty() && token.find_first_of(" \t\r\n") == std::string_view::npos;
}

bool IsCallerHeaderAllowed(std::string_view name)
{
  const std::string lower = ToLower(name);
  return lower != "content-length" && lower != "connection" && lower != "content-type" &&
         lower != "transfer-encoding";
}

std::string ToHttpDecimal(const size_t value)
{
  std::array<char, 32> buffer;
  const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  return ec == std::errc{} ? std::string(buffer.data(), ptr) : std::string();
}
}  // namespace

ParseResult ParseHttpRequest(std::string_view data, const size_t max_header_size)
{
  ParseResult result;

  const size_t header_end = data.find("\r\n\r\n");
  if (header_end == std::string_view::npos)
  {
    result.status =
        data.size() > max_header_size ? ParseStatus::HeaderTooLarge : ParseStatus::NeedMoreData;
    return result;
  }

  if (header_end + 4 > max_header_size)
  {
    result.status = ParseStatus::HeaderTooLarge;
    return result;
  }

  std::string_view headers = data.substr(0, header_end);
  const size_t request_line_end = headers.find(CRLF);
  const std::string_view request_line =
      request_line_end == std::string_view::npos ? headers : headers.substr(0, request_line_end);

  const size_t first_space = request_line.find(' ');
  const size_t second_space = first_space == std::string_view::npos ?
                                  std::string_view::npos :
                                  request_line.find(' ', first_space + 1);
  if (first_space == std::string_view::npos || second_space == std::string_view::npos ||
      request_line.find(' ', second_space + 1) != std::string_view::npos)
  {
    result.status = ParseStatus::Malformed;
    return result;
  }

  const std::string_view method = request_line.substr(0, first_space);
  const std::string_view target =
      request_line.substr(first_space + 1, second_space - first_space - 1);
  const std::string_view version = request_line.substr(second_space + 1);
  if (!IsValidToken(method) || !IsValidToken(target) ||
      (version != "HTTP/1.0" && version != "HTTP/1.1"))
  {
    result.status = ParseStatus::Malformed;
    return result;
  }

  result.request.method = std::string(method);
  result.request.target = std::string(target);
  result.request.version = std::string(version);
  result.request.route = RouteForTarget(target);

  size_t line_start =
      request_line_end == std::string_view::npos ? headers.size() : request_line_end + CRLF.size();
  while (line_start < headers.size())
  {
    const size_t line_end = headers.find(CRLF, line_start);
    const std::string_view line = line_end == std::string_view::npos ?
                                      headers.substr(line_start) :
                                      headers.substr(line_start, line_end - line_start);
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos)
    {
      result.status = ParseStatus::Malformed;
      return result;
    }

    if (ToLower(line.substr(0, colon)) == "host")
      result.request.host = Trim(line.substr(colon + 1));
    else if (ToLower(line.substr(0, colon)) == "content-length")
      result.request.has_request_body = Trim(line.substr(colon + 1)) != "0";
    else if (ToLower(line.substr(0, colon)) == "transfer-encoding")
    {
      result.request.has_chunked_body =
          ToLower(Trim(line.substr(colon + 1))).find("chunked") != std::string::npos;
    }

    if (line_end == std::string_view::npos)
      break;
    line_start = line_end + CRLF.size();
  }

  result.status = ParseStatus::Complete;
  return result;
}

bool IsHostAllowed(const std::optional<std::string>& host, const std::string_view version)
{
  if (!host)
    return version == "HTTP/1.0";

  const std::string lower = ToLower(*host);
  return lower == "127.0.0.1" || lower == "127.0.0.1:32123" || lower == "localhost" ||
         lower == "localhost:32123";
}

std::optional<Route> RouteForTarget(const std::string_view target)
{
  if (target.find('?') != std::string_view::npos || target.find('#') != std::string_view::npos)
    return std::nullopt;
  if (target == "/")
    return Route::Dashboard;
  if (target == "/app.js")
    return Route::JavaScript;
  if (target == "/style.css")
    return Route::Stylesheet;
  if (target == "/api/v1/health")
    return Route::Health;
  if (target == "/api/v1/snapshot")
    return Route::Snapshot;
  if (target == "/api/v1/events")
    return Route::Events;
  return std::nullopt;
}

std::string HttpStatusText(const int status_code)
{
  switch (status_code)
  {
  case 200:
    return "OK";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 431:
    return "Request Header Fields Too Large";
  default:
    return "Bad Request";
  }
}

std::vector<std::pair<std::string, std::string>> DashboardSecurityHeaders(const bool html)
{
  std::vector<std::pair<std::string, std::string>> headers = {
      {"X-Content-Type-Options", "nosniff"},
      {"Referrer-Policy", "no-referrer"},
      {"Cache-Control", "no-store"},
  };

  if (html)
  {
    headers.emplace_back(
        "Content-Security-Policy",
        "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; "
        "img-src 'self' data:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
  }

  return headers;
}

std::string BuildHttpResponse(const int status_code, const std::string_view content_type,
                              const std::string_view body,
                              const std::vector<std::pair<std::string, std::string>>& headers)
{
  std::ostringstream response;
  response << "HTTP/1.1 " << status_code << ' ' << HttpStatusText(status_code) << "\r\n";
  response << "Content-Type: " << content_type << "\r\n";
  response << "Content-Length: " << ToHttpDecimal(body.size()) << "\r\n";
  response << "Connection: close\r\n";
  for (const auto& [name, value] : headers)
  {
    if (IsCallerHeaderAllowed(name))
      response << name << ": " << value << "\r\n";
  }
  response << "\r\n";
  response << body;
  return response.str();
}

std::string BuildSseHeaders()
{
  return "HTTP/1.1 200 OK\r\n"
         "Content-Type: text/event-stream; charset=utf-8\r\n"
         "Cache-Control: no-store\r\n"
         "Connection: keep-alive\r\n"
         "X-Accel-Buffering: no\r\n"
         "X-Content-Type-Options: nosniff\r\n"
         "Referrer-Policy: no-referrer\r\n"
         "\r\n";
}

std::string BuildSseEvent(const std::string_view event_name, const std::string_view json)
{
  std::string event;
  event.reserve(event_name.size() + json.size() + 16);
  event.append("event: ");
  event.append(event_name);
  event.append("\n");
  event.append("data: ");
  event.append(json);
  event.append("\n\n");
  return event;
}

std::string BuildSseKeepalive()
{
  return ": keepalive\n\n";
}
}  // namespace CheevoMap::LocalDashboard
