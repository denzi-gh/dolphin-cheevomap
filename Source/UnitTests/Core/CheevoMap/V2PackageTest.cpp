// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <picojson.h>

#include "Core/CheevoMap/CheevoMapFile.h"
#include "Core/CheevoMap/CheevoMapManager.h"
#include "Core/CheevoMap/Dolphin/DolphinEmulatorDataSource.h"
#include "Core/CheevoMap/V2/PackageParser.h"
#include "Core/CheevoMap/V2/ReadPlanner.h"
#include "Core/CheevoMap/V2/Runtime.h"

namespace
{
using CheevoMap::Manager;
using CheevoMap::Dolphin::ResolveDolphinPointerAddress;
using CheevoMap::V2::BuildReadPlan;
using CheevoMap::V2::DecodeReadResults;
using CheevoMap::V2::DirectMemoryRead;
using CheevoMap::V2::EmulatorDataSource;
using CheevoMap::V2::EmulatorStatus;
using CheevoMap::V2::Endian;
using CheevoMap::V2::EvaluatePackage;
using CheevoMap::V2::EvaluatePackageForSession;
using CheevoMap::V2::GameIdentity;
using CheevoMap::V2::GameInfo;
using CheevoMap::V2::MemoryArea;
using CheevoMap::V2::MemoryReadError;
using CheevoMap::V2::MemoryReadRequest;
using CheevoMap::V2::MemoryReadResult;
using CheevoMap::V2::Package;
using CheevoMap::V2::PackageRuntimeStatus;
using CheevoMap::V2::ParsePackage;
using CheevoMap::V2::PointerAddressResolution;
using CheevoMap::V2::PointerChainRead;
using CheevoMap::V2::PointerType;
using CheevoMap::V2::ReadPlan;
using CheevoMap::V2::StateApplyStatus;
using CheevoMap::V2::StateStore;
using CheevoMap::V2::StateUpdate;
using CheevoMap::V2::StateValue;
using CheevoMap::V2::ValidatePackageGameIdentity;
using CheevoMap::V2::ValueDefinition;
using CheevoMap::V2::ValueType;

class FakeDataSource final : public EmulatorDataSource
{
public:
  EmulatorStatus GetStatus() const override { return EmulatorStatus::Running; }
  GameIdentity GetGameIdentity() const override { return identity; }

  std::vector<MemoryArea> GetMemoryAreas() const override { return memory_areas; }

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

  std::vector<MemoryReadResult>
  ReadMemory(const std::vector<MemoryReadRequest>& requests) const override
  {
    ++grouped_read_count;
    last_requests = requests;
    grouped_requests.push_back(requests);
    if (on_grouped_read)
      on_grouped_read();

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

  PointerAddressResolution ResolvePointerAddress(const std::string& target_area_id,
                                                 u64 raw_pointer) const override
  {
    ++pointer_resolution_count;
    if (failed_pointer_resolutions.contains(raw_pointer))
      return {false, 0, MemoryReadError::InvalidAddress};

    return EmulatorDataSource::ResolvePointerAddress(target_area_id, raw_pointer);
  }

  void Write(u64 address, std::initializer_list<u8> bytes)
  {
    for (const u8 byte : bytes)
      memory[address++] = byte;
  }

  mutable int single_read_count = 0;
  mutable int grouped_read_count = 0;
  mutable int pointer_resolution_count = 0;
  mutable std::vector<MemoryReadRequest> last_requests;
  mutable std::vector<std::vector<MemoryReadRequest>> grouped_requests;
  mutable std::function<void()> on_grouped_read;
  GameIdentity identity{"GAME01", "", 0, 0};
  std::vector<MemoryArea> memory_areas{
      MemoryArea{"mem1", "MEM1", "emulated-physical", 0x00000000, 0x1000},
      MemoryArea{"mem2", "MEM2", "emulated-physical", 0x10000000, 0x1000}};
  std::map<u64, u8> memory;
  std::set<u64> failed_addresses;
  std::set<u64> failed_pointer_resolutions;
};

class TempDir
{
public:
  TempDir()
  {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    m_path = std::filesystem::temp_directory_path() /
             ("dolphin-cheevomap-v2-test-" + std::to_string(now));
    std::filesystem::create_directories(m_path);
  }

  ~TempDir()
  {
    std::error_code error;
    std::filesystem::remove_all(m_path, error);
  }

  std::filesystem::path WriteFile(const std::string& name, const std::string& contents) const
  {
    const std::filesystem::path path = m_path / name;
    std::ofstream stream(path, std::ios::binary);
    stream << contents;
    return path;
  }

private:
  std::filesystem::path m_path;
};

std::optional<Package> ParseText(const std::string& json, std::string* error)
{
  picojson::value root;
  *error = picojson::parse(root, json);
  if (!error->empty())
    return std::nullopt;
  return ParsePackage(root, error);
}

void SetDirectRead(ValueDefinition* value, std::string area_id, u64 address,
                   Endian endian = Endian::None)
{
  value->read = DirectMemoryRead{std::move(area_id), address, endian};
}

ValueDefinition DirectValue(std::string id, ValueType type, std::string area_id, u64 address,
                            Endian endian = Endian::None, u32 bytes = 0)
{
  ValueDefinition value;
  value.id = std::move(id);
  value.type = type;
  value.bytes = bytes;
  SetDirectRead(&value, std::move(area_id), address, endian);
  return value;
}

void SetPointerChainRead(ValueDefinition* value, std::string base_area_id, u64 base_address,
                         std::string target_area_id, std::vector<u64> offsets,
                         Endian pointer_endian, Endian final_endian = Endian::None)
{
  PointerChainRead read;
  read.base.area_id = std::move(base_area_id);
  read.base.address = base_address;
  read.target_area_id = std::move(target_area_id);
  read.offsets = std::move(offsets);
  read.pointer_type = PointerType::U32;
  read.pointer_endian = pointer_endian;
  read.endian = final_endian;
  value->read = std::move(read);
}

void WriteU32(FakeDataSource* data_source, u64 address, u32 value, Endian endian)
{
  if (endian == Endian::Little)
  {
    data_source->Write(address, {static_cast<u8>(value), static_cast<u8>(value >> 8),
                                 static_cast<u8>(value >> 16), static_cast<u8>(value >> 24)});
    return;
  }

  data_source->Write(address, {static_cast<u8>(value >> 24), static_cast<u8>(value >> 16),
                               static_cast<u8>(value >> 8), static_cast<u8>(value)});
}

ValueDefinition PointerValue(std::string id, ValueType type, std::vector<u64> offsets,
                             Endian pointer_endian = Endian::Big, Endian final_endian = Endian::Big,
                             u32 bytes = 0)
{
  ValueDefinition value;
  value.id = std::move(id);
  value.type = type;
  value.bytes = bytes;
  SetPointerChainRead(&value, "mem1", 0x100, "mem1", std::move(offsets), pointer_endian,
                      final_endian);
  return value;
}

Package ValidPackage()
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test Game";

  ValueDefinition coins;
  coins.id = "coins";
  coins.type = ValueType::U16;
  SetDirectRead(&coins, "mem1", 0x10, Endian::Big);
  package.values.push_back(coins);

  return package;
}

}  // namespace

namespace CheevoMap
{
class CheevoMapManagerTestAccessor
{
public:
  static void SetV2PackageAndGeneration(Manager& manager, V2::Package package, u64 generation)
  {
    std::lock_guard lg(manager.m_lock);
    manager.m_file.reset();
    manager.m_v2_package = std::move(package);
    manager.m_live.clear();
    manager.m_loaded_game_id.clear();
    manager.m_generation = generation;
    manager.m_last_poll = {};
  }

