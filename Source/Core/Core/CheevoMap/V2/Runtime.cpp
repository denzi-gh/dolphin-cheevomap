// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CheevoMap/V2/Runtime.h"

#include "Core/CheevoMap/V2/ReadPlanner.h"

namespace CheevoMap::V2
{
std::optional<EvaluationResult> EvaluatePackage(const Package& package,
                                                const EmulatorDataSource& data_source,
                                                std::string* error_out)
{
  const auto plan = BuildReadPlan(package, data_source.GetMemoryAreas(), error_out);
  if (!plan)
    return std::nullopt;

  const std::vector<MemoryReadResult> results = data_source.ReadMemory(plan->requests);
  return EvaluationResult{DecodeReadResults(*plan, results)};
}

StateApplyResult EvaluatePackageForSession(const Package& package,
                                           const EmulatorDataSource& data_source,
                                           StateStore* state_store,
                                           u64 expected_session_id,
                                           std::string* error_out)
{
  if (state_store == nullptr)
    return {};

  const std::optional<EvaluationResult> result = EvaluatePackage(package, data_source, error_out);
  if (!result)
    return {};

  return state_store->ApplyChangesForSession(expected_session_id, result->values);
}
}  // namespace CheevoMap::V2
