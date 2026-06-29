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

enum class PackageRuntimeStatus : u8
{
  Applied,
  NoChanges,
  StaleSession,
  EvaluationFailed,
};

struct PackageRuntimeResult
{
  PackageRuntimeStatus status = PackageRuntimeStatus::EvaluationFailed;
  std::optional<StateUpdate> update;
};

bool ValidatePackageGameIdentity(const GameInfo& package_game, const GameIdentity& running_game,
                                 std::string* error_out);
std::optional<EvaluationResult> EvaluatePackage(const Package& package,
                                                const EmulatorDataSource& data_source,
                                                std::string* error_out);
PackageRuntimeResult EvaluatePackageForSession(const Package& package,
                                               const EmulatorDataSource& data_source,
                                               StateStore& state_store, u64 expected_session_id,
                                               std::string* error_out);
}  // namespace CheevoMap::V2
