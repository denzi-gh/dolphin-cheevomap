// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cmath>
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
using CheevoMap::V2::BuildEvaluationPlan;
using CheevoMap::V2::BuildReadPlan;
using CheevoMap::V2::ConvertExpressionResultToDeclaredType;
using CheevoMap::V2::DecodeReadResults;
using CheevoMap::V2::DirectMemoryRead;
using CheevoMap::V2::EmulatorDataSource;
using CheevoMap::V2::EmulatorStatus;
using CheevoMap::V2::Endian;
using CheevoMap::V2::EvaluateExpression;
using CheevoMap::V2::EvaluatePackage;
using CheevoMap::V2::EvaluatePackageForSession;
using CheevoMap::V2::ExpressionConstant;
using CheevoMap::V2::ExpressionConstantType;
using CheevoMap::V2::ExpressionNode;
using CheevoMap::V2::ExpressionOperation;
using CheevoMap::V2::ExpressionOperator;
using CheevoMap::V2::ExpressionReference;
using CheevoMap::V2::ExpressionValueSource;
using CheevoMap::V2::GameIdentity;
using CheevoMap::V2::GameInfo;
using CheevoMap::V2::GetMutableReadDefinition;
using CheevoMap::V2::GetReadDefinition;
using CheevoMap::V2::IsExpressionBackedValue;
using CheevoMap::V2::IsReadBackedValue;
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
using CheevoMap::V2::ReadValueSource;
using CheevoMap::V2::StateApplyStatus;
using CheevoMap::V2::StateStore;
using CheevoMap::V2::StateUpdate;
using CheevoMap::V2::StateValue;
using CheevoMap::V2::StateValueMap;
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
  value->source = ReadValueSource{DirectMemoryRead{std::move(area_id), address, endian}};
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

ExpressionNode Ref(std::string id)
{
  return ExpressionNode{ExpressionReference{std::move(id)}};
}

ExpressionNode BoolConst(bool value)
{
  return ExpressionNode{
      ExpressionConstant{ExpressionConstantType::Boolean, value},
  };
}

ExpressionNode S64Const(s64 value)
{
  return ExpressionNode{
      ExpressionConstant{ExpressionConstantType::SignedInteger, value},
  };
}

ExpressionNode U64Const(u64 value)
{
  return ExpressionNode{
      ExpressionConstant{ExpressionConstantType::UnsignedInteger, value},
  };
}

ExpressionNode F64Const(double value)
{
  return ExpressionNode{
      ExpressionConstant{ExpressionConstantType::FloatingPoint, value},
  };
}

ExpressionNode StringConst(std::string value)
{
  return ExpressionNode{
      ExpressionConstant{ExpressionConstantType::String, std::move(value)},
  };
}

ExpressionNode Op(ExpressionOperator op, std::vector<ExpressionNode> arguments)
{
  return ExpressionNode{ExpressionOperation{op, std::move(arguments)}};
}

ValueDefinition ExpressionValue(std::string id, ValueType type, ExpressionNode expression)
{
  ValueDefinition value;
  value.id = std::move(id);
  value.type = type;
  value.source = ExpressionValueSource{std::move(expression)};
  return value;
}

void SetPointerChainRead(ValueDefinition* value, std::string base_area_id, u64 base_address,
                         std::vector<std::string> target_area_ids, std::vector<u64> offsets,
                         Endian pointer_endian, Endian final_endian = Endian::None)
{
  PointerChainRead read;
  read.base.area_id = std::move(base_area_id);
  read.base.address = base_address;
  read.target_area_ids = std::move(target_area_ids);
  read.offsets = std::move(offsets);
  read.pointer_type = PointerType::U32;
  read.pointer_endian = pointer_endian;
  read.endian = final_endian;
  value->source = ReadValueSource{std::move(read)};
}

