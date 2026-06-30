// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Core/CheevoMap/V2/Package.h"
#include "Core/CheevoMap/V2/StateStore.h"

namespace CheevoMap::V2
{
enum class ExpressionValueCategory : u8
{
  Boolean,
  SignedInteger,
  UnsignedInteger,
  FloatingPoint,
  String,
};

struct PlannedExpression
{
  std::string value_id;
  ValueType output_type = ValueType::U8;
  ExpressionNode expression;
  std::vector<std::string> dependencies;
  size_t package_index = 0;
};

const char* ExpressionOperatorName(ExpressionOperator op);
std::optional<ExpressionValueCategory> GetValueTypeExpressionCategory(ValueType type);
std::optional<std::vector<PlannedExpression>> BuildExpressionPlan(const Package& package,
                                                                  std::string* error_out);
StateValue EvaluateExpression(const ExpressionNode& expression, const StateValueMap& values);
void EvaluateExpressions(const std::vector<PlannedExpression>& expressions, StateValueMap* values);
StateValue ConvertExpressionResultToDeclaredType(const StateValue& value, ValueType type);
}  // namespace CheevoMap::V2
