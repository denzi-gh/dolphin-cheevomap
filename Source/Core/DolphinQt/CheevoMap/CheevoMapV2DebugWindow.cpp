// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/CheevoMap/CheevoMapV2DebugWindow.h"

#include <string>

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "Core/CheevoMap/CheevoMapManager.h"
#include "Core/CheevoMap/V2/StateStore.h"
#include "Core/CheevoMap/V2/StateValue.h"

#include "DolphinQt/QtUtils/QueueOnObject.h"

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
  m_state_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_state_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
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

  auto* main_layout = new QVBoxLayout(this);
  main_layout->addWidget(m_summary);
  main_layout->addWidget(m_status);
  main_layout->addWidget(m_state_table);
  main_layout->addWidget(log_header);
  main_layout->addWidget(m_update_log);

  connect(clear_log_button, &QPushButton::clicked, m_update_log, &QPlainTextEdit::clear);

  m_event_hook = CheevoMap::Manager::GetInstance().RegisterV2StateUpdatedCallback(
      [this](const CheevoMap::V2::StateUpdate& update) {
        QueueOnObject(this, [this, update] { HandleUpdate(update); });
      });

  RefreshSnapshot();
}

void CheevoMapV2DebugWindow::RefreshSnapshot()
{
  const bool loaded = CheevoMap::Manager::GetInstance().IsLoaded();
  const CheevoMap::V2::StateSnapshot snapshot =
      CheevoMap::Manager::GetInstance().GetV2StateSnapshot();

  m_summary->setText(QStringLiteral("Loaded: %1 | Session: %2 | Sequence: %3 | Values: %4")
                         .arg(loaded ? QStringLiteral("yes") : QStringLiteral("no"))
                         .arg(static_cast<qulonglong>(snapshot.session_id))
                         .arg(static_cast<qulonglong>(snapshot.sequence))
                         .arg(static_cast<qulonglong>(snapshot.values.size())));

  m_status->setText(QStringLiteral("No typed state values are currently available."));
  m_status->setVisible(snapshot.values.empty());

  m_state_table->setRowCount(static_cast<int>(snapshot.values.size()));

  int row = 0;
  for (const auto& [id, value] : snapshot.values)
  {
    m_state_table->setItem(row, 0, MakeTableItem(QString::fromStdString(id)));
    m_state_table->setItem(row, 1, MakeTableItem(FormatType(value.GetType())));
    m_state_table->setItem(row, 2, MakeTableItem(FormatValue(value)));
    ++row;
  }

  m_state_table->resizeColumnToContents(0);
  m_state_table->resizeColumnToContents(1);
}

void CheevoMapV2DebugWindow::HandleUpdate(const CheevoMap::V2::StateUpdate& update)
{
  AppendUpdateLog(update);
  RefreshSnapshot();
}

void CheevoMapV2DebugWindow::AppendUpdateLog(const CheevoMap::V2::StateUpdate& update)
{
  QStringList lines;
  lines << QStringLiteral("session=%1 sequence=%2 full=%3 changed=%4 removed=%5")
               .arg(static_cast<qulonglong>(update.session_id))
               .arg(static_cast<qulonglong>(update.sequence))
               .arg(update.full ? QStringLiteral("true") : QStringLiteral("false"))
               .arg(static_cast<qulonglong>(update.values.size()))
               .arg(static_cast<qulonglong>(update.removed.size()));

  for (const auto& [id, value] : update.values)
  {
    lines << QStringLiteral("  + %1 [%2] = %3")
                 .arg(QString::fromStdString(id), FormatType(value.GetType()), FormatValue(value));
  }

  for (const std::string& id : update.removed)
    lines << QStringLiteral("  - %1").arg(QString::fromStdString(id));

  if (!m_update_log->toPlainText().isEmpty())
    m_update_log->appendPlainText(QString{});

  m_update_log->appendPlainText(lines.join(QStringLiteral("\n")));
}
