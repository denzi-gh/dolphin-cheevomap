// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CheevoMap/V2/StateValue.h"

#include <utility>

namespace CheevoMap::V2
{
StateValue::StateValue() = default;

StateValue::StateValue(Storage value) : m_value(std::move(value))
{
}

StateValue StateValue::Unavailable()
{
  return StateValue{};
}

StateValue StateValue::Boolean(bool value)
{
  return StateValue{value};
}

StateValue StateValue::SignedInteger(s64 value)
{
  return StateValue{value};
}

StateValue StateValue::UnsignedInteger(u64 value)
{
  return StateValue{value};
}

StateValue StateValue::FloatingPoint(double value)
{
  return StateValue{value};
}

StateValue StateValue::String(std::string value)
{
  return StateValue{std::move(value)};
}

StateValueType StateValue::GetType() const
{
  switch (m_value.index())
  {
  case 1:
    return StateValueType::Boolean;
  case 2:
    return StateValueType::SignedInteger;
  case 3:
    return StateValueType::UnsignedInteger;
  case 4:
    return StateValueType::FloatingPoint;
  case 5:
    return StateValueType::String;
  default:
    return StateValueType::Unavailable;
  }
}

bool StateValue::IsAvailable() const
{
  return GetType() != StateValueType::Unavailable;
}

std::optional<bool> StateValue::AsBoolean() const
{
  if (const auto* value = std::get_if<bool>(&m_value))
    return *value;
  return std::nullopt;
}

std::optional<s64> StateValue::AsSignedInteger() const
{
  if (const auto* value = std::get_if<s64>(&m_value))
    return *value;
  return std::nullopt;
}

std::optional<u64> StateValue::AsUnsignedInteger() const
{
  if (const auto* value = std::get_if<u64>(&m_value))
    return *value;
  return std::nullopt;
}

std::optional<double> StateValue::AsFloatingPoint() const
{
  if (const auto* value = std::get_if<double>(&m_value))
    return *value;
  return std::nullopt;
}

const std::string* StateValue::AsString() const
{
  return std::get_if<std::string>(&m_value);
}
}  // namespace CheevoMap::V2
