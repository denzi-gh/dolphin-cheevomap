// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "DolphinQt/CheevoMap/CheevoMapV2LocalDashboardProtocol.h"

namespace CheevoMap::LocalDashboard
{
class LocalDashboardServer final
{
public:
  struct Assets
  {
    std::string html;
    std::string javascript;
    std::string stylesheet;
  };

  struct Options
  {
    std::string bind_address = std::string(kBindAddress);
    std::uint16_t port = kPort;
    int select_timeout_ms = 50;
    int keepalive_interval_ms = 15000;
  };

  LocalDashboardServer();
  ~LocalDashboardServer();

  LocalDashboardServer(const LocalDashboardServer&) = delete;
  LocalDashboardServer& operator=(const LocalDashboardServer&) = delete;

  bool Start(Options options, Assets assets, std::string snapshot_json, std::string* error);
  void Stop();
  bool IsRunning() const;
  std::uint16_t GetBoundPort() const;

  void PublishSnapshotAndUpdate(std::string snapshot_json, std::optional<std::string> update_json);

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
}  // namespace CheevoMap::LocalDashboard
