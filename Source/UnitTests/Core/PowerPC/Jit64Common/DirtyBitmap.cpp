// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <vector>

#include "Common/ScopeGuard.h"
#include "Common/x64ABI.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Jit64/Jit.h"
#include "Core/PowerPC/Jit64Common/EmuCodeBlock.h"
#include "Core/PowerPC/Jit64Common/Jit64Constants.h"
#include "Core/Rollback/DirtyBitmap.h"
#include "Core/System.h"

#include <gtest/gtest.h>

namespace
{
class DirtyMarkCode : public EmuCodeBlock
{
public:
  explicit DirtyMarkCode(Core::System& system) : EmuCodeBlock(jit), jit(system)
  {
    AllocCodeSpace(4096);
  }

  using Mark = u32 (*)(u32);
  Mark Generate(u32 alignment)
  {
    using namespace Gen;
    const auto mark = reinterpret_cast<Mark>(AlignCode4());
    ABI_PushRegistersAndAdjustStack(ABI_ALL_CALLEE_SAVED, 8);
    MOV(32, R(RSCRATCH), R(ABI_PARAM1));
    EmitJITDirtyBitmapUpdate(RSCRATCH, 0, 32,
                            BitSet32{RSCRATCH2, RSCRATCH_EXTRA}, alignment);
    MOV(32, R(ABI_RETURN), R(RSCRATCH));
    ABI_PopRegistersAndAdjustStack(ABI_ALL_CALLEE_SAVED, 8);
    RET();
    return mark;
  }

  Jit64 jit;
};
}  // namespace

TEST(Jit64, AlignedDirtyMarkMatchesGeneric)
{
  Core::DeclareAsCPUThread();
  Common::ScopeGuard cpu_guard([] { Core::UndeclareAsCPUThread(); });
  auto& system = Core::System::GetInstance();
  auto& memory = system.GetMemory();
  auto& bitmap = Rollback::JITDirtyBitmap::Get();
  const bool was_enabled = bitmap.IsEnabled();
  const bool was_emitting = bitmap.ShouldEmitUpdates();
  u8* const fake_vmem = memory.GetFakeVMEM();
  Common::ScopeGuard restore([&] {
    memory.GetFakeVMEM() = fake_vmem;
    bitmap.SetEnabled(was_enabled);
    bitmap.SetEmitUpdates(was_emitting);
    bitmap.Clear();
  });
  bitmap.SetEmitUpdates(true);

  // Cover physical wrap, Wii MEM1/MEM2 aliases, GC FakeVMEM aliases and both compilation modes.
  constexpr std::array<u32, 8> bases = {0, 0x10000000, 0x1ffff000, 0x40000000,
                                      0x70000000, 0x80000000, 0x90000000, 0xfffff000};
  for (const bool fake : {false, true})
  {
    // The emitter only checks for presence; generated marking code never dereferences this pointer.
    memory.GetFakeVMEM() = fake ? reinterpret_cast<u8*>(1) : nullptr;
    for (const bool active_at_compile : {false, true})
    {
      bitmap.SetEnabled(active_at_compile);
      DirtyMarkCode code(system);
      const auto generic = code.Generate(1);
      const auto aligned = code.Generate(32);
      for (const bool active_at_run : {false, true})
      {
        if (active_at_compile && !active_at_run)
          continue;  // Production invalidates specialized blocks when tracking is disabled.
        bitmap.SetEnabled(active_at_run);
        for (const u32 base : bases)
        {
          for (u32 offset = 0; offset < Rollback::DIRTY_PAGE_SIZE; offset += 32)
          {
            const u32 address = base + offset;
            bitmap.Clear();
            EXPECT_EQ(generic(address), address);
            const std::vector<u8> expected(std::begin(bitmap.entries), std::end(bitmap.entries));
            bitmap.Clear();
            EXPECT_EQ(aligned(address), address);
            EXPECT_TRUE(std::equal(expected.begin(), expected.end(), std::begin(bitmap.entries)))
                << "address=" << address << " fake=" << fake;
          }
        }
      }
    }
  }
}
