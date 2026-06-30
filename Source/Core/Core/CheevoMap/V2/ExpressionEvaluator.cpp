// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/CheevoMap/V2/ExpressionEvaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>

#include <fmt/format.h>

namespace CheevoMap::V2
{
namespace
{
struct ExpressionInfo
{
  PlannedExpression planned;
  std::vector<size_t> expression_dependencies;
};

const char* CategoryName(ExpressionValueCategory category)
{
  switch (category)
  {
  case ExpressionValueCategory::Boolean:
    return "Boolean";
  case ExpressionValueCategory::SignedInteger:
    return "SignedInteger";
  case ExpressionValueCategory::UnsignedInteger:
    return "UnsignedInteger";
  case ExpressionValueCategory::FloatingPoint:
    return "FloatingPoint";
  case ExpressionValueCategory::String:
    return "String";
  }
  return "Unknown";
}

ExpressionValueCategory ConstantCategory(ExpressionConstantType type)
{
  switch (type)
  {
  case ExpressionConstantType::Boolean:
    return ExpressionValueCategory::Boolean;
  case ExpressionConstantType::SignedInteger:
    return ExpressionValueCategory::SignedInteger;
  case ExpressionConstantType::UnsignedInteger:
    return ExpressionValueCategory::UnsignedInteger;
  case ExpressionConstantType::FloatingPoint:
    return ExpressionValueCategory::FloatingPoint;
  case ExpressionConstantType::String:
    return ExpressionValueCategory::String;
  }
  return ExpressionValueCategory::UnsignedInteger;
}

bool IsNumeric(ExpressionValueCategory category)
{
  return category == ExpressionValueCategory::SignedInteger ||
         category == ExpressionValueCategory::UnsignedInteger ||
         category == ExpressionValueCategory::FloatingPoint;
}

bool IsInteger(ExpressionValueCategory category)
{
  return category == ExpressionValueCategory::SignedInteger ||
         category == ExpressionValueCategory::UnsignedInteger;
}

std::optional<ExpressionValueCategory>
InferExpressionCategory(const ExpressionNode& node,
                        const std::map<std::string, size_t>& value_index, const Package& package,
                        std::vector<size_t>* dependencies, std::string* error)
{
  if (const auto* reference = std::get_if<ExpressionReference>(&node.node))
  {
    const auto it = value_index.find(reference->value_id);
    if (it == value_index.end())
    {
      *error = fmt::format("references unknown value \"{}\"", reference->value_id);
      return std::nullopt;
    }

    dependencies->push_back(it->second);
    return GetValueTypeExpressionCategory(package.values[it->second].type);
  }

  if (const auto* constant = std::get_if<ExpressionConstant>(&node.node))
    return ConstantCategory(constant->type);

  const auto& operation = std::get<ExpressionOperation>(node.node);
  std::vector<ExpressionValueCategory> arguments;
  arguments.reserve(operation.arguments.size());
  for (const ExpressionNode& argument : operation.arguments)
  {
    const auto category =
        InferExpressionCategory(argument, value_index, package, dependencies, error);
    if (!category)
      return std::nullopt;
    arguments.push_back(*category);
  }

  const auto fail = [&](std::string_view reason) -> std::optional<ExpressionValueCategory> {
    *error = fmt::format("operator \"{}\" {}", ExpressionOperatorName(operation.op), reason);
    return std::nullopt;
  };
  const auto require_same = [&]() -> std::optional<ExpressionValueCategory> {
    if (arguments[0] == arguments[1])
      return arguments[0];
    return fail(fmt::format("requires matching categories, got {} and {}",
                            CategoryName(arguments[0]), CategoryName(arguments[1])));
  };

  switch (operation.op)
  {
  case ExpressionOperator::Equal:
  case ExpressionOperator::NotEqual:
    if (arguments[0] == arguments[1])
      return ExpressionValueCategory::Boolean;
    return fail(fmt::format("requires matching categories, got {} and {}",
                            CategoryName(arguments[0]), CategoryName(arguments[1])));
  case ExpressionOperator::Less:
  case ExpressionOperator::LessEqual:
  case ExpressionOperator::Greater:
  case ExpressionOperator::GreaterEqual:
  {
    const auto same = require_same();
    if (same && IsNumeric(*same))
      return ExpressionValueCategory::Boolean;
    return fail("requires matching numeric arguments");
  }
  case ExpressionOperator::Not:
    if (arguments[0] == ExpressionValueCategory::Boolean)
      return ExpressionValueCategory::Boolean;
    return fail("requires a Boolean argument");
  case ExpressionOperator::And:
  case ExpressionOperator::Or:
    if (std::ranges::all_of(arguments, [](ExpressionValueCategory category) {
          return category == ExpressionValueCategory::Boolean;
        }))
    {
      return ExpressionValueCategory::Boolean;
    }
    return fail("requires Boolean arguments");
  case ExpressionOperator::Add:
  case ExpressionOperator::Subtract:
  case ExpressionOperator::Multiply:
  case ExpressionOperator::Divide:
  {
    const auto same = require_same();
    if (same && IsNumeric(*same))
      return *same;
    return fail("requires matching numeric arguments");
  }
  case ExpressionOperator::Modulo:
  {
    const auto same = require_same();
    if (same && IsInteger(*same))
      return *same;
    return fail("requires matching integer arguments");
  }
  case ExpressionOperator::BitAnd:
  case ExpressionOperator::BitOr:
  case ExpressionOperator::BitXor:
    if (arguments[0] == ExpressionValueCategory::UnsignedInteger &&
        arguments[1] == ExpressionValueCategory::UnsignedInteger)
    {
      return ExpressionValueCategory::UnsignedInteger;
    }
    return fail("requires UnsignedInteger arguments");
  case ExpressionOperator::BitNot:
    if (arguments[0] == ExpressionValueCategory::UnsignedInteger)
      return ExpressionValueCategory::UnsignedInteger;
    return fail("requires an UnsignedInteger argument");
  case ExpressionOperator::ToF64:
    if (IsNumeric(arguments[0]))
      return ExpressionValueCategory::FloatingPoint;
    return fail("requires a numeric argument");
  case ExpressionOperator::If:
    if (arguments[0] != ExpressionValueCategory::Boolean)
      return fail("requires a Boolean condition");
    if (arguments[1] != arguments[2])
    {
      return fail(fmt::format("requires matching branch categories, got {} and {}",
                              CategoryName(arguments[1]), CategoryName(arguments[2])));
    }
    return arguments[1];
  }
  return std::nullopt;
}

StateValue ConstantToStateValue(const ExpressionConstant& constant)
{
  switch (constant.type)
  {
  case ExpressionConstantType::Boolean:
    return StateValue::Boolean(std::get<bool>(constant.value));
  case ExpressionConstantType::SignedInteger:
    return StateValue::SignedInteger(std::get<s64>(constant.value));
  case ExpressionConstantType::UnsignedInteger:
    return StateValue::UnsignedInteger(std::get<u64>(constant.value));
  case ExpressionConstantType::FloatingPoint:
    return StateValue::FloatingPoint(std::get<double>(constant.value));
  case ExpressionConstantType::String:
    return StateValue::String(std::get<std::string>(constant.value));
  }
  return StateValue::Unavailable();
}

bool SafeSignedAdd(s64 lhs, s64 rhs, s64* out)
{
  if ((rhs > 0 && lhs > std::numeric_limits<s64>::max() - rhs) ||
      (rhs < 0 && lhs < std::numeric_limits<s64>::min() - rhs))
  {
    return false;
  }
  *out = lhs + rhs;
  return true;
}

bool SafeSignedSub(s64 lhs, s64 rhs, s64* out)
{
  if ((rhs > 0 && lhs < std::numeric_limits<s64>::min() + rhs) ||
      (rhs < 0 && lhs > std::numeric_limits<s64>::max() + rhs))
  {
    return false;
  }
  *out = lhs - rhs;
  return true;
}

bool SafeSignedMul(s64 lhs, s64 rhs, s64* out)
{
  if (lhs > 0)
  {
    if (rhs > 0 && lhs > std::numeric_limits<s64>::max() / rhs)
      return false;
    if (rhs < 0 && rhs < std::numeric_limits<s64>::min() / lhs)
      return false;
  }
  else if (lhs < 0)
  {
    if (rhs > 0 && lhs < std::numeric_limits<s64>::min() / rhs)
      return false;
    if (rhs < 0 && lhs < std::numeric_limits<s64>::max() / rhs)
      return false;
  }

  *out = lhs * rhs;
  return true;
}

std::optional<u64> AsUnsigned(const StateValue& value)
{
  return value.AsUnsignedInteger();
}

std::optional<s64> AsSigned(const StateValue& value)
{
  return value.AsSignedInteger();
}

std::optional<double> AsFiniteFloat(const StateValue& value)
{
  const std::optional<double> number = value.AsFloatingPoint();
  if (!number || !std::isfinite(*number))
    return std::nullopt;
  return number;
}

StateValue EvaluateBinaryNumeric(ExpressionOperator op, const StateValue& lhs,
                                 const StateValue& rhs)
{
  if (const auto left = AsSigned(lhs))
  {
    const auto right = AsSigned(rhs);
    if (!right)
      return StateValue::Unavailable();

    s64 output = 0;
    switch (op)
    {
    case ExpressionOperator::Add:
      return SafeSignedAdd(*left, *right, &output) ? StateValue::SignedInteger(output) :
                                                     StateValue::Unavailable();
    case ExpressionOperator::Subtract:
      return SafeSignedSub(*left, *right, &output) ? StateValue::SignedInteger(output) :
                                                     StateValue::Unavailable();
    case ExpressionOperator::Multiply:
      return SafeSignedMul(*left, *right, &output) ? StateValue::SignedInteger(output) :
                                                     StateValue::Unavailable();
    case ExpressionOperator::Divide:
      if (*right == 0 || (*left == std::numeric_limits<s64>::min() && *right == -1))
        return StateValue::Unavailable();
      return StateValue::SignedInteger(*left / *right);
    case ExpressionOperator::Modulo:
      if (*right == 0 || (*left == std::numeric_limits<s64>::min() && *right == -1))
        return StateValue::Unavailable();
      return StateValue::SignedInteger(*left % *right);
    default:
      return StateValue::Unavailable();
    }
  }

  if (const auto left = AsUnsigned(lhs))
  {
    const auto right = AsUnsigned(rhs);
    if (!right)
      return StateValue::Unavailable();

    switch (op)
    {
    case ExpressionOperator::Add:
      if (*left > std::numeric_limits<u64>::max() - *right)
        return StateValue::Unavailable();
      return StateValue::UnsignedInteger(*left + *right);
    case ExpressionOperator::Subtract:
      if (*left < *right)
        return StateValue::Unavailable();
      return StateValue::UnsignedInteger(*left - *right);
    case ExpressionOperator::Multiply:
      if (*right != 0 && *left > std::numeric_limits<u64>::max() / *right)
        return StateValue::Unavailable();
      return StateValue::UnsignedInteger(*left * *right);
    case ExpressionOperator::Divide:
      if (*right == 0)
        return StateValue::Unavailable();
      return StateValue::UnsignedInteger(*left / *right);
    case ExpressionOperator::Modulo:
      if (*right == 0)
        return StateValue::Unavailable();
      return StateValue::UnsignedInteger(*left % *right);
    default:
      return StateValue::Unavailable();
    }
  }

  const auto left = AsFiniteFloat(lhs);
  const auto right = AsFiniteFloat(rhs);
  if (!left || !right)
    return StateValue::Unavailable();

  double output = 0.0;
  switch (op)
  {
  case ExpressionOperator::Add:
    output = *left + *right;
    break;
  case ExpressionOperator::Subtract:
    output = *left - *right;
    break;
  case ExpressionOperator::Multiply:
    output = *left * *right;
    break;
  case ExpressionOperator::Divide:
    output = *left / *right;
    break;
  default:
    return StateValue::Unavailable();
  }

  if (!std::isfinite(output))
    return StateValue::Unavailable();
  return StateValue::FloatingPoint(output);
}

StateValue EvaluateComparison(ExpressionOperator op, const StateValue& lhs, const StateValue& rhs)
{
  const auto compare = [&](auto left, auto right) {
    switch (op)
    {
    case ExpressionOperator::Equal:
      return StateValue::Boolean(left == right);
    case ExpressionOperator::NotEqual:
      return StateValue::Boolean(left != right);
    case ExpressionOperator::Less:
      return StateValue::Boolean(left < right);
    case ExpressionOperator::LessEqual:
      return StateValue::Boolean(left <= right);
    case ExpressionOperator::Greater:
      return StateValue::Boolean(left > right);
    case ExpressionOperator::GreaterEqual:
      return StateValue::Boolean(left >= right);
    default:
      return StateValue::Unavailable();
    }
  };

  if (const auto left = lhs.AsBoolean())
  {
    const auto right = rhs.AsBoolean();
    if (!right)
      return StateValue::Unavailable();
    if (op != ExpressionOperator::Equal && op != ExpressionOperator::NotEqual)
      return StateValue::Unavailable();
    return compare(*left, *right);
  }
  if (const auto left = lhs.AsSignedInteger())
  {
    const auto right = rhs.AsSignedInteger();
    return right ? compare(*left, *right) : StateValue::Unavailable();
  }
  if (const auto left = lhs.AsUnsignedInteger())
  {
    const auto right = rhs.AsUnsignedInteger();
    return right ? compare(*left, *right) : StateValue::Unavailable();
  }
  if (const auto left = AsFiniteFloat(lhs))
  {
    const auto right = AsFiniteFloat(rhs);
    return right ? compare(*left, *right) : StateValue::Unavailable();
  }
  if (const std::string* left = lhs.AsString())
  {
    const std::string* right = rhs.AsString();
    if (right == nullptr)
      return StateValue::Unavailable();
    if (op != ExpressionOperator::Equal && op != ExpressionOperator::NotEqual)
      return StateValue::Unavailable();
    return compare(*left, *right);
  }

  return StateValue::Unavailable();
}

StateValue EvaluateOperation(const ExpressionOperation& operation, const StateValueMap& values)
{
  if (operation.op == ExpressionOperator::If)
  {
    const StateValue condition = EvaluateExpression(operation.arguments[0], values);
    const std::optional<bool> selected = condition.AsBoolean();
    if (!selected)
      return StateValue::Unavailable();
    return EvaluateExpression(operation.arguments[*selected ? 1 : 2], values);
  }

  std::vector<StateValue> arguments;
  arguments.reserve(operation.arguments.size());
  for (const ExpressionNode& argument : operation.arguments)
  {
    StateValue value = EvaluateExpression(argument, values);
    if (!value.IsAvailable())
      return StateValue::Unavailable();
    arguments.push_back(std::move(value));
  }

  switch (operation.op)
  {
  case ExpressionOperator::Equal:
  case ExpressionOperator::NotEqual:
  case ExpressionOperator::Less:
  case ExpressionOperator::LessEqual:
  case ExpressionOperator::Greater:
  case ExpressionOperator::GreaterEqual:
    return EvaluateComparison(operation.op, arguments[0], arguments[1]);
  case ExpressionOperator::Not:
  {
    const std::optional<bool> value = arguments[0].AsBoolean();
    return value ? StateValue::Boolean(!*value) : StateValue::Unavailable();
  }
  case ExpressionOperator::And:
  case ExpressionOperator::Or:
  {
    bool result = operation.op == ExpressionOperator::And;
    for (const StateValue& argument : arguments)
    {
      const std::optional<bool> value = argument.AsBoolean();
      if (!value)
        return StateValue::Unavailable();
      result = operation.op == ExpressionOperator::And ? result && *value : result || *value;
    }
    return StateValue::Boolean(result);
  }
  case ExpressionOperator::Add:
  case ExpressionOperator::Subtract:
  case ExpressionOperator::Multiply:
  case ExpressionOperator::Divide:
  case ExpressionOperator::Modulo:
    return EvaluateBinaryNumeric(operation.op, arguments[0], arguments[1]);
  case ExpressionOperator::BitAnd:
  case ExpressionOperator::BitOr:
  case ExpressionOperator::BitXor:
  {
    const auto left = arguments[0].AsUnsignedInteger();
    const auto right = arguments[1].AsUnsignedInteger();
    if (!left || !right)
      return StateValue::Unavailable();
    if (operation.op == ExpressionOperator::BitAnd)
      return StateValue::UnsignedInteger(*left & *right);
    if (operation.op == ExpressionOperator::BitOr)
      return StateValue::UnsignedInteger(*left | *right);
    return StateValue::UnsignedInteger(*left ^ *right);
  }
  case ExpressionOperator::BitNot:
  {
    const auto value = arguments[0].AsUnsignedInteger();
    return value ? StateValue::UnsignedInteger(~*value) : StateValue::Unavailable();
  }
  case ExpressionOperator::ToF64:
    if (const auto value = arguments[0].AsSignedInteger())
      return StateValue::FloatingPoint(static_cast<double>(*value));
    if (const auto value = arguments[0].AsUnsignedInteger())
      return StateValue::FloatingPoint(static_cast<double>(*value));
    if (const auto value = AsFiniteFloat(arguments[0]))
      return StateValue::FloatingPoint(*value);
    return StateValue::Unavailable();
  case ExpressionOperator::If:
    break;
  }

  return StateValue::Unavailable();
}

bool VisitExpression(size_t index, const std::vector<ExpressionInfo>& expressions,
                     std::vector<u8>* state, std::vector<size_t>* stack,
                     std::vector<PlannedExpression>* output, std::string* error)
{
  if ((*state)[index] == 2)
    return true;
  if ((*state)[index] == 1)
  {
    auto cycle_begin = std::ranges::find(*stack, index);
    std::vector<std::string> cycle;
    for (auto it = cycle_begin; it != stack->end(); ++it)
      cycle.push_back(expressions[*it].planned.value_id);
    cycle.push_back(expressions[index].planned.value_id);

    *error = "expression dependency cycle: ";
    for (size_t i = 0; i < cycle.size(); ++i)
    {
      if (i != 0)
        *error += " -> ";
      *error += cycle[i];
    }
    return false;
  }

  (*state)[index] = 1;
  stack->push_back(index);
  for (const size_t dependency : expressions[index].expression_dependencies)
  {
    if (!VisitExpression(dependency, expressions, state, stack, output, error))
      return false;
  }
  stack->pop_back();
  (*state)[index] = 2;
  output->push_back(expressions[index].planned);
  return true;
}
}  // namespace

const char* ExpressionOperatorName(ExpressionOperator op)
{
  switch (op)
  {
  case ExpressionOperator::Equal:
    return "eq";
  case ExpressionOperator::NotEqual:
    return "ne";
  case ExpressionOperator::Less:
    return "lt";
  case ExpressionOperator::LessEqual:
    return "lte";
  case ExpressionOperator::Greater:
    return "gt";
  case ExpressionOperator::GreaterEqual:
    return "gte";
  case ExpressionOperator::Not:
    return "not";
  case ExpressionOperator::And:
    return "and";
  case ExpressionOperator::Or:
    return "or";
  case ExpressionOperator::Add:
    return "add";
  case ExpressionOperator::Subtract:
    return "sub";
  case ExpressionOperator::Multiply:
    return "mul";
  case ExpressionOperator::Divide:
    return "div";
  case ExpressionOperator::Modulo:
    return "mod";
  case ExpressionOperator::BitAnd:
    return "bit_and";
  case ExpressionOperator::BitOr:
    return "bit_or";
  case ExpressionOperator::BitXor:
    return "bit_xor";
  case ExpressionOperator::BitNot:
    return "bit_not";
  case ExpressionOperator::ToF64:
    return "to_f64";
  case ExpressionOperator::If:
    return "if";
  }
  return "unknown";
}

std::optional<ExpressionValueCategory> GetValueTypeExpressionCategory(ValueType type)
{
  if (type == ValueType::Boolean)
    return ExpressionValueCategory::Boolean;
  if (IsSignedIntegerType(type))
    return ExpressionValueCategory::SignedInteger;
  if (IsUnsignedIntegerType(type))
    return ExpressionValueCategory::UnsignedInteger;
  if (IsFloatingPointType(type))
    return ExpressionValueCategory::FloatingPoint;
  if (type == ValueType::String)
    return ExpressionValueCategory::String;
  return std::nullopt;
}

std::optional<std::vector<PlannedExpression>> BuildExpressionPlan(const Package& package,
                                                                  std::string* error_out)
{
  std::map<std::string, size_t> value_index;
  for (size_t i = 0; i < package.values.size(); ++i)
    value_index.emplace(package.values[i].id, i);

  std::vector<size_t> expression_index_by_value(package.values.size(),
                                                std::numeric_limits<size_t>::max());
  std::vector<ExpressionInfo> expressions;
  for (size_t i = 0; i < package.values.size(); ++i)
  {
    if (IsExpressionBackedValue(package.values[i]))
    {
      expression_index_by_value[i] = expressions.size();
      expressions.push_back({});
      expressions.back().planned.value_id = package.values[i].id;
      expressions.back().planned.output_type = package.values[i].type;
      expressions.back().planned.expression = *GetExpressionDefinition(package.values[i]);
      expressions.back().planned.package_index = i;
    }
  }

  for (ExpressionInfo& expression : expressions)
  {
    std::vector<size_t> dependencies;
    const auto inferred = InferExpressionCategory(expression.planned.expression, value_index,
                                                  package, &dependencies, error_out);
    if (!inferred)
    {
      if (error_out->find("unknown value") != std::string::npos)
      {
        *error_out = fmt::format("value \"{}\" {}", expression.planned.value_id, *error_out);
      }
      else
      {
        *error_out = fmt::format("value \"{}\" expression type error: {}",
                                 expression.planned.value_id, *error_out);
      }
      return std::nullopt;
    }

    const auto declared = GetValueTypeExpressionCategory(expression.planned.output_type);
    if (!declared || *declared != *inferred)
    {
      *error_out = fmt::format("value \"{}\" expression category {} does not match declared type "
                               "{}",
                               expression.planned.value_id, CategoryName(*inferred),
                               ValueTypeName(expression.planned.output_type));
      return std::nullopt;
    }

    std::ranges::sort(dependencies);
    dependencies.erase(std::ranges::unique(dependencies).begin(), dependencies.end());
    for (const size_t dependency_value_index : dependencies)
    {
      expression.planned.dependencies.push_back(package.values[dependency_value_index].id);
      const size_t dependency_expression_index = expression_index_by_value[dependency_value_index];
      if (dependency_expression_index != std::numeric_limits<size_t>::max())
        expression.expression_dependencies.push_back(dependency_expression_index);
    }
  }

  std::vector<PlannedExpression> output;
  output.reserve(expressions.size());
  std::vector<u8> state(expressions.size(), 0);
  std::vector<size_t> stack;
  for (size_t i = 0; i < expressions.size(); ++i)
  {
    if (!VisitExpression(i, expressions, &state, &stack, &output, error_out))
      return std::nullopt;
  }

  return output;
}

StateValue EvaluateExpression(const ExpressionNode& expression, const StateValueMap& values)
{
  if (const auto* reference = std::get_if<ExpressionReference>(&expression.node))
  {
    const auto it = values.find(reference->value_id);
    return it == values.end() ? StateValue::Unavailable() : it->second;
  }
  if (const auto* constant = std::get_if<ExpressionConstant>(&expression.node))
    return ConstantToStateValue(*constant);
  return EvaluateOperation(std::get<ExpressionOperation>(expression.node), values);
}

void EvaluateExpressions(const std::vector<PlannedExpression>& expressions, StateValueMap* values)
{
  for (const PlannedExpression& expression : expressions)
  {
    StateValue result = EvaluateExpression(expression.expression, *values);
    values->insert_or_assign(expression.value_id,
                             ConvertExpressionResultToDeclaredType(result, expression.output_type));
  }
}

StateValue ConvertExpressionResultToDeclaredType(const StateValue& value, ValueType type)
{
  if (!value.IsAvailable())
    return StateValue::Unavailable();

  if (type == ValueType::Boolean)
  {
    const auto boolean = value.AsBoolean();
    return boolean ? StateValue::Boolean(*boolean) : StateValue::Unavailable();
  }

  if (IsUnsignedIntegerType(type))
  {
    const auto integer = value.AsUnsignedInteger();
    if (!integer)
      return StateValue::Unavailable();

    u64 max = std::numeric_limits<u64>::max();
    if (type == ValueType::U8)
      max = std::numeric_limits<u8>::max();
    else if (type == ValueType::U16)
      max = std::numeric_limits<u16>::max();
    else if (type == ValueType::U32)
      max = std::numeric_limits<u32>::max();

    return *integer <= max ? StateValue::UnsignedInteger(*integer) : StateValue::Unavailable();
  }

  if (IsSignedIntegerType(type))
  {
    const auto integer = value.AsSignedInteger();
    if (!integer)
      return StateValue::Unavailable();

    s64 min = std::numeric_limits<s64>::min();
    s64 max = std::numeric_limits<s64>::max();
    if (type == ValueType::S8)
    {
      min = std::numeric_limits<s8>::min();
      max = std::numeric_limits<s8>::max();
    }
    else if (type == ValueType::S16)
    {
      min = std::numeric_limits<s16>::min();
      max = std::numeric_limits<s16>::max();
    }
    else if (type == ValueType::S32)
    {
      min = std::numeric_limits<s32>::min();
      max = std::numeric_limits<s32>::max();
    }

    return *integer >= min && *integer <= max ? StateValue::SignedInteger(*integer) :
                                                StateValue::Unavailable();
  }

  if (type == ValueType::F32 || type == ValueType::F64)
  {
    const auto number = value.AsFloatingPoint();
    if (!number || !std::isfinite(*number))
      return StateValue::Unavailable();

    if (type == ValueType::F64)
      return StateValue::FloatingPoint(*number);

    if (std::fabs(*number) > std::numeric_limits<float>::max())
      return StateValue::Unavailable();
    const float rounded = static_cast<float>(*number);
    return std::isfinite(rounded) ? StateValue::FloatingPoint(static_cast<double>(rounded)) :
                                    StateValue::Unavailable();
  }

  if (type == ValueType::String)
  {
    const std::string* text = value.AsString();
    return text != nullptr ? StateValue::String(*text) : StateValue::Unavailable();
  }

  return StateValue::Unavailable();
}
}  // namespace CheevoMap::V2
