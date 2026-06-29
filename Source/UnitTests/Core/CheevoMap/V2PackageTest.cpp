// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <picojson.h>

#include "Core/CheevoMap/V2/PackageParser.h"
#include "Core/CheevoMap/V2/ReadPlanner.h"
#include "Core/CheevoMap/V2/Runtime.h"

namespace
{
using CheevoMap::V2::BuildReadPlan;
using CheevoMap::V2::DecodeReadResults;
using CheevoMap::V2::EmulatorDataSource;
using CheevoMap::V2::EmulatorStatus;
using CheevoMap::V2::Endian;
using CheevoMap::V2::EvaluatePackage;
using CheevoMap::V2::EvaluatePackageForSession;
using CheevoMap::V2::GameIdentity;
using CheevoMap::V2::MemoryArea;
using CheevoMap::V2::MemoryReadError;
using CheevoMap::V2::MemoryReadRequest;
using CheevoMap::V2::MemoryReadResult;
using CheevoMap::V2::Package;
using CheevoMap::V2::ParsePackage;
using CheevoMap::V2::ReadPlan;
using CheevoMap::V2::StateApplyStatus;
using CheevoMap::V2::StateStore;
using CheevoMap::V2::StateUpdate;
using CheevoMap::V2::StateValue;
using CheevoMap::V2::ValueDefinition;
using CheevoMap::V2::ValueType;

class FakeDataSource final : public EmulatorDataSource
{
public:
  EmulatorStatus GetStatus() const override { return EmulatorStatus::Running; }
  GameIdentity GetGameIdentity() const override { return GameIdentity{"GAME01", "", 0, 0}; }

  std::vector<MemoryArea> GetMemoryAreas() const override
  {
    return {MemoryArea{"mem1", "MEM1", "emulated-physical", 0x00000000, 0x1000},
            MemoryArea{"mem2", "MEM2", "emulated-physical", 0x10000000, 0x1000}};
  }

  bool ReadMemory(u64 address, u8* out, std::size_t size) const override
  {
    ++single_read_count;
    for (std::size_t i = 0; i < size; ++i)
    {
      const auto it = memory.find(address + i);
      if (it == memory.end())
        return false;
      out[i] = it->second;
    }
    return true;
  }

  std::vector<MemoryReadResult> ReadMemory(
      const std::vector<MemoryReadRequest>& requests) const override
  {
    ++grouped_read_count;
    last_requests = requests;

    std::vector<MemoryReadResult> results;
    results.reserve(requests.size());
    for (const MemoryReadRequest& request : requests)
    {
      MemoryReadResult result;
      result.request = request;
      if (failed_addresses.contains(request.address))
      {
        result.error = MemoryReadError::ReadFailure;
        results.push_back(std::move(result));
        continue;
      }

      result.bytes.resize(request.size);
      result.success = true;
      for (u32 i = 0; i < request.size; ++i)
      {
        const auto it = memory.find(request.address + i);
        if (it == memory.end())
        {
          result.success = false;
          result.error = MemoryReadError::UnmappedAddress;
          result.bytes.clear();
          break;
        }
        result.bytes[i] = it->second;
      }
      results.push_back(std::move(result));
    }
    return results;
  }

  void Write(u64 address, std::initializer_list<u8> bytes)
  {
    for (const u8 byte : bytes)
      memory[address++] = byte;
  }

  mutable int single_read_count = 0;
  mutable int grouped_read_count = 0;
  mutable std::vector<MemoryReadRequest> last_requests;
  std::map<u64, u8> memory;
  std::set<u64> failed_addresses;
};

std::optional<Package> ParseText(const std::string& json, std::string* error)
{
  picojson::value root;
  *error = picojson::parse(root, json);
  if (!error->empty())
    return std::nullopt;
  return ParsePackage(root, error);
}

Package ValidPackage()
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test Game";

