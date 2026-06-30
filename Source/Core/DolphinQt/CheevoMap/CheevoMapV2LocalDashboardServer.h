// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "Core/CheevoMap/V2/DashboardProtocol.h"
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
    std::uint16_t port = kPort;
    int select_timeout_ms = 50;
    int keepalive_interval_ms = 15000;
  };

  struct SerializedSnapshot
  {
    CheevoMap::V2::StateCursor cursor;
    std::string json;
  };

  struct SerializedUpdate
  {
    CheevoMap::V2::StateCursor cursor;
    std::string json;
  };

  LocalDashboardServer();
  ~LocalDashboardServer();

  LocalDashboardServer(const LocalDashboardServer&) = delete;
  LocalDashboardServer& operator=(const LocalDashboardServer&) = delete;

  bool Start(Options options, Assets assets, SerializedSnapshot initial_snapshot,
             std::string* error);
  void Stop();
  bool IsRunning() const;
  std::uint16_t GetBoundPort() const;
  std::uint32_t GetBoundIPv4AddressForTesting() const;

  void PublishSnapshotAndUpdate(SerializedSnapshot snapshot,
                                std::optional<SerializedUpdate> update);
  void PublishAuthoritativeSnapshot(SerializedSnapshot snapshot);

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
}  // namespace CheevoMap::LocalDashboard
