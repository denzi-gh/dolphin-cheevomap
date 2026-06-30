// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CheevoMap/V2/PackageParser.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <string_view>

#include <fmt/format.h>

#include "Common/JsonUtil.h"

namespace CheevoMap::V2
{
namespace
{
template <typename T>
bool ReadJsonUnsignedInteger(const picojson::value& value, T max_value, T* out)
{
  if (!value.is<double>())
    return false;

  const double number = value.get<double>();
  if (!std::isfinite(number) || number < 0.0 || number > static_cast<double>(max_value))
    return false;

  double integer_part = 0.0;
  if (std::modf(number, &integer_part) != 0.0)
    return false;

  *out = static_cast<T>(integer_part);
  return true;
}

bool CheckAllowedFields(const picojson::object& object,
                        std::initializer_list<std::string_view> allowed, std::string_view context,
                        std::string* error)
{
  for (const auto& [key, value] : object)
  {
    (void)value;
    bool found = false;
    for (const std::string_view allowed_key : allowed)
      found = found || key == allowed_key;

    if (!found)
    {
      *error = fmt::format("{}: unknown field \"{}\"", context, key);
      return false;
    }
  }
  return true;
}

bool ParseHexU64(std::string_view text, u64* out)
{
  if (text.size() < 3 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X'))
    return false;

  u64 value = 0;
  const char* first = text.data() + 2;
  const char* last = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, value, 16);
  if (ec != std::errc{} || ptr != last)
    return false;

  *out = value;
  return true;
}

bool ParseLowerHexU64(std::string_view text, u64* out)
{
  if (text.size() < 3 || text[0] != '0' || text[1] != 'x')
    return false;

  u64 value = 0;
  const char* first = text.data() + 2;
  const char* last = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, value, 16);
  if (ec != std::errc{} || ptr != last)
    return false;

