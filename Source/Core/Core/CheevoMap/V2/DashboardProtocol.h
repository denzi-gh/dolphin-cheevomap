// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Core/CheevoMap/V2/StateStore.h"
#include "Core/CheevoMap/V2/StateValue.h"

namespace CheevoMap::V2
{
constexpr int kDashboardProtocolVersion = 1;

struct StateCursor
{
  u64 session_id = 0;
  u64 sequence = 0;
};

enum class StateUpdateApplyResult : u8
{
  Applied,
  StaleOrDuplicate,
  InvalidSessionTransition,
};

StateCursor CursorForSnapshot(const StateSnapshot& snapshot);
StateCursor CursorForUpdate(const StateUpdate& update);
bool IsCursorNewer(StateCursor candidate, StateCursor current);
StateUpdateApplyResult ApplyStateUpdateToSnapshot(StateSnapshot* snapshot,
                                                  const StateUpdate& update);

bool IsUnsignedDecimalString(std::string_view value);
int CompareUnsignedDecimalStrings(std::string_view left, std::string_view right);

std::string SerializeStateValue(const StateValue& value);
std::string SerializeStateSnapshot(const StateSnapshot& snapshot);
std::string SerializeStateUpdate(const StateUpdate& update);
std::string SerializeDashboardHealth();
}  // namespace CheevoMap::V2