void SetPointerChainRead(ValueDefinition* value, std::string base_area_id, u64 base_address,
                         std::string target_area_id, std::vector<u64> offsets,
                         Endian pointer_endian, Endian final_endian = Endian::None)
{
  const size_t offset_count = offsets.size();
  SetPointerChainRead(value, std::move(base_area_id), base_address,
                      std::vector<std::string>(offset_count, std::move(target_area_id)),
                      std::move(offsets), pointer_endian, final_endian);
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

void WriteU16(FakeDataSource* data_source, u64 address, u16 value, Endian endian)
{
  if (endian == Endian::Little)
  {
    data_source->Write(address, {static_cast<u8>(value), static_cast<u8>(value >> 8)});
    return;
  }

  data_source->Write(address, {static_cast<u8>(value >> 8), static_cast<u8>(value)});
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

std::filesystem::path FindRepositoryFile(const std::filesystem::path& relative_path)
{
  std::vector<std::filesystem::path> starts{std::filesystem::current_path()};
  const std::filesystem::path source_file_path(__FILE__);
  if (source_file_path.is_absolute())
    starts.push_back(source_file_path.parent_path());

  for (const std::filesystem::path& start : starts)
  {
    for (std::filesystem::path directory = start; !directory.empty();
         directory = directory.parent_path())
    {
      const std::filesystem::path candidate = directory / relative_path;
      if (std::filesystem::exists(candidate))
        return candidate;

      if (directory == directory.root_path())
        break;
    }
  }

  ADD_FAILURE() << "Could not find repository file: " << relative_path.string();
  return relative_path;
}

std::filesystem::path R8PP01ReferencePackagePath()
{
  return FindRepositoryFile("docs/examples/cheevomap-v2/R8PP01/cheevomap.json");
}

std::optional<Package> LoadR8PP01ReferencePackage(std::string* error)
{
  return CheevoMap::V2::LoadPackageFromFile(R8PP01ReferencePackagePath().string(), error);
}

constexpr u64 R8PP01_MEM1_BASE = 0x80000000;
constexpr u64 R8PP01_FLIP_ROOT_READ = R8PP01_MEM1_BASE + 0x0050b544;
constexpr u64 R8PP01_MINIGAME_ROOT_READ = R8PP01_MEM1_BASE + 0x004027f8;
constexpr u32 R8PP01_FLIP_ROOT_POINTER = 0x80001000;
constexpr u32 R8PP01_MINIGAME_ROOT_POINTER = 0x80002000;
constexpr u32 R8PP01_SHARED_POINTER = 0x80003000;
constexpr u32 R8PP01_MANSION_PATROL_POINTER = 0x80010000;
constexpr u32 R8PP01_TILT_ISLAND_POINTER = 0x80040000;
constexpr u32 R8PP01_FORGET_ME_NOT_POINTER = 0x80080000;
constexpr u32 R8PP01_HAMMER_WHACKER_POINTER = 0x800c0000;

void ConfigureR8PP01DataSource(FakeDataSource* data_source)
{
  data_source->identity = GameIdentity{"R8PP01", "", 0, 0};
  data_source->memory_areas = {
      MemoryArea{"mem1", "MEM1", "emulated-physical", R8PP01_MEM1_BASE, 0x04000000}};
}

void PopulateR8PP01ReferenceMemory(FakeDataSource* data_source, u32 tilt_status = 0x08000000,
                                   u32 tilt_lives = 1, bool write_tilt_pointer = true)
{
  WriteU32(data_source, R8PP01_FLIP_ROOT_READ, R8PP01_FLIP_ROOT_POINTER, Endian::Big);
  WriteU16(data_source, R8PP01_FLIP_ROOT_POINTER + 0x0a, 1, Endian::Big);

  WriteU32(data_source, R8PP01_MINIGAME_ROOT_READ, R8PP01_MINIGAME_ROOT_POINTER, Endian::Big);
  WriteU32(data_source, R8PP01_MINIGAME_ROOT_POINTER + 0x120, R8PP01_SHARED_POINTER, Endian::Big);
  WriteU32(data_source, R8PP01_SHARED_POINTER + 0x3e4, R8PP01_MANSION_PATROL_POINTER,
           Endian::Big);
  if (write_tilt_pointer)
    WriteU32(data_source, R8PP01_SHARED_POINTER + 0x3f4, R8PP01_TILT_ISLAND_POINTER, Endian::Big);
  WriteU32(data_source, R8PP01_SHARED_POINTER + 0x404, R8PP01_FORGET_ME_NOT_POINTER, Endian::Big);
  WriteU32(data_source, R8PP01_SHARED_POINTER + 0x408, R8PP01_HAMMER_WHACKER_POINTER,
           Endian::Big);

  WriteU32(data_source, R8PP01_MANSION_PATROL_POINTER + 0x0, 0x1f6, Endian::Big);
  WriteU32(data_source, R8PP01_MANSION_PATROL_POINTER + 0x58, 1, Endian::Big);
  WriteU32(data_source, R8PP01_MANSION_PATROL_POINTER + 0x1f8cc, 2, Endian::Big);
  WriteU32(data_source, R8PP01_MANSION_PATROL_POINTER + 0x1f8ec, 12345, Endian::Big);

  WriteU32(data_source, R8PP01_TILT_ISLAND_POINTER + 0x0, tilt_status, Endian::Big);
  WriteU32(data_source, R8PP01_TILT_ISLAND_POINTER + 0xc8, tilt_lives, Endian::Big);
  WriteU32(data_source, R8PP01_TILT_ISLAND_POINTER + 0x717c, 4, Endian::Big);
  WriteU32(data_source, R8PP01_TILT_ISLAND_POINTER + 0x6864, 777, Endian::Big);

  WriteU32(data_source, R8PP01_FORGET_ME_NOT_POINTER + 0x0, 0x1802, Endian::Big);
  data_source->Write(R8PP01_FORGET_ME_NOT_POINTER + 0x917, {0});
  WriteU32(data_source, R8PP01_FORGET_ME_NOT_POINTER + 0xb34, 7, Endian::Big);
  WriteU32(data_source, R8PP01_FORGET_ME_NOT_POINTER + 0xc34, 888, Endian::Big);

  WriteU32(data_source, R8PP01_HAMMER_WHACKER_POINTER + 0x30, 3, Endian::Big);
  WriteU32(data_source, R8PP01_HAMMER_WHACKER_POINTER + 0x2f0, 999, Endian::Big);
  WriteU32(data_source, R8PP01_HAMMER_WHACKER_POINTER + 0xd0, 1, Endian::Big);
}

void ExpectUnsignedValue(const StateValueMap& values, const std::string& id, u64 expected)
{
  SCOPED_TRACE(id);
  ASSERT_TRUE(values.contains(id));
  const std::optional<u64> actual = values.at(id).AsUnsignedInteger();
  ASSERT_TRUE(actual);
  EXPECT_EQ(*actual, expected);
}

void ExpectBoolValue(const StateValueMap& values, const std::string& id, bool expected)
{
  SCOPED_TRACE(id);
  ASSERT_TRUE(values.contains(id));
  const std::optional<bool> actual = values.at(id).AsBoolean();
  ASSERT_TRUE(actual);
  EXPECT_EQ(*actual, expected);
}

void ExpectUnavailableValue(const StateValueMap& values, const std::string& id)
{
  SCOPED_TRACE(id);
  ASSERT_TRUE(values.contains(id));
  EXPECT_FALSE(values.at(id).IsAvailable());
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

TEST(CheevoMapV2Parser, ParsesValueSourceSelection)
{
  std::string error;
  EXPECT_TRUE(ParseText(
      PackageJson(
          R"({"id":"score","type":"u32","read":{"area":"mem1","address":"0x10","endian":"big"}})"),
      &error))
      << error;
  EXPECT_TRUE(ParseText(PackageJson(PointerValueJson("u32", "")), &error)) << error;
  EXPECT_TRUE(ParseText(
      PackageJson(
          R"({"id":"has_score","type":"bool","expression":{"op":"gt","args":[{"const":{"type":"u64","value":"1"}},{"const":{"type":"u64","value":"0"}}]}})"),
      &error))
      << error;

  const std::vector<std::pair<std::string, std::string>> invalid = {
      {R"({"id":"bad","type":"u8","read":{"area":"mem1","address":"0x0"},"expression":{"const":{"type":"u64","value":"1"}}})",
       "exactly one"},
      {R"({"id":"bad","type":"u8"})", "exactly one"},
      {R"({"id":"bad","type":"string","bytes":8,"expression":{"const":{"type":"string","value":"ok"}}})",
       "bytes is invalid"},
      {R"({"id":"bad","type":"string","read":{"area":"mem1","address":"0x0"}})", "bytes"},
  };
  for (const auto& [value_json, expected_error] : invalid)
  {
    EXPECT_FALSE(ParseText(PackageJson(value_json), &error)) << value_json;
    EXPECT_NE(error.find(expected_error), std::string::npos) << error;
  }
}

TEST(CheevoMapV2Parser, ParsesExpressionReferencesAndConstantsStrictly)
{
  std::string error;
  const std::vector<std::string> valid_constants = {
      R"({"const":{"type":"bool","value":true}})",
      R"({"const":{"type":"s64","value":"-9223372036854775808"}})",
      R"({"const":{"type":"s64","value":"9223372036854775807"}})",
      R"({"const":{"type":"u64","value":"0"}})",
      R"({"const":{"type":"u64","value":"18446744073709551615"}})",
      R"({"const":{"type":"u64","value":"0x0b000002"}})",
      R"({"const":{"type":"f64","value":1.5}})",
      R"({"const":{"type":"string","value":"Game"}})",
  };
  for (const std::string& expression_json : valid_constants)
  {
    const std::string type = expression_json.find("bool") != std::string::npos   ? "bool" :
                             expression_json.find("s64") != std::string::npos    ? "s64" :
                             expression_json.find("f64") != std::string::npos    ? "f64" :
                             expression_json.find("string") != std::string::npos ? "string" :
                                                                                   "u64";
    EXPECT_TRUE(ParseText(
        PackageJson(R"({"id":"v","type":")" + type + R"(","expression":)" + expression_json + "}"),
        &error))
        << error;
  }

  EXPECT_TRUE(ParseText(
      PackageJson(
          R"({"id":"raw","type":"u8","read":{"area":"mem1","address":"0x0"}},{"id":"copy","type":"u8","expression":{"ref":"raw"}})"),
      &error))
      << error;

  const std::vector<std::pair<std::string, std::string>> invalid = {
      {R"({"ref":""})", "non-empty"},
      {R"({"ref":7})", "non-empty"},
      {R"({"ref":"a","extra":true})", "exactly ref"},
      {R"({"ref":"a","const":{"type":"u64","value":"1"}})", "exactly ref"},
      {R"({"ref":"a","op":"not"})", "exactly ref"},
      {R"({"const":{"type":"s64","value":"01"}})", "canonical"},
      {R"({"const":{"type":"u64","value":"1.0"}})", "canonical"},
      {R"({"const":{"type":"u64","value":"18446744073709551616"}})", "canonical"},
      {R"({"const":{"type":"u64","value":"-1"}})", "canonical"},
      {R"({"const":{"type":"u64","value":"0X1"}})", "canonical"},
      {R"({"const":{"type":"bool","value":"true"}})", "Boolean"},
      {R"({"const":{"type":"s64","value":1}})", "string"},
      {R"({"const":{"type":"u64","value":1}})", "string"},
      {R"({"const":{"type":"f64","value":"1.0"}})", "finite"},
      {R"({"const":{"type":"string","value":1}})", "string"},
      {R"({"const":{"type":"u64","value":"1","extra":true}})", "unknown field"},
      {R"({"const":{"type":"u32","value":"1"}})", "unsupported"},
  };
  for (const auto& [expression_json, expected_error] : invalid)
  {
    EXPECT_FALSE(ParseText(
        PackageJson(R"({"id":"v","type":"u64","expression":)" + expression_json + "}"), &error))
        << expression_json;
    EXPECT_NE(error.find(expected_error), std::string::npos) << error;
  }

  picojson::object non_finite;
  non_finite["id"] = picojson::value("v");
  non_finite["type"] = picojson::value("f64");
  non_finite["expression"] = picojson::value(picojson::object{
      {"const", picojson::value(picojson::object{
                    {"type", picojson::value("f64")},
                    {"value", NonFiniteNumber(std::numeric_limits<double>::infinity())}})}});
  EXPECT_FALSE(ParsePackage(MinimalRootWithValue(picojson::value(non_finite)), &error));
  EXPECT_NE(error.find("finite"), std::string::npos) << error;
}

TEST(CheevoMapV2Parser, ParsesExpressionOperationsAndLimitsStrictly)
{
  std::string error;
  EXPECT_TRUE(ParseText(
      PackageJson(
          R"({"id":"v","type":"bool","expression":{"op":"and","args":[{"const":{"type":"bool","value":true}},{"op":"not","args":[{"const":{"type":"bool","value":false}}]}]}})"),
      &error))
      << error;

  const std::vector<std::pair<std::string, std::string>> invalid = {
      {R"({"op":"unknown","args":[]})", "unknown"},
      {R"({"args":[]})", "requires op"},
      {R"({"op":"not"})", "requires args"},
      {R"({"op":"not","args":true})", "array"},
      {R"({"op":"not","args":[],"extra":true})", "unknown field"},
      {R"({"op":"not","args":[]})", "exactly 1"},
      {R"({"op":"eq","args":[{"const":{"type":"u64","value":"1"}}]})", "exactly 2"},
      {R"({"op":"if","args":[{"const":{"type":"bool","value":true}}]})", "exactly 3"},
      {R"({"op":"and","args":[{"const":{"type":"bool","value":true}}]})", "2..16"},
  };
  for (const auto& [expression_json, expected_error] : invalid)
  {
    EXPECT_FALSE(ParseText(
        PackageJson(R"({"id":"v","type":"bool","expression":)" + expression_json + "}"), &error))
        << expression_json;
    EXPECT_NE(error.find(expected_error), std::string::npos) << error;
  }

  std::string too_many_args = R"({"op":"and","args":[)";
  for (int i = 0; i < 17; ++i)
  {
    if (i != 0)
      too_many_args += ",";
    too_many_args += R"({"const":{"type":"bool","value":true}})";
  }
  too_many_args += "]}";
  EXPECT_FALSE(ParseText(
      PackageJson(R"({"id":"v","type":"bool","expression":)" + too_many_args + "}"), &error));
  EXPECT_NE(error.find("more than 16"), std::string::npos) << error;

  std::string deep = R"({"const":{"type":"bool","value":true}})";
  for (int i = 0; i < 32; ++i)
    deep = R"({"op":"not","args":[)" + deep + "]}";
  EXPECT_FALSE(
      ParseText(PackageJson(R"({"id":"v","type":"bool","expression":)" + deep + "}"), &error));
  EXPECT_NE(error.find("depth"), std::string::npos) << error;

  std::string many_nodes = R"({"op":"and","args":[)";
  for (int i = 0; i < 16; ++i)
  {
    if (i != 0)
      many_nodes += ",";
    many_nodes += R"({"op":"and","args":[)";
    for (int j = 0; j < 16; ++j)
    {
      if (j != 0)
        many_nodes += ",";
      many_nodes += R"({"const":{"type":"bool","value":true}})";
    }
    many_nodes += "]}";
  }
  many_nodes += "]}";
  EXPECT_FALSE(ParseText(PackageJson(R"({"id":"v","type":"bool","expression":)" + many_nodes + "}"),
                         &error));
  EXPECT_NE(error.find("node count"), std::string::npos) << error;
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
      PointerValueJson(
          "u32", "",
          R"("base":{"area":"mem2","address":"0x860"},"target_areas":["mem2"],"offsets":["0x10"],"pointer_type":"u32","endian":"big")"),
      PointerValueJson(
          "u32", "",
          R"("base":{"area":"mem2","address":"0x860"},"target_areas":["mem1","mem1","mem2"],"offsets":["0x0","0x160","0x420"],"pointer_type":"u32","endian":"big")"),
      PointerValueJson(
          "u32", "",
          R"("base":{"area":"mem2","address":"0x860"},"target_areas":["mem1","mem1","mem1","mem1","mem1","mem1","mem1","mem1","mem1","mem1","mem1","mem1","mem1","mem1","mem1","mem1"],"offsets":["0x0","0x1","0x2","0x3","0x4","0x5","0x6","0x7","0x8","0x9","0xa","0xb","0xc","0xd","0xe","0xf"],"pointer_type":"u32","endian":"big")"),
  };

  for (const std::string& value : values)
  {
    std::string error;
    const auto package = ParseText(PackageJson(value), &error);
    EXPECT_TRUE(package) << error << "\n" << value;
  }

  std::string error;
  const auto mixed = ParseText(
      PackageJson(
          R"({"id":"direct","type":"u8","read":{"area":"mem1","address":"0x0"}},)" +
          PointerValueJson(
              "u32", "",
              R"("base":{"area":"mem2","address":"0x860"},"target_areas":["mem1","mem1","mem2"],"offsets":["0x0","0x160","0x420"],"pointer_type":"u32","endian":"little")")),
      &error);
  ASSERT_TRUE(mixed) << error;
  ASSERT_EQ(mixed->values.size(), 2u);
  const auto& chain = std::get<PointerChainRead>(*GetReadDefinition(mixed->values[1]));
  EXPECT_EQ(chain.target_area_ids, (std::vector<std::string>{"mem1", "mem1", "mem2"}));
  EXPECT_EQ(chain.pointer_endian, Endian::Little);
  EXPECT_EQ(chain.endian, Endian::Big);

  const auto shorthand = ParseText(PackageJson(PointerValueJson("u32", "")), &error);
  ASSERT_TRUE(shorthand) << error;
  const auto& shorthand_chain =
      std::get<PointerChainRead>(*GetReadDefinition(shorthand->values[0]));
  EXPECT_EQ(shorthand_chain.target_area_ids, (std::vector<std::string>{"mem2", "mem2", "mem2"}));
}

TEST(CheevoMapV2Parser, RejectsInvalidPointerChainPackages)
{
  const std::
      vector<std::pair<std::string, std::string>>
          cases =
              {
                  {std::string(
                       R"({"id":"p","type":"u32","read":{"area":"mem1","address":"0x0","pointer_chain":{)") +
                       GoodPointerChainJson() + R"(},"endian":"big"}})",
                   "must not mix"},
                  {std::string(
                       R"({"id":"p","type":"u32","read":{"area":"mem1","pointer_chain":{)") +
                       GoodPointerChainJson() + R"(},"endian":"big"}})",
                   "must not mix"},
                  {std::string(
                       R"({"id":"p","type":"u32","read":{"address":"0x0","pointer_chain":{)") +
                       GoodPointerChainJson() + R"(},"endian":"big"}})",
                   "must not mix"},
                  {R"({"id":"p","type":"u32","read":{"endian":"big"}})", "either direct"},
                  {PointerValueJson("u32", "", GoodPointerChainJson(),
                                    R"(,"endian":"big","extra":true)"),
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
                       R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","target_areas":["mem2"],"offsets":["0x10"],"pointer_type":"u32","endian":"big")"),
                   "not both"},
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
                       R"("base":{"area":"mem2","address":"0x860"},"target_areas":1,"offsets":["0x10"],"pointer_type":"u32","endian":"big")"),
                   "target_areas must be an array"},
                  {PointerValueJson(
                       "u32", "",
                       R"("base":{"area":"mem2","address":"0x860"},"target_areas":[],"offsets":["0x10"],"pointer_type":"u32","endian":"big")"),
                   "target_areas must contain at least 1"},
                  {PointerValueJson(
                       "u32", "",
                       R"("base":{"area":"mem2","address":"0x860"},"target_areas":["mem1","mem1","mem1","mem1","mem1","mem1","mem1","mem1","mem1","mem1","mem1","mem1","mem1","mem1","mem1","mem1","mem1"],"offsets":["0x0","0x1","0x2","0x3","0x4","0x5","0x6","0x7","0x8","0x9","0xa","0xb","0xc","0xd","0xe","0xf"],"pointer_type":"u32","endian":"big")"),
                   "target_areas must contain at most 16"},
                  {PointerValueJson(
                       "u32", "",
                       R"("base":{"area":"mem2","address":"0x860"},"target_areas":["mem2",1],"offsets":["0x0","0x10"],"pointer_type":"u32","endian":"big")"),
                   "target_areas[1]"},
                  {PointerValueJson(
                       "u32", "",
                       R"("base":{"area":"mem2","address":"0x860"},"target_areas":[""],"offsets":["0x10"],"pointer_type":"u32","endian":"big")"),
                   "target_areas[0]"},
                  {PointerValueJson(
                       "u32", "",
                       R"("base":{"area":"mem2","address":"0x860"},"target_areas":["mem2"],"offsets":["0x0","0x10"],"pointer_type":"u32","endian":"big")"),
                   "exactly one entry per offset"},
                  {PointerValueJson(
                       "u32", "",
                       R"("base":{"area":"mem2","address":"0x860"},"target_areas":["mem2","mem2"],"offsets":["0x10"],"pointer_type":"u32","endian":"big")"),
                   "exactly one entry per offset"},
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
                  {PointerValueJson("u32", "", R"("base":{"area":"mem2","address":"0x860"},"target_area":"mem2","offsets":["0xzz"],"pointer_type":"u32","endian":"big")"),
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
                  {PointerValueJson("u8", "", GoodPointerChainJson(), R"(,"endian":"big")"),
                   "not valid"},
                  {PointerValueJson("string", R"("bytes":4,)", GoodPointerChainJson(),
                                    R"(,"endian":"big")"),
                   "not valid"},
                  {PointerValueJson("string", "", GoodPointerChainJson(), ""), "bytes"},
                  {PointerValueJson("string", R"("bytes":0,)", GoodPointerChainJson(), ""),
                   "bytes"},
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
  std::get<DirectMemoryRead>(*GetMutableReadDefinition(package.values.front())).area_id = "missing";

  FakeDataSource data_source;
  std::string error;
  EXPECT_FALSE(EvaluatePackage(package, data_source, &error));
  EXPECT_NE(error.find("unknown memory area"), std::string::npos);
  EXPECT_EQ(data_source.grouped_read_count, 0);

  package = ValidPackage();
  std::get<DirectMemoryRead>(*GetMutableReadDefinition(package.values.front())).address = 0x1000;
  EXPECT_FALSE(EvaluatePackage(package, data_source, &error));
  EXPECT_NE(error.find("outside memory area"), std::string::npos);
  EXPECT_EQ(data_source.grouped_read_count, 0);
}

TEST(CheevoMapV2Planner, PlansPointerTargetAreasByStage)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";

  ValueDefinition shorthand =
      PointerValue("shorthand", ValueType::U8, {0x0, 0x4}, Endian::Big, Endian::None);
  package.values.push_back(shorthand);

  ValueDefinition cross;
  cross.id = "cross";
  cross.type = ValueType::U32;
  SetPointerChainRead(&cross, "mem2", 0x860, std::vector<std::string>{"mem1", "mem1", "mem2"},
                      {0x0, 0x160, 0x420}, Endian::Big, Endian::Big);
  package.values.push_back(cross);

  std::string error;
  const auto plan = BuildReadPlan(package, FakeDataSource().GetMemoryAreas(), &error);
  ASSERT_TRUE(plan) << error;
  ASSERT_EQ(plan->pointer_chains.size(), 2u);

  ASSERT_EQ(plan->pointer_chains[0].target_areas.size(), 2u);
  EXPECT_EQ(plan->pointer_chains[0].target_areas[0].id, "mem1");
  EXPECT_EQ(plan->pointer_chains[0].target_areas[1].id, "mem1");

  ASSERT_EQ(plan->pointer_chains[1].target_areas.size(), 3u);
  EXPECT_EQ(plan->pointer_chains[1].target_areas[0].id, "mem1");
  EXPECT_EQ(plan->pointer_chains[1].target_areas[1].id, "mem1");
  EXPECT_EQ(plan->pointer_chains[1].target_areas[2].id, "mem2");
  EXPECT_EQ(plan->pointer_chains[1].target_areas[2].base_address, 0x10000000u);
}