  static void ClearV2PackageAndGeneration(Manager& manager, u64 generation)
  {
    std::lock_guard lg(manager.m_lock);
    manager.m_file.reset();
    manager.m_v2_package.reset();
    manager.m_live.clear();
    manager.m_loaded_game_id.clear();
    manager.m_generation = generation;
    manager.m_last_poll = {};
  }

  static V2::StateUpdate ResetV2State(Manager& manager, V2::StateValueMap values)
  {
    return manager.m_v2_state.Reset(std::move(values));
  }

  static V2::StateApplyResult CommitV2Evaluation(Manager& manager, u64 captured_generation,
                                                 u64 captured_session_id, V2::StateValueMap values)
  {
    return manager.CommitV2Evaluation(captured_generation, captured_session_id, std::move(values));
  }
};
}  // namespace CheevoMap

namespace
{
using CheevoMap::CheevoMapManagerTestAccessor;

picojson::value MinimalRootWithValue(picojson::value value)
{
  picojson::object root;
  root["schema_version"] = picojson::value(2.0);
  root["game"] = picojson::value(picojson::object{{"id", picojson::value("GAME01")}});
  root["package"] = picojson::value(picojson::object{{"title", picojson::value("Test Game")}});
  root["values"] = picojson::value(picojson::array{std::move(value)});
  return picojson::value(std::move(root));
}

picojson::value StringValueWithBytes(picojson::value bytes)
{
  picojson::object read;
  read["area"] = picojson::value("mem1");
  read["address"] = picojson::value("0x0");

  picojson::object value;
  value["id"] = picojson::value("name");
  value["type"] = picojson::value("string");
  value["bytes"] = std::move(bytes);
  value["read"] = picojson::value(std::move(read));
  return picojson::value(std::move(value));
}

picojson::value NonFiniteNumber(double number)
{
  picojson::value value;
  value.set<double>(number);
  return value;
}

std::string PackageJson(const std::string& values_json)
{
  return R"({"schema_version":2,"game":{"id":"GAME01"},"package":{"title":"Test Game"},"values":[)" +
         values_json + "]}";
}

std::string GoodPointerChainJson()
{
  return R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x0","0x160","0x420"],"pointer_type":"u32","endian":"big")";
}

std::string PointerValueJson(std::string type, std::string bytes_member,
                             std::string pointer_chain_members = GoodPointerChainJson(),
                             std::string read_members = R"(,"endian":"big")")
{
  return R"({"id":"p","type":")" + std::move(type) + "\"," + std::move(bytes_member) +
         R"("read":{"pointer_chain":{)" + std::move(pointer_chain_members) + "}" +
         std::move(read_members) + "}}";
}

TEST(CheevoMapV2Identity, ValidatesGameIdAndOptionalRevision)
{
  GameIdentity running{"GAME01", "", 0, 2};
  std::string error;

  EXPECT_TRUE(ValidatePackageGameIdentity(GameInfo{"GAME01", std::nullopt}, running, &error));
  EXPECT_TRUE(ValidatePackageGameIdentity(GameInfo{"GAME01", 2}, running, &error));

  EXPECT_FALSE(ValidatePackageGameIdentity(GameInfo{"OTHER", std::nullopt}, running, &error));
  EXPECT_NE(error.find("game id"), std::string::npos);

  EXPECT_FALSE(ValidatePackageGameIdentity(GameInfo{"GAME01", 3}, running, &error));
  EXPECT_NE(error.find("revision"), std::string::npos);
}

TEST(CheevoMapV2Identity, MismatchPreventsMemoryReads)
{
  Package package = ValidPackage();
  package.game.id = "OTHER";

  FakeDataSource data_source;
  data_source.Write(0x10, {0x12, 0x34});

  std::string error;
  EXPECT_FALSE(EvaluatePackage(package, data_source, &error));
  EXPECT_NE(error.find("game identity mismatch"), std::string::npos);
  EXPECT_EQ(data_source.grouped_read_count, 0);
  EXPECT_EQ(data_source.single_read_count, 0);

  package = ValidPackage();
  package.game.revision = 1;
  data_source.identity.revision = 2;
  EXPECT_FALSE(EvaluatePackage(package, data_source, &error));
  EXPECT_NE(error.find("revision"), std::string::npos);
  EXPECT_EQ(data_source.grouped_read_count, 0);
  EXPECT_EQ(data_source.single_read_count, 0);
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
      {R"({"schema_version":-1,"game":{"id":"GAME01"},"package":{"title":"T"},"values":[]})",
       "schema_version"},
      {R"({"schema_version":2.5,"game":{"id":"GAME01"},"package":{"title":"T"},"values":[]})",
       "schema_version"},
      {R"({"schema_version":4294967296,"game":{"id":"GAME01"},"package":{"title":"T"},"values":[]})",
       "schema_version"},
      {R"({"schema_version":3,"game":{"id":"GAME01"},"package":{"title":"T"},"values":[]})",
       "unsupported schema_version"},
      {R"({"schema_version":2,"game":{},"package":{"title":"T"},"values":[]})", "game.id"},
      {R"({"schema_version":2,"game":{"id":"GAME01","revision":65536},"package":{"title":"T"},"values":[]})",
       "game.revision"},
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
      {R"({"schema_version":2,"game":{"id":"GAME01"},"package":{"title":"T"},"values":[{"id":"a","type":"string","bytes":257,"read":{"area":"mem1","address":"0x0"}}]})",
       "bytes"},
      {R"({"schema_version":2,"game":{"id":"GAME01"},"package":{"title":"T"},"values":[{"id":"a","type":"string","bytes":1.5,"read":{"area":"mem1","address":"0x0"}}]})",
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

TEST(CheevoMapV2Parser, AcceptsMaximumStringLength)
{
  std::string error;
  const auto package = ParseText(R"({
    "schema_version": 2,
    "game": {"id": "GAME01"},
    "package": {"title": "Test Game"},
    "values": [
      {"id": "name", "type": "string", "bytes": 256,
       "read": {"area": "mem1", "address": "0x0"}}
    ]
  })",
                                 &error);

  ASSERT_TRUE(package) << error;
  ASSERT_EQ(package->values.size(), 1u);
  EXPECT_EQ(package->values[0].bytes, 256u);
}

TEST(CheevoMapV2Parser, AcceptsPointerChainPackages)
{
  const std::vector<std::string> values = {
      PointerValueJson(
          "u32", "",
          R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x10"],"pointer_type":"u32","endian":"big")"),
      PointerValueJson("u32", "", GoodPointerChainJson()),
      PointerValueJson(
          "u32", "",
          R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x0"],"pointer_type":"u32","endian":"big")"),
      PointerValueJson(
          "u32", "",
          R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x10"],"pointer_type":"u32","endian":"little")"),
      PointerValueJson(
          "u8", "",
          R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x10"],"pointer_type":"u32","endian":"big")",
          ""),
      PointerValueJson(
          "u16", "",
          R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x10"],"pointer_type":"u32","endian":"big")"),
      PointerValueJson(
          "f32", "",
          R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x10"],"pointer_type":"u32","endian":"little")",
          R"(,"endian":"big")"),
      PointerValueJson(
          "string", R"("bytes":8,)",
          R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x10"],"pointer_type":"u32","endian":"big")",
          ""),
      PointerValueJson(
          "u32", "",
          R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x0","0x1","0x2","0x3","0x4","0x5","0x6","0x7","0x8","0x9","0xa","0xb","0xc","0xd","0xe","0xf"],"pointer_type":"u32","endian":"big")"),
  };

  for (const std::string& value : values)
  {
    std::string error;
    const auto package = ParseText(PackageJson(value), &error);
    EXPECT_TRUE(package) << error << "\n" << value;
  }

  std::string error;
  const auto mixed = ParseText(
      PackageJson(R"({"id":"direct","type":"u8","read":{"area":"mem1","address":"0x0"}},)" +
                  PointerValueJson("u32", "")),
      &error);
  ASSERT_TRUE(mixed) << error;
  ASSERT_EQ(mixed->values.size(), 2u);
}

