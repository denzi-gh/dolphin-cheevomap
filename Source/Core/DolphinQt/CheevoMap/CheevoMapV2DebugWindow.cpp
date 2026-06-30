// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/CheevoMap/CheevoMapV2DebugWindow.h"

#include <string>

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include "Core/CheevoMap/CheevoMapManager.h"
#include "Core/CheevoMap/V2/StateStore.h"
#include "Core/CheevoMap/V2/StateValue.h"

namespace
{
QString FormatType(CheevoMap::V2::StateValueType type)
{
  switch (type)
  {
  case CheevoMap::V2::StateValueType::Unavailable:
    return QStringLiteral("unavailable");
  case CheevoMap::V2::StateValueType::Boolean:
    return QStringLiteral("bool");
  case CheevoMap::V2::StateValueType::SignedInteger:
    return QStringLiteral("s64");
  case CheevoMap::V2::StateValueType::UnsignedInteger:
    return QStringLiteral("u64");
  case CheevoMap::V2::StateValueType::FloatingPoint:
    return QStringLiteral("f64");
  case CheevoMap::V2::StateValueType::String:
    return QStringLiteral("string");
  }

  return QStringLiteral("unavailable");
}

QString EscapeString(const std::string& value)
{
  std::string escaped;
  escaped.reserve(value.size());

  for (const char c : value)
  {
    switch (c)
    {
    case '\\':
      escaped += "\\\\";
      break;
    case '"':
      escaped += "\\\"";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped += c;
      break;
    }
  }

  return QString::fromStdString(escaped);
}

QString FormatValue(const CheevoMap::V2::StateValue& value)
{
  switch (value.GetType())
  {
  case CheevoMap::V2::StateValueType::Unavailable:
    return QStringLiteral("<unavailable>");
  case CheevoMap::V2::StateValueType::Boolean:
    return value.AsBoolean().value_or(false) ? QStringLiteral("true") : QStringLiteral("false");
  case CheevoMap::V2::StateValueType::SignedInteger:
    return QString::number(static_cast<qlonglong>(value.AsSignedInteger().value_or(0)));
  case CheevoMap::V2::StateValueType::UnsignedInteger:
    return QString::number(static_cast<qulonglong>(value.AsUnsignedInteger().value_or(0)));
  case CheevoMap::V2::StateValueType::FloatingPoint:
    return QString::number(value.AsFloatingPoint().value_or(0.0), 'g', 17);
  case CheevoMap::V2::StateValueType::String:
  {
    const std::string* string = value.AsString();
    return QStringLiteral("\"") + (string ? EscapeString(*string) : QString{}) +
           QStringLiteral("\"");
  }
  }

  return QStringLiteral("<unavailable>");
}

QTableWidgetItem* MakeTableItem(const QString& text)
{
  auto* item = new QTableWidgetItem(text);
  item->setFlags(item->flags() & ~Qt::ItemIsEditable);
  return item;
}
}  // namespace

CheevoMapV2DebugWindow::CheevoMapV2DebugWindow(QWidget* parent) : QDialog(parent)
{
  setWindowTitle(tr("CheevoMap v2 State Inspector"));
  setMinimumSize(640, 480);
  resize(900, 650);

  m_summary = new QLabel(this);

  m_status = new QLabel(this);
  m_status->setWordWrap(true);

  m_state_table = new QTableWidget(this);
  m_state_table->setColumnCount(3);
  m_state_table->setHorizontalHeaderLabels(
      {QStringLiteral("ID"), QStringLiteral("Type"), QStringLiteral("Value")});
  m_state_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_state_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_state_table->setSortingEnabled(false);
  m_state_table->verticalHeader()->setVisible(false);
  m_state_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
  m_state_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
  m_state_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);

  auto* log_header = new QWidget(this);
  auto* log_header_layout = new QHBoxLayout(log_header);
  log_header_layout->setContentsMargins(0, 0, 0, 0);

  auto* log_label = new QLabel(tr("Update Log"), log_header);
  auto* clear_log_button = new QPushButton(tr("Clear Log"), log_header);
  log_header_layout->addWidget(log_label);
  log_header_layout->addStretch();
  log_header_layout->addWidget(clear_log_button);

  m_update_log = new QPlainTextEdit(this);
  m_update_log->setReadOnly(true);
  m_update_log->setMaximumBlockCount(500);

  m_refresh_timer = new QTimer(this);
  m_refresh_timer->setInterval(50);
  connect(m_refresh_timer, &QTimer::timeout, this, &CheevoMapV2DebugWindow::OnRefreshTimer);

  auto* main_layout = new QVBoxLayout(this);
  main_layout->addWidget(m_summary);
  main_layout->addWidget(m_status);
  main_layout->addWidget(m_state_table);
  main_layout->addWidget(log_header);
  main_layout->addWidget(m_update_log);

  connect(clear_log_button, &QPushButton::clicked, this, [this] {
    m_update_log->clear();
    m_log_has_content = false;
  });

  m_event_hook = CheevoMap::Manager::GetInstance().RegisterV2StateUpdatedCallback(
      [this](const CheevoMap::V2::StateUpdate& update) { HandleUpdate(update); });

  m_log_elapsed.start();
}

