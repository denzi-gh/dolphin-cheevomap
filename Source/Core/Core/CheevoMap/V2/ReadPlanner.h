// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Core/CheevoMap/V2/EmulatorDataSource.h"
#include "Core/CheevoMap/V2/Package.h"
#include "Core/CheevoMap/V2/StateStore.h"

namespace CheevoMap::V2
{
struct PlannedValueRead
{
  std::string value_id;
  size_t request_index = 0;
  ValueType type = ValueType::U8;
  Endian endian = Endian::None;
  u32 size = 0;
};

struct PlannedPointerChainTargetArea
{
  std::string id;
  u64 base_address = 0;
  u64 size = 0;
};

struct PlannedPointerChainRead
{
  std::string value_id;
  size_t root_request_index = 0;
  std::vector<PlannedPointerChainTargetArea> target_areas;
  std::vector<u64> offsets;
  PointerType pointer_type = PointerType::U32;
  Endian pointer_endian = Endian::None;
  ValueType type = ValueType::U8;
  Endian endian = Endian::None;
  u32 size = 0;
};

struct ReadPlan
{
  std::vector<MemoryReadRequest> requests;
  std::vector<PlannedValueRead> values;
  std::vector<PlannedPointerChainRead> pointer_chains;
};

std::optional<ReadPlan> BuildReadPlan(const Package& package,
                                      const std::vector<MemoryArea>& memory_areas,
                                      std::string* error_out);
StateValueMap EvaluateReadPlan(const ReadPlan& plan, const EmulatorDataSource& data_source);
StateValueMap DecodeReadResults(const ReadPlan& plan, const std::vector<MemoryReadResult>& results);
}  // namespace CheevoMap::V2