TEST(CheevoMapV2Parser, RejectsInvalidPointerChainPackages)
{
  const std::vector<std::pair<std::string, std::string>> cases = {
      {std::string(
           R"({"id":"p","type":"u32","read":{"area":"mem1","address":"0x0","pointer_chain":{)") +
           GoodPointerChainJson() + R"(},"endian":"big"}})",
       "must not mix"},
      {std::string(R"({"id":"p","type":"u32","read":{"area":"mem1","pointer_chain":{)") +
           GoodPointerChainJson() + R"(},"endian":"big"}})",
       "must not mix"},
      {std::string(R"({"id":"p","type":"u32","read":{"address":"0x0","pointer_chain":{)") +
           GoodPointerChainJson() + R"(},"endian":"big"}})",
       "must not mix"},
      {R"({"id":"p","type":"u32","read":{"endian":"big"}})", "either direct"},
      {PointerValueJson("u32", "", GoodPointerChainJson(), R"(,"endian":"big","extra":true)"),
       "unknown field"},
      {PointerValueJson(
           "u32", "",
           R"("target_area":"mem2","offsets":["0x10"],"pointer_type":"u32","endian":"big")"),
       "base"},
      {PointerValueJson(
           "u32", "",
           R"("base":1,"target_area":"mem2","offsets":["0x10"],"pointer_type":"u32","endian":"big")"),
       "base must be an object"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"address":"0x860"},"target_area":"mem2","offsets":["0x10"],"pointer_type":"u32","endian":"big")"),
       "base.area"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"","address":"0x860"},"target_area":"mem2","offsets":["0x10"],"pointer_type":"u32","endian":"big")"),
       "base.area"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2"},"target_area":"mem2","offsets":["0x10"],"pointer_type":"u32","endian":"big")"),
       "base.address"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"860"},"target_area":"mem2","offsets":["0x10"],"pointer_type":"u32","endian":"big")"),
       "base.address"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":1},"target_area":"mem2","offsets":["0x10"],"pointer_type":"u32","endian":"big")"),
       "base.address"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860","extra":true},"target_area":"mem2","offsets":["0x10"],"pointer_type":"u32","endian":"big")"),
       "unknown field"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"offsets":["0x10"],"pointer_type":"u32","endian":"big")"),
       "target_area"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"","offsets":["0x10"],"pointer_type":"u32","endian":"big")"),
       "target_area"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":1,"offsets":["0x10"],"pointer_type":"u32","endian":"big")"),
       "target_area must be a string"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","pointer_type":"u32","endian":"big")"),
       "offsets"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":1,"pointer_type":"u32","endian":"big")"),
       "offsets must be an array"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":[],"pointer_type":"u32","endian":"big")"),
       "at least 1"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x0","0x1","0x2","0x3","0x4","0x5","0x6","0x7","0x8","0x9","0xa","0xb","0xc","0xd","0xe","0xf","0x10"],"pointer_type":"u32","endian":"big")"),
       "at most 16"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":[1],"pointer_type":"u32","endian":"big")"),
       "offsets[0]"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":[-1],"pointer_type":"u32","endian":"big")"),
       "offsets[0]"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["16"],"pointer_type":"u32","endian":"big")"),
       "offsets[0]"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["10"],"pointer_type":"u32","endian":"big")"),
       "offsets[0]"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0xzz"],"pointer_type":"u32","endian":"big")"),
       "offsets[0]"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x10000000000000000"],"pointer_type":"u32","endian":"big")"),
       "offsets[0]"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x10"],"endian":"big")"),
       "pointer_type"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x10"],"pointer_type":"u64","endian":"big")"),
       "pointer_type"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x10"],"pointer_type":"s32","endian":"big")"),
       "pointer_type"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x10"],"pointer_type":"f32","endian":"big")"),
       "pointer_type"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x10"],"pointer_type":"banana","endian":"big")"),
       "pointer_type"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x10"],"pointer_type":"u32")"),
       "pointer_chain.endian"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x10"],"pointer_type":"u32","endian":"native")"),
       "pointer_chain.endian"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x10"],"pointer_type":"u32","endian":1)"),
       "pointer_chain.endian must be a string"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x10"],"pointer_type":"u32","endian":"big","extra":true)"),
       "unknown field"},
      {PointerValueJson(
           "u16", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x10"],"pointer_type":"u32","endian":"big")",
           ""),
       "read.endian"},
      {PointerValueJson(
           "u32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x10"],"pointer_type":"u32","endian":"big")",
           ""),
       "read.endian"},
      {PointerValueJson(
           "f32", "",
           R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0x10"],"pointer_type":"u32","endian":"big")",
           ""),
       "read.endian"},
      {PointerValueJson("u8", "", GoodPointerChainJson(), R"(,"endian":"big")"), "not valid"},
      {PointerValueJson("string", R"("bytes":4,)", GoodPointerChainJson(), R"(,"endian":"big")"),
       "not valid"},
      {PointerValueJson("string", "", GoodPointerChainJson(), ""), "bytes"},
      {PointerValueJson("string", R"("bytes":0,)", GoodPointerChainJson(), ""), "bytes"},
  };

  for (const auto& [value, expected_error] : cases)
  {
    std::string error;
    EXPECT_FALSE(ParseText(PackageJson(value), &error)) << value;
    EXPECT_NE(error.find(expected_error), std::string::npos) << error << "\n" << value;
  }
}

TEST(CheevoMapV2Parser, RejectsNonFiniteIntegerValues)
{
  const std::vector<picojson::value> invalid_numbers = {
      NonFiniteNumber(std::numeric_limits<double>::quiet_NaN()),
      NonFiniteNumber(std::numeric_limits<double>::infinity()),
  };

  for (const picojson::value& number : invalid_numbers)
  {
    picojson::object schema_root;
    schema_root["schema_version"] = number;
    schema_root["game"] = picojson::value(picojson::object{{"id", picojson::value("GAME01")}});
    schema_root["package"] =
        picojson::value(picojson::object{{"title", picojson::value("Test Game")}});
    schema_root["values"] = picojson::value(picojson::array{});

    std::string error;
    EXPECT_FALSE(ParsePackage(picojson::value(std::move(schema_root)), &error));
    EXPECT_NE(error.find("schema_version"), std::string::npos);

    picojson::object game;
    game["id"] = picojson::value("GAME01");
    game["revision"] = number;
    picojson::object revision_root;
    revision_root["schema_version"] = picojson::value(2.0);
    revision_root["game"] = picojson::value(std::move(game));
    revision_root["package"] =
        picojson::value(picojson::object{{"title", picojson::value("Test Game")}});
    revision_root["values"] = picojson::value(picojson::array{});

    EXPECT_FALSE(ParsePackage(picojson::value(std::move(revision_root)), &error));
    EXPECT_NE(error.find("game.revision"), std::string::npos);

    EXPECT_FALSE(ParsePackage(MinimalRootWithValue(StringValueWithBytes(number)), &error));
    EXPECT_NE(error.find("bytes"), std::string::npos);
  }
}

TEST(CheevoMapV2Parser, RejectsExtremelyLargeJsonNumber)
{
  std::string error;
  EXPECT_FALSE(ParseText(R"({
    "schema_version": 1e100,
    "game": {"id": "GAME01"},
    "package": {"title": "Test Game"},
    "values": []
  })",
                         &error));
  EXPECT_NE(error.find("schema_version"), std::string::npos);
}

