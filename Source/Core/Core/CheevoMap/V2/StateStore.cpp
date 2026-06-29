// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CheevoMap/V2/StateStore.h"

#include <algorithm>
#include <utility>

namespace CheevoMap::V2
{
StateSnapshot StateStore::GetSnapshot() const
{
  std::lock_guard lg(m_mutex);
  return StateSnapshot{m_session_id, m_sequence, m_values};
}

StateUpdate StateStore::Reset(StateValueMap values)
{
  StateUpdate update = ResetDeferred(std::move(values));
  DispatchPendingUpdates();
  return update;
}

StateUpdate StateStore::ResetDeferred(StateValueMap values)
{
  StateUpdate update;
  {
    std::lock_guard lg(m_mutex);
    ++m_session_id;
    m_sequence = 1;
    m_values = std::move(values);
    update = StateUpdate{m_session_id, m_sequence, true, m_values, {}};
    m_pending_updates.push_back(update);
  }

  return update;
}

std::optional<StateUpdate> StateStore::ApplyChanges(StateValueMap values,
                                                    std::vector<std::string> removed)
{
  std::optional<StateUpdate> update =
      ApplyChangesInternal(std::nullopt, std::move(values), std::move(removed)).update;
  DispatchPendingUpdates();
  return update;
}

StateApplyResult StateStore::ApplyChangesForSession(u64 expected_session_id, StateValueMap values,
                                                    std::vector<std::string> removed)
{
  StateApplyResult result =
      ApplyChangesForSessionDeferred(expected_session_id, std::move(values), std::move(removed));
  DispatchPendingUpdates();
  return result;
}

StateApplyResult StateStore::ApplyChangesForSessionDeferred(u64 expected_session_id,
                                                            StateValueMap values,
                                                            std::vector<std::string> removed)
{
  return ApplyChangesInternal(expected_session_id, std::move(values), std::move(removed));
}

StateApplyResult StateStore::ApplyChangesInternal(std::optional<u64> expected_session_id,
                                                  StateValueMap values,
                                                  std::vector<std::string> removed)
{
  StateUpdate update;
  {
    std::lock_guard lg(m_mutex);
    if (expected_session_id && *expected_session_id != m_session_id)
      return {StateApplyStatus::StaleSession, std::nullopt};

    StateValueMap changed_values;
    for (auto& [id, value] : values)
    {
      const auto existing = m_values.find(id);
      if (existing != m_values.end() && existing->second == value)
        continue;

      m_values[id] = value;
      changed_values.emplace(std::move(id), std::move(value));
    }

    std::vector<std::string> actual_removed;
    for (std::string& id : removed)
    {
      if (m_values.erase(id) != 0)
        actual_removed.push_back(std::move(id));
    }
    std::ranges::sort(actual_removed);
    actual_removed.erase(std::ranges::unique(actual_removed).begin(), actual_removed.end());

    if (changed_values.empty() && actual_removed.empty())
      return {StateApplyStatus::NoChanges, std::nullopt};

    ++m_sequence;
    update = StateUpdate{m_session_id, m_sequence, false, std::move(changed_values),
                         std::move(actual_removed)};
    m_pending_updates.push_back(update);
  }

  return {StateApplyStatus::Applied, std::move(update)};
}

void StateStore::DispatchPendingUpdates()
{
  {
    std::lock_guard lg(m_mutex);
    if (m_dispatching)
      return;
    m_dispatching = true;
  }

  while (true)
  {
    StateUpdate update;
    {
      std::lock_guard lg(m_mutex);
      if (m_pending_updates.empty())
      {
        m_dispatching = false;
        return;
      }

      update = std::move(m_pending_updates.front());
      m_pending_updates.pop_front();
    }

    TriggerUpdate(update);
  }
}

Common::EventHook
StateStore::RegisterUpdateCallback(std::function<void(const StateUpdate&)> callback)
{
  return m_update_event.Register(
      [callback = std::move(callback)](StateUpdate update) { callback(update); });
}

void StateStore::TriggerUpdate(const StateUpdate& update)
{
  m_update_event.Trigger(update);
}
}  // namespace CheevoMap::V2
