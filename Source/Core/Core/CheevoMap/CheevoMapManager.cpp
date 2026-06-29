// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CheevoMap/CheevoMapManager.h"

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/CheevoMap/CheevoMapEvaluator.h"
#include "Core/CheevoMap/Dolphin/DolphinEmulatorDataSource.h"
#include "Core/CheevoMap/CheevoMapFile.h"
#include "Core/CheevoMap/V2/Runtime.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/System.h"

#ifdef USE_RETRO_ACHIEVEMENTS
#include <rcheevos/include/rc_client.h>
#include "Core/AchievementManager.h"
#endif

namespace CheevoMap
{
namespace
{
#ifdef USE_RETRO_ACHIEVEMENTS
class RetroAchievementStateSource final : public AchievementStateSource
{
public:
  std::optional<bool> IsAchievementUnlocked(u32 achievement_id,
                                            AchievementMode mode) const override
  {
    auto* client = AchievementManager::GetInstance().GetClient();
    if (!client)
      return std::nullopt;

    const rc_client_achievement_t* info = rc_client_get_achievement_info(client, achievement_id);
    if (!info)
      return std::nullopt;

    if (mode == AchievementMode::Hardcore)
      return (info->unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE) != 0;

    return (info->unlocked & (RC_CLIENT_ACHIEVEMENT_UNLOCKED_SOFTCORE |
                              RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE)) != 0;
  }
};
#endif

std::string ResolveJsonPath(const std::string& game_id)
{
  const std::string base = ::File::GetUserPath(D_CHEEVOMAP_IDX);
  // Preferred per-game directory layout
  const std::string dir_form = base + game_id + DIR_SEP "cheevomap.json";
  if (::File::Exists(dir_form))
    return dir_form;
  // Flat fallback
  const std::string flat = base + game_id + ".json";
  if (::File::Exists(flat))
    return flat;
  return {};
}
}  // namespace

Manager& Manager::GetInstance()
{
  static Manager s_instance;
  return s_instance;
}

void Manager::OnTitleBooted(const Core::CPUThreadGuard& guard)
{
  (void)guard;
  const std::string game_id = SConfig::GetInstance().GetGameID();
  LoadForGameId(game_id);
}

void Manager::OnESTitleChanged()
{
  const std::string game_id = SConfig::GetInstance().GetGameID();
  LoadForGameId(game_id);
}

void Manager::CloseGame()
{
  {
    std::lock_guard lg(m_lock);
    m_file.reset();
    m_v2_package.reset();
    m_live.clear();
    m_loaded_game_id.clear();
    ++m_generation;
    m_last_poll = {};
  }
  m_v2_state.Reset({});
  m_updated_event.Trigger();
}

void Manager::LoadForGameId(const std::string& game_id)
{
  {
    std::lock_guard lg(m_lock);
    if (m_loaded_game_id == game_id && (m_file.has_value() || m_v2_package.has_value()))
      return;

    m_file.reset();
    m_v2_package.reset();
    m_live.clear();
    m_loaded_game_id = game_id;
    ++m_generation;
    m_last_poll = {};
  }

  if (game_id.empty())
  {
    m_v2_state.Reset({});
    m_updated_event.Trigger();
    return;
  }

  const std::string path = ResolveJsonPath(game_id);
  if (path.empty())
  {
    INFO_LOG_FMT(CORE, "CheevoMap: no file found for game id '{}'", game_id);
    m_v2_state.Reset({});
    m_updated_event.Trigger();
    return;
  }

  std::string err;
  auto loaded = LoadPackageFromFile(path, &err);
  if (!loaded)
  {
    WARN_LOG_FMT(CORE, "CheevoMap: failed to load '{}': {}", path, err);
    m_v2_state.Reset({});
    m_updated_event.Trigger();
    return;
  }

  if (auto* v1 = std::get_if<File>(&*loaded))
  {
    if (v1->game_id != game_id)
    {
      WARN_LOG_FMT(CORE, "CheevoMap: file '{}' game_id '{}' does not match running game '{}'",
                   path, v1->game_id, game_id);
      m_v2_state.Reset({});
      m_updated_event.Trigger();
      return;
    }

    std::vector<LiveValue> live_values;
    V2::StateValueMap initial_state;
    live_values.resize(v1->entries.size());
    for (size_t i = 0; i < v1->entries.size(); ++i)
    {
      const auto& def = v1->entries[i];
      live_values[i] = MakeInitialLiveValue(def);
      initial_state.emplace(def.id, V2::StateValue::Unavailable());
    }

    const std::string title = v1->title;
    const size_t entry_count = v1->entries.size();
    {
      std::lock_guard lg(m_lock);
      if (m_loaded_game_id != game_id)
        return;
      m_live = std::move(live_values);
      m_file = std::move(*v1);
    }
    m_v2_state.Reset(std::move(initial_state));
    INFO_LOG_FMT(CORE, "CheevoMap: loaded '{}' ({} schema v1 entries) from {}", title,
                 entry_count, path);
    m_updated_event.Trigger();
    return;
  }

  auto* v2 = std::get_if<V2::Package>(&*loaded);
  if (v2 == nullptr)
    return;

  if (v2->game.id != game_id)
  {
    WARN_LOG_FMT(CORE, "CheevoMap: file '{}' game id '{}' does not match running game '{}'",
                 path, v2->game.id, game_id);
    m_v2_state.Reset({});
    m_updated_event.Trigger();
    return;
  }

  V2::StateValueMap initial_state;
  for (const V2::ValueDefinition& value : v2->values)
    initial_state.emplace(value.id, V2::StateValue::Unavailable());

  const std::string title = v2->metadata.title;
  const size_t value_count = v2->values.size();
  {
    std::lock_guard lg(m_lock);
    if (m_loaded_game_id != game_id)
      return;
    m_live.clear();
    m_v2_package = std::move(*v2);
  }
  m_v2_state.Reset(std::move(initial_state));
  INFO_LOG_FMT(CORE, "CheevoMap: loaded '{}' ({} schema v2 values) from {}", title, value_count,
               path);
  m_updated_event.Trigger();
}

void Manager::DoFrame()
{
  if (!Core::IsCPUThread())
    return;

  const std::string game_id = SConfig::GetInstance().GetGameID();

  std::string loaded_id;
  {
    std::lock_guard lg(m_lock);
    loaded_id = m_loaded_game_id;
  }
  if (game_id.empty() && !loaded_id.empty())
  {
    CloseGame();
    return;
  }
  if (!game_id.empty() && game_id != loaded_id)
  {
    LoadForGameId(game_id);
  }

  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard lg(m_lock);
    if (!m_file && !m_v2_package)
      return;
    const double poll_hz = m_file ? m_file->poll_hz : m_v2_package->poll_hz;
    const double interval_ms = 1000.0 / poll_hz;
    if (m_last_poll != std::chrono::steady_clock::time_point{} &&
        std::chrono::duration<double, std::milli>(now - m_last_poll).count() < interval_ms)
    {
      return;
    }
    m_last_poll = now;
  }