TEST(CheevoMapV2Planner, BuildsDeterministicGroupedRequestsAndMappings)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";

  ValueDefinition mem2;
  mem2.id = "mem2_value";
  mem2.type = ValueType::U8;
  SetDirectRead(&mem2, "mem2", 0x20);
  package.values.push_back(mem2);

  ValueDefinition first = mem2;
  first.id = "first";
  first.type = ValueType::U16;
  SetDirectRead(&first, "mem1", 0x10, Endian::Big);
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
  std::get<DirectMemoryRead>(package.values.front().read).area_id = "missing";

  FakeDataSource data_source;
  std::string error;
  EXPECT_FALSE(EvaluatePackage(package, data_source, &error));
  EXPECT_NE(error.find("unknown memory area"), std::string::npos);
  EXPECT_EQ(data_source.grouped_read_count, 0);

  package = ValidPackage();
  std::get<DirectMemoryRead>(package.values.front().read).address = 0x1000;
  EXPECT_FALSE(EvaluatePackage(package, data_source, &error));
  EXPECT_NE(error.find("outside memory area"), std::string::npos);
  EXPECT_EQ(data_source.grouped_read_count, 0);
}

TEST(CheevoMapV2Decoder, DecodesIntegerTypeMatrix)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";

  auto add_value = [&package](std::string id, ValueType type, u64 address, Endian endian) {
    ValueDefinition value;
    value.id = std::move(id);
    value.type = type;
    SetDirectRead(&value, "mem1", address, endian);
    package.values.push_back(std::move(value));
  };

  add_value("u8_zero", ValueType::U8, 0x00, Endian::None);
  add_value("u8_max", ValueType::U8, 0x01, Endian::None);
  add_value("u16_zero", ValueType::U16, 0x02, Endian::Big);
  add_value("u16_be_max", ValueType::U16, 0x04, Endian::Big);
  add_value("u16_le_value", ValueType::U16, 0x06, Endian::Little);
  add_value("u32_zero", ValueType::U32, 0x08, Endian::Little);
  add_value("u32_be_max", ValueType::U32, 0x0c, Endian::Big);
  add_value("u32_le_value", ValueType::U32, 0x10, Endian::Little);
  add_value("u64_zero", ValueType::U64, 0x18, Endian::Big);
  add_value("u64_be_max", ValueType::U64, 0x20, Endian::Big);
  add_value("u64_le_value", ValueType::U64, 0x28, Endian::Little);

  add_value("s8_neg1", ValueType::S8, 0x30, Endian::None);
  add_value("s8_min", ValueType::S8, 0x31, Endian::None);
  add_value("s16_be_min", ValueType::S16, 0x32, Endian::Big);
  add_value("s16_le_neg1", ValueType::S16, 0x34, Endian::Little);
  add_value("s32_be_min", ValueType::S32, 0x38, Endian::Big);
  add_value("s32_le_neg1", ValueType::S32, 0x40, Endian::Little);
  add_value("s64_be_min", ValueType::S64, 0x48, Endian::Big);
  add_value("s64_le_neg1", ValueType::S64, 0x50, Endian::Little);

  FakeDataSource data_source;
  data_source.Write(0x00, {0x00});
  data_source.Write(0x01, {0xff});
  data_source.Write(0x02, {0x00, 0x00});
  data_source.Write(0x04, {0xff, 0xff});
  data_source.Write(0x06, {0x34, 0x12});
  data_source.Write(0x08, {0x00, 0x00, 0x00, 0x00});
  data_source.Write(0x0c, {0xff, 0xff, 0xff, 0xff});
  data_source.Write(0x10, {0x78, 0x56, 0x34, 0x12});
  data_source.Write(0x18, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  data_source.Write(0x20, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
  data_source.Write(0x28, {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01});

  data_source.Write(0x30, {0xff});
  data_source.Write(0x31, {0x80});
  data_source.Write(0x32, {0x80, 0x00});
  data_source.Write(0x34, {0xff, 0xff});
  data_source.Write(0x38, {0x80, 0x00, 0x00, 0x00});
  data_source.Write(0x40, {0xff, 0xff, 0xff, 0xff});
  data_source.Write(0x48, {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
  data_source.Write(0x50, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;

  EXPECT_EQ(*result->values.at("u8_zero").AsUnsignedInteger(), 0u);
  EXPECT_EQ(*result->values.at("u8_max").AsUnsignedInteger(), 0xffu);
  EXPECT_EQ(*result->values.at("u16_zero").AsUnsignedInteger(), 0u);
  EXPECT_EQ(*result->values.at("u16_be_max").AsUnsignedInteger(), 0xffffu);
  EXPECT_EQ(*result->values.at("u16_le_value").AsUnsignedInteger(), 0x1234u);
  EXPECT_EQ(*result->values.at("u32_zero").AsUnsignedInteger(), 0u);
  EXPECT_EQ(*result->values.at("u32_be_max").AsUnsignedInteger(), std::numeric_limits<u32>::max());
  EXPECT_EQ(*result->values.at("u32_le_value").AsUnsignedInteger(), 0x12345678u);
  EXPECT_EQ(*result->values.at("u64_zero").AsUnsignedInteger(), 0u);
  EXPECT_EQ(*result->values.at("u64_be_max").AsUnsignedInteger(), std::numeric_limits<u64>::max());
  EXPECT_EQ(*result->values.at("u64_le_value").AsUnsignedInteger(), 0x0102030405060708ull);

  EXPECT_EQ(*result->values.at("s8_neg1").AsSignedInteger(), -1);
  EXPECT_EQ(*result->values.at("s8_min").AsSignedInteger(), std::numeric_limits<s8>::min());
  EXPECT_EQ(*result->values.at("s16_be_min").AsSignedInteger(), std::numeric_limits<s16>::min());
  EXPECT_EQ(*result->values.at("s16_le_neg1").AsSignedInteger(), -1);
  EXPECT_EQ(*result->values.at("s32_be_min").AsSignedInteger(), std::numeric_limits<s32>::min());
  EXPECT_EQ(*result->values.at("s32_le_neg1").AsSignedInteger(), -1);
  EXPECT_EQ(*result->values.at("s64_be_min").AsSignedInteger(), std::numeric_limits<s64>::min());
  EXPECT_EQ(*result->values.at("s64_le_neg1").AsSignedInteger(), -1);
  EXPECT_EQ(data_source.grouped_read_count, 1);
  EXPECT_EQ(data_source.single_read_count, 0);
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
    SetDirectRead(&value, "mem1", address, endian);
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
  std::get<DirectMemoryRead>(failed.read).address = 0x20;
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

TEST(CheevoMapV2PointerRuntime, ResolvesOneOffsetChain)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";
  package.values.push_back(PointerValue("score", ValueType::U32, {0x10}));

  FakeDataSource data_source;
  WriteU32(&data_source, 0x100, 0x200, Endian::Big);
  data_source.Write(0x210, {0x12, 0x34, 0x56, 0x78});

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(*result->values.at("score").AsUnsignedInteger(), 0x12345678u);
  ASSERT_EQ(data_source.grouped_requests.size(), 2u);
  EXPECT_EQ(data_source.grouped_requests[0].front().address, 0x100u);
  EXPECT_EQ(data_source.grouped_requests[1].front().address, 0x210u);
}

TEST(CheevoMapV2PointerRuntime, ResolvesMultiLevelChainWithGroupedStages)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";
  package.values.push_back(PointerValue("score", ValueType::U16, {0x0, 0x10, 0x20}));

  FakeDataSource data_source;
  WriteU32(&data_source, 0x100, 0x200, Endian::Big);
  WriteU32(&data_source, 0x200, 0x300, Endian::Big);
  WriteU32(&data_source, 0x310, 0x400, Endian::Big);
  data_source.Write(0x420, {0x12, 0x34});

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(*result->values.at("score").AsUnsignedInteger(), 0x1234u);
  ASSERT_EQ(data_source.grouped_requests.size(), 4u);
  EXPECT_EQ(data_source.grouped_requests[0].front().address, 0x100u);
  EXPECT_EQ(data_source.grouped_requests[1].front().address, 0x200u);
  EXPECT_EQ(data_source.grouped_requests[2].front().address, 0x310u);
  EXPECT_EQ(data_source.grouped_requests[3].front().address, 0x420u);
}

TEST(CheevoMapV2PointerRuntime, PointerAndFinalEndiannessAreIndependent)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";
  package.values.push_back(
      PointerValue("score", ValueType::U32, {0x10}, Endian::Little, Endian::Big));

  FakeDataSource data_source;
  WriteU32(&data_source, 0x100, 0x200, Endian::Little);
  data_source.Write(0x210, {0x12, 0x34, 0x56, 0x78});

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(*result->values.at("score").AsUnsignedInteger(), 0x12345678u);
}

TEST(CheevoMapV2PointerRuntime, DirectAndPointerValuesShareInitialStage)
{
  Package package = ValidPackage();
  package.values.push_back(PointerValue("pointer_score", ValueType::U16, {0x10}));

  FakeDataSource data_source;
  data_source.Write(0x10, {0x00, 0x07});
  WriteU32(&data_source, 0x100, 0x200, Endian::Big);
  data_source.Write(0x210, {0x00, 0x08});

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(*result->values.at("coins").AsUnsignedInteger(), 7u);
  EXPECT_EQ(*result->values.at("pointer_score").AsUnsignedInteger(), 8u);
  ASSERT_EQ(data_source.grouped_requests.size(), 2u);
  ASSERT_EQ(data_source.grouped_requests[0].size(), 2u);
  EXPECT_EQ(data_source.grouped_requests[0][0].address, 0x10u);
  EXPECT_EQ(data_source.grouped_requests[0][1].address, 0x100u);
}

TEST(CheevoMapV2PointerRuntime, ReusesInitialDirectReadForExactPointerFinalRequest)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";
  package.values.push_back(DirectValue("direct", ValueType::U32, "mem1", 0x210, Endian::Big));
  package.values.push_back(PointerValue("pointer", ValueType::U32, {0x10}));

  FakeDataSource data_source;
  WriteU32(&data_source, 0x100, 0x200, Endian::Big);
  data_source.Write(0x210, {0x12, 0x34, 0x56, 0x78});

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(*result->values.at("direct").AsUnsignedInteger(), 0x12345678u);
  EXPECT_EQ(*result->values.at("pointer").AsUnsignedInteger(), 0x12345678u);
  ASSERT_EQ(data_source.grouped_requests.size(), 1u);
  ASSERT_EQ(data_source.grouped_requests[0].size(), 2u);
  EXPECT_EQ(data_source.grouped_requests[0][0].address, 0x100u);
  EXPECT_EQ(data_source.grouped_requests[0][1].address, 0x210u);
}

TEST(CheevoMapV2PointerRuntime, ReusesInitialBytesWithDifferentFinalDecodeType)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";
  package.values.push_back(DirectValue("direct", ValueType::U32, "mem1", 0x210, Endian::Big));
  package.values.push_back(PointerValue("pointer", ValueType::F32, {0x10}));

  FakeDataSource data_source;
  WriteU32(&data_source, 0x100, 0x200, Endian::Big);
  data_source.Write(0x210, {0x3f, 0x80, 0x00, 0x00});

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(*result->values.at("direct").AsUnsignedInteger(), 0x3f800000u);
  EXPECT_DOUBLE_EQ(*result->values.at("pointer").AsFloatingPoint(), 1.0);
  EXPECT_EQ(data_source.grouped_read_count, 1);
}

