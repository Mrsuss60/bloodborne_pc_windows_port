// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include "common/types.h"

namespace BbShaderRebuild {

int RebuildCaches();
int RebuildOne(const std::filesystem::path& dir);
int ExportRecipes(const char* cache_dir, const char* game_dir, const char* out_path);
int ImportRecipes(const char* cache_dir, const char* pack, const char* game_dir, u64 motion_params,
                  u64 motion_positions);
int MergeMetas(const char* cache_dir, const char* pack, const char* game_dir, const char* meta_dir,
               const char* out_pack, const char* work_dir, int jobs);

} // namespace BbShaderRebuild