  Core::CPUThreadGuard guard(Core::System::GetInstance());
  Evaluate(&guard);
}

void Manager::Evaluate(const Core::CPUThreadGuard* guard)
{
  if (!guard)
    return;

  bool use_v2 = false;
  {
    std::lock_guard lg(m_lock);
    use_v2 = m_v2_package.has_value();
  }

  if (use_v2)
    EvaluateV2(*guard);
  else
    EvaluateV1(*guard);
}

void Manager::EvaluateV1(const Core::CPUThreadGuard& guard)
{
  u64 generation = 0;
  u64 v2_session_id = 0;
  std::vector<EntryDef> entries;
  std::vector<LiveValue> live_values;
  {
    std::lock_guard lg(m_lock);
    if (!m_file)
      return;

    generation = m_generation;
    entries = m_file->entries;
    live_values = m_live;
  }
  v2_session_id = m_v2_state.GetSnapshot().session_id;

  if (entries.size() != live_values.size())
    return;

  bool any_changed = false;
  V2::StateValueMap v2_values;
  std::vector<std::string> v2_removed;
  Dolphin::DolphinEmulatorDataSource data_source(guard);
#ifdef USE_RETRO_ACHIEVEMENTS
  RetroAchievementStateSource achievements;
  const AchievementStateSource* achievement_source = &achievements;
#else
  const AchievementStateSource* achievement_source = nullptr;
#endif

  for (size_t i = 0; i < entries.size(); ++i)
  {
    const auto& def = entries[i];
    auto& live = live_values[i];
    const std::string previous = live.value_str;
    const bool previous_visible = live.visible;

    V2::StateValue state_value;
    const EvaluationStatus status =
        EvaluateEntry(def, data_source, achievement_source, &live, &state_value);

    if (status == EvaluationStatus::Hidden)
      v2_removed.push_back(def.id);
    else
      v2_values.emplace(def.id, std::move(state_value));

    if (live.value_str != previous || previous_visible != live.visible)
      any_changed = true;
  }

  {
    std::lock_guard lg(m_lock);
    if (!m_file || m_generation != generation || m_file->entries.size() != entries.size())
      return;

    m_live = std::move(live_values);
  }

  const V2::StateApplyResult v2_result =
      m_v2_state.ApplyChangesForSession(v2_session_id, std::move(v2_values),
                                        std::move(v2_removed));
  if (v2_result.status == V2::StateApplyStatus::StaleSession)
    return;

  if (any_changed)
    m_updated_event.Trigger();
}