void CheevoMapV2DebugWindow::showEvent(QShowEvent* event)
{
  QDialog::showEvent(event);

  DiscardPendingUpdateLog();
  m_log_elapsed.restart();
  if (!m_refresh_timer->isActive())
    m_refresh_timer->start();

  RefreshSnapshot();
}

void CheevoMapV2DebugWindow::hideEvent(QHideEvent* event)
{
  if (m_refresh_timer->isActive())
    m_refresh_timer->stop();

  QDialog::hideEvent(event);
}

void CheevoMapV2DebugWindow::RefreshSnapshot()
{
  if (!isVisible())
  {
    m_state_dirty.store(true, std::memory_order_release);
    return;
  }

  m_state_dirty.exchange(false, std::memory_order_acq_rel);

  const bool loaded = CheevoMap::Manager::GetInstance().IsLoaded();
  const CheevoMap::V2::StateSnapshot snapshot =
      CheevoMap::Manager::GetInstance().GetV2StateSnapshot();

  RenderSnapshot(snapshot, loaded);
}

void CheevoMapV2DebugWindow::HandleUpdate(const CheevoMap::V2::StateUpdate& update)
{
  m_state_dirty.store(true, std::memory_order_release);

  std::lock_guard lg(m_pending_update_mutex);
  m_latest_pending_update = update;
  ++m_pending_update_count;
}

void CheevoMapV2DebugWindow::OnRefreshTimer()
{
  if (!isVisible())
    return;

  if (m_state_dirty.exchange(false, std::memory_order_acq_rel))
  {
    const bool loaded = CheevoMap::Manager::GetInstance().IsLoaded();
    const CheevoMap::V2::StateSnapshot snapshot =
        CheevoMap::Manager::GetInstance().GetV2StateSnapshot();
    RenderSnapshot(snapshot, loaded);
  }

  if (m_log_elapsed.elapsed() >= 200)
    AppendPendingUpdateLog();
}

void CheevoMapV2DebugWindow::RenderSnapshot(const CheevoMap::V2::StateSnapshot& snapshot,
                                            const bool loaded)
{
  m_summary->setText(QStringLiteral("Loaded: %1 | Session: %2 | Sequence: %3 | Values: %4")
                         .arg(loaded ? QStringLiteral("yes") : QStringLiteral("no"))
                         .arg(static_cast<qulonglong>(snapshot.session_id))
                         .arg(static_cast<qulonglong>(snapshot.sequence))
                         .arg(static_cast<qulonglong>(snapshot.values.size())));

  m_status->setText(QStringLiteral("No typed state values are currently available."));
  m_status->setVisible(snapshot.values.empty());

  if (NeedsFullTableRebuild(snapshot))
  {
    RebuildStateTable(snapshot);
  }
  else
  {
    for (const auto& [id, value] : snapshot.values)
    {
      const auto displayed_value = m_displayed_snapshot.values.find(id);
      const auto row = m_rows.find(id);
      if (displayed_value == m_displayed_snapshot.values.end() || row == m_rows.end() ||
          row->second.type == nullptr || row->second.value == nullptr)
      {
        RebuildStateTable(snapshot);
        m_displayed_snapshot = snapshot;
        m_has_displayed_snapshot = true;
        return;
      }

      if (displayed_value->second.GetType() != value.GetType())
        row->second.type->setText(FormatType(value.GetType()));

      if (displayed_value->second != value)
        row->second.value->setText(FormatValue(value));
    }
  }

  m_displayed_snapshot = snapshot;
  m_has_displayed_snapshot = true;
}