TEST(CheevoMapV2Planner, RejectsInvalidPointerTargetAreaStagesBeforeMemoryAccess)
{
  auto expect_failed_plan = [](ValueDefinition value, std::vector<MemoryArea> areas,
                               std::string_view expected_error) {
    Package package;
    package.game.id = "GAME01";
    package.metadata.title = "Test";
    package.values.push_back(std::move(value));

    FakeDataSource data_source;
    data_source.memory_areas = std::move(areas);
    std::string error;
    EXPECT_FALSE(EvaluatePackage(package, data_source, &error));
    EXPECT_NE(error.find(expected_error), std::string::npos) << error;
    EXPECT_EQ(data_source.grouped_read_count, 0);
  };

  ValueDefinition first = PointerValue("first", ValueType::U8, {0x0}, Endian::Big, Endian::None);
  std::get<PointerChainRead>(*GetMutableReadDefinition(first)).target_area_ids = {"missing"};
  expect_failed_plan(first, FakeDataSource().GetMemoryAreas(), "unknown pointer_chain.target_area");

  ValueDefinition intermediate;
  intermediate.id = "intermediate";
  intermediate.type = ValueType::U8;
  SetPointerChainRead(&intermediate, "mem1", 0x100,
                      std::vector<std::string>{"mem1", "missing", "mem1"}, {0x0, 0x4, 0x8},
                      Endian::Big, Endian::None);
  expect_failed_plan(intermediate, FakeDataSource().GetMemoryAreas(),
                     "unknown pointer_chain.target_area");

  ValueDefinition final;
  final.id = "final";
  final.type = ValueType::U8;
  SetPointerChainRead(&final, "mem1", 0x100, std::vector<std::string>{"mem1", "missing"},
                      {0x0, 0x4}, Endian::Big, Endian::None);
  expect_failed_plan(final, FakeDataSource().GetMemoryAreas(), "unknown pointer_chain.target_area");

  ValueDefinition mismatch =
      PointerValue("mismatch", ValueType::U8, {0x0}, Endian::Big, Endian::None);
  std::get<PointerChainRead>(*GetMutableReadDefinition(mismatch)).target_area_ids.clear();
  expect_failed_plan(mismatch, FakeDataSource().GetMemoryAreas(), "exactly one entry per offset");

  ValueDefinition overflow =
      PointerValue("overflow", ValueType::U8, {0x0}, Endian::Big, Endian::None);
  std::get<PointerChainRead>(*GetMutableReadDefinition(overflow)).target_area_ids = {"mem2"};
  std::vector<MemoryArea> overflow_areas = FakeDataSource().GetMemoryAreas();
  overflow_areas[1].base_address = std::numeric_limits<u64>::max();
  overflow_areas[1].size = 1;
  expect_failed_plan(overflow, std::move(overflow_areas), "target memory area");
}

