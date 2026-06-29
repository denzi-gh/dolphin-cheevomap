// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Core/CheevoMap/CheevoMapEvaluator.h"
#include "Core/CheevoMap/CheevoMapFile.h"

namespace
{
using CheevoMap::AchievementMode;
using CheevoMap::AchievementStateSource;
using CheevoMap::EntryDef;
using CheevoMap::EvaluationStatus;
using CheevoMap::EvaluateEntry;
using CheevoMap::IconKind;
using CheevoMap::LiveValue;
using CheevoMap::MakeInitialLiveValue;
using CheevoMap::Op;
using CheevoMap::ReadSpec;
using CheevoMap::ReadType;
using CheevoMap::Source;
using CheevoMap::V2::EmulatorDataSource;
using CheevoMap::V2::EmulatorStatus;
using CheevoMap::V2::GameIdentity;
using CheevoMap::V2::MemoryArea;
using CheevoMap::V2::MemoryReadError;
using CheevoMap::V2::MemoryReadRequest;
using CheevoMap::V2::StateValue;

class FakeDataSource final : public EmulatorDataSource
{
public:
  using EmulatorDataSource::ReadMemory;

  EmulatorStatus GetStatus() const override { return EmulatorStatus::Running; }
  GameIdentity GetGameIdentity() const override { return GameIdentity{"GAME01", "", 0, 0}; }
  std::vector<MemoryArea> GetMemoryAreas() const override { return {}; }

  bool ReadMemory(u64 address, u8* out, std::size_t size) const override
  {
    for (std::size_t i = 0; i < size; ++i)
    {
      const auto it = m_memory.find(address + i);
      if (it == m_memory.end())
        return false;
      out[i] = it->second;
    }
    return true;
  }

  void Write(u64 address, std::initializer_list<u8> bytes)
  {
    for (const u8 byte : bytes)
      m_memory[address++] = byte;
  }

private:
  std::map<u64, u8> m_memory;
};

class FakeAchievements final : public AchievementStateSource
{
public:
  std::optional<bool> IsAchievementUnlocked(u32 achievement_id,
                                            AchievementMode mode) const override
  {
    const auto it = m_unlocked.find({achievement_id, mode});
    if (it == m_unlocked.end())
      return std::nullopt;
    return it->second;
  }

  void Set(u32 achievement_id, AchievementMode mode, bool unlocked)
  {
    m_unlocked[{achievement_id, mode}] = unlocked;
  }

private:
  std::map<std::pair<u32, AchievementMode>, bool> m_unlocked;
};

class TempDir
{
public:
  TempDir()
  {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    m_path = std::filesystem::temp_directory_path() /
             ("dolphin-cheevomap-test-" + std::to_string(now));
    std::filesystem::create_directories(m_path);
  }

  ~TempDir()
  {
    std::error_code error;
    std::filesystem::remove_all(m_path, error);
  }

