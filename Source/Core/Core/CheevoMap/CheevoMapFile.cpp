// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CheevoMap/CheevoMapFile.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

#include <fmt/format.h>
#include <picojson.h>

#include "Common/CommonTypes.h"
#include "Common/JsonUtil.h"
#include "Core/CheevoMap/V2/PackageParser.h"

namespace CheevoMap
{
namespace
{
constexpr u32 PHYSICAL_MASK = 0x1FFFFFFF;
constexpr u32 VIRTUAL_MEM1_BASE = 0x80000000;
constexpr u32 PHYSICAL_MEM1_END_WII = 0x04000000;   // 32 MB on retail Wii (we permit up to here)
constexpr u32 PHYSICAL_MEM2_BASE = 0x10000000;
constexpr u32 PHYSICAL_MEM2_END = 0x14000000;       // 64 MB

bool ParseHex32(std::string_view sv, u32* out)
{
  if (sv.size() < 3 || sv[0] != '0' || (sv[1] != 'x' && sv[1] != 'X'))
    return false;
  const auto first = sv.data() + 2;
  const auto last = sv.data() + sv.size();
  u32 value = 0;
  const auto [ptr, ec] = std::from_chars(first, last, value, 16);
  if (ec != std::errc{} || ptr != last)
    return false;
  *out = value;
  return true;
}

bool ParseS64Literal(std::string_view sv, s64* out)
{
  if (sv.empty())
    return false;

  int base = 10;
  bool negative = false;
  if (sv.front() == '-')
  {
    negative = true;
    sv.remove_prefix(1);
    if (sv.empty())
      return false;
  }

  if (sv.size() >= 3 && sv[0] == '0' && (sv[1] == 'x' || sv[1] == 'X'))
  {
    base = 16;
    sv.remove_prefix(2);
  }

  s64 value = 0;
  const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value, base);
  if (ec != std::errc{} || ptr != sv.data() + sv.size())
    return false;

