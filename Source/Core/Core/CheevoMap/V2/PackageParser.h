// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>

#include <picojson.h>

#include "Core/CheevoMap/V2/Package.h"

namespace CheevoMap::V2
{
std::optional<Package> ParsePackage(const picojson::value& root, std::string* error_out);
std::optional<Package> LoadPackageFromFile(const std::string& json_path, std::string* error_out);
}  // namespace CheevoMap::V2
