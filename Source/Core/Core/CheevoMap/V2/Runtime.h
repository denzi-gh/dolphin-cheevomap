// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>

#include "Core/CheevoMap/V2/EmulatorDataSource.h"
#include "Core/CheevoMap/V2/Package.h"
#include "Core/CheevoMap/V2/StateStore.h"

namespace CheevoMap::V2
{
struct EvaluationResult
{
  StateValueMap values;
};

std::optional<EvaluationResult> EvaluatePackage(const Package& package,
                                                const EmulatorDataSource& data_source,
                                                std::string* error_out);
StateApplyResult EvaluatePackageForSession(const Package& package,
                                           const EmulatorDataSource& data_source,
                                           StateStore* state_store,
                                           u64 expected_session_id,
                                           std::string* error_out);
}  // namespace CheevoMap::V2