  ValueDefinition coins;
  coins.id = "coins";
  coins.type = ValueType::U16;
  coins.read.area_id = "mem1";
  coins.read.address = 0x10;
  coins.read.endian = Endian::Big;
  package.values.push_back(coins);

  return package;
}

TEST(CheevoMapV2Parser, LoadsValidMinimalPackage)
{
  std::string error;
  const auto package = ParseText(R"({
    "schema_version": 2,
    "game": {"id": "GAME01", "revision": 2},
    "package": {"title": "Test Game"},
    "poll_hz": 30.0,
    "values": [
      {"id": "coins", "type": "u16",
       "read": {"area": "mem1", "address": "0x10", "endian": "big"}},
      {"id": "player_name", "type": "string", "bytes": 16,
       "read": {"area": "mem1", "address": "0x120"}}
    ]
  })",
                                 &error);

  ASSERT_TRUE(package) << error;
  EXPECT_EQ(package->schema_version, 2u);
  EXPECT_EQ(package->game.id, "GAME01");
  ASSERT_TRUE(package->game.revision);
  EXPECT_EQ(*package->game.revision, 2u);
  EXPECT_EQ(package->metadata.title, "Test Game");
  EXPECT_DOUBLE_EQ(package->poll_hz, 30.0);
  ASSERT_EQ(package->values.size(), 2u);
  EXPECT_EQ(package->values[1].bytes, 16u);
}

TEST(CheevoMapV2Parser, RejectsInvalidPackages)
{
  const std::vector<std::pair<std::string, std::string>> cases = {
      {R"({"game":{"id":"GAME01"},"package":{"title":"T"},"values":[]})", "schema_version"},
      {R"({"schema_version":2.5,"game":{"id":"GAME01"},"package":{"title":"T"},"values":[]})",
       "schema_version"},
      {R"({"schema_version":3,"game":{"id":"GAME01"},"package":{"title":"T"},"values":[]})",
       "unsupported schema_version"},
      {R"({"schema_version":2,"game":{},"package":{"title":"T"},"values":[]})", "game.id"},
      {R"({"schema_version":2,"game":{"id":"GAME01"},"package":{"title":"T"},"poll_hz":0,"values":[]})",
       "poll_hz"},
      {R"({"schema_version":2,"game":{"id":"GAME01"},"package":{"title":"T"},"values":{}})",
       "values array"},
      {R"({"schema_version":2,"game":{"id":"GAME01"},"package":{"title":"T"},"values":[{"type":"u8","read":{"area":"mem1","address":"0x0"}}]})",
       "id"},
      {R"({"schema_version":2,"game":{"id":"GAME01"},"package":{"title":"T"},"values":[{"id":"a","type":"u8","read":{"area":"mem1","address":"0x0"}},{"id":"a","type":"u8","read":{"area":"mem1","address":"0x1"}}]})",
       "duplicate"},
      {R"({"schema_version":2,"game":{"id":"GAME01"},"package":{"title":"T"},"values":[{"id":"a","type":"u24","read":{"area":"mem1","address":"0x0"}}]})",
       "unsupported type"},
      {R"({"schema_version":2,"game":{"id":"GAME01"},"package":{"title":"T"},"values":[{"id":"a","type":"u8","read":{"area":"mem1","address":"10"}}]})",
       "address"},
      {R"({"schema_version":2,"game":{"id":"GAME01"},"package":{"title":"T"},"values":[{"id":"a","type":"string","bytes":0,"read":{"area":"mem1","address":"0x0"}}]})",
       "bytes"},
      {R"({"schema_version":2,"game":{"id":"GAME01"},"package":{"title":"T"},"values":[{"id":"a","type":"u16","read":{"area":"mem1","address":"0x0"}}]})",
       "endian"},
      {R"({"schema_version":2,"game":{"id":"GAME01"},"package":{"title":"T"},"values":[{"id":"a","type":"u8","read":{"area":"mem1","address":"0x0","endian":"big"}}]})",
       "endian"},
      {R"({"schema_version":2,"game":{"id":"GAME01"},"package":{"title":"T"},"values":[{"id":"a","type":"u8","bytes":1,"read":{"area":"mem1","address":"0x0"}}]})",
       "bytes is only valid"},
      {R"({"schema_version":2,"game":{"id":"GAME01"},"package":{"title":"T"},"extra":true,"values":[]})",
       "unknown field"},
  };

  for (const auto& [json, expected_error] : cases)
  {
    std::string error;
    EXPECT_FALSE(ParseText(json, &error)) << json;
    EXPECT_NE(error.find(expected_error), std::string::npos) << error;
  }
}

