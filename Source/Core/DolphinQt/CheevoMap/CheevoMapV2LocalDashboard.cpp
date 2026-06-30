// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/CheevoMap/CheevoMapV2LocalDashboard.h"

#include <optional>
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

CheevoMap::LocalDashboard::LocalDashboardServer::SerializedSnapshot
SerializeSnapshot(const CheevoMap::V2::StateSnapshot& snapshot)
{
  return {CheevoMap::V2::CursorForSnapshot(snapshot),
          CheevoMap::V2::SerializeStateSnapshot(snapshot)};
}

CheevoMap::LocalDashboard::LocalDashboardServer::SerializedUpdate
SerializeUpdate(const CheevoMap::V2::StateUpdate& update)
{
  return {CheevoMap::V2::CursorForUpdate(update), CheevoMap::V2::SerializeStateUpdate(update)};
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

  Common::EventHook event_hook = CheevoMap::Manager::GetInstance().RegisterV2StateUpdatedCallback(
      [this](const CheevoMap::V2::StateUpdate& update) {
        QueueOnObject(this, [this, update] { OnStateUpdate(update); });
      });

  CheevoMap::V2::StateSnapshot snapshot = CheevoMap::Manager::GetInstance().GetV2StateSnapshot();

  auto server = std::make_unique<CheevoMap::LocalDashboard::LocalDashboardServer>();
  if (!server->Start({}, std::move(assets), SerializeSnapshot(snapshot), error))
  {
    return false;
  }

  m_snapshot = std::move(snapshot);
  m_server = std::move(server);
  m_event_hook = std::move(event_hook);

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

  const CheevoMap::V2::StateUpdateApplyResult result =
      CheevoMap::V2::ApplyStateUpdateToSnapshot(&m_snapshot, update);
  if (result == CheevoMap::V2::StateUpdateApplyResult::StaleOrDuplicate)
    return;

  if (result == CheevoMap::V2::StateUpdateApplyResult::InvalidSessionTransition)
  {
    m_snapshot = CheevoMap::Manager::GetInstance().GetV2StateSnapshot();
    m_server->PublishAuthoritativeSnapshot(SerializeSnapshot(m_snapshot));
    return;
  }

  m_server->PublishSnapshotAndUpdate(SerializeSnapshot(m_snapshot), SerializeUpdate(update));
}
