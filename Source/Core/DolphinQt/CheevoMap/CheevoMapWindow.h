// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include <QDialog>

#include "Common/HookableEvent.h"

class QLabel;
class QScrollArea;
class QVBoxLayout;
class QWidget;

namespace CheevoMap
{
struct LiveValue;
}

class CheevoMapWindow final : public QDialog
{
  Q_OBJECT

public:
  explicit CheevoMapWindow(QWidget* parent = nullptr);

  void UpdateData();

private:
  void CreateMainLayout();
  QWidget* CreateEntryWidget(const CheevoMap::LiveValue& value);
  QLabel* CreateIconLabel(const std::string& path, int size) const;
  void ClearEntries();

  QLabel* m_title = nullptr;
  QLabel* m_status = nullptr;
  QScrollArea* m_scroll_area = nullptr;
  QWidget* m_entries_container = nullptr;
  QVBoxLayout* m_entries_layout = nullptr;

  Common::EventHook m_event_hook;
};
