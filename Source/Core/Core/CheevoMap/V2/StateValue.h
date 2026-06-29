// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <variant>

#include "Common/CommonTypes.h"

namespace CheevoMap::V2
{
enum class StateValueType : u8
{
  Unavailable,
  Boolean,
  SignedInteger,
  UnsignedInteger,
  FloatingPoint,
  String,
};

class StateValue final
{
public:
  StateValue();

  static StateValue Unavailable();
  static StateValue Boolean(bool value);
  static StateValue SignedInteger(s64 value);
  static StateValue UnsignedInteger(u64 value);
  static StateValue FloatingPoint(double value);
  static StateValue String(std::string value);

  StateValueType GetType() const;
  bool IsAvailable() const;

  std::optional<bool> AsBoolean() const;
  std::optional<s64> AsSignedInteger() const;
  std::optional<u64> AsUnsignedInteger() const;
  std::optional<double> AsFloatingPoint() const;
  const std::string* AsString() const;

  bool operator==(const StateValue& rhs) const = default;
  bool operator!=(const StateValue& rhs) const = default;

private:
  using Storage = std::variant<std::monostate, bool, s64, u64, double, std::string>;

  explicit StateValue(Storage value);

  Storage m_value;
};
}  // namespace CheevoMap::V2
