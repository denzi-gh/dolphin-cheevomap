// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CheevoMap/Dolphin/DolphinEmulatorDataSource.h"

#include <limits>

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/PowerPC/MMU.h"
#include "Core/System.h"

namespace CheevoMap::Dolphin
{
namespace
{
constexpr u64 MEM1_BASE = 0x00000000;
constexpr u64 MEM1_SIZE = 0x04000000;
constexpr u64 MEM2_BASE = 0x10000000;
constexpr u64 MEM2_SIZE = 0x04000000;

V2::EmulatorStatus ToStatus(Core::State state)
{
  switch (state)
  {
  case Core::State::Running:
  case Core::State::Starting:
    return V2::EmulatorStatus::Running;
  case Core::State::Paused:
    return V2::EmulatorStatus::Paused;
  case Core::State::Stopping:
  case Core::State::Uninitialized:
    return V2::EmulatorStatus::Stopped;
  }

  return V2::EmulatorStatus::Unavailable;
}
}  // namespace

DolphinEmulatorDataSource::DolphinEmulatorDataSource(const Core::CPUThreadGuard& guard)
    : m_guard(guard)
{
}

V2::EmulatorStatus DolphinEmulatorDataSource::GetStatus() const
{
  return ToStatus(Core::GetState(m_guard.GetSystem()));
}

V2::GameIdentity DolphinEmulatorDataSource::GetGameIdentity() const
{
  const SConfig& config = SConfig::GetInstance();
  return V2::GameIdentity{config.GetGameID(), config.GetGameTDBID(), config.GetTitleID(),
                          config.GetRevision()};
}

std::vector<V2::MemoryArea> DolphinEmulatorDataSource::GetMemoryAreas() const
{
  return {
      V2::MemoryArea{"mem1", "MEM1", MEM1_BASE, MEM1_SIZE},
      V2::MemoryArea{"mem2", "MEM2", MEM2_BASE, MEM2_SIZE},
  };
}

bool DolphinEmulatorDataSource::ReadMemory(u64 address, u8* out, std::size_t size) const
{
  if (out == nullptr && size != 0)
    return false;
  if (address > std::numeric_limits<u32>::max() || size > std::numeric_limits<u32>::max() ||
      address + size > static_cast<u64>(std::numeric_limits<u32>::max()) + 1)
  {
    return false;
  }

  auto& mmu = m_guard.GetSystem().GetMMU();
  for (std::size_t i = 0; i < size; ++i)
  {
    const auto value = mmu.HostTryRead<u8>(m_guard, static_cast<u32>(address + i),
                                           PowerPC::RequestedAddressSpace::Physical);
    if (!value)
      return false;

    out[i] = value->value;
  }

  return true;
}
}  // namespace CheevoMap::Dolphin
