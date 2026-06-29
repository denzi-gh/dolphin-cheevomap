// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CheevoMap/V2/ReadPlanner.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <limits>
#include <map>

#include <fmt/format.h>

namespace CheevoMap::V2
{
namespace
{
struct ReadKey
{
  std::string memory_area_id;
  u64 address = 0;
  u32 size = 0;

  bool operator<(const ReadKey& rhs) const
  {
    if (memory_area_id != rhs.memory_area_id)
      return memory_area_id < rhs.memory_area_id;
    if (address != rhs.address)
      return address < rhs.address;
    return size < rhs.size;
  }
};

u64 AssembleUnsigned(const std::vector<u8>& bytes, Endian endian)
{
  u64 value = 0;
  if (endian == Endian::Little)
  {
    for (auto it = bytes.rbegin(); it != bytes.rend(); ++it)
      value = (value << 8) | *it;
  }
  else
  {
    for (const u8 byte : bytes)
      value = (value << 8) | byte;
  }
  return value;
}

s64 DecodeSigned(u64 raw, u32 size)
{
  switch (size)
  {
  case 1:
    return static_cast<s8>(raw);
  case 2:
    return static_cast<s16>(raw);
  case 4:
    return static_cast<s32>(static_cast<u32>(raw));
  case 8:
    return static_cast<s64>(raw);
  default:
    return 0;
  }
}

std::string DecodeString(const std::vector<u8>& bytes)
{
  std::string output;
  output.reserve(bytes.size());
  for (const u8 byte : bytes)
  {
    if (byte == 0)
      break;
    output.push_back(std::isprint(byte) ? static_cast<char>(byte) : '?');
  }
  return output;
}

StateValue DecodeValue(const PlannedValueRead& value, const MemoryReadResult& result)
{
  if (!result.success || result.bytes.size() != value.size)
    return StateValue::Unavailable();

  if (value.type == ValueType::String)
    return StateValue::String(DecodeString(result.bytes));

  const Endian endian = value.endian == Endian::None ? Endian::Big : value.endian;
  const u64 raw = AssembleUnsigned(result.bytes, endian);

  if (value.type == ValueType::Boolean)
    return StateValue::Boolean(raw != 0);

  if (IsSignedIntegerType(value.type))
    return StateValue::SignedInteger(DecodeSigned(raw, value.size));

  if (IsUnsignedIntegerType(value.type))
    return StateValue::UnsignedInteger(raw);

  if (value.type == ValueType::F32)
    return StateValue::FloatingPoint(std::bit_cast<float>(static_cast<u32>(raw)));

  if (value.type == ValueType::F64)
    return StateValue::FloatingPoint(std::bit_cast<double>(raw));

  return StateValue::Unavailable();
}
}  // namespace

std::optional<ReadPlan> BuildReadPlan(const Package& package,
                                      const std::vector<MemoryArea>& memory_areas,
                                      std::string* error_out)
{
  std::map<std::string, MemoryArea> areas;
  for (const MemoryArea& area : memory_areas)
    areas.emplace(area.id, area);

  std::map<ReadKey, size_t> request_indices;
  std::vector<ReadKey> value_keys;
  value_keys.reserve(package.values.size());

  for (const ValueDefinition& value : package.values)
  {
    const auto area_it = areas.find(value.read.area_id);
    if (area_it == areas.end())
    {
      *error_out = fmt::format("value \"{}\" uses unknown memory area \"{}\"", value.id,
                               value.read.area_id);
      return std::nullopt;
    }

    const MemoryArea& area = area_it->second;
    const u32 size = GetValueReadSize(value);
    if (size == 0)
    {
      *error_out = fmt::format("value \"{}\" has invalid read size", value.id);
      return std::nullopt;
    }

    if (value.read.address > area.size || size > area.size - value.read.address)
    {
      *error_out = fmt::format("value \"{}\" read is outside memory area \"{}\"", value.id,
                               value.read.area_id);
      return std::nullopt;
    }

    if (value.read.address > std::numeric_limits<u64>::max() - area.base_address)
    {
      *error_out = fmt::format("value \"{}\" address overflows memory area \"{}\"", value.id,
                               value.read.area_id);
      return std::nullopt;
    }

    value_keys.push_back(ReadKey{value.read.area_id, area.base_address + value.read.address, size});
    request_indices.emplace(value_keys.back(), 0);
  }

  ReadPlan plan;
  plan.requests.reserve(request_indices.size());
  size_t index = 0;
  for (auto& [key, request_index] : request_indices)
  {
    request_index = index++;
    plan.requests.push_back(MemoryReadRequest{key.memory_area_id, key.address, key.size});
  }

  plan.values.reserve(package.values.size());
  for (size_t i = 0; i < package.values.size(); ++i)
  {
    const ValueDefinition& value = package.values[i];
    plan.values.push_back(PlannedValueRead{value.id, request_indices.at(value_keys[i]), value.type,
                                           value.read.endian, GetValueReadSize(value)});
  }

  return plan;
}

StateValueMap DecodeReadResults(const ReadPlan& plan, const std::vector<MemoryReadResult>& results)
{
  StateValueMap values;
  for (const PlannedValueRead& value : plan.values)
  {
    if (value.request_index >= results.size())
    {
      values.emplace(value.value_id, StateValue::Unavailable());
      continue;
    }

    values.emplace(value.value_id, DecodeValue(value, results[value.request_index]));
  }
  return values;
}
}  // namespace CheevoMap::V2
