// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"
#include "Core/CheevoMap/V2/StateValue.h"

namespace CheevoMap::V2
{
using StateValueMap = std::map<std::string, StateValue>;

struct StateSnapshot
{
  u64 session_id = 0;
  u64 sequence = 0;
  StateValueMap values;
};

struct StateUpdate
{
  u64 session_id = 0;
  u64 sequence = 0;
  bool full = false;
  StateValueMap values;
  std::vector<std::string> removed;
};

enum class StateApplyStatus : u8
{
  Applied,
  NoChanges,
  StaleSession,
};

struct StateApplyResult
{
  StateApplyStatus status = StateApplyStatus::NoChanges;
  std::optional<StateUpdate> update;
};

class StateStore final
{
public:
  StateSnapshot GetSnapshot() const;

  StateUpdate Reset(StateValueMap values = {});
  StateUpdate ResetDeferred(StateValueMap values);
  std::optional<StateUpdate> ApplyChanges(StateValueMap values,
                                          std::vector<std::string> removed = {});
  StateApplyResult ApplyChangesForSession(u64 expected_session_id, StateValueMap values,
                                          std::vector<std::string> removed = {});
  StateApplyResult ApplyChangesForSessionDeferred(u64 expected_session_id, StateValueMap values,
                                                  std::vector<std::string> removed = {});
  void DispatchPendingUpdates();

  Common::EventHook RegisterUpdateCallback(std::function<void(const StateUpdate&)> callback);

private:
  StateApplyResult ApplyChangesInternal(std::optional<u64> expected_session_id,
                                        StateValueMap values, std::vector<std::string> removed);
  void TriggerUpdate(const StateUpdate& update);

  mutable std::mutex m_mutex;
  u64 m_session_id = 0;
  u64 m_sequence = 0;
  StateValueMap m_values;
  std::deque<StateUpdate> m_pending_updates;
  bool m_dispatching = false;
  Common::HookableEvent<StateUpdate> m_update_event;
};
}  // namespace CheevoMap::V2
