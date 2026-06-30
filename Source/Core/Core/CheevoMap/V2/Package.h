// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <variant>
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

enum class PointerType : u8
{
  U32,
};

struct DirectMemoryRead
{
  std::string area_id;
  u64 address = 0;
  Endian endian = Endian::None;
};

struct PointerChainBase
{
  std::string area_id;
  u64 address = 0;
};

struct PointerChainRead
{
  PointerChainBase base;
  std::vector<std::string> target_area_ids;
  std::vector<u64> offsets;
  PointerType pointer_type = PointerType::U32;
  Endian pointer_endian = Endian::None;
  Endian endian = Endian::None;
};

using MemoryReadDefinition = std::variant<DirectMemoryRead, PointerChainRead>;

enum class ExpressionOperator : u8
{
  Equal,
  NotEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  Not,
  And,
  Or,
  Add,
  Subtract,
  Multiply,
  Divide,
  Modulo,
  BitAnd,
  BitOr,
  BitXor,
  BitNot,
  ToF64,
  If,
};

enum class ExpressionConstantType : u8
{
  Boolean,
  SignedInteger,
  UnsignedInteger,
  FloatingPoint,
  String,
};

struct ExpressionReference
{
  std::string value_id;
};

struct ExpressionConstant
{
  ExpressionConstantType type = ExpressionConstantType::UnsignedInteger;
  std::variant<bool, s64, u64, double, std::string> value = u64{};
};

struct ExpressionNode;

struct ExpressionOperation
{
  ExpressionOperator op = ExpressionOperator::Equal;
  std::vector<ExpressionNode> arguments;
};

struct ExpressionNode
{
  std::variant<ExpressionReference, ExpressionConstant, ExpressionOperation> node;
};

struct ReadValueSource
{
  MemoryReadDefinition read;
};

struct ExpressionValueSource
{
  ExpressionNode expression;
};

using ValueSourceDefinition = std::variant<ReadValueSource, ExpressionValueSource>;

struct ValueDefinition
{
  std::string id;
  ValueType type = ValueType::U8;
  u32 bytes = 0;
  ValueSourceDefinition source;
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
const char* PointerTypeName(PointerType type);
u32 GetPointerReadSize(PointerType type);
u32 GetValueReadSize(const ValueDefinition& value);
bool IsReadBackedValue(const ValueDefinition& value);
bool IsExpressionBackedValue(const ValueDefinition& value);
const MemoryReadDefinition* GetReadDefinition(const ValueDefinition& value);
MemoryReadDefinition* GetMutableReadDefinition(ValueDefinition& value);
const ExpressionNode* GetExpressionDefinition(const ValueDefinition& value);
bool ValueTypeRequiresEndian(ValueType type);
bool ValueTypeAllowsEndian(ValueType type);
bool IsSignedIntegerType(ValueType type);
bool IsUnsignedIntegerType(ValueType type);
bool IsFloatingPointType(ValueType type);
}  // namespace CheevoMap::V2
