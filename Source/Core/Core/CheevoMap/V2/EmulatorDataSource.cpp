// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CheevoMap/V2/EmulatorDataSource.h"

#include <limits>
#include <utility>

namespace CheevoMap::V2
{
std::vector<MemoryReadResult> EmulatorDataSource::ReadMemory(
    const std::vector<MemoryReadRequest>& requests) const
{
  std::vector<MemoryReadResult> results;
  results.reserve(requests.size());

  for (const MemoryReadRequest& request : requests)
  {
    MemoryReadResult result;
    result.request = request;
    if (!request.memory_area_id.empty())
    {
      result.error = MemoryReadError::UnsupportedMemoryArea;
      results.push_back(std::move(result));
      continue;
    }

    result.bytes.resize(request.size);
    result.success = request.size == 0 ||
                     ReadMemory(request.address, result.bytes.data(), result.bytes.size());
    if (!result.success)
    {
      result.error = MemoryReadError::ReadFailure;
      result.bytes.clear();
    }
    results.push_back(std::move(result));
  }

  return results;
}

PointerAddressResolution
EmulatorDataSource::ResolvePointerAddress(const std::string& target_area_id,
                                          const u64 raw_pointer) const
{
  if (raw_pointer == 0)
    return {false, 0, MemoryReadError::InvalidAddress};

  for (const MemoryArea& area : GetMemoryAreas())
  {
    if (area.id != target_area_id)
      continue;

    if (area.size > std::numeric_limits<u64>::max() - area.base_address)
      return {false, 0, MemoryReadError::InvalidAddress};

    const u64 area_end = area.base_address + area.size;
    if (raw_pointer < area.base_address || raw_pointer >= area_end)
      return {false, 0, MemoryReadError::InvalidAddress};

    return {true, raw_pointer, MemoryReadError::None};
  }

  return {false, 0, MemoryReadError::UnsupportedMemoryArea};
}
}  // namespace CheevoMap::V2