TEST(CheevoMapV2Planner, DirectOnlyPlanRemainsUnchanged)
{
  Package package = ValidPackage();

  std::string error;
  const auto plan = BuildReadPlan(package, FakeDataSource().GetMemoryAreas(), &error);
  ASSERT_TRUE(plan) << error;
  ASSERT_EQ(plan->requests.size(), 1u);
  EXPECT_EQ(plan->requests[0].memory_area_id, "mem1");
  EXPECT_EQ(plan->requests[0].address, 0x10u);
  EXPECT_EQ(plan->requests[0].size, 2u);
  EXPECT_TRUE(plan->pointer_chains.empty());
}

TEST(CheevoMapV2ExpressionPlanner, ResolvesForwardReferencesAndOrdersTopologically)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";
  package.values.push_back(ExpressionValue("later_copy", ValueType::U32, Ref("later_read")));
  package.values.push_back(ExpressionValue(
      "third", ValueType::U32, Op(ExpressionOperator::Add, {Ref("second"), U64Const(1)})));
  package.values.push_back(DirectValue("later_read", ValueType::U32, "mem1", 0x20, Endian::Big));
  package.values.push_back(
      ExpressionValue("second", ValueType::U32,
                      Op(ExpressionOperator::Add, {Ref("later_copy"), Ref("later_copy")})));
  package.values.push_back(ExpressionValue("independent", ValueType::Boolean, BoolConst(true)));

  std::string error;
  const auto plan = BuildEvaluationPlan(package, FakeDataSource().GetMemoryAreas(), &error);
  ASSERT_TRUE(plan) << error;
  ASSERT_EQ(plan->expressions_in_evaluation_order.size(), 4u);
  EXPECT_EQ(plan->expressions_in_evaluation_order[0].value_id, "later_copy");
  EXPECT_EQ(plan->expressions_in_evaluation_order[1].value_id, "second");
  EXPECT_EQ(plan->expressions_in_evaluation_order[2].value_id, "third");
  EXPECT_EQ(plan->expressions_in_evaluation_order[3].value_id, "independent");
  ASSERT_EQ(plan->expressions_in_evaluation_order[1].dependencies.size(), 1u);
  EXPECT_EQ(plan->expressions_in_evaluation_order[1].dependencies[0], "later_copy");
  ASSERT_EQ(plan->read_plan.requests.size(), 1u);
}

TEST(CheevoMapV2ExpressionPlanner, RejectsUnknownReferencesAndCyclesBeforeReads)
{
  auto expect_failure = [](Package package, std::string_view expected_error) {
    FakeDataSource data_source;
    std::string error;
    EXPECT_FALSE(EvaluatePackage(package, data_source, &error));
    EXPECT_NE(error.find(expected_error), std::string::npos) << error;
    EXPECT_EQ(data_source.grouped_read_count, 0);
    EXPECT_EQ(data_source.single_read_count, 0);
  };

  Package unknown = ValidPackage();
  unknown.values.push_back(ExpressionValue("derived", ValueType::U32, Ref("scroe")));
  expect_failure(std::move(unknown), R"(value "derived" references unknown value "scroe")");

  Package self;
  self.game.id = "GAME01";
  self.metadata.title = "Test";
  self.values.push_back(ExpressionValue("a", ValueType::U32, Ref("a")));
  expect_failure(std::move(self), "expression dependency cycle: a -> a");

  Package two;
  two.game.id = "GAME01";
  two.metadata.title = "Test";
  two.values.push_back(ExpressionValue("a", ValueType::U32, Ref("b")));
  two.values.push_back(ExpressionValue("b", ValueType::U32, Ref("a")));
  expect_failure(std::move(two), "expression dependency cycle: a -> b -> a");

  Package three;
  three.game.id = "GAME01";
  three.metadata.title = "Test";
  three.values.push_back(ExpressionValue("a", ValueType::U32, Ref("b")));
  three.values.push_back(ExpressionValue("b", ValueType::U32, Ref("c")));
  three.values.push_back(ExpressionValue("c", ValueType::U32, Ref("a")));
  expect_failure(std::move(three), "expression dependency cycle: a -> b -> c -> a");
}

