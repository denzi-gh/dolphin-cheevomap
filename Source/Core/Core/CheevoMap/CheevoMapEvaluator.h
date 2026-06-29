// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "Common/CommonTypes.h"
#include "Core/CheevoMap/CheevoMapEntry.h"
#include "Core/CheevoMap/V2/EmulatorDataSource.h"
#include "Core/CheevoMap/V2/StateValue.h"

namespace CheevoMap
{
class AchievementStateSource
{
public:
  virtual ~AchievementStateSource() = default;

  virtual std::optional<bool> IsAchievementUnlocked(u32 achievement_id,
                                                    AchievementMode mode) const = 0;
};

enum class EvaluationStatus : u8
{
  Hidden,
  Unavailable,
  Available,
};

LiveValue MakeInitialLiveValue(const EntryDef& def);

EvaluationStatus EvaluateEntry(const EntryDef& def, const V2::EmulatorDataSource& data_source,
                               const AchievementStateSource* achievements, LiveValue* live,
                               V2::StateValue* state_value);
}  // namespace CheevoMap
