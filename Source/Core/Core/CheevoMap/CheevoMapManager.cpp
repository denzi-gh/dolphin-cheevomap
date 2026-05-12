// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CheevoMap/CheevoMapManager.h"

#include <bit>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/CheevoMap/CheevoMapFile.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/PowerPC/MMU.h"
#include "Core/System.h"

#ifdef USE_RETRO_ACHIEVEMENTS
#include <rcheevos/include/rc_client.h>
#include "Core/AchievementManager.h"
#endif

namespace CheevoMap
{
namespace
{
struct ReadValue
{
  bool ok = false;
  u64 raw_bits = 0;   // for integers, the unsigned bit pattern; for floats, the IEEE bits
  bool is_float = false;
  std::string text;
};

ReadValue ReadRam(const Core::CPUThreadGuard& guard, const ReadSpec& r)
{
  auto& mmu = guard.GetSystem().GetMMU();
  ReadValue out;
  switch (r.type)
  {
  case ReadType::U8:
  case ReadType::S8:
  {
    auto v = mmu.HostTryRead<u8>(guard, r.addr, PowerPC::RequestedAddressSpace::Physical);
    if (!v) return out;
    out.raw_bits = v->value;
    out.ok = true;
    return out;
  }
  case ReadType::U16:
  case ReadType::S16:
  {
    auto v = mmu.HostTryRead<u16>(guard, r.addr, PowerPC::RequestedAddressSpace::Physical);
    if (!v) return out;
    out.raw_bits = v->value;
    out.ok = true;
    return out;
  }
  case ReadType::U32:
  case ReadType::S32:
  {
    auto v = mmu.HostTryRead<u32>(guard, r.addr, PowerPC::RequestedAddressSpace::Physical);
    if (!v) return out;
    out.raw_bits = v->value;
    out.ok = true;
    return out;
  }
  case ReadType::U64:
  case ReadType::S64:
  {
    // Read as two big-endian u32s (no HostTryRead<u64> in the MMU).
    auto hi = mmu.HostTryRead<u32>(guard, r.addr, PowerPC::RequestedAddressSpace::Physical);
    if (!hi) return out;
    auto lo = mmu.HostTryRead<u32>(guard, r.addr + 4, PowerPC::RequestedAddressSpace::Physical);
    if (!lo) return out;
    out.raw_bits = (static_cast<u64>(hi->value) << 32) | static_cast<u64>(lo->value);
    out.ok = true;
    return out;
  }
  case ReadType::F32:
  {
    auto v = mmu.HostTryRead<u32>(guard, r.addr, PowerPC::RequestedAddressSpace::Physical);
    if (!v) return out;
    out.raw_bits = v->value;
    out.is_float = true;
    out.ok = true;
    return out;
  }
  case ReadType::ASCII:
  {
    out.text.reserve(r.bytes);
    for (u32 i = 0; i < r.bytes; ++i)
    {
      auto v = mmu.HostTryRead<u8>(guard, r.addr + i, PowerPC::RequestedAddressSpace::Physical);
      if (!v) return out;
      if (v->value == 0)
        break;
      out.text.push_back(std::isprint(v->value) ? static_cast<char>(v->value) : '?');
    }
    while (!out.text.empty() && out.text.back() == ' ')
      out.text.pop_back();
    out.ok = true;
    return out;
  }
  }
  return out;
}

#ifdef USE_RETRO_ACHIEVEMENTS
ReadValue ReadAchievement(const ReadSpec& r)
{
  ReadValue out;
  auto* client = AchievementManager::GetInstance().GetClient();
  if (!client) return out;
  const rc_client_achievement_t* info = rc_client_get_achievement_info(client, r.achievement_id);
  if (!info) return out;
  const bool unlocked =
      r.achievement_mode == AchievementMode::Hardcore
          ? (info->unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE) != 0
          : (info->unlocked & (RC_CLIENT_ACHIEVEMENT_UNLOCKED_SOFTCORE |
                               RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE)) != 0;
  out.raw_bits = unlocked ? 1u : 0u;
  out.ok = true;
  return out;
}
#endif

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