TEST(CheevoMapV2ExpressionPlanner, RejectsStaticTypeErrorsBeforeReads)
{
  auto expect_type_error = [](ExpressionNode expression, ValueType output_type,
                              std::string_view expected_error) {
    Package package = ValidPackage();
    package.values.push_back(ExpressionValue("derived", output_type, std::move(expression)));

    FakeDataSource data_source;
    std::string error;
    EXPECT_FALSE(EvaluatePackage(package, data_source, &error));
    EXPECT_NE(error.find(expected_error), std::string::npos) << error;
    EXPECT_EQ(data_source.grouped_read_count, 0);
  };

  expect_type_error(BoolConst(true), ValueType::U32, "does not match");
  expect_type_error(Op(ExpressionOperator::Equal, {U64Const(1), S64Const(1)}), ValueType::Boolean,
                    "matching categories");
  expect_type_error(Op(ExpressionOperator::Add, {U64Const(1), F64Const(1.0)}), ValueType::U64,
                    "matching numeric");
  expect_type_error(Op(ExpressionOperator::Add, {StringConst("a"), StringConst("b")}),
                    ValueType::String, "numeric");
  expect_type_error(Op(ExpressionOperator::Add, {BoolConst(true), BoolConst(false)}),
                    ValueType::Boolean, "numeric");
  expect_type_error(Op(ExpressionOperator::BitAnd, {S64Const(1), S64Const(1)}), ValueType::U64,
                    "UnsignedInteger");
  expect_type_error(Op(ExpressionOperator::Less, {StringConst("a"), StringConst("b")}),
                    ValueType::Boolean, "numeric");
  expect_type_error(Op(ExpressionOperator::If, {BoolConst(true), U64Const(1), S64Const(1)}),
                    ValueType::U64, "branch");

  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";
  package.values.push_back(ExpressionValue(
      "valid", ValueType::F64,
      Op(ExpressionOperator::ToF64, {Op(ExpressionOperator::Add, {U64Const(2), U64Const(3)})})));
  std::string error;
  EXPECT_TRUE(BuildEvaluationPlan(package, FakeDataSource().GetMemoryAreas(), &error)) << error;
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

TEST(CheevoMapV2ExpressionEvaluator, EvaluatesComparisonsLogicBitwiseConversionAndIf)
{
  StateValueMap values{{"u", StateValue::UnsignedInteger(5)},
                       {"s", StateValue::SignedInteger(-2)},
                       {"f", StateValue::FloatingPoint(1.5)},
                       {"text", StateValue::String("Game")},
                       {"missing", StateValue::Unavailable()}};
  const auto eval = [&values](ExpressionNode node) { return EvaluateExpression(node, values); };

  StateValue result = eval(Op(ExpressionOperator::Equal, {U64Const(5), Ref("u")}));
  EXPECT_TRUE(*result.AsBoolean());
  result = eval(Op(ExpressionOperator::NotEqual, {S64Const(-1), Ref("s")}));
  EXPECT_TRUE(*result.AsBoolean());
  result = eval(Op(ExpressionOperator::Less, {S64Const(-3), Ref("s")}));
  EXPECT_TRUE(*result.AsBoolean());
  result = eval(Op(ExpressionOperator::LessEqual, {F64Const(1.5), Ref("f")}));
  EXPECT_TRUE(*result.AsBoolean());
  result = eval(Op(ExpressionOperator::Greater, {Ref("u"), U64Const(4)}));
  EXPECT_TRUE(*result.AsBoolean());
  result = eval(Op(ExpressionOperator::GreaterEqual, {Ref("u"), U64Const(5)}));
  EXPECT_TRUE(*result.AsBoolean());
  result = eval(Op(ExpressionOperator::Equal, {StringConst("Game"), Ref("text")}));
  EXPECT_TRUE(*result.AsBoolean());

  result = eval(Op(ExpressionOperator::Not, {BoolConst(false)}));
  EXPECT_TRUE(*result.AsBoolean());
  result = eval(Op(ExpressionOperator::And, {BoolConst(true), BoolConst(true), BoolConst(false)}));
  EXPECT_FALSE(*result.AsBoolean());
  result = eval(Op(ExpressionOperator::Or, {BoolConst(false), BoolConst(false), BoolConst(true)}));
  EXPECT_TRUE(*result.AsBoolean());
  result = eval(Op(ExpressionOperator::And, {BoolConst(true), Ref("missing")}));
  EXPECT_FALSE(result.IsAvailable());

  result = eval(Op(ExpressionOperator::BitAnd, {U64Const(0b1100), U64Const(0b1010)}));
  EXPECT_EQ(*result.AsUnsignedInteger(), 0b1000u);
  result = eval(Op(ExpressionOperator::BitOr, {U64Const(0b1100), U64Const(0b0011)}));
  EXPECT_EQ(*result.AsUnsignedInteger(), 0b1111u);
  result = eval(Op(ExpressionOperator::BitXor, {U64Const(0b1100), U64Const(0b1010)}));
  EXPECT_EQ(*result.AsUnsignedInteger(), 0b0110u);
  result = eval(Op(ExpressionOperator::BitNot, {U64Const(0)}));
  EXPECT_EQ(*result.AsUnsignedInteger(), std::numeric_limits<u64>::max());

  result = eval(Op(ExpressionOperator::ToF64, {S64Const(-7)}));
  EXPECT_DOUBLE_EQ(*result.AsFloatingPoint(), -7.0);
  result = eval(Op(ExpressionOperator::ToF64, {U64Const(7)}));
  EXPECT_DOUBLE_EQ(*result.AsFloatingPoint(), 7.0);
  result = eval(Op(ExpressionOperator::ToF64, {F64Const(7.5)}));
  EXPECT_DOUBLE_EQ(*result.AsFloatingPoint(), 7.5);

  result = eval(Op(ExpressionOperator::If, {BoolConst(true), StringConst("yes"), Ref("missing")}));
  EXPECT_EQ(*result.AsString(), "yes");
  result = eval(Op(ExpressionOperator::If, {BoolConst(false), Ref("missing"), StringConst("no")}));
  EXPECT_EQ(*result.AsString(), "no");
  result = eval(Op(ExpressionOperator::If, {Ref("missing"), U64Const(1), U64Const(2)}));
  EXPECT_FALSE(result.IsAvailable());
  result = eval(
      Op(ExpressionOperator::If,
         {BoolConst(true), Op(ExpressionOperator::If, {BoolConst(false), U64Const(1), U64Const(2)}),
          U64Const(3)}));
  EXPECT_EQ(*result.AsUnsignedInteger(), 2u);
}

TEST(CheevoMapV2ExpressionEvaluator, EvaluatesArithmeticAndRuntimeSafety)
{
  StateValue result =
      EvaluateExpression(Op(ExpressionOperator::Add, {S64Const(-2), S64Const(5)}), {});
  EXPECT_EQ(*result.AsSignedInteger(), 3);
  result = EvaluateExpression(Op(ExpressionOperator::Subtract, {U64Const(5), U64Const(2)}), {});
  EXPECT_EQ(*result.AsUnsignedInteger(), 3u);
  result = EvaluateExpression(Op(ExpressionOperator::Multiply, {F64Const(2.5), F64Const(2.0)}), {});
  EXPECT_DOUBLE_EQ(*result.AsFloatingPoint(), 5.0);
  result = EvaluateExpression(Op(ExpressionOperator::Divide, {S64Const(-5), S64Const(2)}), {});
  EXPECT_EQ(*result.AsSignedInteger(), -2);
  result = EvaluateExpression(Op(ExpressionOperator::Modulo, {U64Const(5), U64Const(2)}), {});
  EXPECT_EQ(*result.AsUnsignedInteger(), 1u);

  const std::vector<ExpressionNode> unavailable = {
      Op(ExpressionOperator::Add, {S64Const(std::numeric_limits<s64>::max()), S64Const(1)}),
      Op(ExpressionOperator::Subtract, {U64Const(0), U64Const(1)}),
      Op(ExpressionOperator::Multiply, {U64Const(std::numeric_limits<u64>::max()), U64Const(2)}),
      Op(ExpressionOperator::Divide, {U64Const(1), U64Const(0)}),
      Op(ExpressionOperator::Modulo, {S64Const(1), S64Const(0)}),
      Op(ExpressionOperator::Divide, {S64Const(std::numeric_limits<s64>::min()), S64Const(-1)}),
      Op(ExpressionOperator::Modulo, {S64Const(std::numeric_limits<s64>::min()), S64Const(-1)}),
      Op(ExpressionOperator::Multiply,
         {F64Const(std::numeric_limits<double>::max()), F64Const(2.0)}),
      Op(ExpressionOperator::Add, {StringConst("a"), StringConst("b")}),
  };
  for (const ExpressionNode& expression : unavailable)
  {
    result = EvaluateExpression(expression, {});
    EXPECT_FALSE(result.IsAvailable());
  }
}

TEST(CheevoMapV2ExpressionEvaluator, ConvertsFinalDeclaredTypes)
{
  EXPECT_TRUE(*ConvertExpressionResultToDeclaredType(StateValue::Boolean(true), ValueType::Boolean)
                   .AsBoolean());
  EXPECT_TRUE(ConvertExpressionResultToDeclaredType(StateValue::UnsignedInteger(255), ValueType::U8)
                  .IsAvailable());
  EXPECT_FALSE(
      ConvertExpressionResultToDeclaredType(StateValue::UnsignedInteger(256), ValueType::U8)
          .IsAvailable());
  EXPECT_TRUE(ConvertExpressionResultToDeclaredType(StateValue::SignedInteger(-128), ValueType::S8)
                  .IsAvailable());
  EXPECT_FALSE(ConvertExpressionResultToDeclaredType(StateValue::SignedInteger(-129), ValueType::S8)
                   .IsAvailable());
  EXPECT_TRUE(
      ConvertExpressionResultToDeclaredType(StateValue::UnsignedInteger(65535), ValueType::U16)
          .IsAvailable());
  EXPECT_FALSE(
      ConvertExpressionResultToDeclaredType(StateValue::UnsignedInteger(65536), ValueType::U16)
          .IsAvailable());
  EXPECT_TRUE(ConvertExpressionResultToDeclaredType(StateValue::UnsignedInteger(4294967295ULL),
                                                    ValueType::U32)
                  .IsAvailable());
  EXPECT_FALSE(ConvertExpressionResultToDeclaredType(StateValue::UnsignedInteger(4294967296ULL),
                                                     ValueType::U32)
                   .IsAvailable());
  EXPECT_TRUE(
      ConvertExpressionResultToDeclaredType(StateValue::SignedInteger(32767), ValueType::S16)
          .IsAvailable());
  EXPECT_FALSE(
      ConvertExpressionResultToDeclaredType(StateValue::SignedInteger(32768), ValueType::S16)
          .IsAvailable());
  EXPECT_TRUE(ConvertExpressionResultToDeclaredType(
                  StateValue::SignedInteger(std::numeric_limits<s32>::max()), ValueType::S32)
                  .IsAvailable());
  EXPECT_FALSE(ConvertExpressionResultToDeclaredType(
                   StateValue::SignedInteger(static_cast<s64>(std::numeric_limits<s32>::max()) + 1),
                   ValueType::S32)
                   .IsAvailable());
  EXPECT_TRUE(ConvertExpressionResultToDeclaredType(StateValue::FloatingPoint(1.25), ValueType::F32)
                  .IsAvailable());
  EXPECT_FALSE(
      ConvertExpressionResultToDeclaredType(
          StateValue::FloatingPoint(static_cast<double>(std::numeric_limits<float>::max()) * 2.0),
          ValueType::F32)
          .IsAvailable());
  EXPECT_DOUBLE_EQ(
      *ConvertExpressionResultToDeclaredType(StateValue::FloatingPoint(1.25), ValueType::F64)
           .AsFloatingPoint(),
      1.25);
  EXPECT_EQ(*ConvertExpressionResultToDeclaredType(StateValue::String("ok"), ValueType::String)
                 .AsString(),
            "ok");
}

TEST(CheevoMapV2Decoder, KeepsSuccessfulValuesWhenAnotherReadFails)
{
  Package package = ValidPackage();
  ValueDefinition failed = package.values.front();
  failed.id = "failed";
  std::get<DirectMemoryRead>(*GetMutableReadDefinition(failed)).address = 0x20;
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

TEST(CheevoMapV2PointerRuntime, ResolvesCrossAreaChainWithStageTargetAreas)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";

  ValueDefinition pointer;
  pointer.id = "score";
  pointer.type = ValueType::U32;
  SetPointerChainRead(&pointer, "mem2", 0x860, std::vector<std::string>{"mem1", "mem1", "mem2"},
                      {0x0, 0x160, 0x420}, Endian::Big, Endian::Big);
  package.values.push_back(pointer);

  FakeDataSource data_source;
  data_source.memory_areas = {MemoryArea{"mem1", "MEM1", "emulated-physical", 0x00000000, 0x1000},
                              MemoryArea{"mem2", "MEM2", "emulated-physical", 0x10000000, 0x2000}};
  WriteU32(&data_source, 0x10000860, 0x310, Endian::Big);
  WriteU32(&data_source, 0x310, 0x500, Endian::Big);
  WriteU32(&data_source, 0x660, 0x10000400, Endian::Big);
  data_source.Write(0x10000820, {0x12, 0x34, 0x56, 0x78});

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(*result->values.at("score").AsUnsignedInteger(), 0x12345678u);
  ASSERT_EQ(data_source.grouped_requests.size(), 4u);

  ASSERT_EQ(data_source.grouped_requests[0].size(), 1u);
  EXPECT_EQ(data_source.grouped_requests[0][0].memory_area_id, "mem2");
  EXPECT_EQ(data_source.grouped_requests[0][0].address, 0x10000860u);

  ASSERT_EQ(data_source.grouped_requests[1].size(), 1u);
  EXPECT_EQ(data_source.grouped_requests[1][0].memory_area_id, "mem1");
  EXPECT_EQ(data_source.grouped_requests[1][0].address, 0x310u);

  ASSERT_EQ(data_source.grouped_requests[2].size(), 1u);
  EXPECT_EQ(data_source.grouped_requests[2][0].memory_area_id, "mem1");
  EXPECT_EQ(data_source.grouped_requests[2][0].address, 0x660u);

  ASSERT_EQ(data_source.grouped_requests[3].size(), 1u);
  EXPECT_EQ(data_source.grouped_requests[3][0].memory_area_id, "mem2");
  EXPECT_EQ(data_source.grouped_requests[3][0].address, 0x10000820u);
}

TEST(CheevoMapV2PointerRuntime, ResolvesOneOffsetExplicitTargetAreasChain)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";

  ValueDefinition pointer;
  pointer.id = "score";
  pointer.type = ValueType::U32;
  SetPointerChainRead(&pointer, "mem1", 0x100, std::vector<std::string>{"mem2"}, {0x420},
                      Endian::Big, Endian::Big);
  package.values.push_back(pointer);

  FakeDataSource data_source;
  data_source.memory_areas = {MemoryArea{"mem1", "MEM1", "emulated-physical", 0x00000000, 0x1000},
                              MemoryArea{"mem2", "MEM2", "emulated-physical", 0x10000000, 0x2000}};
  WriteU32(&data_source, 0x100, 0x10000400, Endian::Big);
  data_source.Write(0x10000820, {0x00, 0x00, 0x00, 0x2a});

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(*result->values.at("score").AsUnsignedInteger(), 42u);
  ASSERT_EQ(data_source.grouped_requests.size(), 2u);
  EXPECT_EQ(data_source.grouped_requests[1][0].memory_area_id, "mem2");
  EXPECT_EQ(data_source.grouped_requests[1][0].address, 0x10000820u);
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

TEST(CheevoMapV2PointerRuntime, CrossAreaFailuresOnlyAffectRelevantValues)
{
  Package package = ValidPackage();
  ValueDefinition same_area =
      PointerValue("same_area", ValueType::U8, {0x1}, Endian::Big, Endian::None);
  package.values.push_back(same_area);

  ValueDefinition wrong_intermediate;
  wrong_intermediate.id = "wrong_intermediate";
  wrong_intermediate.type = ValueType::U8;
  SetPointerChainRead(&wrong_intermediate, "mem1", 0x104,
                      std::vector<std::string>{"mem1", "mem1", "mem2"}, {0x0, 0x4, 0x8},
                      Endian::Big, Endian::None);
  package.values.push_back(wrong_intermediate);

  ValueDefinition final_mismatch;
  final_mismatch.id = "final_mismatch";
  final_mismatch.type = ValueType::U8;
  SetPointerChainRead(&final_mismatch, "mem1", 0x108,
                      std::vector<std::string>{"mem1", "mem1", "mem2"}, {0x0, 0x4, 0x8},
                      Endian::Big, Endian::None);
  package.values.push_back(final_mismatch);

  FakeDataSource data_source;
  data_source.memory_areas = {MemoryArea{"mem1", "MEM1", "emulated-physical", 0x00000000, 0x1000},
                              MemoryArea{"mem2", "MEM2", "emulated-physical", 0x10000000, 0x1000}};
  data_source.Write(0x10, {0x00, 0x07});
  WriteU32(&data_source, 0x100, 0x200, Endian::Big);
  data_source.Write(0x201, {0x44});

  WriteU32(&data_source, 0x104, 0x300, Endian::Big);
  WriteU32(&data_source, 0x300, 0x10000400, Endian::Big);

  WriteU32(&data_source, 0x108, 0x500, Endian::Big);
  WriteU32(&data_source, 0x500, 0x600, Endian::Big);
  WriteU32(&data_source, 0x604, 0x10000ff8, Endian::Big);

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(*result->values.at("coins").AsUnsignedInteger(), 7u);
  EXPECT_EQ(*result->values.at("same_area").AsUnsignedInteger(), 0x44u);
  EXPECT_FALSE(result->values.at("wrong_intermediate").IsAvailable());
  EXPECT_FALSE(result->values.at("final_mismatch").IsAvailable());
}

TEST(CheevoMapV2PointerRuntime, MixedIntermediateRequestsAreSortedAndAreaSensitive)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";

  ValueDefinition mem1_chain;
  mem1_chain.id = "mem1_chain";
  mem1_chain.type = ValueType::U8;
  SetPointerChainRead(&mem1_chain, "mem1", 0x100, std::vector<std::string>{"mem1", "mem1"},
                      {0x0, 0x2}, Endian::Big, Endian::None);
  package.values.push_back(mem1_chain);

  ValueDefinition mem2_chain;
  mem2_chain.id = "mem2_chain";
  mem2_chain.type = ValueType::U8;
  SetPointerChainRead(&mem2_chain, "mem1", 0x104, std::vector<std::string>{"mem2", "mem2"},
                      {0x0, 0x2}, Endian::Big, Endian::None);
  package.values.push_back(mem2_chain);

  FakeDataSource data_source;
  data_source.memory_areas = {MemoryArea{"mem1", "MEM1", "emulated-physical", 0x00000000, 0x1000},
                              MemoryArea{"mem2", "MEM2", "emulated-physical", 0x00000000, 0x1000}};
  WriteU32(&data_source, 0x100, 0x200, Endian::Big);
  WriteU32(&data_source, 0x104, 0x200, Endian::Big);
  WriteU32(&data_source, 0x200, 0x300, Endian::Big);
  data_source.Write(0x302, {0x11});

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(*result->values.at("mem1_chain").AsUnsignedInteger(), 0x11u);
  EXPECT_EQ(*result->values.at("mem2_chain").AsUnsignedInteger(), 0x11u);
  ASSERT_EQ(data_source.grouped_requests.size(), 3u);
  ASSERT_EQ(data_source.grouped_requests[1].size(), 2u);
  EXPECT_EQ(data_source.grouped_requests[1][0].memory_area_id, "mem1");
  EXPECT_EQ(data_source.grouped_requests[1][0].address, 0x200u);
  EXPECT_EQ(data_source.grouped_requests[1][1].memory_area_id, "mem2");
  EXPECT_EQ(data_source.grouped_requests[1][1].address, 0x200u);
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
  std::get<PointerChainRead>(*GetMutableReadDefinition(value)).base.area_id = "missing";
  package.values.push_back(value);

  FakeDataSource data_source;
  std::string error;
  EXPECT_FALSE(EvaluatePackage(package, data_source, &error));
  EXPECT_NE(error.find("unknown pointer_chain.base area"), std::string::npos);
  EXPECT_EQ(data_source.grouped_read_count, 0);

  package.values.front() = PointerValue("score", ValueType::U32, {0x10});
  std::get<PointerChainRead>(*GetMutableReadDefinition(package.values.front())).target_area_ids = {
      "missing"};
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

TEST(CheevoMapV2ExpressionRuntime, EvaluatesReadsExpressionsAndForwardChains)
{
  Package package = ValidPackage();
  package.values.push_back(
      ExpressionValue("has_coins", ValueType::Boolean,
                      Op(ExpressionOperator::Greater, {Ref("coins"), U64Const(0)})));
  package.values.push_back(ExpressionValue(
      "double_coins", ValueType::U16, Op(ExpressionOperator::Add, {Ref("coins"), Ref("coins")})));
  package.values.push_back(ExpressionValue("future_copy", ValueType::U16, Ref("future")));
  package.values.push_back(ExpressionValue(
      "future", ValueType::U16, Op(ExpressionOperator::Add, {Ref("double_coins"), U64Const(1)})));

  FakeDataSource data_source;
  data_source.Write(0x10, {0x00, 0x03});

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_TRUE(*result->values.at("has_coins").AsBoolean());
  EXPECT_EQ(*result->values.at("double_coins").AsUnsignedInteger(), 6u);
  EXPECT_EQ(*result->values.at("future").AsUnsignedInteger(), 7u);
  EXPECT_EQ(*result->values.at("future_copy").AsUnsignedInteger(), 7u);
  EXPECT_EQ(data_source.grouped_read_count, 1);
}

TEST(CheevoMapV2ExpressionRuntime, PointerReadsFeedExpressionsWithoutExtraGroupedReads)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";
  package.values.push_back(PointerValue("score", ValueType::U32, {0x10}));
  package.values.push_back(
      ExpressionValue("score_ready", ValueType::Boolean,
                      Op(ExpressionOperator::Equal, {Ref("score"), U64Const(9)})));

  FakeDataSource data_source;
  WriteU32(&data_source, 0x100, 0x200, Endian::Big);
  data_source.Write(0x210, {0x00, 0x00, 0x00, 0x09});

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_TRUE(*result->values.at("score_ready").AsBoolean());
  EXPECT_EQ(data_source.grouped_read_count, 2);
}

