// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/CheevoMap/V2/EmulatorDataSource.h"

namespace Core
{
class CPUThreadGuard;
}

namespace CheevoMap::Dolphin
{
class DolphinEmulatorDataSource final : public V2::EmulatorDataSource
{
public:
  explicit DolphinEmulatorDataSource(const Core::CPUThreadGuard& guard);

  V2::EmulatorStatus GetStatus() const override;
  V2::GameIdentity GetGameIdentity() const override;
  std::vector<V2::MemoryArea> GetMemoryAreas() const override;
  bool ReadMemory(u64 address, u8* out, std::size_t size) const override;
  std::vector<V2::MemoryReadResult> ReadMemory(
      const std::vector<V2::MemoryReadRequest>& requests) const override;

private:
  V2::MemoryReadError ValidateRead(const V2::MemoryReadRequest& request) const;

  const Core::CPUThreadGuard& m_guard;
};
}  // namespace CheevoMap::Dolphin
