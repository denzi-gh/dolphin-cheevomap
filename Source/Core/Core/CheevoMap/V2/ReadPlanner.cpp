// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CheevoMap/V2/ReadPlanner.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <limits>
#include <map>
#include <optional>

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

struct RequestBatch
{
  std::vector<MemoryReadRequest> requests;
  std::vector<size_t> request_indices;
};

struct PendingDirectRead
{
  const ValueDefinition* value = nullptr;
  size_t key_index = 0;
};

struct PendingPointerChainRead
{
  const ValueDefinition* value = nullptr;
  const PointerChainRead* chain = nullptr;
  size_t root_key_index = 0;
  std::vector<PlannedPointerChainTargetArea> target_areas;
};

struct RuntimePointerChain
{
  const PlannedPointerChainRead* plan = nullptr;
  bool available = false;
  u64 resolved_address = 0;
};

RequestBatch MakeRequestBatch(const std::vector<ReadKey>& keys)
{
  std::map<ReadKey, size_t> request_indices;
  for (const ReadKey& key : keys)
    request_indices.emplace(key, 0);

  RequestBatch batch;
  batch.requests.reserve(request_indices.size());

  size_t index = 0;
  for (auto& [key, request_index] : request_indices)
  {
    request_index = index++;
    batch.requests.push_back(MemoryReadRequest{key.memory_area_id, key.address, key.size});
  }

  batch.request_indices.reserve(keys.size());
  for (const ReadKey& key : keys)
    batch.request_indices.push_back(request_indices.at(key));

  return batch;
}

std::map<ReadKey, size_t> MakeRequestIndex(const std::vector<MemoryReadRequest>& requests)
{
  std::map<ReadKey, size_t> request_indices;
  for (size_t i = 0; i < requests.size(); ++i)
  {
    const MemoryReadRequest& request = requests[i];
    request_indices.emplace(ReadKey{request.memory_area_id, request.address, request.size}, i);
  }
  return request_indices;
}

bool AddUnsignedOffset(u64 address, u64 offset, u64* out)
{
  if (address > std::numeric_limits<u64>::max() - offset)
    return false;

  *out = address + offset;
  return true;
}

bool IsRangeInsideArea(u64 address, u32 size, u64 area_base, u64 area_size)
{
  if (area_size > std::numeric_limits<u64>::max() - area_base)
    return false;

  const u64 area_end = area_base + area_size;
  if (address < area_base || address > area_end)
    return false;

  return size <= area_end - address;
}

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