TEST(CheevoMapV2ExpressionRuntime, UnavailabilityAndArithmeticErrorsAreLocal)
{
  Package package = ValidPackage();
  package.values.push_back(DirectValue("missing", ValueType::U8, "mem1", 0x20));
  package.values.push_back(
      ExpressionValue("missing_bool", ValueType::Boolean,
                      Op(ExpressionOperator::Equal, {Ref("missing"), U64Const(1)})));
  package.values.push_back(
      ExpressionValue("divide_by_zero", ValueType::U64,
                      Op(ExpressionOperator::Divide, {Ref("coins"), U64Const(0)})));
  package.values.push_back(ExpressionValue("constant_ok", ValueType::Boolean, BoolConst(true)));

  FakeDataSource data_source;
  data_source.Write(0x10, {0x00, 0x03});

  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_EQ(*result->values.at("coins").AsUnsignedInteger(), 3u);
  EXPECT_FALSE(result->values.at("missing").IsAvailable());
  EXPECT_FALSE(result->values.at("missing_bool").IsAvailable());
  EXPECT_FALSE(result->values.at("divide_by_zero").IsAvailable());
  EXPECT_TRUE(*result->values.at("constant_ok").AsBoolean());
}

TEST(CheevoMapV2ExpressionRuntime, ConstantOnlyPackagePerformsZeroReads)
{
  Package package;
  package.game.id = "GAME01";
  package.metadata.title = "Test";
  package.values.push_back(ExpressionValue("ready", ValueType::Boolean, BoolConst(true)));
  package.values.push_back(ExpressionValue("title", ValueType::String, StringConst("Game")));

  FakeDataSource data_source;
  std::string error;
  const auto result = EvaluatePackage(package, data_source, &error);
  ASSERT_TRUE(result) << error;
  EXPECT_TRUE(*result->values.at("ready").AsBoolean());
  EXPECT_EQ(*result->values.at("title").AsString(), "Game");
  EXPECT_EQ(data_source.grouped_read_count, 0);
  EXPECT_EQ(data_source.single_read_count, 0);
}