  const std::filesystem::path& Path() const { return m_path; }

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

EntryDef MakeRamEntry(std::string id, ReadType type, u32 address)
{
  EntryDef def;
  def.id = std::move(id);
  def.label = def.id;
  def.read.source = Source::Ram;
  def.read.type = type;
  def.read.addr = address;
  return def;
}

StateValue EvaluateAvailable(const EntryDef& def, const FakeDataSource& data_source,
                             LiveValue* live)
{
  StateValue state_value;
  EXPECT_EQ(EvaluateEntry(def, data_source, nullptr, live, &state_value),
            EvaluationStatus::Available);
  return state_value;
}

TEST(CheevoMapEvaluator, PreservesV1FormattingAndTypedIntegerState)
{
  FakeDataSource data_source;
  data_source.Write(0x10, {0x12, 0x34});

  EntryDef def = MakeRamEntry("coins", ReadType::U16, 0x10);
  def.display.format = "{value}";
  LiveValue live = MakeInitialLiveValue(def);
  const StateValue state = EvaluateAvailable(def, data_source, &live);

  EXPECT_EQ(live.value_str, "4660");
  EXPECT_EQ(live.raw_value, 4660);
  ASSERT_TRUE(state.AsUnsignedInteger());
  EXPECT_EQ(*state.AsUnsignedInteger(), 4660u);
}

TEST(CheevoMapEvaluator, SignExtendsSignedIntegerState)
{
  FakeDataSource data_source;
  data_source.Write(0x20, {0xff});

  EntryDef def = MakeRamEntry("signed", ReadType::S8, 0x20);
  LiveValue live = MakeInitialLiveValue(def);
  const StateValue state = EvaluateAvailable(def, data_source, &live);

  EXPECT_EQ(live.value_str, "-1");
  ASSERT_TRUE(state.AsSignedInteger());
  EXPECT_EQ(*state.AsSignedInteger(), -1);
}

TEST(CheevoMapEvaluator, FloatUiRemainsV1ButTypedStateKeepsFraction)
{
  FakeDataSource data_source;
  data_source.Write(0x30, {0x3f, 0xc0, 0x00, 0x00});

  EntryDef def = MakeRamEntry("timer", ReadType::F32, 0x30);
  LiveValue live = MakeInitialLiveValue(def);
  const StateValue state = EvaluateAvailable(def, data_source, &live);

  EXPECT_EQ(live.value_str, "1");
  ASSERT_TRUE(state.AsFloatingPoint());
  EXPECT_DOUBLE_EQ(*state.AsFloatingPoint(), 1.5);
}

TEST(CheevoMapEvaluator, TypedFloatStatePreservesFractionalAndNearbyValues)
{
  FakeDataSource data_source;
  data_source.Write(0x30, {0x3f, 0x00, 0x00, 0x00});
  data_source.Write(0x34, {0x3f, 0xa0, 0x00, 0x00});
  data_source.Write(0x38, {0xbf, 0x00, 0x00, 0x00});
  data_source.Write(0x3c, {0x3f, 0x80, 0x00, 0x00});
  data_source.Write(0x40, {0x3f, 0x80, 0x00, 0x01});

  LiveValue half_live = MakeInitialLiveValue(MakeRamEntry("half", ReadType::F32, 0x30));
  const StateValue half =
      EvaluateAvailable(MakeRamEntry("half", ReadType::F32, 0x30), data_source, &half_live);
  LiveValue one_and_quarter_live =
      MakeInitialLiveValue(MakeRamEntry("one_and_quarter", ReadType::F32, 0x34));
  const StateValue one_and_quarter = EvaluateAvailable(
      MakeRamEntry("one_and_quarter", ReadType::F32, 0x34), data_source, &one_and_quarter_live);
  LiveValue negative_half_live =
      MakeInitialLiveValue(MakeRamEntry("negative_half", ReadType::F32, 0x38));
  const StateValue negative_half = EvaluateAvailable(
      MakeRamEntry("negative_half", ReadType::F32, 0x38), data_source, &negative_half_live);
  LiveValue one_live = MakeInitialLiveValue(MakeRamEntry("one", ReadType::F32, 0x3c));
  const StateValue one =
      EvaluateAvailable(MakeRamEntry("one", ReadType::F32, 0x3c), data_source, &one_live);
  LiveValue next_live = MakeInitialLiveValue(MakeRamEntry("next", ReadType::F32, 0x40));
  const StateValue next =
      EvaluateAvailable(MakeRamEntry("next", ReadType::F32, 0x40), data_source, &next_live);

  ASSERT_TRUE(half.AsFloatingPoint());
  ASSERT_TRUE(one_and_quarter.AsFloatingPoint());
  ASSERT_TRUE(negative_half.AsFloatingPoint());
  ASSERT_TRUE(one.AsFloatingPoint());
  ASSERT_TRUE(next.AsFloatingPoint());
  EXPECT_DOUBLE_EQ(*half.AsFloatingPoint(), 0.5);
  EXPECT_DOUBLE_EQ(*one_and_quarter.AsFloatingPoint(), 1.25);
  EXPECT_DOUBLE_EQ(*negative_half.AsFloatingPoint(), -0.5);
  EXPECT_NE(*one.AsFloatingPoint(), *next.AsFloatingPoint());
}

TEST(CheevoMapDataSource, GroupedReadsKeepOrderAndPerResultFailures)
{
  FakeDataSource data_source;
  data_source.Write(0x10, {0xaa, 0xbb});
  data_source.Write(0x20, {0xcc});

  const auto results = data_source.ReadMemory({
      MemoryReadRequest{{}, 0x10, 2},
      MemoryReadRequest{{}, 0x18, 2},
      MemoryReadRequest{"unsupported", 0x20, 1},
      MemoryReadRequest{{}, 0x20, 1},
  });

  ASSERT_EQ(results.size(), 4u);
  EXPECT_TRUE(results[0].success);
  EXPECT_EQ(results[0].bytes, std::vector<u8>({0xaa, 0xbb}));
  EXPECT_EQ(results[0].request.address, 0x10u);
  EXPECT_FALSE(results[1].success);
  EXPECT_EQ(results[1].error, MemoryReadError::ReadFailure);
  EXPECT_FALSE(results[2].success);
  EXPECT_EQ(results[2].error, MemoryReadError::UnsupportedMemoryArea);
  EXPECT_TRUE(results[3].success);
  EXPECT_EQ(results[3].bytes, std::vector<u8>({0xcc}));
}

TEST(CheevoMapEvaluator, AsciiValuesTrimSpacesAndMapLabels)
{
  FakeDataSource data_source;
  data_source.Write(0x40, {'A', 'B', ' ', 0});

  EntryDef def = MakeRamEntry("room", ReadType::ASCII, 0x40);
  def.read.bytes = 4;
  def.display.format = "{label}";
  def.display.value_map.emplace_back("AB", "Alpha Base");
  LiveValue live = MakeInitialLiveValue(def);
  const StateValue state = EvaluateAvailable(def, data_source, &live);

  EXPECT_EQ(live.value_str, "Alpha Base");
  ASSERT_NE(state.AsString(), nullptr);
  EXPECT_EQ(*state.AsString(), "AB");
}

TEST(CheevoMapEvaluator, BitsAndPopcountKeepV1Results)
{
  FakeDataSource data_source;
  data_source.Write(0x50, {0xb4});

  EntryDef bits = MakeRamEntry("bits", ReadType::U8, 0x50);
  bits.read.bits = std::pair<u8, u8>{5, 2};
  LiveValue bits_live = MakeInitialLiveValue(bits);
  const StateValue bits_state = EvaluateAvailable(bits, data_source, &bits_live);
  EXPECT_EQ(bits_live.value_str, "13");
  ASSERT_TRUE(bits_state.AsUnsignedInteger());
  EXPECT_EQ(*bits_state.AsUnsignedInteger(), 13u);

  EntryDef popcount = MakeRamEntry("popcount", ReadType::U8, 0x50);
  popcount.read.op = Op::Popcount;
  LiveValue popcount_live = MakeInitialLiveValue(popcount);
  const StateValue popcount_state = EvaluateAvailable(popcount, data_source, &popcount_live);
  EXPECT_EQ(popcount_live.value_str, "4");
  ASSERT_TRUE(popcount_state.AsUnsignedInteger());
  EXPECT_EQ(*popcount_state.AsUnsignedInteger(), 4u);
}

TEST(CheevoMapEvaluator, VisibilityGateHidesWithoutOverwritingV1Value)
{
  FakeDataSource data_source;
  data_source.Write(0x60, {0});
  data_source.Write(0x61, {5});

  EntryDef def = MakeRamEntry("gated", ReadType::U8, 0x61);
  ReadSpec gate;
  gate.source = Source::Ram;
  gate.type = ReadType::U8;
  gate.addr = 0x60;
  def.requires_read = gate;

  LiveValue live = MakeInitialLiveValue(def);
  live.value_str = "previous";
  StateValue state;
  EXPECT_EQ(EvaluateEntry(def, data_source, nullptr, &live, &state), EvaluationStatus::Hidden);
  EXPECT_FALSE(live.visible);
  EXPECT_EQ(live.value_str, "previous");
  EXPECT_FALSE(state.IsAvailable());
}

TEST(CheevoMapEvaluator, MilestonesValueMapsIconsAndAchievementsRemainCompatible)
{
  FakeDataSource data_source;
  data_source.Write(0x70, {2});

  EntryDef milestones = MakeRamEntry("milestones", ReadType::U8, 0x70);
  milestones.display.milestones.push_back({1, "One"});
  milestones.display.milestones.push_back({3, "Three"});
  LiveValue milestone_live = MakeInitialLiveValue(milestones);
  EvaluateAvailable(milestones, data_source, &milestone_live);
  EXPECT_EQ(milestone_live.value_str, "[x] One\n[ ] Three");

  EntryDef mapped = MakeRamEntry("mapped", ReadType::U8, 0x70);
  mapped.display.format = "{label}";
  mapped.display.value_map.emplace_back("2", "Open");
  LiveValue mapped_live = MakeInitialLiveValue(mapped);
  EvaluateAvailable(mapped, data_source, &mapped_live);
  EXPECT_EQ(mapped_live.value_str, "Open");

  EntryDef icons = MakeRamEntry("icons", ReadType::U8, 0x70);
  icons.display.max = 3;
  icons.display.icon_kind = IconKind::FillN;
  icons.display.icons_filled = "filled.png";
  icons.display.icons_empty = "empty.png";
  LiveValue icons_live = MakeInitialLiveValue(icons);
  EvaluateAvailable(icons, data_source, &icons_live);
  ASSERT_EQ(icons_live.icon_slots.size(), 3u);
  EXPECT_EQ(icons_live.icon_slots[0], "filled.png");
  EXPECT_EQ(icons_live.icon_slots[1], "filled.png");
  EXPECT_EQ(icons_live.icon_slots[2], "empty.png");

  FakeAchievements achievements;
  achievements.Set(123, AchievementMode::Hardcore, true);
  EntryDef achievement;
  achievement.id = "achievement";
  achievement.label = "achievement";
  achievement.read.source = Source::Achievement;
  achievement.read.achievement_id = 123;
  achievement.read.achievement_mode = AchievementMode::Hardcore;
  LiveValue achievement_live = MakeInitialLiveValue(achievement);
  StateValue achievement_state;
  EXPECT_EQ(EvaluateEntry(achievement, data_source, &achievements, &achievement_live,
                          &achievement_state),
            EvaluationStatus::Available);
  EXPECT_EQ(achievement_live.value_str, "1");
  ASSERT_TRUE(achievement_state.AsBoolean());
  EXPECT_TRUE(*achievement_state.AsBoolean());
}

TEST(CheevoMapParser, LoadsValidSchemaV1Package)
{
  TempDir dir;
  dir.WriteFile("icon.png", "x");
  const auto json = dir.WriteFile("cheevomap.json", R"({
    "schema_version": 1,
    "game_id": "GAME01",
    "title": "Test Game",
    "entries": [
      {
        "id": "coins",
        "label": "Coins",
        "read": {"addr": "0x80000010", "type": "u8"},
        "display": {"icon": {"1": "icon.png"}}
      }
    ]
  })");