TEST(CheevoMapV2Planner, BuildsDeterministicGroupedRequestsAndMappings)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";

  ValueDefinition mem2;
  mem2.id = "mem2_value";
  mem2.type = ValueType::U8;
  mem2.read.area_id = "mem2";
  mem2.read.address = 0x20;
  package.values.push_back(mem2);

  ValueDefinition first = mem2;
  first.id = "first";
  first.type = ValueType::U16;
  first.read.area_id = "mem1";
  first.read.address = 0x10;
  first.read.endian = Endian::Big;
  package.values.push_back(first);

  ValueDefinition duplicate = first;
  duplicate.id = "duplicate";
  package.values.push_back(duplicate);

  std::string error;
  const auto plan = BuildReadPlan(package, FakeDataSource().GetMemoryAreas(), &error);
  ASSERT_TRUE(plan) << error;
  ASSERT_EQ(plan->requests.size(), 2u);
  EXPECT_EQ(plan->requests[0].memory_area_id, "mem1");
  EXPECT_EQ(plan->requests[0].address, 0x10u);
  EXPECT_EQ(plan->requests[0].size, 2u);
  EXPECT_EQ(plan->requests[1].memory_area_id, "mem2");
  EXPECT_EQ(plan->requests[1].address, 0x10000020u);
  EXPECT_EQ(plan->values[0].request_index, 1u);
  EXPECT_EQ(plan->values[1].request_index, 0u);
  EXPECT_EQ(plan->values[2].request_index, 0u);
}

TEST(CheevoMapV2Planner, RejectsInvalidPlansBeforeMemoryAccess)
{
  Package package = ValidPackage();
  package.values.front().read.area_id = "missing";

  FakeDataSource data_source;
  std::string error;
  EXPECT_FALSE(EvaluatePackage(package, data_source, &error));
  EXPECT_NE(error.find("unknown memory area"), std::string::npos);
  EXPECT_EQ(data_source.grouped_read_count, 0);

  package = ValidPackage();
  package.values.front().read.address = 0x1000;
  EXPECT_FALSE(EvaluatePackage(package, data_source, &error));
  EXPECT_NE(error.find("outside memory area"), std::string::npos);
  EXPECT_EQ(data_source.grouped_read_count, 0);
}

