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
constexpr u64 PPC_PHYSICAL_MASK = 0x1fffffff;

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

V2::PointerAddressResolution ResolveDolphinPointerAddress(std::string_view target_area_id,
                                                          const u64 raw_pointer)
{
  // Dolphin accepts PowerPC virtual aliases here portable pointer chain runtime only sees the
  //normalized emulated-physical address returned by this adapter boundaryy
  if (raw_pointer == 0)
    return {false, 0, V2::MemoryReadError::InvalidAddress};

  const u64 physical = raw_pointer & PPC_PHYSICAL_MASK;
  if (physical == 0)
    return {false, 0, V2::MemoryReadError::InvalidAddress};

  const bool in_mem1 = physical >= MEM1_BASE && physical < MEM1_BASE + MEM1_SIZE;
  const bool in_mem2 = physical >= MEM2_BASE && physical < MEM2_BASE + MEM2_SIZE;
  if (!in_mem1 && !in_mem2)
    return {false, 0, V2::MemoryReadError::InvalidAddress};

  const std::string_view actual_area = in_mem1 ? std::string_view{"mem1"} : std::string_view{"mem2"};
  if (target_area_id != actual_area)
    return {false, 0, V2::MemoryReadError::InvalidAddress};

  return {true, physical, V2::MemoryReadError::None};
}

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
      V2::MemoryArea{"mem1", "MEM1", "emulated-physical", MEM1_BASE, MEM1_SIZE},
      V2::MemoryArea{"mem2", "MEM2", "emulated-physical", MEM2_BASE, MEM2_SIZE},
  };
}

V2::MemoryReadError DolphinEmulatorDataSource::ValidateRead(
    const V2::MemoryReadRequest& request) const
{
  const bool supported_area = request.memory_area_id.empty() ||
                              request.memory_area_id == "mem1" ||
                              request.memory_area_id == "mem2";
  if (!supported_area)
    return V2::MemoryReadError::UnsupportedMemoryArea;

  if (request.memory_area_id == "mem1" &&
      (request.address < MEM1_BASE || request.address > MEM1_BASE + MEM1_SIZE ||
       request.size > MEM1_BASE + MEM1_SIZE - request.address))
  {
    return V2::MemoryReadError::InvalidAddress;
  }
  if (request.memory_area_id == "mem2" &&
      (request.address < MEM2_BASE || request.address > MEM2_BASE + MEM2_SIZE ||
       request.size > MEM2_BASE + MEM2_SIZE - request.address))
  {
    return V2::MemoryReadError::InvalidAddress;
  }

  constexpr u64 ADDRESS_LIMIT = static_cast<u64>(std::numeric_limits<u32>::max()) + 1;
  if (request.address > std::numeric_limits<u32>::max() || request.size > ADDRESS_LIMIT ||
      request.size > ADDRESS_LIMIT - request.address)
  {
    return V2::MemoryReadError::InvalidAddress;
  }

  return V2::MemoryReadError::None;
}

bool DolphinEmulatorDataSource::ReadMemory(u64 address, u8* out, std::size_t size) const
{
  if (out == nullptr && size != 0)
    return false;

  const V2::MemoryReadRequest request{{}, address, static_cast<u32>(size)};
  if (size > std::numeric_limits<u32>::max() ||
      ValidateRead(request) != V2::MemoryReadError::None)
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

std::vector<V2::MemoryReadResult> DolphinEmulatorDataSource::ReadMemory(
    const std::vector<V2::MemoryReadRequest>& requests) const
{
  std::vector<V2::MemoryReadResult> results;
  results.reserve(requests.size());

  for (const V2::MemoryReadRequest& request : requests)
  {
    V2::MemoryReadResult result;
    result.request = request;
    result.error = ValidateRead(request);
    if (result.error == V2::MemoryReadError::None)
    {
      result.bytes.resize(request.size);
      result.success =
          request.size == 0 || ReadMemory(request.address, result.bytes.data(), result.bytes.size());
      if (!result.success)
      {
        result.error = V2::MemoryReadError::UnmappedAddress;
        result.bytes.clear();
      }
    }
    results.push_back(std::move(result));
  }

  return results;
}

V2::PointerAddressResolution
DolphinEmulatorDataSource::ResolvePointerAddress(const std::string& target_area_id,
                                                 const u64 raw_pointer) const
{
  return ResolveDolphinPointerAddress(target_area_id, raw_pointer);
}
}  // namespace CheevoMap::Dolphin
