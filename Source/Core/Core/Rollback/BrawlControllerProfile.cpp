// Copyright 2026 Project+ Rollback Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/BrawlControllerProfile.h"

#include <algorithm>
#include <iterator>

#include "Core/Core.h"
#include "Core/PowerPC/MMU.h"

namespace Rollback
{
namespace
{
constexpr u32 SCENE_MANAGER = 0x805A0060;
constexpr u32 GAME_GLOBAL = 0x805A00E0;
constexpr u32 GAME_GLOBAL_NAME_RECORDS = 0x28;
constexpr u32 RECORDS_TAGS = 0xE0;
constexpr u32 TAG_SIZE = 0x124;
constexpr u32 TAG_RUMBLE = 0x0C;
constexpr u32 TAG_CONTROLS = 0x14;
constexpr u32 CONTROLS_TEMPLATE = 0x80406938;
constexpr u32 CONTROLS_LAYOUT_SIZE = 0x2D;
constexpr u32 SCENE_SELCHAR_TASK = 0x400;
constexpr u32 TASK_AREAS = 0x44;
constexpr u32 AREA_INDEX = 0x1B0;
constexpr u32 AREA_PLAYER_KIND = 0x1B4;
constexpr u32 AREA_NAME_ID = 0x1C8;
constexpr u32 PLAYER_KIND_HUMAN = 1;
constexpr u32 NO_TAG = 0xFFFFFFFF;
constexpr int TAG_CHARS = 5;
constexpr int TAG_SLOTS = 120;
constexpr std::string_view CHARACTER_SELECT = "scSelctCharacter";

class GuestMemory
{
public:
  explicit GuestMemory(const Core::CPUThreadGuard& guard) : m_guard(guard) {}
  bool Valid(u32 address) const { return PowerPC::MMU::HostIsRAMAddress(m_guard, address); }
  u32 Read32(u32 address) const { return PowerPC::MMU::HostRead<u32>(m_guard, address); }
  u16 Read16(u32 address) const { return PowerPC::MMU::HostRead<u16>(m_guard, address); }
  u8 Read8(u32 address) const { return PowerPC::MMU::HostRead<u8>(m_guard, address); }
  void Write8(u32 address, u8 value) { PowerPC::MMU::HostWrite<u8>(m_guard, value, address); }
  void Write16(u32 address, u16 value) { PowerPC::MMU::HostWrite<u16>(m_guard, value, address); }
  void Write32(u32 address, u32 value) { PowerPC::MMU::HostWrite<u32>(m_guard, value, address); }

private:
  const Core::CPUThreadGuard& m_guard;
};

bool Pointer(const GuestMemory& memory, u32 address)
{
  return address % 4 == 0 && memory.Valid(address);
}

bool SceneIs(const GuestMemory& memory)
{
  if (!Pointer(memory, SCENE_MANAGER))
    return false;
  const u32 manager = memory.Read32(SCENE_MANAGER);
  if (!Pointer(memory, manager + 4))
    return false;
  const u32 scene = memory.Read32(manager + 4);
  if (!Pointer(memory, scene))
    return false;
  const u32 name = memory.Read32(scene);
  for (std::size_t i = 0; i <= CHARACTER_SELECT.size(); ++i)
  {
    if (!memory.Valid(name + static_cast<u32>(i)) ||
        memory.Read8(name + static_cast<u32>(i)) !=
            static_cast<u8>(i < CHARACTER_SELECT.size() ? CHARACTER_SELECT[i] : '\0'))
      return false;
  }
  return true;
}

std::u16string TagAt(const GuestMemory& memory, u32 address)
{
  std::u16string tag;
  for (int i = 0; i < TAG_CHARS; ++i)
  {
    const u16 character = memory.Read16(address + static_cast<u32>(i) * 2);
    if (character == 0)
      break;
    tag.push_back(static_cast<char16_t>(character));
  }
  return tag;
}

int FindTag(const GuestMemory& memory, u32 tags, const std::u16string& name)
{
  for (int i = 0; i < TAG_SLOTS; ++i)
  {
    if (TagAt(memory, tags + static_cast<u32>(i) * TAG_SIZE) == name)
      return i;
  }
  return -1;
}

int MakeTag(GuestMemory& memory, u32 tags, const std::u16string& name)
{
  int unused = -1;
  for (int i = 0; i < TAG_SLOTS; ++i)
  {
    if (TagAt(memory, tags + static_cast<u32>(i) * TAG_SIZE).empty())
      unused = i;
  }
  if (unused < 0 || !memory.Valid(CONTROLS_TEMPLATE) ||
      !memory.Valid(CONTROLS_TEMPLATE + CONTROLS_LAYOUT_SIZE - 1))
    return -1;
  const u32 tag = tags + static_cast<u32>(unused) * TAG_SIZE;
  for (u32 offset = 0; offset < TAG_SIZE; offset += 2)
    memory.Write16(tag + offset, 0);
  for (std::size_t i = 0; i < name.size(); ++i)
    memory.Write16(tag + static_cast<u32>(i) * 2, static_cast<u16>(name[i]));
  memory.Write8(tag + TAG_RUMBLE, 1);
  for (u32 i = 0; i < CONTROLS_LAYOUT_SIZE; ++i)
    memory.Write8(tag + TAG_CONTROLS + i, memory.Read8(CONTROLS_TEMPLATE + i));
  return unused;
}

bool WornByAnotherPort(const GuestMemory& memory, u32 task, int port, int slot)
{
  for (int other = 0; other < 4; ++other)
  {
    if (other == port)
      continue;
    const u32 area = memory.Read32(task + TASK_AREAS + 4 * static_cast<u32>(other));
    if (Pointer(memory, area) && Pointer(memory, area + AREA_NAME_ID) &&
        memory.Read32(area + AREA_NAME_ID) == static_cast<u32>(slot))
      return true;
  }
  return false;
}

std::u16string Variant(std::u16string name, int port)
{
  const char16_t digit = static_cast<char16_t>(u'1' + port);
  if (name.size() < TAG_CHARS)
    name.push_back(digit);
  else
    name.back() = digit;
  return name;
}

std::optional<u32> JoinedArea(const GuestMemory& memory, u32 task, int port)
{
  const u32 pointer = task + TASK_AREAS + 4 * static_cast<u32>(port);
  if (!Pointer(memory, pointer))
    return std::nullopt;
  const u32 area = memory.Read32(pointer);
  if (!Pointer(memory, area) || !Pointer(memory, area + AREA_NAME_ID) ||
      memory.Read32(area + AREA_INDEX) != static_cast<u32>(port) ||
      memory.Read32(area + AREA_PLAYER_KIND) != PLAYER_KIND_HUMAN)
    return std::nullopt;
  return area;
}

bool LocateCharacterSelect(const GuestMemory& memory, u32* task, u32* tags)
{
  if (!SceneIs(memory))
    return false;
  const u32 scene = memory.Read32(memory.Read32(SCENE_MANAGER) + 4);
  *task = memory.Read32(scene + SCENE_SELCHAR_TASK);
  if (!Pointer(memory, GAME_GLOBAL))
    return false;
  const u32 global = memory.Read32(GAME_GLOBAL);
  if (!Pointer(memory, *task + TASK_AREAS) || !Pointer(memory, global + GAME_GLOBAL_NAME_RECORDS))
    return false;
  const u32 records = memory.Read32(global + GAME_GLOBAL_NAME_RECORDS);
  *tags = records + RECORDS_TAGS;
  return Pointer(memory, records) && memory.Valid(*tags + TAG_SLOTS * TAG_SIZE - 1);
}

std::u16string ToTag(std::string_view tag)
{
  std::u16string result;
  std::transform(tag.begin(), tag.end(), std::back_inserter(result),
                 [](char c) { return static_cast<char16_t>(c); });
  return result;
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

void ApplyBrawlControllerProfiles(const Core::CPUThreadGuard& guard,
                                  const std::vector<BrawlControllerProfile>& profiles)
{
  if (std::none_of(profiles.begin(), profiles.end(),
                   [](const BrawlControllerProfile& profile) { return profile.Enabled(); }))
    return;
  GuestMemory memory(guard);
  u32 task = 0;
  u32 tags = 0;
  if (!LocateCharacterSelect(memory, &task, &tags))
    return;
  for (std::size_t port = 0; port < profiles.size() && port < 4; ++port)
  {
    const auto& profile = profiles[port];
    if (!profile.Enabled())
      continue;
    const auto area = JoinedArea(memory, task, static_cast<int>(port));
    if (!area || memory.Read32(*area + AREA_NAME_ID) != NO_TAG)
      continue;
    std::u16string name = ToTag(profile.tag);
    int slot = FindTag(memory, tags, name);
    if (slot >= 0 && WornByAnotherPort(memory, task, static_cast<int>(port), slot))
    {
      name = Variant(name, static_cast<int>(port));
      slot = FindTag(memory, tags, name);
      if (slot >= 0 && WornByAnotherPort(memory, task, static_cast<int>(port), slot))
        continue;
    }
    if (slot < 0)
      slot = MakeTag(memory, tags, name);
    if (slot < 0)
      continue;
    const u32 tag = tags + static_cast<u32>(slot) * TAG_SIZE;
    if (memory.Read8(tag + TAG_RUMBLE) != profile.controls[0])
      memory.Write8(tag + TAG_RUMBLE, profile.controls[0]);
    for (u32 i = 0; i < CONTROLS_LAYOUT_SIZE; ++i)
    {
      if (memory.Read8(tag + TAG_CONTROLS + i) != profile.controls[1 + i])
        memory.Write8(tag + TAG_CONTROLS + i, profile.controls[1 + i]);
    }
    memory.Write32(*area + AREA_NAME_ID, static_cast<u32>(slot));
  }
}
}  // namespace Rollback
