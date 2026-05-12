// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/CheevoMap/CheevoMapWindow.h"

#include <string>
#include <vector>

#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QPixmap>
#include <QScrollArea>
#include <QSizePolicy>
#include <QVBoxLayout>

#include "Core/CheevoMap/CheevoMapEntry.h"
#include "Core/CheevoMap/CheevoMapManager.h"

#include "DolphinQt/QtUtils/QueueOnObject.h"

CheevoMapWindow::CheevoMapWindow(QWidget* parent) : QDialog(parent)
{
  setWindowTitle(tr("CheevoMap"));
  setMinimumSize(360, 480);
  resize(440, 640);

  CreateMainLayout();

  m_event_hook = CheevoMap::Manager::GetInstance().RegisterUpdatedCallback(
      [this] { QueueOnObject(this, [this] { UpdateData(); }); });

  UpdateData();
}

void CheevoMapWindow::CreateMainLayout()
{
  m_title = new QLabel(this);
  QFont title_font = m_title->font();
  if (title_font.pointSize() > 0)
    title_font.setPointSize(title_font.pointSize() + 3);
  title_font.setBold(true);
  m_title->setFont(title_font);
  m_title->setWordWrap(true);

  m_status = new QLabel(this);
  m_status->setWordWrap(true);
  m_status->setAlignment(Qt::AlignCenter);
  m_status->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

  m_entries_container = new QWidget(this);
  m_entries_layout = new QVBoxLayout(m_entries_container);
  m_entries_layout->setContentsMargins(0, 0, 0, 0);
  m_entries_layout->setSpacing(4);
  m_entries_layout->addStretch();

  m_scroll_area = new QScrollArea(this);
  m_scroll_area->setWidgetResizable(true);
  m_scroll_area->setFrameShape(QFrame::NoFrame);
  m_scroll_area->setWidget(m_entries_container);

  auto* layout = new QVBoxLayout(this);
  layout->addWidget(m_title);
  layout->addWidget(m_status);
  layout->addWidget(m_scroll_area);
}

QLabel* CheevoMapWindow::CreateIconLabel(const std::string& path, int size) const
{
  auto* icon = new QLabel;
  icon->setFixedSize(size, size);
  icon->setAlignment(Qt::AlignCenter);
  icon->setScaledContents(false);

  const QPixmap pixmap(QString::fromStdString(path));
  if (!pixmap.isNull())
  {
    icon->setPixmap(pixmap.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
  }

  return icon;
}

QWidget* CheevoMapWindow::CreateEntryWidget(const CheevoMap::LiveValue& value)
{
  auto* row = new QWidget(m_entries_container);
  row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

  auto* row_layout = new QHBoxLayout(row);
  row_layout->setContentsMargins(8, 6, 8, 6);
  row_layout->setSpacing(8);

  if (!value.icon_path.empty())
    row_layout->addWidget(CreateIconLabel(value.icon_path, 32), 0, Qt::AlignTop);

  if (!value.icon_slots.empty())
  {
    auto* slots = new QWidget(row);
    auto* slots_layout = new QHBoxLayout(slots);
    slots_layout->setContentsMargins(0, 0, 0, 0);
    slots_layout->setSpacing(2);
    for (const std::string& path : value.icon_slots)
      slots_layout->addWidget(CreateIconLabel(path, 24));
    row_layout->addWidget(slots, 0, Qt::AlignTop);
  }

  auto* text_container = new QWidget(row);
  auto* text_layout = new QVBoxLayout(text_container);
  text_layout->setContentsMargins(0, 0, 0, 0);
  text_layout->setSpacing(2);

  auto* label = new QLabel(QString::fromStdString(value.label), text_container);
  label->setWordWrap(true);

  auto* value_label = new QLabel(QString::fromStdString(value.value_str), text_container);
  QFont value_font = value_label->font();
  value_font.setBold(true);
  value_label->setFont(value_font);
  value_label->setWordWrap(true);

  text_layout->addWidget(label);
  text_layout->addWidget(value_label);
  row_layout->addWidget(text_container, 1);

  return row;
}

void CheevoMapWindow::ClearEntries()
{
  while (QLayoutItem* item = m_entries_layout->takeAt(0))
  {
    delete item->widget();
    delete item;
  }
}

void CheevoMapWindow::UpdateData()
{
  const bool loaded = CheevoMap::Manager::GetInstance().IsLoaded();
  const std::string title = CheevoMap::Manager::GetInstance().GetCurrentTitle();
  const std::vector<CheevoMap::LiveValue> snapshot =
      CheevoMap::Manager::GetInstance().GetSnapshot();

  m_title->setText(title.empty() ? tr("CheevoMap") : QString::fromStdString(title));

  ClearEntries();

  int visible_count = 0;
  for (const CheevoMap::LiveValue& value : snapshot)
  {
    if (!value.visible)
      continue;

    m_entries_layout->addWidget(CreateEntryWidget(value));
    ++visible_count;
  }
  m_entries_layout->addStretch();

  if (!loaded)
  {
    m_status->setText(tr("No CheevoMap is loaded for the current game."));
  }
  else if (visible_count == 0)
  {
    m_status->setText(tr("No CheevoMap entries are currently visible."));
  }
  else
  {
    m_status->clear();
  }

  m_status->setVisible(!m_status->text().isEmpty());
  m_scroll_area->setVisible(visible_count > 0);
}