void CheevoMapV2DebugWindow::RebuildStateTable(const CheevoMap::V2::StateSnapshot& snapshot)
{
  m_rows.clear();
  m_state_table->setUpdatesEnabled(false);
  m_state_table->clearContents();
  m_state_table->setRowCount(static_cast<int>(snapshot.values.size()));

  int row = 0;
  for (const auto& [id, value] : snapshot.values)
  {
    auto* const id_item = MakeTableItem(QString::fromStdString(id));
    auto* const type_item = MakeTableItem(FormatType(value.GetType()));
    auto* const value_item = MakeTableItem(FormatValue(value));

    m_state_table->setItem(row, 0, id_item);
    m_state_table->setItem(row, 1, type_item);
    m_state_table->setItem(row, 2, value_item);
    m_rows.emplace(id, RowItems{type_item, value_item});
    ++row;
  }

  m_state_table->resizeColumnToContents(0);
  m_state_table->resizeColumnToContents(1);
  m_state_table->setUpdatesEnabled(true);
}

bool CheevoMapV2DebugWindow::NeedsFullTableRebuild(
    const CheevoMap::V2::StateSnapshot& snapshot) const
{
  if (!m_has_displayed_snapshot)
    return true;

  if (m_displayed_snapshot.session_id != snapshot.session_id)
    return true;

  if (m_displayed_snapshot.values.size() != snapshot.values.size())
    return true;

  auto displayed_value = m_displayed_snapshot.values.begin();
  auto new_value = snapshot.values.begin();
  for (; displayed_value != m_displayed_snapshot.values.end(); ++displayed_value, ++new_value)
  {
    if (displayed_value->first != new_value->first)
      return true;

    const auto row = m_rows.find(new_value->first);
    if (row == m_rows.end() || row->second.type == nullptr || row->second.value == nullptr)
      return true;
  }

  return false;
}

void CheevoMapV2DebugWindow::AppendPendingUpdateLog()
{
  std::optional<CheevoMap::V2::StateUpdate> update;
  u64 coalesced_updates = 0;

  {
    std::lock_guard lg(m_pending_update_mutex);
    if (!m_latest_pending_update || m_pending_update_count == 0)
      return;

    update = m_latest_pending_update;
    coalesced_updates = m_pending_update_count - 1;
    m_latest_pending_update.reset();
    m_pending_update_count = 0;
  }

  AppendUpdateLog(*update, coalesced_updates);
  m_log_elapsed.restart();
}

void CheevoMapV2DebugWindow::AppendUpdateLog(const CheevoMap::V2::StateUpdate& update,
                                             const u64 coalesced_updates)
{
  QStringList lines;
  lines << QStringLiteral("session=%1 sequence=%2 full=%3 changed=%4 removed=%5 coalesced=%6")
               .arg(static_cast<qulonglong>(update.session_id))
               .arg(static_cast<qulonglong>(update.sequence))
               .arg(update.full ? QStringLiteral("true") : QStringLiteral("false"))
               .arg(static_cast<qulonglong>(update.values.size()))
               .arg(static_cast<qulonglong>(update.removed.size()))
               .arg(static_cast<qulonglong>(coalesced_updates));

  for (const auto& [id, value] : update.values)
  {
    lines << QStringLiteral("  + %1 [%2] = %3")
                 .arg(QString::fromStdString(id), FormatType(value.GetType()), FormatValue(value));
  }

  for (const std::string& id : update.removed)
    lines << QStringLiteral("  - %1").arg(QString::fromStdString(id));

  if (m_log_has_content)
    m_update_log->appendPlainText(QString{});

  m_update_log->appendPlainText(lines.join(QStringLiteral("\n")));
  m_log_has_content = true;
}

void CheevoMapV2DebugWindow::DiscardPendingUpdateLog()
{
  std::lock_guard lg(m_pending_update_mutex);
  m_latest_pending_update.reset();
  m_pending_update_count = 0;
}
