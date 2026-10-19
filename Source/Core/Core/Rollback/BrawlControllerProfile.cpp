// Copyright 2026 Project+ Rollback Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/BrawlControllerProfile.h"

#include <algorithm>
#include <cstddef>
#include <limits>

#include "Common/Hash.h"

namespace Rollback
{
namespace
{
constexpr std::size_t TAG_CHARS = 5;
constexpr std::size_t TAG_SIZE = 0x124;
constexpr std::size_t TAG_COUNT = 120;
constexpr std::size_t TAGS_OFFSET = 0x3CF8;
constexpr std::size_t TAG_NAME = 0xE0;
constexpr std::size_t TAG_RUMBLE = TAG_NAME + 0x0C;
constexpr std::size_t TAG_TIMESTAMP = TAG_NAME + 0x10;
constexpr std::size_t TAG_CONTROLS = TAG_NAME + 0x14;
constexpr std::size_t CONTROLS_LAYOUT_SIZE = 0x2D;
constexpr std::size_t TAGS_END = 0xC5D8;
constexpr std::size_t CHECKSUM_OFFSET = 0xC67C;
constexpr std::size_t SAVE_SIZE = CHECKSUM_OFFSET + sizeof(u32);
static_assert(TAGS_OFFSET + TAG_COUNT * TAG_SIZE == TAGS_END);

void WriteBigEndianU32(std::vector<u8>* bytes, std::size_t offset, u32 value)
{
  (*bytes)[offset] = static_cast<u8>(value >> 24);
  (*bytes)[offset + 1] = static_cast<u8>(value >> 16);
  (*bytes)[offset + 2] = static_cast<u8>(value >> 8);
  (*bytes)[offset + 3] = static_cast<u8>(value);
}

u32 ReadBigEndianU32(const std::vector<u8>& bytes, std::size_t offset)
{
  return static_cast<u32>(bytes[offset]) << 24 |
         static_cast<u32>(bytes[offset + 1]) << 16 |
         static_cast<u32>(bytes[offset + 2]) << 8 |
         static_cast<u32>(bytes[offset + 3]);
}
}  // namespace

std::optional<BrawlControllerProfile> ParseBrawlControllerProfile(std::string_view tag,
                                                                  std::string_view controls_hex)
{
  if (tag.empty() && controls_hex.empty())
    return BrawlControllerProfile{};
  if (tag.empty() || tag.size() > TAG_CHARS || controls_hex.size() != BRAWL_CONTROLLER_PROFILE_SIZE * 2)
    return std::nullopt;
  for (const char c : tag)
  {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '!' || c == '?' || c == '-'))
      return std::nullopt;
  }
  BrawlControllerProfile profile;
  profile.tag = tag;
  const auto digit = [](char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < profile.controls.size(); ++i)
  {
    const int high = digit(controls_hex[i * 2]);
    const int low = digit(controls_hex[i * 2 + 1]);
    if (high < 0 || low < 0)
      return std::nullopt;
    profile.controls[i] = static_cast<u8>((high << 4) | low);
  }
  if (profile.controls[0] > 1)
    return std::nullopt;
  for (u32 i = 0; i < CONTROLS_LAYOUT_SIZE; ++i)
  {
    const u8 value = profile.controls[1 + i];
    const u8 mask = i == 11 ? 0xF0 : i == 0x1F ? 0xC3 : i == 0x2C ? 0x80 : 0;
    if (mask ? (value & ~mask) != 0 : value > 0x0E)
      return std::nullopt;
  }
  return profile;
}

bool ApplyBrawlControllerProfilesToSave(
    std::vector<u8>* save, const std::vector<BrawlControllerProfile>& profiles)
{
  if (!save || save->size() != SAVE_SIZE)
    return false;

  // Brawl orders its saved personal-data records by this monotonically increasing value. Derive
  // ours from the synchronized save instead of inventing an out-of-range future timestamp.
  u32 next_timestamp = 1;
  for (std::size_t slot = 0; slot < TAG_COUNT; ++slot)
  {
    const std::size_t offset = TAGS_OFFSET + slot * TAG_SIZE;
    const u32 timestamp = ReadBigEndianU32(*save, offset + TAG_TIMESTAMP);
    if (timestamp != std::numeric_limits<u32>::max())
      next_timestamp = std::max(next_timestamp, timestamp + 1);
  }

  // The game's tag menu treats this as a packed list and stops at its first empty record. Rebuild
  // the temporary session's table so the synchronized P1-P4 records are contiguous and ordered.
  std::fill(save->begin() + TAGS_OFFSET, save->begin() + TAGS_END, 0);

  std::size_t output_slot = 0;
  for (const BrawlControllerProfile& profile : profiles)
  {
    if (!profile.Enabled())
      continue;
    if (output_slot >= TAG_COUNT)
      return false;

    const std::size_t offset = TAGS_OFFSET + output_slot * TAG_SIZE;
    // Brawl reads the name as a zero-terminated UTF-16BE string. Orca's authoritative writer uses
    // the same left-aligned layout; a leading zero makes the slot appear unused.
    for (std::size_t character = 0; character < profile.tag.size(); ++character)
    {
      const std::size_t glyph_offset = offset + TAG_NAME + character * 2;
      (*save)[glyph_offset] = 0;
      (*save)[glyph_offset + 1] = static_cast<u8>(profile.tag[character]);
    }
    (*save)[offset + TAG_RUMBLE] = profile.controls[0] != 0 ? 1 : 0;
    WriteBigEndianU32(save, offset + TAG_TIMESTAMP, next_timestamp++);
    std::copy_n(profile.controls.begin() + 1, CONTROLS_LAYOUT_SIZE,
                save->begin() + offset + TAG_CONTROLS);
    ++output_slot;
  }

  // Validate every field against the bytes Brawl will read. Do not publish a save if the
  // serialization and the negotiated profiles disagree.
  std::size_t validated_slot = 0;
  for (const BrawlControllerProfile& profile : profiles)
  {
    if (!profile.Enabled())
      continue;

    const std::size_t offset = TAGS_OFFSET + validated_slot * TAG_SIZE;
    for (std::size_t character = 0; character < TAG_CHARS; ++character)
    {
      const bool occupied = character < profile.tag.size();
      const char source = occupied ? profile.tag[character] : 0;
      const std::size_t glyph_offset = offset + TAG_NAME + character * 2;
      if ((*save)[glyph_offset] != 0 ||
          (*save)[glyph_offset + 1] != static_cast<u8>(source))
        return false;
    }
    if ((*save)[offset + TAG_NAME + TAG_CHARS * 2] != 0 ||
        (*save)[offset + TAG_NAME + TAG_CHARS * 2 + 1] != 0 ||
        (*save)[offset + TAG_RUMBLE] != (profile.controls[0] != 0 ? 1 : 0) ||
        !std::equal(profile.controls.begin() + 1, profile.controls.end(),
                    save->begin() + offset + TAG_CONTROLS))
      return false;
    ++validated_slot;
  }
  if (validated_slot != output_slot)
    return false;

  if (output_slot < TAG_COUNT)
  {
    const std::size_t first_empty = TAGS_OFFSET + output_slot * TAG_SIZE;
    if (!std::all_of(save->begin() + first_empty, save->begin() + first_empty + TAG_SIZE,
                     [](u8 value) { return value == 0; }))
      return false;
  }

  const u32 checksum = Common::ComputeCRC32(save->data(), CHECKSUM_OFFSET);
  WriteBigEndianU32(save, CHECKSUM_OFFSET, checksum);
  return true;
}
}  // namespace Rollback
