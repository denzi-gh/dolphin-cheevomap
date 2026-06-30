// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/CheevoMap/CheevoMapV2LocalDashboard.h"

#include <utility>

#include <QDesktopServices>
#include <QFile>
#include <QIODevice>
#include <QUrl>

#include "Core/CheevoMap/CheevoMapManager.h"
#include "Core/CheevoMap/V2/DashboardProtocol.h"
#include "DolphinQt/CheevoMap/CheevoMapV2LocalDashboardServer.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"

namespace
{
bool ReadResource(const char* path, std::string* output, std::string* error)
{
  QFile file(QString::fromUtf8(path));
  if (!file.open(QIODevice::ReadOnly))
  {
    if (error)
      *error = std::string("Could not open dashboard resource: ") + path;
    return false;
  }

  const QByteArray data = file.readAll();
  output->assign(data.constData(), static_cast<size_t>(data.size()));
  return true;
}

bool LoadAssets(CheevoMap::LocalDashboard::LocalDashboardServer::Assets* assets, std::string* error)
{
  return ReadResource(":/cheevomap-v2-dashboard/Dashboard/index.html", &assets->html, error) &&
         ReadResource(":/cheevomap-v2-dashboard/Dashboard/app.js", &assets->javascript, error) &&
         ReadResource(":/cheevomap-v2-dashboard/Dashboard/style.css", &assets->stylesheet, error);
}

void ApplyUpdateToSnapshot(CheevoMap::V2::StateSnapshot* snapshot,
                           const CheevoMap::V2::StateUpdate& update)
{
  snapshot->session_id = update.session_id;
  snapshot->sequence = update.sequence;

  if (update.full)
  {
    snapshot->values = update.values;
    return;
  }

  for (const std::string& id : update.removed)
    snapshot->values.erase(id);
  for (const auto& [id, value] : update.values)
    snapshot->values[id] = value;
}
}  // namespace

CheevoMapV2LocalDashboard::CheevoMapV2LocalDashboard(QObject* parent) : QObject(parent)
{
}

CheevoMapV2LocalDashboard::~CheevoMapV2LocalDashboard()
{
  Stop();
}

bool CheevoMapV2LocalDashboard::Start(std::string* error)
{
  if (IsRunning())
    return true;

  CheevoMap::LocalDashboard::LocalDashboardServer::Assets assets;
  if (!LoadAssets(&assets, error))
    return false;

  m_snapshot = CheevoMap::Manager::GetInstance().GetV2StateSnapshot();

  auto server = std::make_unique<CheevoMap::LocalDashboard::LocalDashboardServer>();
  if (!server->Start({}, std::move(assets), CheevoMap::V2::SerializeStateSnapshot(m_snapshot),
                     error))
  {
    return false;
  }

  m_server = std::move(server);
  m_event_hook = CheevoMap::Manager::GetInstance().RegisterV2StateUpdatedCallback(
      [this](const CheevoMap::V2::StateUpdate& update) {
        QueueOnObject(this, [this, update] { OnStateUpdate(update); });
      });

  QDesktopServices::openUrl(QUrl(QStringLiteral("http://127.0.0.1:32123/")));
  return true;
}

void CheevoMapV2LocalDashboard::Stop()
{
  m_event_hook = {};
  if (m_server)
  {
    m_server->Stop();
    m_server.reset();
  }
}

bool CheevoMapV2LocalDashboard::IsRunning() const
{
  return m_server && m_server->IsRunning();
}

void CheevoMapV2LocalDashboard::OnStateUpdate(CheevoMap::V2::StateUpdate update)
{
  if (!IsRunning())
    return;

  ApplyUpdateToSnapshot(&m_snapshot, update);
  m_server->PublishSnapshotAndUpdate(CheevoMap::V2::SerializeStateSnapshot(m_snapshot),
                                     CheevoMap::V2::SerializeStateUpdate(update));
}
