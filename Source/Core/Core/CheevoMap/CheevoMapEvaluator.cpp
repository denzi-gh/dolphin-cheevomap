// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CheevoMap/CheevoMapEvaluator.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

namespace CheevoMap
{
namespace
{
struct ReadValue
{
  bool ok = false;
  u64 raw_bits = 0;
  bool is_float = false;
  std::string text;
};

u32 GetReadSize(const ReadSpec& r)
{
  switch (r.type)
  {
  case ReadType::U8:
  case ReadType::S8:
    return 1;
  case ReadType::U16:
  case ReadType::S16:
    return 2;
  case ReadType::U32:
  case ReadType::S32:
  case ReadType::F32:
    return 4;
  case ReadType::U64:
  case ReadType::S64:
    return 8;
  case ReadType::ASCII:
    return r.bytes;
  }
  return 0;
}

u64 ReadBigEndian(const std::vector<u8>& bytes)
{
  u64 value = 0;
  for (const u8 byte : bytes)
    value = (value << 8) | byte;
  return value;
}

ReadValue ReadRam(const V2::EmulatorDataSource& data_source, const ReadSpec& r)
{
  const u32 size = GetReadSize(r);
  std::vector<u8> bytes(size);
  ReadValue out;
  if (!data_source.ReadMemory(r.addr, bytes.data(), bytes.size()))
    return out;

  if (r.type == ReadType::ASCII)
  {
    out.text.reserve(bytes.size());
    for (const u8 byte : bytes)
    {
      if (byte == 0)
        break;
      out.text.push_back(std::isprint(byte) ? static_cast<char>(byte) : '?');
    }
    while (!out.text.empty() && out.text.back() == ' ')
      out.text.pop_back();
    out.ok = true;
    return out;
  }

  out.raw_bits = ReadBigEndian(bytes);
  out.is_float = r.type == ReadType::F32;
  out.ok = true;
  return out;
}

ReadValue ReadAchievement(const AchievementStateSource& achievements, const ReadSpec& r)
{
  ReadValue out;
  const std::optional<bool> unlocked =
      achievements.IsAchievementUnlocked(r.achievement_id, r.achievement_mode);
  if (!unlocked)
    return out;

  out.raw_bits = *unlocked ? 1u : 0u;
  out.ok = true;
  return out;
}

ReadValue ReadSource(const V2::EmulatorDataSource& data_source,
                     const AchievementStateSource* achievements, const ReadSpec& r)
{
  if (r.source == Source::Ram)
    return ReadRam(data_source, r);
  if (achievements == nullptr)
    return {};
  return ReadAchievement(*achievements, r);
}

s64 ApplyExpression(const ReadValue& v, const ReadSpec& r)
{
  if (v.is_float)
  {
    const float f = std::bit_cast<float>(static_cast<u32>(v.raw_bits));
    return static_cast<s64>(f);
  }

  u64 raw = v.raw_bits;
  if (r.bits)
  {
    const u8 hi = r.bits->first;
    const u8 lo = r.bits->second;
    const u64 mask = (hi - lo + 1 >= 64) ? ~u64{0} : ((u64{1} << (hi - lo + 1)) - 1);
    raw = (raw >> lo) & mask;
  }
  if (r.op == Op::Popcount)
    return static_cast<s64>(std::popcount(raw));

  if (!r.bits)
  {
    switch (r.type)
    {
    case ReadType::S8:
      return static_cast<s8>(raw);
    case ReadType::S16:
      return static_cast<s16>(raw);
    case ReadType::S32:
      return static_cast<s32>(static_cast<u32>(raw));
    case ReadType::S64:
      return static_cast<s64>(raw);
    default:
      break;
    }
  }
  return static_cast<s64>(raw);
}

std::string ResolveMappedLabel(const DisplaySpec& d, std::string_view value)
{
  for (const auto& [key, label] : d.value_map)
  {
    if (key == value)
      return label;
  }
  return std::string(value);
}

std::string FormatValue(const DisplaySpec& d, s64 value,
                        std::optional<std::string_view> text_value = std::nullopt)
{
  if (d.divisor > 0)
    value /= d.divisor;
  if (text_value)
  {
    const std::string label = ResolveMappedLabel(d, *text_value);
    if (d.format.empty())
      return label;

    std::string out;
    out.reserve(d.format.size() + label.size() + text_value->size());
    for (size_t i = 0; i < d.format.size();)
    {
      if (d.format[i] == '{')
      {
        const auto end = d.format.find('}', i + 1);
        if (end == std::string::npos)
        {
          out.push_back(d.format[i++]);
          continue;
        }
        const auto token = std::string_view(d.format).substr(i + 1, end - i - 1);
        if (token == "value")
          out.append(text_value->data(), text_value->size());
        else if (token == "label")
          out += label;
        else
          out.append(d.format, i, end - i + 1);
        i = end + 1;
      }
      else
      {
        out.push_back(d.format[i++]);
      }
    }
    return out;
  }
  if (!d.milestones.empty())
  {
    std::string out;
    for (size_t i = 0; i < d.milestones.size(); ++i)
    {
      if (i != 0)
        out.push_back('\n');
      const auto& milestone = d.milestones[i];
      out += value >= milestone.at ? "[x] " : "[ ] ";
      out += milestone.label;
    }
    return out;
  }
  if (d.format.empty())
    return fmt::format("{}", value);
  std::string out;
  out.reserve(d.format.size() + 16);
  for (size_t i = 0; i < d.format.size();)
  {
    if (d.format[i] == '{')
    {
      const auto end = d.format.find('}', i + 1);
      if (end == std::string::npos)
      {
        out.push_back(d.format[i++]);
        continue;
      }
      const auto token = std::string_view(d.format).substr(i + 1, end - i - 1);
      if (token == "value")
      {
        out += fmt::format("{}", value);
      }
      else if (token == "label")
      {
        out += ResolveMappedLabel(d, fmt::format("{}", value));
      }
      else if (token == "max")
      {
        if (d.max)
          out += fmt::format("{}", static_cast<s64>(*d.max));
        else
          out += "?";
      }
      else if (token == "percent")
      {
        if (d.max && *d.max > 0)
          out += fmt::format("{}", static_cast<int>((value * 100) / static_cast<s64>(*d.max)));
        else
          out += "?";
      }
      else
      {
        out.append(d.format, i, end - i + 1);
      }
      i = end + 1;
    }
    else
    {
      out.push_back(d.format[i++]);
    }
  }
  return out;
}

std::string ResolveSingleIcon(const DisplaySpec& d, s64 value)
{
  if (d.icon_kind != IconKind::Single)
    return {};
  for (const auto& [k, path] : d.icon_map)
  {
    if (k == value)
      return path;
  }
  return {};
}

std::vector<std::string> ResolveIconSlots(const DisplaySpec& d, u64 raw)
{
  std::vector<std::string> out;
  if (d.icon_kind == IconKind::FillN)
  {
    if (!d.max)
      return out;
    const u32 total = static_cast<u32>(*d.max);
    out.reserve(total);
    for (u32 i = 0; i < total; ++i)
      out.push_back(static_cast<u64>(i) < raw ? d.icons_filled : d.icons_empty);
  }
  else if (d.icon_kind == IconKind::BitmapArray)
  {
    out.reserve(d.icons_count);
    for (u32 i = 0; i < d.icons_count; ++i)
      out.push_back(((raw >> i) & 1) ? d.icons_filled : d.icons_empty);
  }
  return out;
}

bool IsSignedIntegerType(ReadType type)
{
  switch (type)
  {
  case ReadType::S8:
  case ReadType::S16:
  case ReadType::S32:
  case ReadType::S64:
    return true;
  default:
    return false;
  }
}

V2::StateValue MakeStateValue(const ReadValue& value, const ReadSpec& read)
{
  if (read.source == Source::Achievement)
    return V2::StateValue::Boolean(value.raw_bits != 0);

  if (read.type == ReadType::ASCII)
    return V2::StateValue::String(value.text);

  if (read.type == ReadType::F32)
    return V2::StateValue::FloatingPoint(std::bit_cast<float>(static_cast<u32>(value.raw_bits)));

  const s64 resolved = ApplyExpression(value, read);
  if (IsSignedIntegerType(read.type) && !read.bits && read.op == Op::None)
    return V2::StateValue::SignedInteger(resolved);

  return V2::StateValue::UnsignedInteger(static_cast<u64>(resolved));
}
}  // namespace

LiveValue MakeInitialLiveValue(const EntryDef& def)
{
  LiveValue value;
  value.id = def.id;
  value.label = def.label;
  value.group = def.group;
  value.value_str.clear();
  value.raw_value = 0;
  value.visible = true;
  return value;
}

EvaluationStatus EvaluateEntry(const EntryDef& def, const V2::EmulatorDataSource& data_source,
                               const AchievementStateSource* achievements, LiveValue* live,
                               V2::StateValue* state_value)
{
  if (live == nullptr || state_value == nullptr)
    return EvaluationStatus::Unavailable;

  if (def.requires_read)
  {
    const ReadValue rv = ReadSource(data_source, achievements, *def.requires_read);
    const bool gate = rv.ok && ApplyExpression(rv, *def.requires_read) != 0;
    live->visible = gate;
    if (!gate)
    {
      *state_value = V2::StateValue::Unavailable();
      return EvaluationStatus::Hidden;
    }
  }
  else
  {
    live->visible = true;
  }

  const ReadValue rv = ReadSource(data_source, achievements, def.read);
  if (!rv.ok)
  {
    *state_value = V2::StateValue::Unavailable();
    return EvaluationStatus::Unavailable;
  }

  const bool is_text = def.read.type == ReadType::ASCII;
  const s64 value = is_text ? 0 : ApplyExpression(rv, def.read);
  live->raw_value = value;
  live->value_str =
      is_text ? FormatValue(def.display, value, std::string_view(rv.text)) :
                FormatValue(def.display, value);
  live->icon_path = is_text ? std::string{} : ResolveSingleIcon(def.display, value);
  live->icon_slots =
      is_text ? std::vector<std::string>{} : ResolveIconSlots(def.display, rv.raw_bits);
  *state_value = MakeStateValue(rv, def.read);
  return EvaluationStatus::Available;
}
}  // namespace CheevoMap
