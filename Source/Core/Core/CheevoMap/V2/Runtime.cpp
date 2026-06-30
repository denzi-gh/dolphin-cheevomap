// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CheevoMap/V2/Runtime.h"

#include <fmt/format.h>

#include "Core/CheevoMap/V2/ReadPlanner.h"

namespace CheevoMap::V2
{
bool ValidatePackageGameIdentity(const GameInfo& package_game, const GameIdentity& running_game,
                                 std::string* error_out)
{
  if (package_game.id != running_game.game_id)
  {
    if (error_out)
    {
      *error_out = fmt::format("game identity mismatch: package game id \"{}\" does not match "
                               "running game id \"{}\"",
                               package_game.id, running_game.game_id);
    }
    return false;
  }

  if (package_game.revision && *package_game.revision != running_game.revision)
  {
    if (error_out)
    {
      *error_out = fmt::format("game identity mismatch: package revision {} does not match "
                               "running revision {}",
                               *package_game.revision, running_game.revision);
    }
    return false;
  }

  return true;
}

std::optional<EvaluationResult> EvaluatePackage(const Package& package,
                                                const EmulatorDataSource& data_source,
                                                std::string* error_out)
{
  if (!ValidatePackageGameIdentity(package.game, data_source.GetGameIdentity(), error_out))
    return std::nullopt;

  const auto plan = BuildReadPlan(package, data_source.GetMemoryAreas(), error_out);
  if (!plan)
    return std::nullopt;

  return EvaluationResult{EvaluateReadPlan(*plan, data_source)};
}

PackageRuntimeResult EvaluatePackageForSession(const Package& package,
                                               const EmulatorDataSource& data_source,
                                               StateStore& state_store, u64 expected_session_id,
                                               std::string* error_out)
{
  const std::optional<EvaluationResult> result = EvaluatePackage(package, data_source, error_out);
  if (!result)
    return {};

  const StateApplyResult apply_result =
      state_store.ApplyChangesForSession(expected_session_id, result->values);

  switch (apply_result.status)
  {
  case StateApplyStatus::Applied:
    return PackageRuntimeResult{PackageRuntimeStatus::Applied, apply_result.update};
  case StateApplyStatus::NoChanges:
    return PackageRuntimeResult{PackageRuntimeStatus::NoChanges, std::nullopt};
  case StateApplyStatus::StaleSession:
    return PackageRuntimeResult{PackageRuntimeStatus::StaleSession, std::nullopt};
  }

  return {};
}
}  // namespace CheevoMap::V2