  *out = negative ? -value : value;
  return true;
}

bool ParseAddress(std::string_view sv, u32* phys_out, u32* raw_out, std::string* err)
{
  u32 raw = 0;
  if (!ParseHex32(sv, &raw))
  {
    *err = fmt::format("address '{}' is not a valid 0x… hex literal", sv);
    return false;
  }
  if (raw < VIRTUAL_MEM1_BASE)
  {
    *err = fmt::format("address {:#010x} must be a PowerPC virtual address (0x80…/0x90…)", raw);
    return false;
  }
  const u32 phys = raw & PHYSICAL_MASK;
  const bool in_mem1 = phys < PHYSICAL_MEM1_END_WII;
  const bool in_mem2 = phys >= PHYSICAL_MEM2_BASE && phys < PHYSICAL_MEM2_END;
  if (!in_mem1 && !in_mem2)
  {
    *err = fmt::format("address {:#010x} is outside MEM1/MEM2 ranges", raw);
    return false;
  }
  *phys_out = phys;
  *raw_out = raw;
  return true;
}

bool ParseReadType(std::string_view sv, ReadType* out)
{
  if (sv == "u8") { *out = ReadType::U8; return true; }
  if (sv == "u16") { *out = ReadType::U16; return true; }
  if (sv == "u32") { *out = ReadType::U32; return true; }
  if (sv == "u64") { *out = ReadType::U64; return true; }
  if (sv == "s8") { *out = ReadType::S8; return true; }
  if (sv == "s16") { *out = ReadType::S16; return true; }
  if (sv == "s32") { *out = ReadType::S32; return true; }
  if (sv == "s64") { *out = ReadType::S64; return true; }
  if (sv == "f32") { *out = ReadType::F32; return true; }
  if (sv == "ascii") { *out = ReadType::ASCII; return true; }
  return false;
}

u8 TypeBitWidth(ReadType t)
{
  switch (t)
  {
  case ReadType::U8:
  case ReadType::S8:
    return 8;
  case ReadType::U16:
  case ReadType::S16:
    return 16;
  case ReadType::U32:
  case ReadType::S32:
  case ReadType::F32:
    return 32;
  case ReadType::U64:
  case ReadType::S64:
    return 64;
  case ReadType::ASCII:
    return 0;
  }
  return 0;
}

bool IsIntegerType(ReadType t)
{
  return t != ReadType::F32 && t != ReadType::ASCII;
}

bool ParseReadSpec(const picojson::object& obj, ReadSpec* out, std::string* err)
{
  // source (default "ram")
  std::string source_str = "ram";
  if (auto s = ReadStringFromJson(obj, "source"))
    source_str = *s;
  if (source_str == "ram")
  {
    out->source = Source::Ram;
  }
  else if (source_str == "achievement")
  {
    out->source = Source::Achievement;
  }
  else
  {
    *err = fmt::format("read.source must be \"ram\" or \"achievement\", got \"{}\"", source_str);
    return false;
  }

  if (out->source == Source::Ram)
  {
    auto addr_str = ReadStringFromJson(obj, "addr");
    if (!addr_str)
    {
      *err = "ram-source read requires \"addr\" string";
      return false;
    }
    std::string addr_err;
    if (!ParseAddress(*addr_str, &out->addr, &out->raw_addr, &addr_err))
    {
      *err = std::move(addr_err);
      return false;
    }

    auto type_str = ReadStringFromJson(obj, "type");
    if (!type_str)
    {
      *err = "ram-source read requires \"type\" string";
      return false;
    }
    if (!ParseReadType(*type_str, &out->type))
    {
      *err = fmt::format("unknown read.type \"{}\"", *type_str);
      return false;
    }
    if (out->type == ReadType::ASCII)
    {
      auto bytes = ReadNumericFromJson<u32>(obj, "bytes");
      if (!bytes || *bytes == 0 || *bytes > 256)
      {
        *err = "read.type = ascii requires \"bytes\" in 1..256";
        return false;
      }
      out->bytes = *bytes;
    }

    if (auto it = obj.find("bits"); it != obj.end())
    {
      if (!IsIntegerType(out->type))
      {
        *err = "read.bits is only valid for integer types";
        return false;
      }
      if (!it->second.is<picojson::array>())
      {
        *err = "read.bits must be an array [hi, lo]";
        return false;
      }
      const auto& arr = it->second.get<picojson::array>();
      if (arr.size() != 2 || !arr[0].is<double>() || !arr[1].is<double>())
      {
        *err = "read.bits must be [hi, lo] integers";
        return false;
      }
      const int hi = static_cast<int>(arr[0].get<double>());
      const int lo = static_cast<int>(arr[1].get<double>());
      const int width = TypeBitWidth(out->type);
      if (hi < lo || lo < 0 || hi >= width)
      {
        *err = fmt::format("read.bits [{}, {}] out of range for {}-bit type", hi, lo, width);
        return false;
      }
      out->bits = std::make_pair(static_cast<u8>(hi), static_cast<u8>(lo));
    }

    if (auto op_str = ReadStringFromJson(obj, "op"))
    {
      if (*op_str == "popcount")
      {
        if (!IsIntegerType(out->type))
        {
          *err = "read.op = popcount requires an integer type";
          return false;
        }
        out->op = Op::Popcount;
      }
      else
      {
        *err = fmt::format("unknown read.op \"{}\"", *op_str);
        return false;
      }
    }
  }
  else  // Source::Achievement
  {
#ifndef USE_RETRO_ACHIEVEMENTS
    *err = "achievement-source entries require a build with USE_RETRO_ACHIEVEMENTS";
    return false;
#else
    auto id = ReadNumericFromJson<u32>(obj, "id");
    if (!id || *id == 0)
    {
      *err = "achievement-source read requires \"id\" number";
      return false;
    }
    out->achievement_id = *id;
    if (auto mode = ReadStringFromJson(obj, "mode"))
    {
      if (*mode == "any") out->achievement_mode = AchievementMode::Any;
      else if (*mode == "hardcore") out->achievement_mode = AchievementMode::Hardcore;
      else
      {
        *err = fmt::format("unknown read.mode \"{}\"", *mode);
        return false;
      }
    }
#endif
  }
  return true;
}

// Resolve `relative` against `base_dir`. Reject if the result escapes base_dir,
// is absolute, contains "..", or references a non-existent file.
bool ResolveAsset(const std::filesystem::path& base_dir, const std::string& relative,
                  std::string* abs_out, std::string* err)
{
  const std::filesystem::path rel(relative);
  if (rel.is_absolute())
  {
    *err = fmt::format("asset path '{}' must be relative to the CheevoMap directory", relative);
    return false;
  }
  for (const auto& part : rel)
  {
    if (part == "..")
    {
      *err = fmt::format("asset path '{}' may not contain '..'", relative);
      return false;
    }
  }
  std::error_code ec;
  const auto candidate = base_dir / rel;
  const auto canonical_base = std::filesystem::weakly_canonical(base_dir, ec);
  if (ec)
  {
    *err = fmt::format("could not canonicalize base directory '{}'", base_dir.string());
    return false;
  }
  const auto canonical = std::filesystem::weakly_canonical(candidate, ec);
  if (ec)
  {
    *err = fmt::format("could not resolve asset path '{}'", relative);
    return false;
  }
  // Containment check
  const auto base_str = canonical_base.string();
  const auto canon_str = canonical.string();
  if (canon_str.size() < base_str.size() || canon_str.compare(0, base_str.size(), base_str) != 0)
  {
    *err = fmt::format("asset '{}' resolves outside the CheevoMap directory", relative);
    return false;
  }
  std::error_code ec2;
  if (!std::filesystem::exists(canonical, ec2))
  {
    *err = fmt::format("asset file not found: '{}'", canonical.string());
    return false;
  }
  *abs_out = canonical.string();
  return true;
}

bool ParseDisplaySpec(const picojson::object& obj, const std::filesystem::path& base_dir,
                     DisplaySpec* out, std::string* err)
{
  if (auto fmt_str = ReadStringFromJson(obj, "format"))
    out->format = *fmt_str;

  if (auto max_v = ReadNumericFromJson<double>(obj, "max"))
    out->max = *max_v;

  if (auto div_v = ReadNumericFromJson<s64>(obj, "divisor"))
  {
    if (*div_v <= 0)
    {
      *err = "display.divisor must be a positive integer";
      return false;
    }
    out->divisor = *div_v;
  }

  if (auto map_it = obj.find("map"); map_it != obj.end())
  {
    if (!map_it->second.is<picojson::object>())
    {
      *err = "display.map must be an object mapping raw values to labels";
      return false;
    }

    for (const auto& [key, v] : map_it->second.get<picojson::object>())
    {
      if (!v.is<std::string>())
      {
        *err = fmt::format("display.map[\"{}\"] must be a string label", key);
        return false;
      }
      out->value_map.emplace_back(key, v.get<std::string>());
    }
  }

  if (auto milestones_it = obj.find("milestones"); milestones_it != obj.end())
  {
    if (!milestones_it->second.is<picojson::array>())
    {
      *err = "display.milestones must be an array";
      return false;
    }

    const auto& milestones = milestones_it->second.get<picojson::array>();
    if (milestones.empty())
    {
      *err = "display.milestones must contain at least one milestone";
      return false;
    }

    out->milestones.reserve(milestones.size());
    for (size_t i = 0; i < milestones.size(); ++i)
    {
      const auto& milestone_v = milestones[i];
      if (!milestone_v.is<picojson::object>())
      {
        *err = fmt::format("display.milestones[{}] must be an object", i);
        return false;
      }

      const auto& milestone_obj = milestone_v.get<picojson::object>();
      auto label = ReadStringFromJson(milestone_obj, "label");
      if (!label || label->empty())
      {
        *err = fmt::format("display.milestones[{}] requires non-empty \"label\"", i);
        return false;
      }

      s64 at = 0;
      const auto at_it = milestone_obj.find("at");
      if (at_it == milestone_obj.end())
      {
        *err = fmt::format("display.milestones[{}] requires \"at\"", i);
        return false;
      }
      if (at_it->second.is<double>())
      {
        at = static_cast<s64>(at_it->second.get<double>());
      }
      else if (at_it->second.is<std::string>())
      {
        const auto& at_str = at_it->second.get<std::string>();
        if (!ParseS64Literal(at_str, &at))
        {
          *err = fmt::format(
              "display.milestones[{}].at string '{}' must be a decimal or 0x hex integer", i,
              at_str);
          return false;
        }
      }
      else
      {
        *err = fmt::format("display.milestones[{}].at must be a number or string", i);
        return false;
      }

      out->milestones.push_back({at, *label});
    }
  }

  const bool has_icon = obj.find("icon") != obj.end();
  const bool has_icons = obj.find("icons") != obj.end();
  if (has_icon && has_icons)
  {
    *err = "display.icon and display.icons are mutually exclusive";
    return false;
  }

  if (has_icon)
  {
    const auto& icon_v = obj.at("icon");
    if (!icon_v.is<picojson::object>())
    {
      *err = "display.icon must be an object mapping value-strings to asset paths";
      return false;
    }
    out->icon_kind = IconKind::Single;
    for (const auto& [key, v] : icon_v.get<picojson::object>())
    {
      if (!v.is<std::string>())
      {
        *err = fmt::format("display.icon[\"{}\"] must be a string asset path", key);
        return false;
      }
      s64 value = 0;
      const auto first = key.data();
      const auto last = key.data() + key.size();
      auto [ptr, ec] = std::from_chars(first, last, value, 10);
      if (ec != std::errc{} || ptr != last)
      {
        *err = fmt::format("display.icon key \"{}\" must be an integer", key);
        return false;
      }
      std::string abs;
      if (!ResolveAsset(base_dir, v.get<std::string>(), &abs, err))
        return false;
      out->icon_map.emplace_back(value, std::move(abs));
    }
  }

  if (has_icons)
  {
    const auto& icons_v = obj.at("icons");
    if (!icons_v.is<picojson::object>())
    {
      *err = "display.icons must be an object";
      return false;
    }
    const auto& icons_obj = icons_v.get<picojson::object>();
    auto kind_str = ReadStringFromJson(icons_obj, "kind");
    if (!kind_str)
    {
      *err = "display.icons.kind is required (\"fill_n\" or \"bitmap_array\")";
      return false;
    }
    if (*kind_str == "fill_n")
    {
      out->icon_kind = IconKind::FillN;
      if (!out->max)
      {
        *err = "display.icons.kind = fill_n requires display.max";
        return false;
      }
    }
    else if (*kind_str == "bitmap_array")
    {
      out->icon_kind = IconKind::BitmapArray;
      auto count = ReadNumericFromJson<u32>(icons_obj, "count");
      if (!count || *count == 0 || *count > 64)
      {
        *err = "display.icons.kind = bitmap_array requires \"count\" in 1..64";
        return false;
      }
      out->icons_count = *count;
    }
    else
    {
      *err = fmt::format("unknown display.icons.kind \"{}\"", *kind_str);
      return false;
    }

    auto filled = ReadStringFromJson(icons_obj, "filled");
    auto empty = ReadStringFromJson(icons_obj, "empty");
    if (!filled || !empty)
    {
      *err = "display.icons requires \"filled\" and \"empty\" asset paths";
      return false;
    }
    if (!ResolveAsset(base_dir, *filled, &out->icons_filled, err))
      return false;
    if (!ResolveAsset(base_dir, *empty, &out->icons_empty, err))
      return false;
  }
  return true;
}

bool ParseEntry(const picojson::object& obj, const std::filesystem::path& base_dir,
                EntryDef* out, std::string* err)
{
  auto id = ReadStringFromJson(obj, "id");
  if (!id || id->empty())
  {
    *err = "entry requires non-empty \"id\"";
    return false;
  }
  out->id = *id;

  auto label = ReadStringFromJson(obj, "label");
  if (!label || label->empty())
  {
    *err = fmt::format("entry \"{}\" requires non-empty \"label\"", out->id);
    return false;
  }
  out->label = *label;

  if (auto group = ReadStringFromJson(obj, "group"))
    out->group = *group;

  auto read_it = obj.find("read");
  if (read_it == obj.end() || !read_it->second.is<picojson::object>())
  {
    *err = fmt::format("entry \"{}\" requires \"read\" object", out->id);
    return false;
  }
  if (!ParseReadSpec(read_it->second.get<picojson::object>(), &out->read, err))
  {
    *err = fmt::format("entry \"{}\".read: {}", out->id, *err);
    return false;
  }

  auto display_it = obj.find("display");
  if (display_it != obj.end())
  {
    if (!display_it->second.is<picojson::object>())
    {
      *err = fmt::format("entry \"{}\" \"display\" must be an object", out->id);
      return false;
    }
    if (!ParseDisplaySpec(display_it->second.get<picojson::object>(), base_dir, &out->display, err))
    {
      *err = fmt::format("entry \"{}\".display: {}", out->id, *err);
      return false;
    }
  }

  if (auto req_it = obj.find("requires"); req_it != obj.end())
  {
    if (!req_it->second.is<picojson::object>())
    {
      *err = fmt::format("entry \"{}\".requires must be an object", out->id);
      return false;
    }
    ReadSpec rs;
    if (!ParseReadSpec(req_it->second.get<picojson::object>(), &rs, err))
    {
      *err = fmt::format("entry \"{}\".requires: {}", out->id, *err);
      return false;
    }
    out->requires_read = std::move(rs);
  }

  return true;
}

}  // namespace