std::optional<u64> DecodePointerResult(const MemoryReadResult& result, PointerType pointer_type,
                                       Endian endian)
{
  const u32 size = GetPointerReadSize(pointer_type);
  if (endian == Endian::None || !result.success || result.bytes.size() != size)
    return std::nullopt;

  return AssembleUnsigned(result.bytes, endian);
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

  std::vector<ReadKey> initial_keys;
  initial_keys.reserve(package.values.size());
  std::vector<PendingDirectRead> direct_reads;
  std::vector<PendingPointerChainRead> pointer_chains;

  for (const ValueDefinition& value : package.values)
  {
    const u32 size = GetValueReadSize(value);
    if (size == 0)
    {
      *error_out = fmt::format("value \"{}\" has invalid read size", value.id);
      return std::nullopt;
    }

    if (const auto* direct = std::get_if<DirectMemoryRead>(&value.read))
    {
      const auto area_it = areas.find(direct->area_id);
      if (area_it == areas.end())
      {
        *error_out =
            fmt::format("value \"{}\" uses unknown memory area \"{}\"", value.id, direct->area_id);
        return std::nullopt;
      }

      const MemoryArea& area = area_it->second;
      if (direct->address > area.size || size > area.size - direct->address)
      {
        *error_out = fmt::format("value \"{}\" read is outside memory area \"{}\"", value.id,
                                 direct->area_id);
        return std::nullopt;
      }

      if (direct->address > std::numeric_limits<u64>::max() - area.base_address)
      {
        *error_out = fmt::format("value \"{}\" address overflows memory area \"{}\"", value.id,
                                 direct->area_id);
        return std::nullopt;
      }

      direct_reads.push_back(PendingDirectRead{&value, initial_keys.size()});
      initial_keys.push_back(ReadKey{direct->area_id, area.base_address + direct->address, size});
      continue;
    }

    const auto* chain = std::get_if<PointerChainRead>(&value.read);
    if (chain == nullptr)
    {
      *error_out = fmt::format("value \"{}\" has unsupported read definition", value.id);
      return std::nullopt;
    }

    if (chain->offsets.empty() || chain->offsets.size() > 16)
    {
      *error_out =
          fmt::format("value \"{}\" pointer_chain.offsets must contain 1..16 offsets", value.id);
      return std::nullopt;
    }

    const u32 pointer_size = GetPointerReadSize(chain->pointer_type);
    if (pointer_size == 0 || chain->pointer_endian == Endian::None)
    {
      *error_out =
          fmt::format("value \"{}\" has invalid pointer_chain pointer type or endian", value.id);
      return std::nullopt;
    }

    const auto base_area_it = areas.find(chain->base.area_id);
    if (base_area_it == areas.end())
    {
      *error_out = fmt::format("value \"{}\" uses unknown pointer_chain.base area \"{}\"", value.id,
                               chain->base.area_id);
      return std::nullopt;
    }

    if (chain->target_area_ids.size() != chain->offsets.size())
    {
      // Every offset has a matching target area: intermediate reads validate against the current
      // stage's area, while decoded pointers resolve into the next stage's area.
      *error_out = fmt::format(
          "value \"{}\" pointer_chain.target_areas must contain exactly one entry per offset",
          value.id);
      return std::nullopt;
    }

    const MemoryArea& base_area = base_area_it->second;
    if (chain->base.address > base_area.size || pointer_size > base_area.size - chain->base.address)
    {
      *error_out = fmt::format("value \"{}\" pointer_chain.base read is outside memory area \"{}\"",
                               value.id, chain->base.area_id);
      return std::nullopt;
    }

    if (chain->base.address > std::numeric_limits<u64>::max() - base_area.base_address)
    {
      *error_out = fmt::format("value \"{}\" pointer_chain.base address overflows memory area "
                               "\"{}\"",
                               value.id, chain->base.area_id);
      return std::nullopt;
    }

    std::vector<PlannedPointerChainTargetArea> target_areas;
    target_areas.reserve(chain->target_area_ids.size());
    for (size_t i = 0; i < chain->target_area_ids.size(); ++i)
    {
      const std::string& target_area_id = chain->target_area_ids[i];
      if (target_area_id.empty())
      {
        *error_out = fmt::format(
            "value \"{}\" pointer_chain.target_areas[{}] must be a non-empty string", value.id, i);
        return std::nullopt;
      }

      const auto target_area_it = areas.find(target_area_id);
      if (target_area_it == areas.end())
      {
        *error_out = fmt::format("value \"{}\" uses unknown pointer_chain.target_area \"{}\"",
                                 value.id, target_area_id);
        return std::nullopt;
      }

      const MemoryArea& target_area = target_area_it->second;
      if (target_area.size > std::numeric_limits<u64>::max() - target_area.base_address)
      {
        *error_out = fmt::format("value \"{}\" target memory area \"{}\" overflows", value.id,
                                 target_area_id);
        return std::nullopt;
      }

      target_areas.push_back(PlannedPointerChainTargetArea{target_area.id, target_area.base_address,
                                                           target_area.size});
    }

    pointer_chains.push_back(
        PendingPointerChainRead{&value, chain, initial_keys.size(), std::move(target_areas)});
    initial_keys.push_back(
        ReadKey{chain->base.area_id, base_area.base_address + chain->base.address, pointer_size});
  }

  const RequestBatch initial_batch = MakeRequestBatch(initial_keys);

  ReadPlan plan;
  plan.requests = initial_batch.requests;

  plan.values.reserve(direct_reads.size());
  for (const PendingDirectRead& pending : direct_reads)
  {
    const auto& value = *pending.value;
    const auto& direct = std::get<DirectMemoryRead>(value.read);
    plan.values.push_back(PlannedValueRead{value.id,
                                           initial_batch.request_indices[pending.key_index],
                                           value.type, direct.endian, GetValueReadSize(value)});
  }

  plan.pointer_chains.reserve(pointer_chains.size());
  for (const PendingPointerChainRead& pending : pointer_chains)
  {
    const auto& value = *pending.value;
    const auto& chain = *pending.chain;
    plan.pointer_chains.push_back(PlannedPointerChainRead{
        value.id,
        initial_batch.request_indices[pending.root_key_index],
        pending.target_areas,
        chain.offsets,
        chain.pointer_type,
        chain.pointer_endian,
        value.type,
        chain.endian,
        GetValueReadSize(value),
    });
  }

  return plan;
}

