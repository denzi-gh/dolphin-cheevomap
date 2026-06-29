// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <variant>

#include "Core/CheevoMap/CheevoMapEntry.h"
#include "Core/CheevoMap/V2/Package.h"

namespace CheevoMap
{
using LoadedPackage = std::variant<File, V2::Package>;

// Loads a CheevoMap JSON. Returns nullopt on any parse/validation failure and writes
// a human-readable description to *error_out. Asset paths inside the file are resolved
// to absolute paths under the JSON file's directory; any path that escapes that
// directory (`..`, absolute paths, symlinks pointing outside) is rejected.
std::optional<File> LoadFromFile(const std::string& json_path, std::string* error_out);

// Loads either a schema v1 compatibility package or a schema v2 typed-state package.
std::optional<LoadedPackage> LoadPackageFromFile(const std::string& json_path,
                                                 std::string* error_out);
}  // namespace CheevoMap
