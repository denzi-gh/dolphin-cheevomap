// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace CheevoMap::V2
{
enum class EmulatorStatus : u8
{
  Unavailable,
  Stopped,
  Running,
  Paused,
};

struct GameIdentity
{
  std::string game_id;
  std::string gametdb_id;
  u64 title_id = 0;
  u16 revision = 0;
};

struct MemoryArea
{
  std::string id;
  std::string label;
  std::string address_space;
  u64 base_address = 0;
  u64 size = 0;
};

struct MemoryReadRequest
{
  // Empty means the adapter's default address space. Dolphin's current v1 path uses
  // physical MEM1/MEM2 addresses after the parser strips PowerPC virtual aliases.
  std::string memory_area_id;
  u64 address = 0;
  u32 size = 0;
};

enum class MemoryReadError : u8
{
  None,
  EmulatorUnavailable,
  UnsupportedMemoryArea,
  InvalidRequest,
  InvalidAddress,
  UnmappedAddress,
  ReadFailure,
  TemporarilyUnavailable,
};

struct MemoryReadResult
{
  MemoryReadRequest request;
  bool success = false;
  MemoryReadError error = MemoryReadError::None;
  std::vector<u8> bytes;
};

struct PointerAddressResolution
{
  bool success = false;
  u64 address = 0;
  MemoryReadError error = MemoryReadError::None;
};

class EmulatorDataSource
{
public:
  virtual ~EmulatorDataSource() = default;

  virtual EmulatorStatus GetStatus() const = 0;
  virtual GameIdentity GetGameIdentity() const = 0;
  virtual std::vector<MemoryArea> GetMemoryAreas() const = 0;

  virtual bool ReadMemory(u64 address, u8* out, std::size_t size) const = 0;
  virtual std::vector<MemoryReadResult> ReadMemory(
      const std::vector<MemoryReadRequest>& requests) const;
  virtual PointerAddressResolution ResolvePointerAddress(const std::string& target_area_id,
                                                         u64 raw_pointer) const;
};
}  // namespace CheevoMap::V2
