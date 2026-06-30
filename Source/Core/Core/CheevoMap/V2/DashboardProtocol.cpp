// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CheevoMap/V2/DashboardProtocol.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <utility>

#include <picojson.h>

namespace CheevoMap::V2
{
namespace
{
picojson::value StringValue(u64 value)
{
  return picojson::value(std::to_string(value));
}

picojson::value StringValue(s64 value)
{
  return picojson::value(std::to_string(value));
}

picojson::value StateValueToJson(const StateValue& value)
{
  picojson::object object;
  const auto set_unavailable = [&object] {
    object["kind"] = picojson::value("unavailable");
    object["value"] = picojson::value();
  };

  switch (value.GetType())
  {
  case StateValueType::Boolean:
    object["kind"] = picojson::value("bool");
    object["value"] = picojson::value(value.AsBoolean().value_or(false));
    break;
  case StateValueType::SignedInteger:
    object["kind"] = picojson::value("s64");
    object["value"] = StringValue(value.AsSignedInteger().value_or(0));
    break;
  case StateValueType::UnsignedInteger:
    object["kind"] = picojson::value("u64");
    object["value"] = StringValue(value.AsUnsignedInteger().value_or(0));
    break;
  case StateValueType::FloatingPoint:
  {
    const double number = value.AsFloatingPoint().value_or(0.0);
    if (std::isfinite(number))
    {
      object["kind"] = picojson::value("f64");
      object["value"] = picojson::value(number);
    }
    else
    {
      set_unavailable();
    }
    break;
  }
  case StateValueType::String:
  {
    object["kind"] = picojson::value("string");
    const std::string* string = value.AsString();
    object["value"] = picojson::value(string ? *string : std::string{});
    break;
  }
  case StateValueType::Unavailable:
    set_unavailable();
    break;
  }

  return picojson::value(std::move(object));
}

picojson::value StateValueMapToJson(const StateValueMap& values)
{
  picojson::object object;
  for (const auto& [id, value] : values)
    object[id] = StateValueToJson(value);
  return picojson::value(std::move(object));
}

picojson::value RemovedIdsToJson(const std::vector<std::string>& removed)
{
  picojson::array array;
  array.reserve(removed.size());
  for (const std::string& id : removed)
    array.emplace_back(id);
  return picojson::value(std::move(array));
}

std::string Serialize(const picojson::object& object)
{
  return picojson::value(object).serialize(false);
}
}  // namespace

std::string SerializeStateValue(const StateValue& value)
{
  return StateValueToJson(value).serialize(false);
}

StateCursor CursorForSnapshot(const StateSnapshot& snapshot)
{
  return StateCursor{snapshot.session_id, snapshot.sequence};
}

StateCursor CursorForUpdate(const StateUpdate& update)
{
  return StateCursor{update.session_id, update.sequence};
}

bool IsCursorNewer(const StateCursor candidate, const StateCursor current)
{
  return candidate.session_id > current.session_id ||
         (candidate.session_id == current.session_id && candidate.sequence > current.sequence);
}

StateUpdateApplyResult ApplyStateUpdateToSnapshot(StateSnapshot* snapshot,
                                                  const StateUpdate& update)
{
  if (update.session_id < snapshot->session_id)
    return StateUpdateApplyResult::StaleOrDuplicate;

  if (update.session_id == snapshot->session_id && update.sequence <= snapshot->sequence)
    return StateUpdateApplyResult::StaleOrDuplicate;

  if (update.session_id > snapshot->session_id && !update.full)
    return StateUpdateApplyResult::InvalidSessionTransition;

  snapshot->session_id = update.session_id;
  snapshot->sequence = update.sequence;

  if (update.full)
  {
    snapshot->values = update.values;
    return StateUpdateApplyResult::Applied;
  }

  for (const std::string& id : update.removed)
    snapshot->values.erase(id);
  for (const auto& [id, value] : update.values)
    snapshot->values[id] = value;

  return StateUpdateApplyResult::Applied;
}

bool IsUnsignedDecimalString(const std::string_view value)
{
  return !value.empty() &&
         std::ranges::all_of(value, [](const unsigned char c) { return std::isdigit(c) != 0; });
}

int CompareUnsignedDecimalStrings(std::string_view left, std::string_view right)
{
  if (!IsUnsignedDecimalString(left) || !IsUnsignedDecimalString(right))
    return 0;

  while (left.size() > 1 && left.front() == '0')
    left.remove_prefix(1);
  while (right.size() > 1 && right.front() == '0')
    right.remove_prefix(1);

  if (left.size() < right.size())
    return -1;
  if (left.size() > right.size())
    return 1;
  if (left < right)
    return -1;
  if (left > right)
    return 1;
  return 0;
}

std::string SerializeStateSnapshot(const StateSnapshot& snapshot)
{
  picojson::object object;
  object["protocol_version"] = picojson::value(static_cast<double>(kDashboardProtocolVersion));
  object["message_type"] = picojson::value("snapshot");
  object["session_id"] = StringValue(snapshot.session_id);
  object["sequence"] = StringValue(snapshot.sequence);
  object["values"] = StateValueMapToJson(snapshot.values);
  return Serialize(object);
}

std::string SerializeStateUpdate(const StateUpdate& update)
{
  picojson::object object;
  object["protocol_version"] = picojson::value(static_cast<double>(kDashboardProtocolVersion));
  object["message_type"] = picojson::value("update");
  object["session_id"] = StringValue(update.session_id);
  object["sequence"] = StringValue(update.sequence);
  object["full"] = picojson::value(update.full);
  object["values"] = StateValueMapToJson(update.values);
  object["removed"] = RemovedIdsToJson(update.removed);
  return Serialize(object);
}

std::string SerializeDashboardHealth()
{
  picojson::object object;
  object["protocol_version"] = picojson::value(static_cast<double>(kDashboardProtocolVersion));
  object["service"] = picojson::value("cheevomap-v2-local-dashboard");
  object["status"] = picojson::value("ok");
  return Serialize(object);
}
}  // namespace CheevoMap::V2
