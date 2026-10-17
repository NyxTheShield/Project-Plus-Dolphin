// Copyright 2026 Project+ Rollback Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Rollback
{
constexpr std::size_t BRAWL_CONTROLLER_PROFILE_SIZE = 46;

struct BrawlControllerProfile
{
  std::string tag;
  std::array<u8, BRAWL_CONTROLLER_PROFILE_SIZE> controls{};

  bool Enabled() const { return !tag.empty(); }
};

std::optional<BrawlControllerProfile> ParseBrawlControllerProfile(std::string_view tag,
                                                                  std::string_view controls_hex);
bool ApplyBrawlControllerProfilesToSave(
    std::vector<u8>* save, const std::vector<BrawlControllerProfile>& profiles);
}  // namespace Rollback