  *out = value;
  return true;
}

bool ParseGame(const picojson::value& value, GameInfo* out, std::string* error)
{
  if (!value.is<picojson::object>())
  {
    *error = "game must be an object";
    return false;
  }

  const auto& object = value.get<picojson::object>();
  if (!CheckAllowedFields(object, {"id", "revision"}, "game", error))
    return false;

  const auto id = ReadStringFromJson(object, "id");
  if (!id || id->empty())
  {
    *error = "game.id must be a non-empty string";
    return false;
  }
  out->id = *id;

  if (const auto revision_it = object.find("revision"); revision_it != object.end())
  {
    u16 revision = 0;
    if (!ReadJsonU16(revision_it->second, &revision))
    {
      *error = "game.revision must fit in u16";
      return false;
    }
    out->revision = revision;
  }

  return true;
}

bool ParseMetadata(const picojson::value& value, PackageMetadata* out, std::string* error)
{
  if (!value.is<picojson::object>())
  {
    *error = "package must be an object";
    return false;
  }

  const auto& object = value.get<picojson::object>();
  if (!CheckAllowedFields(object, {"title"}, "package", error))
    return false;

  const auto title = ReadStringFromJson(object, "title");
  if (!title || title->empty())
  {
    *error = "package.title must be a non-empty string";
    return false;
  }
  out->title = *title;
  return true;
}

bool ParseFinalEndian(const picojson::object& object, ValueType type, Endian* out,
                      std::string* error)
{
  const auto endian_it = object.find("endian");
  if (ValueTypeRequiresEndian(type))
  {
    if (endian_it == object.end())
    {
      *error = fmt::format("read.endian is required for {}", ValueTypeName(type));
      return false;
    }
    if (!endian_it->second.is<std::string>())
    {
      *error = "read.endian must be \"big\" or \"little\"";
      return false;
    }

    const std::optional<Endian> parsed = ParseEndian(endian_it->second.get<std::string>());
    if (!parsed)
    {
      *error = "read.endian must be \"big\" or \"little\"";
      return false;
    }
    *out = *parsed;
  }
  else if (endian_it != object.end())
  {
    *error = fmt::format("read.endian is not valid for {}", ValueTypeName(type));
    return false;
  }

  return true;
}

bool ParsePointerChainBase(const picojson::value& value, PointerChainBase* out, std::string* error)
{
  if (!value.is<picojson::object>())
  {
    *error = "read.pointer_chain.base must be an object";
    return false;
  }

  const auto& object = value.get<picojson::object>();
  if (!CheckAllowedFields(object, {"area", "address"}, "read.pointer_chain.base", error))
    return false;

  const auto area = ReadStringFromJson(object, "area");
  if (!area || area->empty())
  {
    *error = "read.pointer_chain.base.area must be a non-empty string";
    return false;
  }
  out->area_id = *area;

  const auto address = ReadStringFromJson(object, "address");
  if (!address || !ParseLowerHexU64(*address, &out->address))
  {
    *error = "read.pointer_chain.base.address must be a 0x-prefixed u64 hex string";
    return false;
  }

  return true;
}

bool ParsePointerChain(const picojson::value& value, PointerChainRead* out, std::string* error)
{
  if (!value.is<picojson::object>())
  {
    *error = "read.pointer_chain must be an object";
    return false;
  }

  const auto& object = value.get<picojson::object>();
  if (!CheckAllowedFields(object, {"base", "target_area", "offsets", "pointer_type", "endian"},
                          "read.pointer_chain", error))
  {
    return false;
  }

  const auto base_it = object.find("base");
  if (base_it == object.end())
  {
    *error = "read.pointer_chain.base is required";
    return false;
  }
  if (!ParsePointerChainBase(base_it->second, &out->base, error))
    return false;

  const auto target_area_it = object.find("target_area");
  if (target_area_it == object.end())
  {
    *error = "read.pointer_chain.target_area must be a non-empty string";
    return false;
  }
  if (!target_area_it->second.is<std::string>())
  {
    *error = "read.pointer_chain.target_area must be a string";
    return false;
  }
  out->target_area_id = target_area_it->second.get<std::string>();
  if (out->target_area_id.empty())
  {
    *error = "read.pointer_chain.target_area must be a non-empty string";
    return false;
  }

  const auto offsets_it = object.find("offsets");
  if (offsets_it == object.end())
  {
    *error = "read.pointer_chain.offsets is required";
    return false;
  }
  if (!offsets_it->second.is<picojson::array>())
  {
    *error = "read.pointer_chain.offsets must be an array";
    return false;
  }

  const auto& offsets = offsets_it->second.get<picojson::array>();
  if (offsets.empty())
  {
    *error = "read.pointer_chain.offsets must contain at least 1 offset";
    return false;
  }
  if (offsets.size() > 16)
  {
    *error = "read.pointer_chain.offsets must contain at most 16 offsets";
    return false;
  }

  out->offsets.clear();
  out->offsets.reserve(offsets.size());
  for (size_t i = 0; i < offsets.size(); ++i)
  {
    if (!offsets[i].is<std::string>())
    {
      *error = fmt::format("read.pointer_chain.offsets[{}] must be a 0x-prefixed u64 hex string",
                           i);
      return false;
    }

    u64 offset = 0;
    if (!ParseLowerHexU64(offsets[i].get<std::string>(), &offset))
    {
      *error = fmt::format("read.pointer_chain.offsets[{}] must be a 0x-prefixed u64 hex string",
                           i);
      return false;
    }
    out->offsets.push_back(offset);
  }

  const auto pointer_type_it = object.find("pointer_type");
  if (pointer_type_it == object.end())
  {
    *error = "read.pointer_chain.pointer_type is required";
    return false;
  }
  if (!pointer_type_it->second.is<std::string>() ||
      pointer_type_it->second.get<std::string>() != "u32")
  {
    *error = "read.pointer_chain.pointer_type must be \"u32\"";
    return false;
  }
  out->pointer_type = PointerType::U32;

  const auto endian_it = object.find("endian");
  if (endian_it == object.end())
  {
    *error = "read.pointer_chain.endian is required";
    return false;
  }
  if (!endian_it->second.is<std::string>())
  {
    *error = "read.pointer_chain.endian must be a string";
    return false;
  }

  const std::optional<Endian> pointer_endian = ParseEndian(endian_it->second.get<std::string>());
  if (!pointer_endian)
  {
    *error = "read.pointer_chain.endian must be \"big\" or \"little\"";
    return false;
  }
  out->pointer_endian = *pointer_endian;

  return true;
}

bool ParseRead(const picojson::value& value, ValueType type, MemoryReadDefinition* out,
               std::string* error)
{
  if (!value.is<picojson::object>())
  {
    *error = "read must be an object";
    return false;
  }

  const auto& object = value.get<picojson::object>();
  if (!CheckAllowedFields(object, {"area", "address", "endian", "pointer_chain"}, "read", error))
    return false;

  const bool has_pointer_chain = object.contains("pointer_chain");
  const bool has_direct_area = object.contains("area");
  const bool has_direct_address = object.contains("address");
  if (has_pointer_chain && (has_direct_area || has_direct_address))
  {
    *error = "read must not mix direct area/address fields with pointer_chain";
    return false;
  }
  if (!has_pointer_chain && !has_direct_area && !has_direct_address)
  {
    *error = "read must contain either direct area/address fields or pointer_chain";
    return false;
  }

  Endian final_endian = Endian::None;
  if (!ParseFinalEndian(object, type, &final_endian, error))
    return false;

  if (has_pointer_chain)
  {
    PointerChainRead pointer_chain;
    if (!ParsePointerChain(object.at("pointer_chain"), &pointer_chain, error))
      return false;
    pointer_chain.endian = final_endian;
    *out = std::move(pointer_chain);
    return true;
  }

  DirectMemoryRead direct;
  const auto area = ReadStringFromJson(object, "area");
  if (!area || area->empty())
  {
    *error = "read.area must be a non-empty string";
    return false;
  }
  direct.area_id = *area;

  const auto address = ReadStringFromJson(object, "address");
  if (!address || !ParseHexU64(*address, &direct.address))
  {
    *error = "read.address must be a 0x-prefixed u64 hex string";
    return false;
  }

  direct.endian = final_endian;
  *out = std::move(direct);
  return true;
}

bool ParseValue(const picojson::value& value, size_t index, ValueDefinition* out,
                std::string* error)
{
  if (!value.is<picojson::object>())
  {
    *error = fmt::format("values[{}] must be an object", index);
    return false;
  }

  const auto& object = value.get<picojson::object>();
  const std::string context = fmt::format("values[{}]", index);
  if (!CheckAllowedFields(object, {"id", "type", "bytes", "read"}, context, error))
    return false;

  const auto id = ReadStringFromJson(object, "id");
  if (!id || id->empty())
  {
    *error = fmt::format("values[{}].id must be a non-empty string", index);
    return false;
  }
  out->id = *id;

  const auto type_text = ReadStringFromJson(object, "type");
  if (!type_text)
  {
    *error = fmt::format("value \"{}\" requires type", out->id);
    return false;
  }

  const std::optional<ValueType> type = ParseValueType(*type_text);
  if (!type)
  {
    *error = fmt::format("value \"{}\" has unsupported type \"{}\"", out->id, *type_text);
    return false;
  }
  out->type = *type;

  if (out->type == ValueType::String)
  {
    const auto bytes_it = object.find("bytes");
    u32 bytes = 0;
    if (bytes_it == object.end() || !ReadJsonUnsignedInteger<u32>(bytes_it->second, 256, &bytes))
    {
      *error = fmt::format("value \"{}\" string requires bytes in 1..256", out->id);
      return false;
    }
    if (bytes == 0)
    {
      *error = fmt::format("value \"{}\" string requires bytes in 1..256", out->id);
      return false;
    }
    out->bytes = bytes;
  }
  else if (object.contains("bytes"))
  {
    *error = fmt::format("value \"{}\" bytes is only valid for string", out->id);
    return false;
  }

  const auto read_it = object.find("read");
  if (read_it == object.end())
  {
    *error = fmt::format("value \"{}\" requires read", out->id);
    return false;
  }
  if (!ParseRead(read_it->second, out->type, &out->read, error))
  {
    *error = fmt::format("value \"{}\".{}", out->id, *error);
    return false;
  }

  return true;
}
}  // namespace

