// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>
#include <map>
#include <string>

#include <gtest/gtest.h>
#include <picojson.h>

#include "Core/CheevoMap/V2/DashboardProtocol.h"

namespace
{
using CheevoMap::V2::SerializeStateSnapshot;
using CheevoMap::V2::SerializeStateUpdate;
using CheevoMap::V2::StateSnapshot;
using CheevoMap::V2::StateUpdate;
using CheevoMap::V2::StateValue;
using CheevoMap::V2::StateValueMap;

picojson::value ParseJson(const std::string& json)
{
  picojson::value value;
  const std::string error = picojson::parse(value, json);
  EXPECT_TRUE(error.empty()) << error;
  return value;
}

const picojson::value& ValueForId(const picojson::value& root, const std::string& id)
{
  return root.get("values").get(id);
}

class ProtocolStateMirror
{
public:
  void ApplySnapshot(const StateSnapshot& snapshot)
  {
    session_id = snapshot.session_id;
    sequence = snapshot.sequence;
    values = snapshot.values;
  }

  void ApplyUpdate(const StateUpdate& update)
  {
    session_id = update.session_id;
    sequence = update.sequence;
    if (update.full)
    {
      values = update.values;
      return;
    }

    for (const std::string& id : update.removed)
      values.erase(id);
    for (const auto& [id, value] : update.values)
      values[id] = value;
  }

  u64 session_id = 0;
  u64 sequence = 0;
  StateValueMap values;
};
}  // namespace

TEST(CheevoMapV2DashboardProtocol, SerializesEmptySnapshot)
{
  const picojson::value root = ParseJson(SerializeStateSnapshot(StateSnapshot{}));

  EXPECT_EQ(root.get("protocol_version").get<double>(), 1.0);
  EXPECT_EQ(root.get("message_type").get<std::string>(), "snapshot");
  EXPECT_EQ(root.get("session_id").get<std::string>(), "0");
  EXPECT_EQ(root.get("sequence").get<std::string>(), "0");
  EXPECT_TRUE(root.get("values").get<picojson::object>().empty());
}

TEST(CheevoMapV2DashboardProtocol, SerializesSnapshotMetadataAsStrings)
{
  StateSnapshot snapshot;
  snapshot.session_id = std::numeric_limits<u64>::max();
  snapshot.sequence = 42;

  const picojson::value root = ParseJson(SerializeStateSnapshot(snapshot));

  EXPECT_EQ(root.get("session_id").get<std::string>(), "18446744073709551615");
  EXPECT_EQ(root.get("sequence").get<std::string>(), "42");
}

TEST(CheevoMapV2DashboardProtocol, SerializesAllValueKinds)
{
  StateSnapshot snapshot;
  snapshot.values = {
      {"bool_false", StateValue::Boolean(false)},
      {"bool_true", StateValue::Boolean(true)},
      {"f64", StateValue::FloatingPoint(1.5)},
      {"s64_max", StateValue::SignedInteger(std::numeric_limits<s64>::max())},
      {"s64_min", StateValue::SignedInteger(std::numeric_limits<s64>::min())},
      {"string", StateValue::String("quote\" slash\\ control\n unicode \xe2\x98\x83")},
      {"u64_max", StateValue::UnsignedInteger(std::numeric_limits<u64>::max())},
      {"u64_zero", StateValue::UnsignedInteger(0)},
      {"unavailable", StateValue::Unavailable()},
  };

  const picojson::value root = ParseJson(SerializeStateSnapshot(snapshot));

  EXPECT_EQ(ValueForId(root, "bool_true").get("kind").get<std::string>(), "bool");
  EXPECT_TRUE(ValueForId(root, "bool_true").get("value").get<bool>());
  EXPECT_FALSE(ValueForId(root, "bool_false").get("value").get<bool>());
  EXPECT_EQ(ValueForId(root, "s64_min").get("value").get<std::string>(), "-9223372036854775808");
  EXPECT_EQ(ValueForId(root, "s64_max").get("value").get<std::string>(), "9223372036854775807");
  EXPECT_EQ(ValueForId(root, "u64_zero").get("value").get<std::string>(), "0");
  EXPECT_EQ(ValueForId(root, "u64_max").get("value").get<std::string>(), "18446744073709551615");
  EXPECT_EQ(ValueForId(root, "f64").get("value").get<double>(), 1.5);
  EXPECT_EQ(ValueForId(root, "string").get("value").get<std::string>(),
            "quote\" slash\\ control\n unicode \xe2\x98\x83");
  EXPECT_EQ(ValueForId(root, "unavailable").get("kind").get<std::string>(), "unavailable");
  EXPECT_TRUE(ValueForId(root, "unavailable").get("value").is<picojson::null>());
}

