// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>

#include <QObject>

#include "Common/HookableEvent.h"
#include "Core/CheevoMap/V2/StateStore.h"

namespace CheevoMap::LocalDashboard
{
class LocalDashboardServer;
}

class CheevoMapV2LocalDashboard final : public QObject
{
public:
  explicit CheevoMapV2LocalDashboard(QObject* parent = nullptr);
  ~CheevoMapV2LocalDashboard() override;

  bool Start(std::string* error);
  void Stop();
  bool IsRunning() const;

private:
  void OnStateUpdate(CheevoMap::V2::StateUpdate update);

  Common::EventHook m_event_hook;
  CheevoMap::V2::StateSnapshot m_snapshot;
  std::unique_ptr<CheevoMap::LocalDashboard::LocalDashboardServer> m_server;
};