TEST(CheevoMapV2Decoder, DecodesPrimitiveTypesEndianAndStrings)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";

  auto add_value = [&package](std::string id, ValueType type, u64 address, Endian endian,
                              u32 bytes = 0) {
    ValueDefinition value;
    value.id = std::move(id);
    value.type = type;
    value.bytes = bytes;
    value.read.area_id = "mem1";
    value.read.address = address;
    value.read.endian = endian;
    package.values.push_back(std::move(value));
  };

  add_value("flag", ValueType::Boolean, 0x00, Endian::None);
  add_value("u16be", ValueType::U16, 0x02, Endian::Big);
  add_value("s16le", ValueType::S16, 0x04, Endian::Little);
  add_value("f32be", ValueType::F32, 0x08, Endian::Big);
  add_value("f64le", ValueType::F64, 0x10, Endian::Little);
  add_value("name", ValueType::String, 0x20, Endian::None, 5);

  FakeDataSource data_source;
  data_source.Write(0x00, {1});
  data_source.Write(0x02, {0x12, 0x34});
  data_source.Write(0x04, {0xff, 0xff});
  data_source.Write(0x08, {0x3f, 0xa0, 0x00, 0x00});
  data_source.Write(0x10, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe0, 0xbf});
  data_source.Write(0x20, {'A', 1, ' ', 'Z', 0});

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;

  EXPECT_TRUE(*result->values.at("flag").AsBoolean());
  EXPECT_EQ(*result->values.at("u16be").AsUnsignedInteger(), 0x1234u);
  EXPECT_EQ(*result->values.at("s16le").AsSignedInteger(), -1);
  EXPECT_DOUBLE_EQ(*result->values.at("f32be").AsFloatingPoint(), 1.25);
  EXPECT_DOUBLE_EQ(*result->values.at("f64le").AsFloatingPoint(), -0.5);
  ASSERT_NE(result->values.at("name").AsString(), nullptr);
  EXPECT_EQ(*result->values.at("name").AsString(), "A? Z");
  EXPECT_EQ(data_source.grouped_read_count, 1);
  EXPECT_EQ(data_source.single_read_count, 0);
}

TEST(CheevoMapV2Decoder, KeepsSuccessfulValuesWhenAnotherReadFails)
{
  Package package = ValidPackage();
  ValueDefinition failed = package.values.front();
  failed.id = "failed";
  failed.read.address = 0x20;
  package.values.push_back(failed);

  FakeDataSource data_source;
  data_source.Write(0x10, {0x12, 0x34});
  data_source.failed_addresses.insert(0x20);

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(*result->values.at("coins").AsUnsignedInteger(), 0x1234u);
  EXPECT_FALSE(result->values.at("failed").IsAvailable());
}

TEST(CheevoMapV2RuntimeState, PublishesDeltasAndRejectsStaleSessions)
{
  Package package = ValidPackage();
  FakeDataSource data_source;
  data_source.Write(0x10, {0x00, 0x03});

  StateStore store;
  std::vector<StateUpdate> updates;
  auto hook = store.RegisterUpdateCallback([&updates](const StateUpdate& update) {
    updates.push_back(update);
  });
  (void)hook;

  const StateUpdate reset = store.Reset({{"coins", StateValue::Unavailable()}});
  ASSERT_EQ(updates.size(), 1u);

  std::string error;
  auto applied = EvaluatePackageForSession(package, data_source, &store, reset.session_id, &error);
  EXPECT_EQ(applied.status, StateApplyStatus::Applied);
  ASSERT_EQ(updates.size(), 2u);
  EXPECT_EQ(*updates.back().values.at("coins").AsUnsignedInteger(), 3u);

  auto no_change = EvaluatePackageForSession(package, data_source, &store, reset.session_id, &error);
  EXPECT_EQ(no_change.status, StateApplyStatus::NoChanges);
  ASSERT_EQ(updates.size(), 2u);

  data_source.Write(0x10, {0x00, 0x04});
  auto changed = EvaluatePackageForSession(package, data_source, &store, reset.session_id, &error);
  EXPECT_EQ(changed.status, StateApplyStatus::Applied);
  ASSERT_EQ(updates.size(), 3u);
  EXPECT_EQ(*updates.back().values.at("coins").AsUnsignedInteger(), 4u);

  const StateUpdate reloaded = store.Reset({{"coins", StateValue::Unavailable()}});
  auto stale = EvaluatePackageForSession(package, data_source, &store, reset.session_id, &error);
  EXPECT_EQ(stale.status, StateApplyStatus::StaleSession);
  ASSERT_EQ(updates.size(), 4u);
  EXPECT_EQ(store.GetSnapshot().session_id, reloaded.session_id);
  EXPECT_FALSE(store.GetSnapshot().values.at("coins").IsAvailable());
}
}  // namespace