StateValueMap EvaluateReadPlan(const ReadPlan& plan, const EmulatorDataSource& data_source)
{
  const std::vector<MemoryReadResult> initial_results = data_source.ReadMemory(plan.requests);
  const std::map<ReadKey, size_t> initial_request_indices = MakeRequestIndex(plan.requests);
  StateValueMap values = DecodeReadResults(plan, initial_results);

  if (plan.pointer_chains.empty())
    return values;

  std::vector<RuntimePointerChain> chains;
  chains.reserve(plan.pointer_chains.size());
  for (const PlannedPointerChainRead& planned : plan.pointer_chains)
  {
    RuntimePointerChain chain;
    chain.plan = &planned;
    if (planned.root_request_index < initial_results.size())
    {
      const std::optional<u64> root =
          DecodePointerResult(initial_results[planned.root_request_index], planned.pointer_type,
                              planned.pointer_endian);
      if (root)
      {
        const PointerAddressResolution resolved =
            data_source.ResolvePointerAddress(planned.target_areas.front().id, *root);
        if (resolved.success)
        {
          chain.available = true;
          chain.resolved_address = resolved.address;
        }
      }
    }
    chains.push_back(chain);
  }

  // Pointer-chain offsets are dereference offsets followed by one final value offset.
  // Each dereference depth is batched, sorted, and deduplicated before memory access.
  size_t max_intermediate_depth = 0;
  for (const PlannedPointerChainRead& planned : plan.pointer_chains)
    max_intermediate_depth = std::max(max_intermediate_depth, planned.offsets.size() - 1);

  for (size_t depth = 0; depth < max_intermediate_depth; ++depth)
  {
    std::vector<ReadKey> keys;
    std::vector<size_t> chain_indices;
    for (size_t i = 0; i < chains.size(); ++i)
    {
      RuntimePointerChain& chain = chains[i];
      const PlannedPointerChainRead& planned = *chain.plan;
      if (!chain.available || depth >= planned.offsets.size() - 1)
        continue;

      u64 pointer_read_address = 0;
      if (!AddUnsignedOffset(chain.resolved_address, planned.offsets[depth],
                             &pointer_read_address) ||
          !IsRangeInsideArea(pointer_read_address, GetPointerReadSize(planned.pointer_type),
                             planned.target_areas[depth].base_address,
                             planned.target_areas[depth].size))
      {
        chain.available = false;
        continue;
      }

      // The current area range validates the pointer read the next target area is only used
      // after the raw pointer has been decoded
      keys.push_back(ReadKey{planned.target_areas[depth].id, pointer_read_address,
                             GetPointerReadSize(planned.pointer_type)});
      chain_indices.push_back(i);
    }

    if (keys.empty())
      continue;

    const RequestBatch batch = MakeRequestBatch(keys);
    const std::vector<MemoryReadResult> results = data_source.ReadMemory(batch.requests);

    for (size_t i = 0; i < chain_indices.size(); ++i)
    {
      RuntimePointerChain& chain = chains[chain_indices[i]];
      const PlannedPointerChainRead& planned = *chain.plan;
      const size_t request_index = batch.request_indices[i];
      if (request_index >= results.size())
      {
        chain.available = false;
        continue;
      }

      const std::optional<u64> raw_pointer =
          DecodePointerResult(results[request_index], planned.pointer_type, planned.pointer_endian);
      if (!raw_pointer)
      {
        chain.available = false;
        continue;
      }

      const PointerAddressResolution resolved =
          data_source.ResolvePointerAddress(planned.target_areas[depth + 1].id, *raw_pointer);
      if (!resolved.success)
      {
        chain.available = false;
        continue;
      }

      chain.resolved_address = resolved.address;
    }
  }

  std::vector<bool> emitted(chains.size(), false);
  std::vector<ReadKey> final_keys;
  std::vector<size_t> final_chain_indices;
  for (size_t i = 0; i < chains.size(); ++i)
  {
    RuntimePointerChain& chain = chains[i];
    const PlannedPointerChainRead& planned = *chain.plan;
    if (!chain.available)
      continue;

    u64 final_address = 0;
    if (!AddUnsignedOffset(chain.resolved_address, planned.offsets.back(), &final_address) ||
        !IsRangeInsideArea(final_address, planned.size, planned.target_areas.back().base_address,
                           planned.target_areas.back().size))
    {
      chain.available = false;
      continue;
    }

    const ReadKey final_key{planned.target_areas.back().id, final_address, planned.size};
    if (const auto initial_it = initial_request_indices.find(final_key);
        initial_it != initial_request_indices.end())
    {
      const MemoryReadResult* result = initial_it->second < initial_results.size() ?
                                           &initial_results[initial_it->second] :
                                           nullptr;
      if (result == nullptr)
      {
        values.emplace(planned.value_id, StateValue::Unavailable());
      }
      else
      {
        values.emplace(planned.value_id,
                       DecodeValue(PlannedValueRead{planned.value_id, initial_it->second,
                                                    planned.type, planned.endian, planned.size},
                                   *result));
      }
      emitted[i] = true;
      continue;
    }

    final_keys.push_back(final_key);
    final_chain_indices.push_back(i);
  }

  if (!final_keys.empty())
  {
    const RequestBatch batch = MakeRequestBatch(final_keys);
    const std::vector<MemoryReadResult> results = data_source.ReadMemory(batch.requests);

    for (size_t i = 0; i < final_chain_indices.size(); ++i)
    {
      const size_t chain_index = final_chain_indices[i];
      const RuntimePointerChain& chain = chains[chain_index];
      const PlannedPointerChainRead& planned = *chain.plan;
      const size_t request_index = batch.request_indices[i];
      const MemoryReadResult* result =
          request_index < results.size() ? &results[request_index] : nullptr;

      if (result == nullptr)
      {
        values.emplace(planned.value_id, StateValue::Unavailable());
      }
      else
      {
        values.emplace(planned.value_id,
                       DecodeValue(PlannedValueRead{planned.value_id, request_index, planned.type,
                                                    planned.endian, planned.size},
                                   *result));
      }
      emitted[chain_index] = true;
    }
  }

  // Runtime pointer-chain failures make only the affected value unavailable; they do not turn
  // the package evaluation into EvaluationFailed.
  for (size_t i = 0; i < chains.size(); ++i)
  {
    if (!emitted[i])
      values.emplace(chains[i].plan->value_id, StateValue::Unavailable());
  }

  return values;
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