TEST(CheevoMapV2DashboardProtocol, SerializesValuesInDeterministicLexicalOrder)
{
  StateSnapshot snapshot;
  snapshot.values = {{"z", StateValue::Boolean(true)}, {"a", StateValue::Boolean(false)}};

  const std::string first = SerializeStateSnapshot(snapshot);
  const std::string second = SerializeStateSnapshot(snapshot);

  EXPECT_EQ(first, second);
  EXPECT_LT(first.find("\"a\""), first.find("\"z\""));
}

TEST(CheevoMapV2DashboardProtocol, SerializesFullAndDeltaUpdates)
{
  StateUpdate full;
  full.session_id = 7;
  full.sequence = 8;
  full.full = true;
  full.values = {{"a", StateValue::UnsignedInteger(1)}};

  picojson::value root = ParseJson(SerializeStateUpdate(full));
  EXPECT_EQ(root.get("message_type").get<std::string>(), "update");
  EXPECT_EQ(root.get("session_id").get<std::string>(), "7");
  EXPECT_EQ(root.get("sequence").get<std::string>(), "8");
  EXPECT_TRUE(root.get("full").get<bool>());
  EXPECT_TRUE(root.get("removed").get<picojson::array>().empty());

  StateUpdate delta;
  delta.session_id = 7;
  delta.sequence = 9;
  delta.full = false;
  delta.values = {{"b", StateValue::Unavailable()}};
  delta.removed = {"a", "c"};

  root = ParseJson(SerializeStateUpdate(delta));
  EXPECT_FALSE(root.get("full").get<bool>());
  EXPECT_EQ(root.get("removed").get<picojson::array>()[0].get<std::string>(), "a");
  EXPECT_EQ(root.get("removed").get<picojson::array>()[1].get<std::string>(), "c");
  EXPECT_EQ(ValueForId(root, "b").get("kind").get<std::string>(), "unavailable");
}

TEST(CheevoMapV2DashboardProtocol, SerializationIsCompactAndPreservesIntegerPrecision)
{
  StateSnapshot snapshot;
  snapshot.values = {{"large", StateValue::UnsignedInteger(18446744073709551615ull)}};

  const std::string json = SerializeStateSnapshot(snapshot);

  EXPECT_EQ(json.find('\n'), std::string::npos);
  EXPECT_EQ(json.find('\r'), std::string::npos);
  EXPECT_NE(json.find("\"18446744073709551615\""), std::string::npos);
}

TEST(CheevoMapV2DashboardProtocol, NonFiniteFloatingPointDoesNotWriteNanOrInfinity)
{
  StateSnapshot snapshot;
  snapshot.values = {{"bad", StateValue::FloatingPoint(std::numeric_limits<double>::infinity())}};

  const std::string json = SerializeStateSnapshot(snapshot);

  EXPECT_EQ(json.find("Infinity"), std::string::npos);
  EXPECT_EQ(json.find("NaN"), std::string::npos);
  EXPECT_EQ(ValueForId(ParseJson(json), "bad").get("kind").get<std::string>(), "unavailable");
}

TEST(CheevoMapV2DashboardStateMirror, AppliesSnapshotFullDeltaRemovalAndSessionChange)
{
  ProtocolStateMirror mirror;

  mirror.ApplySnapshot(StateSnapshot{1, 1, {{"a", StateValue::Boolean(true)}}});
  ASSERT_EQ(mirror.values.size(), 1u);
  EXPECT_TRUE(mirror.values.contains("a"));

  mirror.ApplyUpdate(StateUpdate{1, 2, false, {{"b", StateValue::UnsignedInteger(4)}}, {}});
  EXPECT_TRUE(mirror.values.contains("a"));
  EXPECT_TRUE(mirror.values.contains("b"));

  mirror.ApplyUpdate(StateUpdate{1, 3, false, {{"c", StateValue::Unavailable()}}, {"a"}});
  EXPECT_FALSE(mirror.values.contains("a"));
  EXPECT_TRUE(mirror.values.contains("b"));
  EXPECT_TRUE(mirror.values.contains("c"));
  EXPECT_FALSE(mirror.values.at("c").IsAvailable());

  mirror.ApplyUpdate(StateUpdate{2, 1, true, {{"fresh", StateValue::String("yes")}}, {}});
  EXPECT_EQ(mirror.session_id, 2u);
  EXPECT_EQ(mirror.sequence, 1u);
  EXPECT_EQ(mirror.values.size(), 1u);
  EXPECT_TRUE(mirror.values.contains("fresh"));
}
