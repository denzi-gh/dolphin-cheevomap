// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

namespace CheevoMap
{
enum class ReadType : u8
{
  U8,
  U16,
  U32,
  U64,
  S8,
  S16,
  S32,
  S64,
  F32,
  ASCII,
};

enum class Op : u8
{
  None,
  Popcount,
};

enum class Source : u8
{
  Ram,
  Achievement,
};

enum class IconKind : u8
{
  None,
  Single,        // display.icon: { value-string -> path }
  FillN,         // display.icons.kind = "fill_n"  (max slots, first `value` filled)
  BitmapArray,   // display.icons.kind = "bitmap_array"  (per-bit slots)
};

enum class AchievementMode : u8
{
  Any,
  Hardcore,
};

struct ReadSpec
{
  Source source = Source::Ram;

  // Source::Ram
  u32 addr = 0;            // physical (post-strip)
  u32 raw_addr = 0;        // original 0x80…/0x90… kept for diagnostics
  ReadType type = ReadType::U8;
  u32 bytes = 0;           // ReadType::ASCII
  std::optional<std::pair<u8, u8>> bits;  // [hi, lo] inclusive
  Op op = Op::None;

  // Source::Achievement
  u32 achievement_id = 0;
  AchievementMode achievement_mode = AchievementMode::Any;
};

struct DisplaySpec
{
  std::string format;              // optional; supports {value}, {max}, {percent}
  std::optional<double> max;
  s64 divisor = 0;                 // when > 0, raw value is divided by this before formatting

  IconKind icon_kind = IconKind::None;
  // IconKind::Single:
  std::vector<std::pair<s64, std::string>> icon_map;  // value -> absolute asset path
  // IconKind::FillN / BitmapArray:
  std::string icons_filled;        // absolute asset path
  std::string icons_empty;         // absolute asset path
  u32 icons_count = 0;             // BitmapArray; FillN uses display.max

  std::vector<std::pair<std::string, std::string>> value_map;  // raw value string -> label

  struct Milestone
  {
    s64 at = 0;
    std::string label;
  };
  std::vector<Milestone> milestones;  // checklist rows; complete when value >= at
};

struct EntryDef
{
  std::string id;
  std::string label;
  std::string group;
  ReadSpec read;
  DisplaySpec display;
  std::optional<ReadSpec> requires_read;
};

struct File
{
  u32 schema_version = 0;
  std::string game_id;
  std::string title;
  std::optional<u32> revision;
  double poll_hz = 10.0;
  std::vector<EntryDef> entries;
};

// Snapshot of one entry, computed each tick. Exposed to the UI.
struct LiveValue
{
  std::string id;
  std::string label;
  std::string group;
  std::string value_str;
  s64 raw_value = 0;
  bool visible = true;
  std::string icon_path;                // single-icon mode (already state-resolved)
  std::vector<std::string> icon_slots;  // multi-slot modes (per-slot resolved)
};

}  // namespace CheevoMap