TEST(CheevoMapV2PointerRuntime, DoesNotReuseInitialReadWhenSizeDiffers)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";
  package.values.push_back(DirectValue("direct", ValueType::U16, "mem1", 0x210, Endian::Big));
  package.values.push_back(PointerValue("pointer", ValueType::U32, {0x10}));

  FakeDataSource data_source;
  WriteU32(&data_source, 0x100, 0x200, Endian::Big);
  data_source.Write(0x210, {0x12, 0x34, 0x56, 0x78});

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(*result->values.at("direct").AsUnsignedInteger(), 0x1234u);
  EXPECT_EQ(*result->values.at("pointer").AsUnsignedInteger(), 0x12345678u);
  ASSERT_EQ(data_source.grouped_requests.size(), 2u);
  ASSERT_EQ(data_source.grouped_requests[1].size(), 1u);
  EXPECT_EQ(data_source.grouped_requests[1][0].address, 0x210u);
  EXPECT_EQ(data_source.grouped_requests[1][0].size, 4u);
}

TEST(CheevoMapV2PointerRuntime, DoesNotReuseInitialReadWhenAreaDiffers)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";
  package.values.push_back(DirectValue("direct", ValueType::U32, "mem1", 0x210, Endian::Big));

  ValueDefinition pointer;
  pointer.id = "pointer";
  pointer.type = ValueType::U32;
  SetPointerChainRead(&pointer, "mem1", 0x100, "mem2", {0x10}, Endian::Big, Endian::Big);
  package.values.push_back(pointer);

  FakeDataSource data_source;
  data_source.memory_areas = {MemoryArea{"mem1", "MEM1", "emulated-physical", 0x00000000, 0x1000},
                              MemoryArea{"mem2", "MEM2", "emulated-physical", 0x00000000, 0x1000}};
  WriteU32(&data_source, 0x100, 0x200, Endian::Big);
  data_source.Write(0x210, {0x12, 0x34, 0x56, 0x78});

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(*result->values.at("direct").AsUnsignedInteger(), 0x12345678u);
  EXPECT_EQ(*result->values.at("pointer").AsUnsignedInteger(), 0x12345678u);
  ASSERT_EQ(data_source.grouped_requests.size(), 2u);
  ASSERT_EQ(data_source.grouped_requests[1].size(), 1u);
  EXPECT_EQ(data_source.grouped_requests[1][0].memory_area_id, "mem2");
  EXPECT_EQ(data_source.grouped_requests[1][0].address, 0x210u);
}

TEST(CheevoMapV2PointerRuntime, ReusesFailedInitialReadAsUnavailableWithoutReread)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";
  package.values.push_back(DirectValue("direct", ValueType::U32, "mem1", 0x210, Endian::Big));
  package.values.push_back(PointerValue("pointer", ValueType::U32, {0x10}));

  FakeDataSource data_source;
  WriteU32(&data_source, 0x100, 0x200, Endian::Big);
  data_source.Write(0x210, {0x12, 0x34, 0x56, 0x78});
  data_source.failed_addresses.insert(0x210);

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_FALSE(result->values.at("direct").IsAvailable());
  EXPECT_FALSE(result->values.at("pointer").IsAvailable());
  EXPECT_EQ(data_source.grouped_read_count, 1);
}

TEST(CheevoMapV2PointerRuntime, HandlesMixedCachedAndUncachedFinalRequests)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";
  package.values.push_back(DirectValue("direct", ValueType::U32, "mem1", 0x210, Endian::Big));
  package.values.push_back(PointerValue("cached", ValueType::U32, {0x10}));

  ValueDefinition uncached = PointerValue("uncached", ValueType::U32, {0x20});
  SetPointerChainRead(&uncached, "mem1", 0x104, "mem1", {0x20}, Endian::Big, Endian::Big);
  package.values.push_back(uncached);

  FakeDataSource data_source;
  WriteU32(&data_source, 0x100, 0x200, Endian::Big);
  WriteU32(&data_source, 0x104, 0x300, Endian::Big);
  data_source.Write(0x210, {0x00, 0x00, 0x00, 0x11});
  data_source.Write(0x320, {0x00, 0x00, 0x00, 0x22});

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(*result->values.at("direct").AsUnsignedInteger(), 0x11u);
  EXPECT_EQ(*result->values.at("cached").AsUnsignedInteger(), 0x11u);
  EXPECT_EQ(*result->values.at("uncached").AsUnsignedInteger(), 0x22u);
  ASSERT_EQ(data_source.grouped_requests.size(), 2u);
  ASSERT_EQ(data_source.grouped_requests[1].size(), 1u);
  EXPECT_EQ(data_source.grouped_requests[1][0].address, 0x320u);
}

