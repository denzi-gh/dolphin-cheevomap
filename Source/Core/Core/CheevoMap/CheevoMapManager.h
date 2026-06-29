// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"
#include "Core/CheevoMap/CheevoMapEntry.h"
#include "Core/CheevoMap/V2/StateStore.h"

namespace Core
{
class CPUThreadGuard;
}

namespace CheevoMap
{
class Manager
{
public:
  static Manager& GetInstance();

  // Lifecycle
  void OnTitleBooted(const Core::CPUThreadGuard& guard);
  void OnESTitleChanged();
  void CloseGame();

  // Per-frame; CPU thread; cheap when idle
  void DoFrame();

  // Thread-safe queries for the UI / JNI layers
  bool IsLoaded() const;
  std::string GetCurrentTitle() const;
  std::vector<LiveValue> GetSnapshot() const;
  V2::StateSnapshot GetV2StateSnapshot() const;

  // Fires whenever any v1 value_str changes (or the loaded file changes)
  Common::EventHook RegisterUpdatedCallback(std::function<void()> cb);

  // Fires whenever the v2 typed state store publishes a full or delta update
  Common::EventHook RegisterV2StateUpdatedCallback(
      std::function<void(const V2::StateUpdate&)> cb);

  Manager(const Manager&) = delete;
  Manager& operator=(const Manager&) = delete;

private:
  Manager() = default;
  ~Manager() = default;

  void LoadForGameId(const std::string& game_id);
  void EvaluateLocked(const Core::CPUThreadGuard* guard);

  mutable std::mutex m_lock;
  std::optional<File> m_file;
  std::vector<LiveValue> m_live;
  std::string m_loaded_game_id;
  std::chrono::steady_clock::time_point m_last_poll{};
  Common::HookableEvent<> m_updated_event;
  V2::StateStore m_v2_state;
};
}  // namespace CheevoMap
