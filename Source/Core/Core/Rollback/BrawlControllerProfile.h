// Copyright 2026 Project+ Rollback Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
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
  std::u16string tag;
  std::array<u8, BRAWL_CONTROLLER_PROFILE_SIZE> controls{};

  bool Enabled() const { return !tag.empty(); }
};

// Converts a UTF-8 tag to the exact UTF-16 code units produced by Brawl's name-entry keyboard.
// Common keyboard characters are accepted in narrow form and converted to Brawl's canonical
// fullwidth form. Unsupported glyphs, invalid UTF-8, and names longer than five characters fail.
std::optional<std::u16string> EncodeBrawlTag(std::string_view tag);
std::optional<BrawlControllerProfile> ParseBrawlControllerProfile(std::string_view tag,
                                                                  std::string_view controls_hex);
bool ApplyBrawlControllerProfilesToSave(std::vector<u8>* save,
                                        const std::vector<BrawlControllerProfile>& profiles);

// Stable metadata representation used by NPR files. Tags are stored as the exact UTF-16 code
// units written to Brawl's save, so playback does not depend on a local profile database.
std::vector<u8> SerializeBrawlControllerProfiles(std::span<const BrawlControllerProfile> profiles);
std::optional<std::vector<BrawlControllerProfile>>
DeserializeBrawlControllerProfiles(std::span<const u8> bytes);
}  // namespace Rollback