TEST(CheevoMapV2PointerRuntime, MultiplePointerValuesReuseOneInitialResult)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";
  package.values.push_back(DirectValue("direct", ValueType::U32, "mem1", 0x210, Endian::Big));
  package.values.push_back(PointerValue("as_u32", ValueType::U32, {0x10}));
  package.values.push_back(PointerValue("as_f32", ValueType::F32, {0x10}));

  FakeDataSource data_source;
  WriteU32(&data_source, 0x100, 0x200, Endian::Big);
  data_source.Write(0x210, {0x3f, 0x80, 0x00, 0x00});

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(*result->values.at("direct").AsUnsignedInteger(), 0x3f800000u);
  EXPECT_EQ(*result->values.at("as_u32").AsUnsignedInteger(), 0x3f800000u);
  EXPECT_DOUBLE_EQ(*result->values.at("as_f32").AsFloatingPoint(), 1.0);
  EXPECT_EQ(data_source.grouped_read_count, 1);
}

TEST(CheevoMapV2PointerRuntime, DeduplicatesRootIntermediateAndFinalRequests)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";
  package.values.push_back(PointerValue("as_u32", ValueType::U32, {0x0, 0x10}));
  package.values.push_back(PointerValue("as_f32", ValueType::F32, {0x0, 0x10}));

  FakeDataSource data_source;
  WriteU32(&data_source, 0x100, 0x200, Endian::Big);
  WriteU32(&data_source, 0x200, 0x300, Endian::Big);
  data_source.Write(0x310, {0x3f, 0x80, 0x00, 0x00});

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(*result->values.at("as_u32").AsUnsignedInteger(), 0x3f800000u);
  EXPECT_DOUBLE_EQ(*result->values.at("as_f32").AsFloatingPoint(), 1.0);
  ASSERT_EQ(data_source.grouped_requests.size(), 3u);
  EXPECT_EQ(data_source.grouped_requests[0].size(), 1u);
  EXPECT_EQ(data_source.grouped_requests[1].size(), 1u);
  EXPECT_EQ(data_source.grouped_requests[2].size(), 1u);
}

TEST(CheevoMapV2PointerRuntime, ChainsOfDifferentDepthsRemainBatchedByStage)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";
  package.values.push_back(PointerValue("short", ValueType::U8, {0x4}, Endian::Big, Endian::None));
  package.values.push_back(
      PointerValue("long", ValueType::U8, {0x0, 0x2}, Endian::Big, Endian::None));

  FakeDataSource data_source;
  WriteU32(&data_source, 0x100, 0x200, Endian::Big);
  WriteU32(&data_source, 0x200, 0x300, Endian::Big);
  data_source.Write(0x204, {0x11});
  data_source.Write(0x302, {0x22});

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(*result->values.at("short").AsUnsignedInteger(), 0x11u);
  EXPECT_EQ(*result->values.at("long").AsUnsignedInteger(), 0x22u);
  ASSERT_EQ(data_source.grouped_requests.size(), 3u);
  EXPECT_EQ(data_source.grouped_requests[1].front().address, 0x200u);
  ASSERT_EQ(data_source.grouped_requests[2].size(), 2u);
  EXPECT_EQ(data_source.grouped_requests[2][0].address, 0x204u);
  EXPECT_EQ(data_source.grouped_requests[2][1].address, 0x302u);
}

TEST(CheevoMapV2PointerRuntime, RuntimeFailuresOnlyMakeAffectedChainUnavailable)
{
  Package package = ValidPackage();
  package.values.push_back(PointerValue("null_root", ValueType::U32, {0x10}));
  ValueDefinition failed = PointerValue("failed_final", ValueType::U32, {0x20});
  SetPointerChainRead(&failed, "mem1", 0x104, "mem1", {0x20}, Endian::Big, Endian::Big);
  package.values.push_back(failed);

  FakeDataSource data_source;
  data_source.Write(0x10, {0x00, 0x05});
  WriteU32(&data_source, 0x100, 0x0, Endian::Big);
  WriteU32(&data_source, 0x104, 0x300, Endian::Big);
  data_source.failed_addresses.insert(0x320);

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(*result->values.at("coins").AsUnsignedInteger(), 5u);
  EXPECT_FALSE(result->values.at("null_root").IsAvailable());
  EXPECT_FALSE(result->values.at("failed_final").IsAvailable());
}

TEST(CheevoMapV2PointerRuntime, MultipleTargetAreasAndAdapterFailures)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";
  ValueDefinition mem2_value;
  mem2_value.id = "mem2_value";
  mem2_value.type = ValueType::U8;
  SetPointerChainRead(&mem2_value, "mem1", 0x100, "mem2", {0x4}, Endian::Big, Endian::None);
  package.values.push_back(mem2_value);

  ValueDefinition adapter_failure;
  adapter_failure.id = "adapter_failure";
  adapter_failure.type = ValueType::U8;
  SetPointerChainRead(&adapter_failure, "mem1", 0x104, "mem1", {0x0}, Endian::Big, Endian::None);
  package.values.push_back(adapter_failure);

  ValueDefinition wrong_area;
  wrong_area.id = "wrong_area";
  wrong_area.type = ValueType::U8;
  SetPointerChainRead(&wrong_area, "mem1", 0x108, "mem1", {0x0}, Endian::Big, Endian::None);
  package.values.push_back(wrong_area);

  FakeDataSource data_source;
  WriteU32(&data_source, 0x100, 0x10000020, Endian::Big);
  data_source.Write(0x10000024, {0x66});
  WriteU32(&data_source, 0x104, 0x220, Endian::Big);
  data_source.failed_pointer_resolutions.insert(0x220);
  WriteU32(&data_source, 0x108, 0x10000030, Endian::Big);

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(*result->values.at("mem2_value").AsUnsignedInteger(), 0x66u);
  EXPECT_FALSE(result->values.at("adapter_failure").IsAvailable());
  EXPECT_FALSE(result->values.at("wrong_area").IsAvailable());
}

TEST(CheevoMapV2PointerRuntime, IntermediateFailuresOnlyMakeAffectedChainUnavailable)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";
  package.values.push_back(
      PointerValue("bad", ValueType::U8, {0x0, 0x1}, Endian::Big, Endian::None));
  ValueDefinition good = PointerValue("good", ValueType::U8, {0x2}, Endian::Big, Endian::None);
  SetPointerChainRead(&good, "mem1", 0x104, "mem1", {0x2}, Endian::Big, Endian::None);
  package.values.push_back(good);

  FakeDataSource data_source;
  WriteU32(&data_source, 0x100, 0x200, Endian::Big);
  WriteU32(&data_source, 0x104, 0x300, Endian::Big);
  data_source.failed_addresses.insert(0x200);
  data_source.Write(0x302, {0x44});

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_FALSE(result->values.at("bad").IsAvailable());
  EXPECT_EQ(*result->values.at("good").AsUnsignedInteger(), 0x44u);
}