bool ReadJsonU32(const picojson::value& value, u32* out)
{
  return ReadJsonUnsignedInteger<u32>(value, std::numeric_limits<u32>::max(), out);
}

bool ReadJsonU16(const picojson::value& value, u16* out)
{
  return ReadJsonUnsignedInteger<u16>(value, std::numeric_limits<u16>::max(), out);
}

std::optional<Package> ParsePackage(const picojson::value& root, std::string* error_out)
{
  if (!root.is<picojson::object>())
  {
    *error_out = "top-level JSON must be an object";
    return std::nullopt;
  }

  const auto& object = root.get<picojson::object>();
  if (!CheckAllowedFields(object, {"schema_version", "game", "package", "poll_hz", "values"},
                          "root", error_out))
  {
    return std::nullopt;
  }

  const auto schema_it = object.find("schema_version");
  u32 schema = 0;
  if (schema_it == object.end() || !ReadJsonU32(schema_it->second, &schema))
  {
    *error_out = "missing or invalid schema_version";
    return std::nullopt;
  }

  if (schema != 2)
  {
    *error_out = fmt::format("unsupported schema_version {} (only 2 is known)", schema);
    return std::nullopt;
  }

  Package package;
  package.schema_version = schema;

  const auto game_it = object.find("game");
  if (game_it == object.end() || !ParseGame(game_it->second, &package.game, error_out))
    return std::nullopt;

  const auto package_it = object.find("package");
  if (package_it == object.end() ||
      !ParseMetadata(package_it->second, &package.metadata, error_out))
    return std::nullopt;

  if (const auto hz = ReadNumericFromJson<double>(object, "poll_hz"))
  {
    if (*hz <= 0.0 || *hz > 240.0)
    {
      *error_out = fmt::format("poll_hz {} out of range (0, 240]", *hz);
      return std::nullopt;
    }
    package.poll_hz = *hz;
  }

  const auto values_it = object.find("values");
  if (values_it == object.end() || !values_it->second.is<picojson::array>())
  {
    *error_out = "missing or invalid values array";
    return std::nullopt;
  }

  const auto& values = values_it->second.get<picojson::array>();
  package.values.reserve(values.size());
  std::set<std::string> ids;
  for (size_t i = 0; i < values.size(); ++i)
  {
    ValueDefinition value;
    if (!ParseValue(values[i], i, &value, error_out))
      return std::nullopt;

    if (!ids.insert(value.id).second)
    {
      *error_out = fmt::format("duplicate value id \"{}\"", value.id);
      return std::nullopt;
    }
    package.values.push_back(std::move(value));
  }

  return package;
}

std::optional<Package> LoadPackageFromFile(const std::string& json_path, std::string* error_out)
{
  picojson::value root;
  std::string parse_error;
  if (!JsonFromFile(json_path, &root, &parse_error))
  {
    *error_out = fmt::format("could not read JSON: {}", parse_error);
    return std::nullopt;
  }

  return ParsePackage(root, error_out);
}
}  // namespace CheevoMap::V2
