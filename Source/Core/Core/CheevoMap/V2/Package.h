// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace CheevoMap::V2
{
enum class ValueType : u8
{
  Boolean,
  U8,
  U16,
  U32,
  U64,
  S8,
  S16,
  S32,
  S64,
  F32,
  F64,
  String,
};

enum class Endian : u8
{
  None,
  Big,
  Little,
};

struct DirectMemoryRead
{
  std::string area_id;
  u64 address = 0;
  Endian endian = Endian::None;
};

struct ValueDefinition
{
  std::string id;
  ValueType type = ValueType::U8;
  u32 bytes = 0;
  DirectMemoryRead read;
};

struct GameInfo
{
  std::string id;
  std::optional<u16> revision;
};

struct PackageMetadata
{
  std::string title;
};

struct Package
{
  u32 schema_version = 2;
  GameInfo game;
  PackageMetadata metadata;
  double poll_hz = 10.0;
  std::vector<ValueDefinition> values;
};

std::optional<ValueType> ParseValueType(std::string_view text);
std::optional<Endian> ParseEndian(std::string_view text);
const char* ValueTypeName(ValueType type);
u32 GetValueReadSize(const ValueDefinition& value);
bool ValueTypeRequiresEndian(ValueType type);
bool ValueTypeAllowsEndian(ValueType type);
bool IsSignedIntegerType(ValueType type);
bool IsUnsignedIntegerType(ValueType type);
bool IsFloatingPointType(ValueType type);
}  // namespace CheevoMap::V2