TEST(CheevoMapV2PointerRuntime, OverflowAndBoundaryFailuresBecomeUnavailable)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";
  package.values.push_back(PointerValue(
      "overflow", ValueType::U8, {std::numeric_limits<u64>::max()}, Endian::Big, Endian::None));
  ValueDefinition boundary = PointerValue("boundary", ValueType::U16, {0xfff});
  SetPointerChainRead(&boundary, "mem1", 0x104, "mem1", {0xfff}, Endian::Big, Endian::Big);
  package.values.push_back(boundary);

  FakeDataSource data_source;
  WriteU32(&data_source, 0x100, 0x10, Endian::Big);
  WriteU32(&data_source, 0x104, 0x1, Endian::Big);

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_FALSE(result->values.at("overflow").IsAvailable());
  EXPECT_FALSE(result->values.at("boundary").IsAvailable());
}

TEST(CheevoMapV2PointerRuntime, StaticPointerPlanFailuresPreventMemoryReads)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";
  ValueDefinition value = PointerValue("score", ValueType::U32, {0x10});
  std::get<PointerChainRead>(value.read).base.area_id = "missing";
  package.values.push_back(value);

  FakeDataSource data_source;
  std::string error;
  EXPECT_FALSE(EvaluatePackage(package, data_source, &error));
  EXPECT_NE(error.find("unknown pointer_chain.base area"), std::string::npos);
  EXPECT_EQ(data_source.grouped_read_count, 0);

  package.values.front() = PointerValue("score", ValueType::U32, {0x10});
  std::get<PointerChainRead>(package.values.front().read).target_area_id = "missing";
  EXPECT_FALSE(EvaluatePackage(package, data_source, &error));
  EXPECT_NE(error.find("unknown pointer_chain.target_area"), std::string::npos);
  EXPECT_EQ(data_source.grouped_read_count, 0);
}

TEST(CheevoMapV2PointerRuntime, DirectOnlyStillMakesExactlyOneGroupedRead)
{
  Package package = ValidPackage();
  FakeDataSource data_source;
  data_source.Write(0x10, {0x12, 0x34});

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(*result->values.at("coins").AsUnsignedInteger(), 0x1234u);
  EXPECT_EQ(data_source.grouped_read_count, 1);
  EXPECT_EQ(data_source.single_read_count, 0);
}

TEST(CheevoMapV2PointerRuntimeState, PublishesPointerAvailabilityDeltas)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";
  package.values.push_back(PointerValue("score", ValueType::U32, {0x10}));

  FakeDataSource data_source;
  WriteU32(&data_source, 0x100, 0x200, Endian::Big);
  data_source.Write(0x210, {0x00, 0x00, 0x00, 0x01});

  StateStore store;
  std::vector<StateUpdate> updates;
  auto hook = store.RegisterUpdateCallback(
      [&updates](const StateUpdate& update) { updates.push_back(update); });
  (void)hook;

  const StateUpdate reset = store.Reset({{"score", StateValue::Unavailable()}});
  std::string error;
  auto applied = EvaluatePackageForSession(package, data_source, store, reset.session_id, &error);
  EXPECT_EQ(applied.status, PackageRuntimeStatus::Applied);
  ASSERT_EQ(updates.size(), 2u);
  EXPECT_EQ(*updates.back().values.at("score").AsUnsignedInteger(), 1u);

  data_source.Write(0x210, {0x00, 0x00, 0x00, 0x02});
  applied = EvaluatePackageForSession(package, data_source, store, reset.session_id, &error);
  EXPECT_EQ(applied.status, PackageRuntimeStatus::Applied);
  ASSERT_EQ(updates.size(), 3u);
  EXPECT_EQ(*updates.back().values.at("score").AsUnsignedInteger(), 2u);

  data_source.failed_addresses.insert(0x210);
  applied = EvaluatePackageForSession(package, data_source, store, reset.session_id, &error);
  EXPECT_EQ(applied.status, PackageRuntimeStatus::Applied);
  ASSERT_EQ(updates.size(), 4u);
  EXPECT_FALSE(updates.back().values.at("score").IsAvailable());

  data_source.failed_addresses.clear();
  applied = EvaluatePackageForSession(package, data_source, store, reset.session_id, &error);
  EXPECT_EQ(applied.status, PackageRuntimeStatus::Applied);
  ASSERT_EQ(updates.size(), 5u);
  EXPECT_EQ(*updates.back().values.at("score").AsUnsignedInteger(), 2u);
}

TEST(CheevoMapDolphinPointerNormalization, ResolvesPhysicalAndPowerPcAliases)
{
  const auto mem1_physical = ResolveDolphinPointerAddress("mem1", 0x00000010);
  ASSERT_TRUE(mem1_physical.success);
  EXPECT_EQ(mem1_physical.address, 0x10u);

  const auto mem2_physical = ResolveDolphinPointerAddress("mem2", 0x10000010);
  ASSERT_TRUE(mem2_physical.success);
  EXPECT_EQ(mem2_physical.address, 0x10000010u);

  EXPECT_EQ(ResolveDolphinPointerAddress("mem1", 0x80000010).address, 0x10u);
  EXPECT_EQ(ResolveDolphinPointerAddress("mem2", 0x90000010).address, 0x10000010u);
  EXPECT_EQ(ResolveDolphinPointerAddress("mem1", 0xc0000010).address, 0x10u);
  EXPECT_EQ(ResolveDolphinPointerAddress("mem2", 0xd0000010).address, 0x10000010u);
}

TEST(CheevoMapDolphinPointerNormalization, RejectsInvalidAndWrongAreaPointers)
{
  EXPECT_FALSE(ResolveDolphinPointerAddress("mem1", 0).success);
  EXPECT_FALSE(ResolveDolphinPointerAddress("mem1", 0x80000000).success);
  EXPECT_FALSE(ResolveDolphinPointerAddress("mem1", 0x90000010).success);
  EXPECT_FALSE(ResolveDolphinPointerAddress("mem2", 0x80000010).success);
  EXPECT_FALSE(ResolveDolphinPointerAddress("mem1", 0x04000000).success);
  EXPECT_FALSE(ResolveDolphinPointerAddress("mem2", 0x14000000).success);

  const auto mem1_boundary = ResolveDolphinPointerAddress("mem1", 0x03ffffff);
  ASSERT_TRUE(mem1_boundary.success);
  EXPECT_EQ(mem1_boundary.address, 0x03ffffffu);
  EXPECT_FALSE(ResolveDolphinPointerAddress("mem1", 0x04000000).success);

  const auto mem2_boundary = ResolveDolphinPointerAddress("mem2", 0x13ffffff);
  ASSERT_TRUE(mem2_boundary.success);
  EXPECT_EQ(mem2_boundary.address, 0x13ffffffu);
  EXPECT_FALSE(ResolveDolphinPointerAddress("mem2", 0x14000000).success);
}

TEST(CheevoMapV2RuntimeState, PublishesDeltasAndRejectsStaleSessions)
{
  Package package = ValidPackage();
  FakeDataSource data_source;
  data_source.Write(0x10, {0x00, 0x03});

  StateStore store;
  std::vector<StateUpdate> updates;
  auto hook = store.RegisterUpdateCallback(
      [&updates](const StateUpdate& update) { updates.push_back(update); });
  (void)hook;

  const StateUpdate reset = store.Reset({{"coins", StateValue::Unavailable()}});
  ASSERT_EQ(updates.size(), 1u);

  std::string error;
  auto applied = EvaluatePackageForSession(package, data_source, store, reset.session_id, &error);
  EXPECT_EQ(applied.status, PackageRuntimeStatus::Applied);
  ASSERT_EQ(updates.size(), 2u);
  EXPECT_EQ(*updates.back().values.at("coins").AsUnsignedInteger(), 3u);

  auto no_change = EvaluatePackageForSession(package, data_source, store, reset.session_id, &error);
  EXPECT_EQ(no_change.status, PackageRuntimeStatus::NoChanges);
  ASSERT_EQ(updates.size(), 2u);

  data_source.Write(0x10, {0x00, 0x04});
  auto changed = EvaluatePackageForSession(package, data_source, store, reset.session_id, &error);
  EXPECT_EQ(changed.status, PackageRuntimeStatus::Applied);
  ASSERT_EQ(updates.size(), 3u);
  EXPECT_EQ(*updates.back().values.at("coins").AsUnsignedInteger(), 4u);

  const StateUpdate reloaded = store.Reset({{"coins", StateValue::Unavailable()}});
  auto stale = EvaluatePackageForSession(package, data_source, store, reset.session_id, &error);
  EXPECT_EQ(stale.status, PackageRuntimeStatus::StaleSession);
  ASSERT_EQ(updates.size(), 4u);
  EXPECT_EQ(store.GetSnapshot().session_id, reloaded.session_id);
  EXPECT_FALSE(store.GetSnapshot().values.at("coins").IsAvailable());
}