  // Sign-extend for signed types when no bit-mask was applied
  if (!r.bits)
  {
    switch (r.type)
    {
    case ReadType::S8:  return static_cast<s8>(raw);
    case ReadType::S16: return static_cast<s16>(raw);
    case ReadType::S32: return static_cast<s32>(static_cast<u32>(raw));
    case ReadType::S64: return static_cast<s64>(raw);
    default: break;
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
        if (d.max) out += fmt::format("{}", static_cast<s64>(*d.max));
        else out += "?";
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
  if (d.icon_kind != IconKind::Single) return {};
  for (const auto& [k, path] : d.icon_map)
    if (k == value) return path;
  return {};
}

std::vector<std::string> ResolveIconSlots(const DisplaySpec& d, u64 raw)
{
  std::vector<std::string> out;
  if (d.icon_kind == IconKind::FillN)
  {
    if (!d.max) return out;
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

std::string ResolveJsonPath(const std::string& game_id)
{
  const std::string base = ::File::GetUserPath(D_CHEEVOMAP_IDX);
  // Preferred per-game directory layout
  const std::string dir_form = base + game_id + DIR_SEP "cheevomap.json";
  if (::File::Exists(dir_form))
    return dir_form;
  // Flat fallback
  const std::string flat = base + game_id + ".json";
  if (::File::Exists(flat))
    return flat;
  return {};
}
}  // namespace

Manager& Manager::GetInstance()
{
  static Manager s_instance;
  return s_instance;
}

void Manager::OnTitleBooted(const Core::CPUThreadGuard& guard)
{
  (void)guard;
  const std::string game_id = SConfig::GetInstance().GetGameID();
  LoadForGameId(game_id);
}

void Manager::OnESTitleChanged()
{
  const std::string game_id = SConfig::GetInstance().GetGameID();
  LoadForGameId(game_id);
}

void Manager::CloseGame()
{
  {
    std::lock_guard lg(m_lock);
    m_file.reset();
    m_live.clear();
    m_loaded_game_id.clear();
    m_last_poll = {};
  }
  m_updated_event.Trigger();
}

void Manager::LoadForGameId(const std::string& game_id)
{
  {
    std::lock_guard lg(m_lock);
    if (m_loaded_game_id == game_id && m_file.has_value())
      return;

    m_file.reset();
    m_live.clear();
    m_loaded_game_id = game_id;
    m_last_poll = {};
  }

  if (game_id.empty())
  {
    m_updated_event.Trigger();
    return;
  }

  const std::string path = ResolveJsonPath(game_id);
  if (path.empty())
  {
    INFO_LOG_FMT(CORE, "CheevoMap: no file found for game id '{}'", game_id);
    m_updated_event.Trigger();
    return;
  }

  std::string err;
  auto loaded = LoadFromFile(path, &err);
  if (!loaded)
  {
    WARN_LOG_FMT(CORE, "CheevoMap: failed to load '{}': {}", path, err);
    m_updated_event.Trigger();
    return;
  }
  if (loaded->game_id != game_id)
  {
    WARN_LOG_FMT(CORE, "CheevoMap: file '{}' game_id '{}' does not match running game '{}'",
                 path, loaded->game_id, game_id);
    m_updated_event.Trigger();
    return;
  }

  std::vector<LiveValue> live_values;
  live_values.resize(loaded->entries.size());
  for (size_t i = 0; i < loaded->entries.size(); ++i)
  {
    const auto& def = loaded->entries[i];
    live_values[i].id = def.id;
    live_values[i].label = def.label;
    live_values[i].group = def.group;
    live_values[i].value_str.clear();
    live_values[i].raw_value = 0;
    live_values[i].visible = true;
  }

  const std::string title = loaded->title;
  const size_t entry_count = loaded->entries.size();
  {
    std::lock_guard lg(m_lock);
    if (m_loaded_game_id != game_id)
      return;
    m_live = std::move(live_values);
    m_file = std::move(loaded);
  }
  INFO_LOG_FMT(CORE, "CheevoMap: loaded '{}' ({} entries) from {}", title, entry_count, path);
  m_updated_event.Trigger();
}

void Manager::DoFrame()
{
  if (!Core::IsCPUThread())
    return;

  const std::string game_id = SConfig::GetInstance().GetGameID();

  std::string loaded_id;
  {
    std::lock_guard lg(m_lock);
    loaded_id = m_loaded_game_id;
  }
  if (game_id.empty() && !loaded_id.empty())
  {
    CloseGame();
    return;
  }
  if (!game_id.empty() && game_id != loaded_id)
  {
    LoadForGameId(game_id);
  }

  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard lg(m_lock);
    if (!m_file)
      return;
    const double interval_ms = 1000.0 / m_file->poll_hz;
    if (m_last_poll != std::chrono::steady_clock::time_point{} &&
        std::chrono::duration<double, std::milli>(now - m_last_poll).count() < interval_ms)
    {
      return;
    }
    m_last_poll = now;
  }

  Core::CPUThreadGuard guard(Core::System::GetInstance());
  EvaluateLocked(&guard);
}

void Manager::EvaluateLocked(const Core::CPUThreadGuard* guard)
{
  bool any_changed = false;
  {
    std::lock_guard lg(m_lock);
    if (!m_file || !guard)
      return;

    for (size_t i = 0; i < m_file->entries.size(); ++i)
    {
      const auto& def = m_file->entries[i];
      auto& live = m_live[i];
      const std::string previous = live.value_str;
      bool previous_visible = live.visible;

      // requires gate
      if (def.requires_read)
      {
        ReadValue rv = (def.requires_read->source == Source::Ram)
                           ? ReadRam(*guard, *def.requires_read)
#ifdef USE_RETRO_ACHIEVEMENTS
                           : ReadAchievement(*def.requires_read);
#else
                           : ReadValue{};
#endif
        const bool gate = rv.ok && ApplyExpression(rv, *def.requires_read) != 0;
        live.visible = gate;
        if (!gate)
        {
          if (previous_visible != live.visible)
            any_changed = true;
          continue;
        }
      }
      else
      {
        live.visible = true;
      }

      ReadValue rv = (def.read.source == Source::Ram)
                         ? ReadRam(*guard, def.read)
#ifdef USE_RETRO_ACHIEVEMENTS
                         : ReadAchievement(def.read);
#else
                         : ReadValue{};
#endif
      if (!rv.ok)
      {
        // Keep last value rather than poisoning the entry
        if (previous_visible != live.visible)
          any_changed = true;
        continue;
      }
      const bool is_text = def.read.type == ReadType::ASCII;
      const s64 value = is_text ? 0 : ApplyExpression(rv, def.read);
      live.raw_value = value;
      live.value_str = is_text ? FormatValue(def.display, value, std::string_view(rv.text)) :
                                 FormatValue(def.display, value);
      live.icon_path = is_text ? std::string{} : ResolveSingleIcon(def.display, value);
      live.icon_slots =
          is_text ? std::vector<std::string>{} : ResolveIconSlots(def.display, rv.raw_bits);

      if (live.value_str != previous || previous_visible != live.visible)
        any_changed = true;
    }
  }

  if (any_changed)
    m_updated_event.Trigger();
}

bool Manager::IsLoaded() const
{
  std::lock_guard lg(m_lock);
  return m_file.has_value();
}

std::string Manager::GetCurrentTitle() const
{
  std::lock_guard lg(m_lock);
  return m_file ? m_file->title : std::string{};
}

std::vector<LiveValue> Manager::GetSnapshot() const
{
  std::lock_guard lg(m_lock);
  return m_live;
}

Common::EventHook Manager::RegisterUpdatedCallback(std::function<void()> cb)
{
  return m_updated_event.Register(std::move(cb));
}

}  // namespace CheevoMap