TEST(CheevoMapV2ExpressionRuntimeState, PublishesExpressionDeltasAndRejectsStaleSessions)
{
  Package package = ValidPackage();
  package.values.push_back(
      ExpressionValue("has_coins", ValueType::Boolean,
                      Op(ExpressionOperator::Greater, {Ref("coins"), U64Const(0)})));

  FakeDataSource data_source;
  data_source.Write(0x10, {0x00, 0x00});

  StateStore store;
  std::vector<StateUpdate> updates;
  auto hook = store.RegisterUpdateCallback(
      [&updates](const StateUpdate& update) { updates.push_back(update); });
  (void)hook;

  const StateUpdate reset =
      store.Reset({{"coins", StateValue::Unavailable()}, {"has_coins", StateValue::Unavailable()}});
  std::string error;
  auto applied = EvaluatePackageForSession(package, data_source, store, reset.session_id, &error);
  EXPECT_EQ(applied.status, PackageRuntimeStatus::Applied);
  ASSERT_EQ(updates.size(), 2u);
  EXPECT_FALSE(*updates.back().values.at("has_coins").AsBoolean());

  data_source.Write(0x10, {0x00, 0x01});
  applied = EvaluatePackageForSession(package, data_source, store, reset.session_id, &error);
  EXPECT_EQ(applied.status, PackageRuntimeStatus::Applied);
  ASSERT_EQ(updates.size(), 3u);
  EXPECT_TRUE(*updates.back().values.at("has_coins").AsBoolean());

  data_source.Write(0x10, {0x00, 0x02});
  applied = EvaluatePackageForSession(package, data_source, store, reset.session_id, &error);
  EXPECT_EQ(applied.status, PackageRuntimeStatus::Applied);
  ASSERT_EQ(updates.size(), 4u);
  EXPECT_TRUE(updates.back().values.contains("coins"));
  EXPECT_FALSE(updates.back().values.contains("has_coins"));

  data_source.failed_addresses.insert(0x10);
  applied = EvaluatePackageForSession(package, data_source, store, reset.session_id, &error);
  EXPECT_EQ(applied.status, PackageRuntimeStatus::Applied);
  ASSERT_EQ(updates.size(), 5u);
  EXPECT_FALSE(updates.back().values.at("has_coins").IsAvailable());

  const StateUpdate reloaded =
      store.Reset({{"coins", StateValue::Unavailable()}, {"has_coins", StateValue::Unavailable()}});
  applied = EvaluatePackageForSession(package, data_source, store, reset.session_id, &error);
  EXPECT_EQ(applied.status, PackageRuntimeStatus::StaleSession);
  EXPECT_EQ(store.GetSnapshot().session_id, reloaded.session_id);
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
  std::get<DirectMemoryRead>(*GetMutableReadDefinition(package.values.front())).area_id = "missing";

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

TEST(CheevoMapV2ReferencePackage, R8PP01ExactFileParses)
{
  std::string error;
  const std::optional<Package> package = LoadR8PP01ReferencePackage(&error);
  ASSERT_TRUE(package) << error;

  EXPECT_EQ(package->game.id, "R8PP01");
  EXPECT_FALSE(package->game.revision);
  EXPECT_EQ(package->metadata.title, "Super Paper Mario PAL - Semantic Reference Package");
  EXPECT_DOUBLE_EQ(package->poll_hz, 20.0);
  ASSERT_EQ(package->values.size(), 33u);

  const std::vector<std::string> expected_ids{
      "flipped_into_3d_raw",
      "is_3d",
      "mansion_patrol_status",
      "mansion_patrol_lives",
      "mansion_patrol_round_index",
      "mansion_patrol_score",
      "mansion_patrol_round_number",
      "mansion_patrol_cleared",
      "mansion_patrol_has_lives",
      "mansion_patrol_one_life_remaining",
      "mansion_patrol_in_danger",
      "tilt_island_status",
      "tilt_island_lives",
      "tilt_island_round_index",
      "tilt_island_score",
      "tilt_island_round_number",
      "tilt_island_cleared",
      "tilt_island_has_lives",
      "tilt_island_one_life_remaining",
      "tilt_island_in_danger",
      "forget_me_not_status",
      "forget_me_not_lives",
      "forget_me_not_round",
      "forget_me_not_score",
      "forget_me_not_cleared",
      "forget_me_not_has_lives",
      "forget_me_not_one_life_remaining",
      "forget_me_not_in_danger",
      "hammer_whacker_round",
      "hammer_whacker_score",
      "hammer_whacker_lives",
      "hammer_whacker_has_lives",
      "hammer_whacker_one_life_remaining",
  };

  std::set<std::string> ids;
  size_t read_backed_count = 0;
  size_t expression_backed_count = 0;
  for (const ValueDefinition& value : package->values)
  {
    EXPECT_TRUE(ids.insert(value.id).second) << value.id;
    EXPECT_EQ(value.id.find("pointer"), std::string::npos) << value.id;
    if (IsReadBackedValue(value))
      ++read_backed_count;
    if (IsExpressionBackedValue(value))
      ++expression_backed_count;
  }

  EXPECT_EQ(read_backed_count, 16u);
  EXPECT_EQ(expression_backed_count, 17u);
  EXPECT_EQ(ids.size(), expected_ids.size());
  for (const std::string& id : expected_ids)
    EXPECT_TRUE(ids.contains(id)) << id;
}

TEST(CheevoMapV2ReferencePackage, R8PP01EvaluatesAndUsesExpectedGroupedReads)
{
  std::string error;
  const std::optional<Package> package = LoadR8PP01ReferencePackage(&error);
  ASSERT_TRUE(package) << error;

  FakeDataSource data_source;
  ConfigureR8PP01DataSource(&data_source);
  PopulateR8PP01ReferenceMemory(&data_source);

  const auto result = EvaluatePackage(*package, data_source, &error);
  ASSERT_TRUE(result) << error;
  ASSERT_EQ(result->values.size(), 33u);

  ExpectUnsignedValue(result->values, "flipped_into_3d_raw", 1);
  ExpectBoolValue(result->values, "is_3d", true);

  ExpectUnsignedValue(result->values, "mansion_patrol_status", 0x1f6);
  ExpectUnsignedValue(result->values, "mansion_patrol_lives", 1);
  ExpectUnsignedValue(result->values, "mansion_patrol_round_index", 2);
  ExpectUnsignedValue(result->values, "mansion_patrol_score", 12345);
  ExpectUnsignedValue(result->values, "mansion_patrol_round_number", 3);
  ExpectBoolValue(result->values, "mansion_patrol_cleared", true);
  ExpectBoolValue(result->values, "mansion_patrol_has_lives", true);
  ExpectBoolValue(result->values, "mansion_patrol_one_life_remaining", true);
  ExpectBoolValue(result->values, "mansion_patrol_in_danger", false);

  ExpectUnsignedValue(result->values, "tilt_island_status", 0x08000000);
  ExpectUnsignedValue(result->values, "tilt_island_lives", 1);
  ExpectUnsignedValue(result->values, "tilt_island_round_index", 4);
  ExpectUnsignedValue(result->values, "tilt_island_score", 777);
  ExpectUnsignedValue(result->values, "tilt_island_round_number", 5);
  ExpectBoolValue(result->values, "tilt_island_cleared", false);
  ExpectBoolValue(result->values, "tilt_island_has_lives", true);
  ExpectBoolValue(result->values, "tilt_island_one_life_remaining", true);
  ExpectBoolValue(result->values, "tilt_island_in_danger", true);

  ExpectUnsignedValue(result->values, "forget_me_not_status", 0x1802);
  ExpectUnsignedValue(result->values, "forget_me_not_lives", 0);
  ExpectUnsignedValue(result->values, "forget_me_not_round", 7);
  ExpectUnsignedValue(result->values, "forget_me_not_score", 888);
  ExpectBoolValue(result->values, "forget_me_not_cleared", true);
  ExpectBoolValue(result->values, "forget_me_not_has_lives", false);
  ExpectBoolValue(result->values, "forget_me_not_one_life_remaining", false);
  ExpectBoolValue(result->values, "forget_me_not_in_danger", false);

  ExpectUnsignedValue(result->values, "hammer_whacker_round", 3);
  ExpectUnsignedValue(result->values, "hammer_whacker_score", 999);
  ExpectUnsignedValue(result->values, "hammer_whacker_lives", 1);
  ExpectBoolValue(result->values, "hammer_whacker_has_lives", true);
  ExpectBoolValue(result->values, "hammer_whacker_one_life_remaining", true);

  ASSERT_EQ(data_source.grouped_requests.size(), 4u);
  EXPECT_EQ(data_source.grouped_requests[0].size(), 2u);
  EXPECT_EQ(data_source.grouped_requests[1].size(), 1u);
  EXPECT_EQ(data_source.grouped_requests[2].size(), 4u);
  EXPECT_EQ(data_source.grouped_requests[3].size(), 16u);
  EXPECT_EQ(data_source.grouped_read_count, 4);
  EXPECT_EQ(data_source.single_read_count, 0);

  EXPECT_EQ(data_source.grouped_requests[0][0].address, R8PP01_MINIGAME_ROOT_READ);
  EXPECT_EQ(data_source.grouped_requests[0][1].address, R8PP01_FLIP_ROOT_READ);
  EXPECT_EQ(data_source.grouped_requests[1][0].address, R8PP01_MINIGAME_ROOT_POINTER + 0x120);
  EXPECT_EQ(data_source.grouped_requests[2][0].address, R8PP01_SHARED_POINTER + 0x3e4);
  EXPECT_EQ(data_source.grouped_requests[2][1].address, R8PP01_SHARED_POINTER + 0x3f4);
  EXPECT_EQ(data_source.grouped_requests[2][2].address, R8PP01_SHARED_POINTER + 0x404);
  EXPECT_EQ(data_source.grouped_requests[2][3].address, R8PP01_SHARED_POINTER + 0x408);

  const std::vector<u64> expected_final_addresses{
      R8PP01_FLIP_ROOT_POINTER + 0x0a,
      R8PP01_MANSION_PATROL_POINTER + 0x0,
      R8PP01_MANSION_PATROL_POINTER + 0x58,
      R8PP01_MANSION_PATROL_POINTER + 0x1f8cc,
      R8PP01_MANSION_PATROL_POINTER + 0x1f8ec,
      R8PP01_TILT_ISLAND_POINTER + 0x0,
      R8PP01_TILT_ISLAND_POINTER + 0xc8,
      R8PP01_TILT_ISLAND_POINTER + 0x6864,
      R8PP01_TILT_ISLAND_POINTER + 0x717c,
      R8PP01_FORGET_ME_NOT_POINTER + 0x0,
      R8PP01_FORGET_ME_NOT_POINTER + 0x917,
      R8PP01_FORGET_ME_NOT_POINTER + 0xb34,
      R8PP01_FORGET_ME_NOT_POINTER + 0xc34,
      R8PP01_HAMMER_WHACKER_POINTER + 0x30,
      R8PP01_HAMMER_WHACKER_POINTER + 0xd0,
      R8PP01_HAMMER_WHACKER_POINTER + 0x2f0,
  };
  ASSERT_EQ(data_source.grouped_requests[3].size(), expected_final_addresses.size());
  for (size_t i = 0; i < expected_final_addresses.size(); ++i)
    EXPECT_EQ(data_source.grouped_requests[3][i].address, expected_final_addresses[i]) << i;
}

TEST(CheevoMapV2ReferencePackage, R8PP01TiltPointerFailureIsIsolated)
{
  std::string error;
  const std::optional<Package> package = LoadR8PP01ReferencePackage(&error);
  ASSERT_TRUE(package) << error;

  FakeDataSource data_source;
  ConfigureR8PP01DataSource(&data_source);
  PopulateR8PP01ReferenceMemory(&data_source, 0x08000000, 1, false);

  const auto result = EvaluatePackage(*package, data_source, &error);
  ASSERT_TRUE(result) << error;

  ExpectUnavailableValue(result->values, "tilt_island_status");
  ExpectUnavailableValue(result->values, "tilt_island_lives");
  ExpectUnavailableValue(result->values, "tilt_island_round_index");
  ExpectUnavailableValue(result->values, "tilt_island_score");
  ExpectUnavailableValue(result->values, "tilt_island_round_number");
  ExpectUnavailableValue(result->values, "tilt_island_cleared");
  ExpectUnavailableValue(result->values, "tilt_island_has_lives");
  ExpectUnavailableValue(result->values, "tilt_island_one_life_remaining");
  ExpectUnavailableValue(result->values, "tilt_island_in_danger");

  ExpectUnsignedValue(result->values, "flipped_into_3d_raw", 1);
  ExpectBoolValue(result->values, "is_3d", true);
  ExpectUnsignedValue(result->values, "mansion_patrol_score", 12345);
  ExpectBoolValue(result->values, "mansion_patrol_cleared", true);
  ExpectUnsignedValue(result->values, "forget_me_not_score", 888);
  ExpectBoolValue(result->values, "forget_me_not_cleared", true);
  ExpectUnsignedValue(result->values, "hammer_whacker_score", 999);
  ExpectBoolValue(result->values, "hammer_whacker_one_life_remaining", true);
}

TEST(CheevoMapV2ReferencePackage, R8PP01TiltZeroLivesIsNotDanger)
{
  std::string error;
  const std::optional<Package> package = LoadR8PP01ReferencePackage(&error);
  ASSERT_TRUE(package) << error;

  const auto evaluate_tilt = [&](u32 status, u32 lives) {
    FakeDataSource data_source;
    ConfigureR8PP01DataSource(&data_source);
    PopulateR8PP01ReferenceMemory(&data_source, status, lives);
    return EvaluatePackage(*package, data_source, &error);
  };

  std::optional<CheevoMap::V2::EvaluationResult> result = evaluate_tilt(0x08000000, 0);
  ASSERT_TRUE(result) << error;
  ExpectBoolValue(result->values, "tilt_island_has_lives", false);
  ExpectBoolValue(result->values, "tilt_island_one_life_remaining", false);
  ExpectBoolValue(result->values, "tilt_island_in_danger", false);

  result = evaluate_tilt(0x08000000, 1);
  ASSERT_TRUE(result) << error;
  ExpectBoolValue(result->values, "tilt_island_has_lives", true);
  ExpectBoolValue(result->values, "tilt_island_one_life_remaining", true);
  ExpectBoolValue(result->values, "tilt_island_in_danger", true);

  result = evaluate_tilt(0x0b000002, 1);
  ASSERT_TRUE(result) << error;
  ExpectBoolValue(result->values, "tilt_island_has_lives", true);
  ExpectBoolValue(result->values, "tilt_island_one_life_remaining", true);
  ExpectBoolValue(result->values, "tilt_island_cleared", true);
  ExpectBoolValue(result->values, "tilt_island_in_danger", false);
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