TEST(CheevoMapV2RuntimeState, EvaluationFailureIsDistinctFromNoChanges)
{
  Package package = ValidPackage();
  std::get<DirectMemoryRead>(package.values.front().read).area_id = "missing";

  FakeDataSource data_source;
  StateStore store;
  const StateUpdate reset = store.Reset({{"coins", StateValue::Unavailable()}});

  std::string error;
  const auto result =
      EvaluatePackageForSession(package, data_source, store, reset.session_id, &error);
  EXPECT_EQ(result.status, PackageRuntimeStatus::EvaluationFailed);
  EXPECT_NE(error.find("unknown memory area"), std::string::npos);
  EXPECT_EQ(data_source.grouped_read_count, 0);
  EXPECT_EQ(data_source.single_read_count, 0);
}

TEST(CheevoMapV2ManagerLifecycle, StaleGenerationCommitDoesNotMutateOrPublish)
{
  Manager& manager = Manager::GetInstance();
  CheevoMapManagerTestAccessor::ClearV2PackageAndGeneration(manager, 0);

  const StateUpdate reset =
      CheevoMapManagerTestAccessor::ResetV2State(manager, {{"coins", StateValue::Unavailable()}});
  const auto before = manager.GetV2StateSnapshot();

  std::vector<StateUpdate> updates;
  auto hook = manager.RegisterV2StateUpdatedCallback(
      [&updates](const StateUpdate& update) { updates.push_back(update); });
  (void)hook;

  const u64 captured_session_id = reset.session_id;
  const u64 captured_generation = 4;
  CheevoMapManagerTestAccessor::SetV2PackageAndGeneration(manager, ValidPackage(),
                                                          captured_generation + 1);

  const auto result = CheevoMapManagerTestAccessor::CommitV2Evaluation(
      manager, captured_generation, captured_session_id,
      {{"coins", StateValue::UnsignedInteger(7)}});

  EXPECT_EQ(result.status, StateApplyStatus::StaleSession);
  EXPECT_FALSE(result.update);
  const auto snapshot = manager.GetV2StateSnapshot();
  EXPECT_EQ(snapshot.session_id, captured_session_id);
  EXPECT_EQ(snapshot.sequence, before.sequence);
  EXPECT_FALSE(snapshot.values.at("coins").IsAvailable());
  EXPECT_TRUE(updates.empty());

  hook.reset();
  CheevoMapManagerTestAccessor::ClearV2PackageAndGeneration(manager, 0);
  CheevoMapManagerTestAccessor::ResetV2State(manager, {});
}

TEST(CheevoMapV2ManagerLifecycle, CurrentGenerationCommitMutatesAndPublishes)
{
  Manager& manager = Manager::GetInstance();
  CheevoMapManagerTestAccessor::ClearV2PackageAndGeneration(manager, 0);

  const StateUpdate reset =
      CheevoMapManagerTestAccessor::ResetV2State(manager, {{"coins", StateValue::Unavailable()}});
  const auto before = manager.GetV2StateSnapshot();

  std::vector<StateUpdate> updates;
  auto hook = manager.RegisterV2StateUpdatedCallback(
      [&updates](const StateUpdate& update) { updates.push_back(update); });
  (void)hook;

  const u64 captured_session_id = reset.session_id;
  const u64 captured_generation = 4;
  CheevoMapManagerTestAccessor::SetV2PackageAndGeneration(manager, ValidPackage(),
                                                          captured_generation);

  const auto applied = CheevoMapManagerTestAccessor::CommitV2Evaluation(
      manager, captured_generation, captured_session_id,
      {{"coins", StateValue::UnsignedInteger(8)}});
  EXPECT_EQ(applied.status, StateApplyStatus::Applied);

  const auto snapshot = manager.GetV2StateSnapshot();
  EXPECT_EQ(snapshot.session_id, captured_session_id);
  EXPECT_EQ(snapshot.sequence, before.sequence + 1);
  EXPECT_EQ(*snapshot.values.at("coins").AsUnsignedInteger(), 8u);
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_EQ(*updates.back().values.at("coins").AsUnsignedInteger(), 8u);

  hook.reset();
  CheevoMapManagerTestAccessor::ClearV2PackageAndGeneration(manager, 0);
  CheevoMapManagerTestAccessor::ResetV2State(manager, {});
}

TEST(CheevoMapSchemaDispatch, DelegatesSchemaV1ToExistingParser)
{
  TempDir dir;
  const std::filesystem::path path = dir.WriteFile("v1.json", R"({
    "schema_version": 1,
    "game_id": "GAME01",
    "title": "Schema v1",
    "entries": [
      {"id": "coins", "label": "Coins",
       "read": {"addr": "0x80000010", "type": "u8"}}
    ]
  })");

  std::string error;
  const auto loaded = CheevoMap::LoadPackageFromFile(path.string(), &error);
  ASSERT_TRUE(loaded) << error;
  ASSERT_TRUE(std::holds_alternative<CheevoMap::File>(*loaded));
  EXPECT_EQ(std::get<CheevoMap::File>(*loaded).game_id, "GAME01");
}

TEST(CheevoMapSchemaDispatch, DelegatesSchemaV2ToV2Parser)
{
  TempDir dir;
  const std::filesystem::path path = dir.WriteFile("v2.json", R"({
    "schema_version": 2,
    "game": {"id": "GAME01"},
    "package": {"title": "Schema v2"},
    "values": []
  })");

  std::string error;
  const auto loaded = CheevoMap::LoadPackageFromFile(path.string(), &error);
  ASSERT_TRUE(loaded) << error;
  ASSERT_TRUE(std::holds_alternative<Package>(*loaded));
  EXPECT_EQ(std::get<Package>(*loaded).metadata.title, "Schema v2");
}

TEST(CheevoMapSchemaDispatch, RejectsUnknownSchemaVersion)
{
  TempDir dir;
  const std::filesystem::path path = dir.WriteFile("unknown.json", R"({
    "schema_version": 3,
    "game": {"id": "GAME01"},
    "package": {"title": "Unknown"},
    "values": []
  })");

  std::string error;
  EXPECT_FALSE(CheevoMap::LoadPackageFromFile(path.string(), &error));
  EXPECT_NE(error.find("unsupported schema_version"), std::string::npos);
}

TEST(CheevoMapSchemaDispatch, SchemaV1LoadFromFileRemainsDirectlyUsable)
{
  TempDir dir;
  const std::filesystem::path path = dir.WriteFile("v1-direct.json", R"({
    "schema_version": 1,
    "game_id": "GAME01",
    "title": "Schema v1",
    "entries": [
      {"id": "coins", "label": "Coins",
       "read": {"addr": "0x80000010", "type": "u8"}}
    ]
  })");

  std::string error;
  const auto file = CheevoMap::LoadFromFile(path.string(), &error);
  ASSERT_TRUE(file) << error;
  EXPECT_EQ(file->schema_version, 1u);
  EXPECT_EQ(file->entries.size(), 1u);
}
}  // namespace