std::optional<File> LoadFromFile(const std::string& json_path, std::string* error_out)
{
  picojson::value root;
  std::string parse_err;
  if (!JsonFromFile(json_path, &root, &parse_err))
  {
    *error_out = fmt::format("could not read JSON: {}", parse_err);
    return std::nullopt;
  }
  if (!root.is<picojson::object>())
  {
    *error_out = "top-level JSON must be an object";
    return std::nullopt;
  }
  const auto& obj = root.get<picojson::object>();

  File file;

  auto schema = ReadNumericFromJson<u32>(obj, "schema_version");
  if (!schema)
  {
    *error_out = "missing or invalid schema_version";
    return std::nullopt;
  }
  if (*schema != 1)
  {
    *error_out = fmt::format("unsupported schema_version {} (only 1 is known)", *schema);
    return std::nullopt;
  }
  file.schema_version = *schema;

  auto game_id = ReadStringFromJson(obj, "game_id");
  if (!game_id || game_id->empty())
  {
    *error_out = "missing or empty game_id";
    return std::nullopt;
  }
  file.game_id = *game_id;

  auto title = ReadStringFromJson(obj, "title");
  if (!title || title->empty())
  {
    *error_out = "missing or empty title";
    return std::nullopt;
  }
  file.title = *title;

  if (auto rev = ReadNumericFromJson<u32>(obj, "revision"))
    file.revision = *rev;

  if (auto hz = ReadNumericFromJson<double>(obj, "poll_hz"))
  {
    if (*hz <= 0.0 || *hz > 240.0)
    {
      *error_out = fmt::format("poll_hz {} out of range (0, 240]", *hz);
      return std::nullopt;
    }
    file.poll_hz = *hz;
  }

  auto entries_it = obj.find("entries");
  if (entries_it == obj.end() || !entries_it->second.is<picojson::array>())
  {
    *error_out = "missing or invalid entries array";
    return std::nullopt;
  }
  const auto& entries_arr = entries_it->second.get<picojson::array>();
  if (entries_arr.empty())
  {
    *error_out = "entries array must contain at least one entry";
    return std::nullopt;
  }

  const auto base_dir = std::filesystem::path(json_path).parent_path();
  file.entries.reserve(entries_arr.size());
  for (const auto& v : entries_arr)
  {
    if (!v.is<picojson::object>())
    {
      *error_out = "each entry must be an object";
      return std::nullopt;
    }
    EntryDef def;
    std::string entry_err;
    if (!ParseEntry(v.get<picojson::object>(), base_dir, &def, &entry_err))
    {
      *error_out = std::move(entry_err);
      return std::nullopt;
    }
    file.entries.push_back(std::move(def));
  }

  // Unique ids
  for (size_t i = 0; i < file.entries.size(); ++i)
  {
    for (size_t j = i + 1; j < file.entries.size(); ++j)
    {
      if (file.entries[i].id == file.entries[j].id)
      {
        *error_out = fmt::format("duplicate entry id \"{}\"", file.entries[i].id);
        return std::nullopt;
      }
    }
  }

  return file;
}

