// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CheevoMap/V2/EmulatorDataSource.h"

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
}  // namespace CheevoMap::V2
