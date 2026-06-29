// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CheevoMap/V2/PackageParser.h"

#include <charconv>
#include <limits>
#include <set>
#include <string_view>

#include <fmt/format.h>

#include "Common/JsonUtil.h"

namespace CheevoMap::V2
{
namespace
{
bool IsIntegerJsonNumber(const picojson::value& value)
{
  if (!value.is<double>())
    return false;

  const double number = value.get<double>();
  return number >= 0.0 && number <= static_cast<double>(std::numeric_limits<u64>::max()) &&
         number == static_cast<double>(static_cast<u64>(number));
}

bool CheckAllowedFields(const picojson::object& object, std::initializer_list<std::string_view> allowed,
                        std::string_view context, std::string* error)
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
    if (!IsIntegerJsonNumber(revision_it->second) ||
        revision_it->second.get<double>() > std::numeric_limits<u16>::max())
    {
      *error = "game.revision must fit in u16";
      return false;
    }
    out->revision = static_cast<u16>(revision_it->second.get<double>());
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

bool ParseRead(const picojson::value& value, ValueType type, DirectMemoryRead* out,
               std::string* error)
{
  if (!value.is<picojson::object>())
  {
    *error = "read must be an object";
    return false;
  }

  const auto& object = value.get<picojson::object>();
  if (!CheckAllowedFields(object, {"area", "address", "endian"}, "read", error))
    return false;

  const auto area = ReadStringFromJson(object, "area");
  if (!area || area->empty())
  {
    *error = "read.area must be a non-empty string";
    return false;
  }
  out->area_id = *area;

  const auto address = ReadStringFromJson(object, "address");
  if (!address || !ParseHexU64(*address, &out->address))
  {
    *error = "read.address must be a 0x-prefixed u64 hex string";
    return false;
  }

  const auto endian = ReadStringFromJson(object, "endian");
  if (ValueTypeRequiresEndian(type))
  {
    if (!endian)
    {
      *error = fmt::format("read.endian is required for {}", ValueTypeName(type));
      return false;
    }
    const std::optional<Endian> parsed = ParseEndian(*endian);
    if (!parsed)
    {
      *error = "read.endian must be \"big\" or \"little\"";
      return false;
    }
    out->endian = *parsed;
  }
  else if (endian)
  {
    *error = fmt::format("read.endian is not valid for {}", ValueTypeName(type));
    return false;
  }

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
    if (bytes_it == object.end() || !IsIntegerJsonNumber(bytes_it->second))
    {
      *error = fmt::format("value \"{}\" string requires bytes in 1..256", out->id);
      return false;
    }
    const u64 bytes = static_cast<u64>(bytes_it->second.get<double>());
    if (bytes == 0 || bytes > 256)
    {
      *error = fmt::format("value \"{}\" string requires bytes in 1..256", out->id);
      return false;
    }
    out->bytes = static_cast<u32>(bytes);
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
  if (schema_it == object.end() || !IsIntegerJsonNumber(schema_it->second) ||
      schema_it->second.get<double>() > std::numeric_limits<u32>::max())
  {
    *error_out = "missing or invalid schema_version";
    return std::nullopt;
  }

  const u32 schema = static_cast<u32>(schema_it->second.get<double>());
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
  if (package_it == object.end() || !ParseMetadata(package_it->second, &package.metadata, error_out))
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
