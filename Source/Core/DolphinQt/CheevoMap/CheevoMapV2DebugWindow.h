// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include <QDialog>
#include <QElapsedTimer>

#include "Common/HookableEvent.h"
#include "Core/CheevoMap/V2/StateStore.h"

class QHideEvent;
class QLabel;
class QPlainTextEdit;
class QShowEvent;
class QTableWidget;
class QTableWidgetItem;
class QTimer;
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
  struct RowItems
  {
    QTableWidgetItem* type = nullptr;
    QTableWidgetItem* value = nullptr;
  };

  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

  void HandleUpdate(const CheevoMap::V2::StateUpdate& update);
  void OnRefreshTimer();
  void RenderSnapshot(const CheevoMap::V2::StateSnapshot& snapshot, bool loaded);
  void RebuildStateTable(const CheevoMap::V2::StateSnapshot& snapshot);
  bool NeedsFullTableRebuild(const CheevoMap::V2::StateSnapshot& snapshot) const;
  void AppendPendingUpdateLog();
  void AppendUpdateLog(const CheevoMap::V2::StateUpdate& update, u64 coalesced_updates);
  void DiscardPendingUpdateLog();

  QLabel* m_summary = nullptr;
  QLabel* m_status = nullptr;
  QTableWidget* m_state_table = nullptr;
  QPlainTextEdit* m_update_log = nullptr;
  QTimer* m_refresh_timer = nullptr;

  Common::EventHook m_event_hook;

  std::atomic_bool m_state_dirty{false};

  std::mutex m_pending_update_mutex;
  std::optional<CheevoMap::V2::StateUpdate> m_latest_pending_update;
  u64 m_pending_update_count = 0;

  QElapsedTimer m_log_elapsed;
  CheevoMap::V2::StateSnapshot m_displayed_snapshot;
  bool m_has_displayed_snapshot = false;
  std::map<std::string, RowItems> m_rows;
  bool m_log_has_content = false;
};
