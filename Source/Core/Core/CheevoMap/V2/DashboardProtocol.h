// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Core/CheevoMap/V2/StateStore.h"
#include "Core/CheevoMap/V2/StateValue.h"

namespace CheevoMap::V2
{
constexpr int kDashboardProtocolVersion = 1;

std::string SerializeStateValue(const StateValue& value);
std::string SerializeStateSnapshot(const StateSnapshot& snapshot);
std::string SerializeStateUpdate(const StateUpdate& update);
std::string SerializeDashboardHealth();
}  // namespace CheevoMap::V2