  std::string error;
  const auto file = CheevoMap::LoadFromFile(json.string(), &error);
  ASSERT_TRUE(file) << error;
  EXPECT_EQ(file->schema_version, 1u);
  EXPECT_EQ(file->entries.front().read.addr, 0x10u);
  ASSERT_EQ(file->entries.front().display.icon_map.size(), 1u);
  EXPECT_TRUE(
      std::filesystem::path(file->entries.front().display.icon_map.front().second).is_absolute());
}

TEST(CheevoMapParser, RejectsDuplicateIds)
{
  TempDir dir;
  const auto json = dir.WriteFile("cheevomap.json", R"({
    "schema_version": 1,
    "game_id": "GAME01",
    "title": "Test Game",
    "entries": [
      {"id": "coins", "label": "Coins", "read": {"addr": "0x80000010", "type": "u8"}},
      {"id": "coins", "label": "Coins 2", "read": {"addr": "0x80000011", "type": "u8"}}
    ]
  })");

  std::string error;
  EXPECT_FALSE(CheevoMap::LoadFromFile(json.string(), &error));
  EXPECT_NE(error.find("duplicate entry id"), std::string::npos);
}

TEST(CheevoMapParser, RejectsBadAddress)
{
  TempDir dir;
  const auto json = dir.WriteFile("cheevomap.json", R"({
    "schema_version": 1,
    "game_id": "GAME01",
    "title": "Test Game",
    "entries": [
      {"id": "coins", "label": "Coins", "read": {"addr": "0x70000010", "type": "u8"}}
    ]
  })");

  std::string error;
  EXPECT_FALSE(CheevoMap::LoadFromFile(json.string(), &error));
  EXPECT_NE(error.find("must be a PowerPC virtual address"), std::string::npos);
}

TEST(CheevoMapParser, RejectsAssetTraversal)
{
  TempDir dir;
  const auto json = dir.WriteFile("cheevomap.json", R"({
    "schema_version": 1,
    "game_id": "GAME01",
    "title": "Test Game",
    "entries": [
      {
        "id": "coins",
        "label": "Coins",
        "read": {"addr": "0x80000010", "type": "u8"},
        "display": {"icon": {"1": "../icon.png"}}
      }
    ]
  })");

  std::string error;
  EXPECT_FALSE(CheevoMap::LoadFromFile(json.string(), &error));
  EXPECT_NE(error.find("may not contain '..'"), std::string::npos);
}
}  // namespace
