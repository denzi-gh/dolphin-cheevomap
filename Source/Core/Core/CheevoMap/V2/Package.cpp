// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CheevoMap/V2/Package.h"

#include <string_view>

namespace CheevoMap::V2
{
std::optional<ValueType> ParseValueType(std::string_view text)
{
  if (text == "bool")
    return ValueType::Boolean;
  if (text == "u8")
    return ValueType::U8;
  if (text == "u16")
    return ValueType::U16;
  if (text == "u32")
    return ValueType::U32;
  if (text == "u64")
    return ValueType::U64;
  if (text == "s8")
    return ValueType::S8;
  if (text == "s16")
    return ValueType::S16;
  if (text == "s32")
    return ValueType::S32;
  if (text == "s64")
    return ValueType::S64;
  if (text == "f32")
    return ValueType::F32;
  if (text == "f64")
    return ValueType::F64;
  if (text == "string")
    return ValueType::String;
  return std::nullopt;
}

std::optional<Endian> ParseEndian(std::string_view text)
{
  if (text == "big")
    return Endian::Big;
  if (text == "little")
    return Endian::Little;
  return std::nullopt;
}

const char* ValueTypeName(ValueType type)
{
  switch (type)
  {
  case ValueType::Boolean:
    return "bool";
  case ValueType::U8:
    return "u8";
  case ValueType::U16:
    return "u16";
  case ValueType::U32:
    return "u32";
  case ValueType::U64:
    return "u64";
  case ValueType::S8:
    return "s8";
  case ValueType::S16:
    return "s16";
  case ValueType::S32:
    return "s32";
  case ValueType::S64:
    return "s64";
  case ValueType::F32:
    return "f32";
  case ValueType::F64:
    return "f64";
  case ValueType::String:
    return "string";
  }
  return "unknown";
}

const char* PointerTypeName(PointerType type)
{
  switch (type)
  {
  case PointerType::U32:
    return "u32";
  }
  return "unknown";
}

u32 GetPointerReadSize(PointerType type)
{
  switch (type)
  {
  case PointerType::U32:
    return 4;
  }
  return 0;
}

u32 GetValueReadSize(const ValueDefinition& value)
{
  switch (value.type)
  {
  case ValueType::Boolean:
  case ValueType::U8:
  case ValueType::S8:
    return 1;
  case ValueType::U16:
  case ValueType::S16:
    return 2;
  case ValueType::U32:
  case ValueType::S32:
  case ValueType::F32:
    return 4;
  case ValueType::U64:
  case ValueType::S64:
  case ValueType::F64:
    return 8;
  case ValueType::String:
    return value.bytes;
  }
  return 0;
}

bool ValueTypeRequiresEndian(ValueType type)
{
  switch (type)
  {
  case ValueType::U16:
  case ValueType::U32:
  case ValueType::U64:
  case ValueType::S16:
  case ValueType::S32:
  case ValueType::S64:
  case ValueType::F32:
  case ValueType::F64:
    return true;
  default:
    return false;
  }
}

bool ValueTypeAllowsEndian(ValueType type)
{
  return ValueTypeRequiresEndian(type);
}

bool IsSignedIntegerType(ValueType type)
{
  switch (type)
  {
  case ValueType::S8:
  case ValueType::S16:
  case ValueType::S32:
  case ValueType::S64:
    return true;
  default:
    return false;
  }
}

bool IsUnsignedIntegerType(ValueType type)
{
  switch (type)
  {
  case ValueType::U8:
  case ValueType::U16:
  case ValueType::U32:
  case ValueType::U64:
    return true;
  default:
    return false;
  }
}

bool IsFloatingPointType(ValueType type)
{
  return type == ValueType::F32 || type == ValueType::F64;
}
}  // namespace CheevoMap::V2
