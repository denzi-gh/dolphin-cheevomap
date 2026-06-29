// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDialog>

#include "Common/HookableEvent.h"

class QLabel;
class QPlainTextEdit;
class QTableWidget;
class QWidget;

namespace CheevoMap::V2
{
struct StateUpdate;
}

class CheevoMapV2DebugWindow final : public QDialog
{
  Q_OBJECT

public:
  explicit CheevoMapV2DebugWindow(QWidget* parent = nullptr);

  void RefreshSnapshot();

private:
  void HandleUpdate(const CheevoMap::V2::StateUpdate& update);
  void AppendUpdateLog(const CheevoMap::V2::StateUpdate& update);

  QLabel* m_summary = nullptr;
  QLabel* m_status = nullptr;
  QTableWidget* m_state_table = nullptr;
  QPlainTextEdit* m_update_log = nullptr;

  Common::EventHook m_event_hook;
};