std::optional<LoadedPackage> LoadPackageFromFile(const std::string& json_path,
                                                 std::string* error_out)
{
  picojson::value root;
  std::string parse_err;
  if (!JsonFromFile(json_path, &root, &parse_err))
  {
    *error_out = fmt::format("could not read JSON: {}", parse_err);
    return std::nullopt;
  }
  if (!root.is<picojson::object>())
  {
    *error_out = "top-level JSON must be an object";
    return std::nullopt;
  }

  const auto& obj = root.get<picojson::object>();
  const auto schema_it = obj.find("schema_version");
  if (schema_it == obj.end() || !schema_it->second.is<double>() ||
      schema_it->second.get<double>() < 0.0 ||
      schema_it->second.get<double>() > static_cast<double>(std::numeric_limits<u32>::max()) ||
      schema_it->second.get<double>() !=
          static_cast<double>(static_cast<u32>(schema_it->second.get<double>())))
  {
    *error_out = "missing or invalid schema_version";
    return std::nullopt;
  }

  const u32 schema = static_cast<u32>(schema_it->second.get<double>());
  if (schema == 1)
  {
    auto file = LoadFromFile(json_path, error_out);
    if (!file)
      return std::nullopt;
    return LoadedPackage{std::move(*file)};
  }

  if (schema == 2)
  {
    auto package = V2::ParsePackage(root, error_out);
    if (!package)
      return std::nullopt;
    return LoadedPackage{std::move(*package)};
  }

  *error_out = fmt::format("unsupported schema_version {} (only 1 and 2 are known)", schema);
  return std::nullopt;
}

}  // namespace CheevoMap