void Manager::EvaluateV2(const Core::CPUThreadGuard& guard)
{
  u64 generation = 0;
  u64 v2_session_id = 0;
  V2::Package package;
  {
    std::lock_guard lg(m_lock);
    if (!m_v2_package)
      return;

    generation = m_generation;
    package = *m_v2_package;
  }
  v2_session_id = m_v2_state.GetSnapshot().session_id;

  Dolphin::DolphinEmulatorDataSource data_source(guard);
  std::string error;
  const auto result = V2::EvaluatePackage(package, data_source, &error);
  if (!result)
  {
    WARN_LOG_FMT(CORE, "CheevoMap: schema v2 evaluation failed: {}", error);
    return;
  }

  {
    std::lock_guard lg(m_lock);
    if (!m_v2_package || m_generation != generation)
      return;
  }

  const V2::StateApplyResult v2_result =
      m_v2_state.ApplyChangesForSession(v2_session_id, result->values);
  if (v2_result.status == V2::StateApplyStatus::StaleSession)
    return;
}

bool Manager::IsLoaded() const
{
  std::lock_guard lg(m_lock);
  return m_file.has_value() || m_v2_package.has_value();
}

std::string Manager::GetCurrentTitle() const
{
  std::lock_guard lg(m_lock);
  if (m_file)
    return m_file->title;
  if (m_v2_package)
    return m_v2_package->metadata.title;
  return {};
}

std::vector<LiveValue> Manager::GetSnapshot() const
{
  std::lock_guard lg(m_lock);
  return m_live;
}

V2::StateSnapshot Manager::GetV2StateSnapshot() const
{
  return m_v2_state.GetSnapshot();
}

Common::EventHook Manager::RegisterUpdatedCallback(std::function<void()> cb)
{
  return m_updated_event.Register(std::move(cb));
}

Common::EventHook Manager::RegisterV2StateUpdatedCallback(
    std::function<void(const V2::StateUpdate&)> cb)
{
  return m_v2_state.RegisterUpdateCallback(std::move(cb));
}

}  // namespace CheevoMap
