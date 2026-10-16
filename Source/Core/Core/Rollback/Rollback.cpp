// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/Rollback.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string_view>
#include <tuple>
#include <unordered_map>

#include <xxh3.h>

#include "Common/FPURoundMode.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/DSPEmulator.h"
#include "Core/HW/DSP.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/IOS.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/DirtyBitmap.h"
#include "Core/Rollback/DirtyPages.h"
#include "Core/State.h"
#include "Core/System.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/VideoState.h"

namespace Rollback
{
namespace
{
std::atomic<bool> s_in_snapshot{false};
std::atomic<bool> s_resimulating{false};
std::atomic<bool> s_detailed_resim_profiling{false};
std::atomic<bool> s_coarse_fifo_timing{false};
std::atomic<u64> s_resim_jit_compile_blocks{0};
std::atomic<u64> s_resim_jit_compile_nanoseconds{0};
std::atomic<u64> s_rollback_burst_sequence{0};
std::atomic<u64> s_rollback_burst_serial{0};
std::atomic<u64> s_rollback_burst_start_time_ns{0};
std::atomic<u64> s_rollback_burst_end_time_ns{0};
std::atomic<u64> s_rollback_burst_duration_ns{0};
std::atomic<u64> s_rollback_burst_load_ns{0};
std::atomic<u64> s_rollback_burst_exec_ns{0};
std::atomic<u64> s_rollback_burst_save_ns{0};
std::atomic<u64> s_rollback_burst_other_ns{0};
std::atomic<u32> s_rollback_burst_replay_frames{0};
std::atomic<u64> s_frame_pacing_sequence{0};
std::atomic<u64> s_frame_pacing_serial{0};
std::atomic<u64> s_frame_pacing_boundary_time_ns{0};
std::atomic<u64> s_frame_pacing_throttle_start_time_ns{0};
std::atomic<u64> s_frame_pacing_throttle_end_time_ns{0};
std::atomic<u64> s_frame_pacing_throttle_requested_ns{0};
std::atomic<u64> s_frame_pacing_throttle_actual_ns{0};
std::atomic<s32> s_frame_pacing_gekko_frame{0};
std::atomic<bool> s_frame_pacing_follows_rollback{false};
ResimEventStats s_resim_event_stats;
std::unordered_map<u32, ResimGuestHotspot> s_resim_guest_hotspots;
ResimJitMemoryStats s_resim_jit_memory_stats;

constexpr std::size_t RESTORE_PAGE = 4096;
constexpr std::size_t JIT_CACHE_LINE = 32;
constexpr u32 MEM1_VIRTUAL = 0x80000000u;
constexpr u32 MEM2_VIRTUAL = 0x90000000u;
// Effective to physical for MEM1/MEM2 and their uncached mirrors (0x8/0xC -> 0x0, 0x9/0xD -> 0x1).
constexpr u32 PHYSICAL_MASK = 0x1FFFFFFFu;

double MillisecondsSince(std::chrono::steady_clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
      .count();
}

// A rollback snapshot represents one emulated instant. Drain deterministic FIFO work and hold the
// DSP lock across the complete Orca save or restore so the GPU and DSP cannot advance between the
// serialized device state and RAM commit. The GPU loop stays alive to service blocking video-state
// requests, but restored FIFO commands remain suspended until the whole snapshot operation ends.
class SnapshotExecutionBarrier
{
public:
  explicit SnapshotExecutionBarrier(Core::System& system) : m_system(system)
  {
    if (!system.IsDualCoreMode() || !Core::IsRunning(system))
      return;

    const auto start = std::chrono::steady_clock::now();
    system.GetDSP().GetDSPEmulator()->PauseAndLock();
    system.GetFifo().SyncGPU(Fifo::SyncGPUReason::Other);
    system.GetFifo().SetRollbackSnapshotExecutionSuspended(true);
    m_wait_ms = MillisecondsSince(start);
    m_active = true;
  }

  ~SnapshotExecutionBarrier()
  {
    if (!m_active)
      return;

    m_system.GetFifo().SetRollbackSnapshotExecutionSuspended(false);
    m_system.GetDSP().GetDSPEmulator()->UnpauseAndUnlock();
  }

  SnapshotExecutionBarrier(const SnapshotExecutionBarrier&) = delete;
  SnapshotExecutionBarrier& operator=(const SnapshotExecutionBarrier&) = delete;

  double WaitMilliseconds() const { return m_wait_ms; }

private:
  Core::System& m_system;
  bool m_active = false;
  double m_wait_ms = 0.0;
};

void HashRamPages(const u8* data, std::size_t size, std::vector<u64>* hashes)
{
  hashes->resize((size + RESTORE_PAGE - 1) / RESTORE_PAGE);
  for (std::size_t page = 0; page < hashes->size(); ++page)
  {
    const std::size_t offset = page * RESTORE_PAGE;
    (*hashes)[page] = XXH3_64bits(data + offset, std::min(RESTORE_PAGE, size - offset));
  }
}

void AuditRestoredRamPages(const u8* data, std::size_t size, const std::vector<u64>& expected,
                           u32 physical_base, s64 frame, std::string_view phase)
{
  std::size_t reported = 0;
  for (std::size_t page = 0; page < expected.size(); ++page)
  {
    const std::size_t offset = page * RESTORE_PAGE;
    const u64 actual = XXH3_64bits(data + offset, std::min(RESTORE_PAGE, size - offset));
    if (actual == expected[page])
      continue;
    ERROR_LOG_FMT(CORE,
                  "Rollback restore audit: WRONG RAM after {} frame {} physical=0x{:08x} "
                  "expected={:016x} actual={:016x}",
                  phase, frame, physical_base + static_cast<u32>(offset), expected[page], actual);
    if (++reported == 16)
      break;
  }
}

bool AuditStateRoundTrip(std::span<const u8> expected,
                         const State::RollbackStateLayout& expected_layout,
                         std::span<const u8> actual,
                         const State::RollbackStateLayout& actual_layout, s64 frame,
                         std::string_view comparison = "state roundtrip",
                         bool report_byte_details = false)
{
  static constexpr std::array<std::string_view, State::ROLLBACK_STATE_SECTION_COUNT> section_names =
      {"Movie", "VideoBackend", "CoreTiming", "HW", "PowerPC", "Wiimote", "Gecko"};
  static constexpr std::array<std::string_view, State::ROLLBACK_HW_SECTION_COUNT> hw_names = {
      "Memory",
      "MemoryInterface",
      "VideoInterface",
      "SerialInterface",
      "ProcessorInterface",
      "DSP",
      "DVDInterface",
      "GPFifo",
      "ExpansionInterface",
      "AudioInterface",
      "HSP",
      "WiiIPC",
      "IOSHLE",
      "WIIHW"};
  static constexpr std::array<std::string_view, State::ROLLBACK_VIDEO_SECTION_COUNT> video_names = {
      "BPMemory",
      "CPMemory",
      "XFMemory",
      "TextureDecoder",
      "TMEM",
      "FIFO",
      "CommandProcessor",
      "PixelEngine",
      "PixelShaderManager",
      "VertexShaderManager",
      "GeometryShaderManager",
      "VertexManager",
      "FramebufferManager",
      "TextureCache",
      "Presenter",
      "BoundingBox",
      "Widescreen",
      "XFStateManager"};
  static constexpr std::array<std::string_view, State::ROLLBACK_PPC_SECTION_COUNT> ppc_names = {
      "GPR", "PC",        "CR",      "Scalar", "FPR",    "SR",  "SPR",
      "TLB", "PageTable", "Reserve", "ICache", "DCache", "MMU", "JIT"};

  bool any_difference = false;
  const auto log_range = [&](std::string_view group, std::string_view name,
                             std::size_t expected_begin, std::size_t actual_begin,
                             std::size_t expected_end, std::size_t actual_end) {
    if (expected_end < expected_begin || actual_end < actual_begin ||
        expected_end > expected.size() || actual_end > actual.size())
    {
      ERROR_LOG_FMT(CORE,
                    "Rollback {}: frame {} invalid {} {} range begin={} expected_end={} "
                    "actual_end={}",
                    comparison, frame, group, name, expected_begin, expected_end, actual_end);
      any_difference = true;
      return;
    }
    const std::span<const u8> expected_range =
        expected.subspan(expected_begin, expected_end - expected_begin);
    const std::span<const u8> actual_range =
        actual.subspan(actual_begin, actual_end - actual_begin);
    const u64 expected_hash = XXH3_64bits(expected_range.data(), expected_range.size());
    const u64 actual_hash = XXH3_64bits(actual_range.data(), actual_range.size());
    if (expected_range.size() != actual_range.size() || expected_hash != actual_hash)
    {
      any_difference = true;
      std::size_t differing_bytes = expected_range.size() > actual_range.size() ?
                                        expected_range.size() - actual_range.size() :
                                        actual_range.size() - expected_range.size();
      std::size_t reported = 0;
      const std::size_t common_size = std::min(expected_range.size(), actual_range.size());
      for (std::size_t offset = 0; offset < common_size; ++offset)
      {
        if (expected_range[offset] == actual_range[offset])
          continue;
        ++differing_bytes;
        if (report_byte_details && reported++ < 16)
        {
          ERROR_LOG_FMT(CORE,
                        "Rollback {} byte diff: frame {} {} {} relative=0x{:x} "
                        "expected_absolute=0x{:x} actual_absolute=0x{:x} expected=0x{:02x} "
                        "actual=0x{:02x}",
                        comparison, frame, group, name, offset, expected_begin + offset,
                        actual_begin + offset, expected_range[offset], actual_range[offset]);
        }
      }
      ERROR_LOG_FMT(CORE,
                    "Rollback {}: frame {} {} {} differs expected={:016x}/{} "
                    "actual={:016x}/{} differing_bytes={}",
                    comparison, frame, group, name, expected_hash, expected_range.size(),
                    actual_hash, actual_range.size(), differing_bytes);
    }
  };

  std::size_t expected_begin = 0;
  std::size_t actual_begin = 0;
  for (std::size_t i = 0; i < section_names.size(); ++i)
  {
    log_range("section", section_names[i], expected_begin, actual_begin,
              expected_layout.section_ends[i], actual_layout.section_ends[i]);
    expected_begin = expected_layout.section_ends[i];
    actual_begin = actual_layout.section_ends[i];
  }

  expected_begin = expected_layout.section_ends[2];
  actual_begin = actual_layout.section_ends[2];
  for (std::size_t i = 0; i < hw_names.size(); ++i)
  {
    log_range("HW", hw_names[i], expected_begin, actual_begin, expected_layout.hw_section_ends[i],
              actual_layout.hw_section_ends[i]);
    expected_begin = expected_layout.hw_section_ends[i];
    actual_begin = actual_layout.hw_section_ends[i];
  }

  expected_begin = expected_layout.section_ends[0];
  actual_begin = actual_layout.section_ends[0];
  for (std::size_t i = 0; i < video_names.size(); ++i)
  {
    log_range("Video", video_names[i], expected_begin, actual_begin,
              expected_layout.video_section_ends[i], actual_layout.video_section_ends[i]);
    expected_begin = expected_layout.video_section_ends[i];
    actual_begin = actual_layout.video_section_ends[i];
  }

  expected_begin = expected_layout.section_ends[3];
  actual_begin = actual_layout.section_ends[3];
  for (std::size_t i = 0; i < ppc_names.size(); ++i)
  {
    log_range("PowerPC", ppc_names[i], expected_begin, actual_begin,
              expected_layout.ppc_section_ends[i], actual_layout.ppc_section_ends[i]);
    expected_begin = expected_layout.ppc_section_ends[i];
    actual_begin = actual_layout.ppc_section_ends[i];
  }
  return any_difference;
}

bool AuditEFBImage(const EFBRollbackAuditImage& expected, const EFBRollbackAuditImage& actual,
                   s64 frame, std::string_view name, bool report_byte_details)
{
  const u64 expected_hash = XXH3_64bits(expected.bytes.data(), expected.bytes.size());
  const u64 actual_hash = XXH3_64bits(actual.bytes.data(), actual.bytes.size());
  const bool layout_differs = expected.width != actual.width || expected.height != actual.height ||
                              expected.layers != actual.layers ||
                              expected.stride != actual.stride || expected.format != actual.format;
  if (!layout_differs && expected.bytes.size() == actual.bytes.size() &&
      expected_hash == actual_hash)
    return false;

  std::size_t differing_bytes = expected.bytes.size() > actual.bytes.size() ?
                                    expected.bytes.size() - actual.bytes.size() :
                                    actual.bytes.size() - expected.bytes.size();
  std::size_t reported = 0;
  const std::size_t common_size = std::min(expected.bytes.size(), actual.bytes.size());
  for (std::size_t offset = 0; offset < common_size; ++offset)
  {
    if (expected.bytes[offset] == actual.bytes[offset])
      continue;
    ++differing_bytes;
    if (report_byte_details && reported++ < 16)
    {
      ERROR_LOG_FMT(CORE,
                    "Rollback EFB audit byte diff: frame {} {} offset=0x{:x} expected=0x{:02x} "
                    "actual=0x{:02x}",
                    frame, name, offset, expected.bytes[offset], actual.bytes[offset]);
    }
  }
  ERROR_LOG_FMT(CORE,
                "Rollback EFB audit: frame {} {} differs expected={:016x}/{} "
                "actual={:016x}/{} differing_bytes={} expected_layout={}x{}x{} stride={} fmt={} "
                "actual_layout={}x{}x{} stride={} fmt={}",
                frame, name, expected_hash, expected.bytes.size(), actual_hash, actual.bytes.size(),
                differing_bytes, expected.width, expected.height, expected.layers, expected.stride,
                static_cast<u32>(expected.format), actual.width, actual.height, actual.layers,
                actual.stride, static_cast<u32>(actual.format));
  return true;
}

std::size_t CountDifferentPageHashes(const std::vector<u64>& original,
                                     const std::vector<u64>& replayed, u32 physical_base, s64 frame,
                                     std::string_view area, bool report)
{
  const std::size_t common = std::min(original.size(), replayed.size());
  std::size_t different = original.size() > replayed.size() ? original.size() - replayed.size() :
                                                              replayed.size() - original.size();
  std::size_t reported = 0;
  for (std::size_t page = 0; page < common; ++page)
  {
    if (original[page] == replayed[page])
      continue;
    ++different;
    if (report && reported++ < 16)
    {
      ERROR_LOG_FMT(CORE,
                    "Rollback replay audit: frame {} {} page differs physical=0x{:08x} "
                    "original={:016x} replayed={:016x}",
                    frame, area, physical_base + static_cast<u32>(page * RESTORE_PAGE),
                    original[page], replayed[page]);
    }
  }
  return different;
}

bool DeterministicStateDiffers(std::span<const u8> original,
                               const State::RollbackStateLayout& original_layout,
                               std::span<const u8> replayed,
                               const State::RollbackStateLayout& replayed_layout)
{
  // Movie and video state are host-facing. Resimulation deliberately skips rendering, so those
  // sections may differ. CoreTiming through Gecko drive guest execution.
  for (std::size_t section = 2; section < State::ROLLBACK_STATE_SECTION_COUNT; ++section)
  {
    const std::size_t original_begin = original_layout.section_ends[section - 1];
    const std::size_t replayed_begin = replayed_layout.section_ends[section - 1];
    const std::size_t original_end = original_layout.section_ends[section];
    const std::size_t replayed_end = replayed_layout.section_ends[section];
    if (original_end < original_begin || replayed_end < replayed_begin ||
        original_end > original.size() || replayed_end > replayed.size())
    {
      return true;
    }
    const std::size_t original_size = original_end - original_begin;
    const std::size_t replayed_size = replayed_end - replayed_begin;
    if (original_size != replayed_size ||
        XXH3_64bits(original.data() + original_begin, original_size) !=
            XXH3_64bits(replayed.data() + replayed_begin, replayed_size))
    {
      return true;
    }
  }
  return false;
}

// Copies saved RAM back one page at a time, skipping pages that already match. Only changed pages
// have their JIT blocks invalidated; clearing the whole JIT (as a normal state load does) would
// recompile the hot code after every rollback. Changed pages are collected by physical address.
void RestoreRam(u8* live, const std::vector<u8>& saved, u32 virtual_base,
                std::vector<u32>* changed_pages, std::vector<ChangedMemoryRange>* changed_ranges)
{
  const std::size_t size = saved.size();
  for (std::size_t offset = 0; offset < size; offset += RESTORE_PAGE)
  {
    const std::size_t length = std::min(RESTORE_PAGE, size - offset);
    if (std::memcmp(live + offset, saved.data() + offset, length) == 0)
      continue;
    const u32 physical = (virtual_base + static_cast<u32>(offset)) & PHYSICAL_MASK;
    for (std::size_t line = 0; line < length; line += JIT_CACHE_LINE)
    {
      const std::size_t line_length = std::min(JIT_CACHE_LINE, length - line);
      if (std::memcmp(live + offset + line, saved.data() + offset + line, line_length) != 0)
      {
        changed_ranges->push_back(
            {physical + static_cast<u32>(line), static_cast<u32>(line_length)});
      }
    }
    std::memcpy(live + offset, saved.data() + offset, length);
    changed_pages->push_back(physical);
  }
}

// The JIT also remembers per-instruction facts (GPU FIFO writes, paired quantization, no
// speculative constants). A restored page may hold different instructions, so forget its entries.
// Done here in bulk because the JIT's own per-word invalidation is too slow for thousands of pages.
void ForgetLearnedJitAddresses(Core::System& system, const std::vector<u32>& changed_pages)
{
  // JitInterface only holds a JitBase (null for the interpreter). static_cast because MSVC builds
  // have RTTI off.
  auto* jit = static_cast<JitBase*>(system.GetJitInterface().GetCore());
  if (!jit || changed_pages.empty())
    return;
  const auto in_changed_page = [&changed_pages](u32 address) {
    const u32 page = address & PHYSICAL_MASK & ~static_cast<u32>(RESTORE_PAGE - 1);
    return std::binary_search(changed_pages.begin(), changed_pages.end(), page);
  };
  std::erase_if(jit->js.fifoWriteAddresses, in_changed_page);
  std::erase_if(jit->js.pairedQuantizeAddresses, in_changed_page);
  std::erase_if(jit->js.noSpeculativeConstantsAddresses, in_changed_page);
}

// The NAND file system, which is always the host backend. Its journal is part of Orca's
// authoritative whole-emulator rollback state.
IOS::HLE::FS::HostFileSystem* HostNand(Core::System& system)
{
  IOS::HLE::EmulationKernel* ios = system.GetIOS();
  return ios ? static_cast<IOS::HLE::FS::HostFileSystem*>(ios->GetFS().get()) : nullptr;
}
}  // namespace

bool InSnapshotDoState()
{
  return s_in_snapshot.load(std::memory_order_relaxed);
}

bool IsResimulating()
{
  return s_resimulating.load(std::memory_order_relaxed);
}

void SetResimulating(bool resimulating)
{
  const bool changed = s_resimulating.load(std::memory_order_relaxed) != resimulating;
  auto& system = Core::System::GetInstance();
  if (changed && system.IsDualCoreMode() && Core::IsRunning(system))
    system.GetFifo().SyncGPU(Fifo::SyncGPUReason::Other);

  s_resimulating.store(resimulating, std::memory_order_relaxed);
  VideoCommon_SetSkipRender(resimulating);
}

bool IsDetailedResimProfilingEnabled()
{
  return s_detailed_resim_profiling.load(std::memory_order_relaxed);
}

void SetDetailedResimProfilingEnabled(bool enabled)
{
  s_detailed_resim_profiling.store(enabled, std::memory_order_relaxed);
}

bool UseCoarseFifoTiming()
{
  return s_coarse_fifo_timing.load(std::memory_order_relaxed);
}

void SetCoarseFifoTiming(bool enabled)
{
  s_coarse_fifo_timing.store(enabled, std::memory_order_relaxed);
}

void RecordRollbackBurst(u64 duration_ns, u64 load_ns, u64 exec_ns, u64 save_ns, u64 other_ns,
                         u32 replay_frames)
{
  const u64 end_time_ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count());
  s_rollback_burst_sequence.fetch_add(1, std::memory_order_acq_rel);
  s_rollback_burst_start_time_ns.store(end_time_ns - duration_ns, std::memory_order_relaxed);
  s_rollback_burst_duration_ns.store(duration_ns, std::memory_order_relaxed);
  s_rollback_burst_load_ns.store(load_ns, std::memory_order_relaxed);
  s_rollback_burst_exec_ns.store(exec_ns, std::memory_order_relaxed);
  s_rollback_burst_save_ns.store(save_ns, std::memory_order_relaxed);
  s_rollback_burst_other_ns.store(other_ns, std::memory_order_relaxed);
  s_rollback_burst_replay_frames.store(replay_frames, std::memory_order_relaxed);
  s_rollback_burst_end_time_ns.store(end_time_ns, std::memory_order_relaxed);
  s_rollback_burst_serial.fetch_add(1, std::memory_order_relaxed);
  s_rollback_burst_sequence.fetch_add(1, std::memory_order_release);
}

RollbackBurstMarker GetLastRollbackBurst()
{
  RollbackBurstMarker marker;
  while (true)
  {
    const u64 sequence_before = s_rollback_burst_sequence.load(std::memory_order_acquire);
    if ((sequence_before & 1) != 0)
      continue;
    marker.serial = s_rollback_burst_serial.load(std::memory_order_relaxed);
    marker.start_time_ns = s_rollback_burst_start_time_ns.load(std::memory_order_relaxed);
    marker.end_time_ns = s_rollback_burst_end_time_ns.load(std::memory_order_relaxed);
    marker.duration_ns = s_rollback_burst_duration_ns.load(std::memory_order_relaxed);
    marker.load_ns = s_rollback_burst_load_ns.load(std::memory_order_relaxed);
    marker.exec_ns = s_rollback_burst_exec_ns.load(std::memory_order_relaxed);
    marker.save_ns = s_rollback_burst_save_ns.load(std::memory_order_relaxed);
    marker.other_ns = s_rollback_burst_other_ns.load(std::memory_order_relaxed);
    marker.replay_frames = s_rollback_burst_replay_frames.load(std::memory_order_relaxed);
    const u64 sequence_after = s_rollback_burst_sequence.load(std::memory_order_acquire);
    if (sequence_before == sequence_after)
      return marker;
  }
}

void RecordFramePacingBoundary(s32 gekko_frame, bool follows_rollback)
{
  const u64 now_ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
  s_frame_pacing_sequence.fetch_add(1, std::memory_order_acq_rel);
  s_frame_pacing_boundary_time_ns.store(now_ns, std::memory_order_relaxed);
  s_frame_pacing_throttle_start_time_ns.store(0, std::memory_order_relaxed);
  s_frame_pacing_throttle_end_time_ns.store(0, std::memory_order_relaxed);
  s_frame_pacing_throttle_requested_ns.store(0, std::memory_order_relaxed);
  s_frame_pacing_throttle_actual_ns.store(0, std::memory_order_relaxed);
  s_frame_pacing_gekko_frame.store(gekko_frame, std::memory_order_relaxed);
  s_frame_pacing_follows_rollback.store(follows_rollback, std::memory_order_relaxed);
  s_frame_pacing_serial.fetch_add(1, std::memory_order_relaxed);
  s_frame_pacing_sequence.fetch_add(1, std::memory_order_release);
}

void RecordFramePacingThrottle(u64 start_time_ns, u64 end_time_ns, u64 requested_ns, u64 actual_ns)
{
  s_frame_pacing_sequence.fetch_add(1, std::memory_order_acq_rel);
  s_frame_pacing_throttle_start_time_ns.store(start_time_ns, std::memory_order_relaxed);
  s_frame_pacing_throttle_end_time_ns.store(end_time_ns, std::memory_order_relaxed);
  s_frame_pacing_throttle_requested_ns.store(requested_ns, std::memory_order_relaxed);
  s_frame_pacing_throttle_actual_ns.store(actual_ns, std::memory_order_relaxed);
  s_frame_pacing_sequence.fetch_add(1, std::memory_order_release);
}

FramePacingMarker GetLastFramePacingMarker()
{
  FramePacingMarker marker;
  while (true)
  {
    const u64 sequence_before = s_frame_pacing_sequence.load(std::memory_order_acquire);
    if ((sequence_before & 1) != 0)
      continue;
    marker.serial = s_frame_pacing_serial.load(std::memory_order_relaxed);
    marker.boundary_time_ns = s_frame_pacing_boundary_time_ns.load(std::memory_order_relaxed);
    marker.throttle_start_time_ns =
        s_frame_pacing_throttle_start_time_ns.load(std::memory_order_relaxed);
    marker.throttle_end_time_ns =
        s_frame_pacing_throttle_end_time_ns.load(std::memory_order_relaxed);
    marker.throttle_requested_ns =
        s_frame_pacing_throttle_requested_ns.load(std::memory_order_relaxed);
    marker.throttle_actual_ns = s_frame_pacing_throttle_actual_ns.load(std::memory_order_relaxed);
    marker.gekko_frame = s_frame_pacing_gekko_frame.load(std::memory_order_relaxed);
    marker.follows_rollback = s_frame_pacing_follows_rollback.load(std::memory_order_relaxed);
    const u64 sequence_after = s_frame_pacing_sequence.load(std::memory_order_acquire);
    if (sequence_before == sequence_after)
      return marker;
  }
}

void RecordResimJitCompile(u64 nanoseconds)
{
  s_resim_jit_compile_blocks.fetch_add(1, std::memory_order_relaxed);
  s_resim_jit_compile_nanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
}

ResimJitCompileStats GetResimJitCompileStats()
{
  return {s_resim_jit_compile_blocks.load(std::memory_order_relaxed),
          s_resim_jit_compile_nanoseconds.load(std::memory_order_relaxed)};
}

void RecordResimGuestSlice(u32 pc, u64 nanoseconds, u64 cycles)
{
  auto [it, inserted] = s_resim_guest_hotspots.try_emplace(pc);
  ResimGuestHotspot& hotspot = it->second;
  if (inserted)
    hotspot.pc = pc;
  hotspot.nanoseconds += nanoseconds;
  hotspot.cycles += cycles;
  ++hotspot.slices;
}

std::vector<ResimGuestHotspot> TakeResimGuestHotspots(std::size_t limit)
{
  std::vector<ResimGuestHotspot> hotspots;
  hotspots.reserve(s_resim_guest_hotspots.size());
  for (const auto& [pc, hotspot] : s_resim_guest_hotspots)
    hotspots.push_back(hotspot);
  s_resim_guest_hotspots.clear();

  std::ranges::sort(hotspots, std::greater{}, &ResimGuestHotspot::nanoseconds);
  if (hotspots.size() > limit)
    hotspots.resize(limit);
  return hotspots;
}

void RecordResimJitMemoryHelper(bool write, ResimJitMemoryRegion region, u64 nanoseconds)
{
  const std::size_t region_index = static_cast<std::size_t>(region);
  if (write)
  {
    ++s_resim_jit_memory_stats.write_calls;
    s_resim_jit_memory_stats.write_nanoseconds += nanoseconds;
    ++s_resim_jit_memory_stats.write_region_calls[region_index];
    s_resim_jit_memory_stats.write_region_nanoseconds[region_index] += nanoseconds;
  }
  else
  {
    ++s_resim_jit_memory_stats.read_calls;
    s_resim_jit_memory_stats.read_nanoseconds += nanoseconds;
    ++s_resim_jit_memory_stats.read_region_calls[region_index];
    s_resim_jit_memory_stats.read_region_nanoseconds[region_index] += nanoseconds;
  }
}

ResimJitMemoryStats TakeResimJitMemoryStats()
{
  return std::exchange(s_resim_jit_memory_stats, {});
}

void RecordResimEvent(ResimEventCategory category, u64 nanoseconds)
{
  const std::size_t index = static_cast<std::size_t>(category);
  s_resim_event_stats.nanoseconds[index] += nanoseconds;
  ++s_resim_event_stats.callbacks[index];
}

ResimEventStats GetResimEventStats()
{
  return s_resim_event_stats;
}

SnapshotScope::SnapshotScope()
{
  s_in_snapshot.store(true, std::memory_order_relaxed);
  // In a session, host GPU state (EFB, texture cache, bounding box) never reaches emulated RAM, so
  // snapshots skip it: no GPU readback on save, no texture-cache reload on load.
  VideoCommon_SetRollbackSnapshot(true);
}

SnapshotScope::~SnapshotScope()
{
  VideoCommon_SetRollbackSnapshot(false);
  s_in_snapshot.store(false, std::memory_order_relaxed);
}

SnapshotRing::SnapshotRing(std::size_t slots, bool tracked_bitmap_clear, bool force_full_scan,
                           bool keep_jit_tracking, bool audit_untracked_writes, bool audit_video,
                           bool trace_snapshots)
    : m_slots(std::max<std::size_t>(slots, 2)), m_tracked_bitmap_clear(tracked_bitmap_clear),
      m_force_full_scan(force_full_scan), m_keep_jit_tracking(keep_jit_tracking),
      m_audit_untracked_writes(audit_untracked_writes), m_audit_video(audit_video),
      m_trace_snapshots(trace_snapshots)
{
  m_changed_blocks.reserve(4096);
  m_changed_ranges.reserve(32768);
}

SnapshotRing::~SnapshotRing()
{
  DirtyPages::Disarm(this);
}

void SnapshotRing::Forget(Slot* slot)
{
  if (slot->page_snapshot_id != 0)
    DirtyPages::Drop(slot->page_snapshot_id);
  slot->page_snapshot_id = 0;
  slot->frame = -1;
}

bool SnapshotRing::UseDirtyPageTracking(Core::System& system, bool* armed_now)
{
  *armed_now = false;
  // If tracking stopped under us (emulation stopped, or an unsupported RAM mapping), the
  // undo-log snapshots are gone.
  if (m_dirty_page_tracking.value_or(false) && !DirtyPages::IsArmedFor(this))
  {
    for (Slot& slot : m_slots)
    {
      if (slot.page_snapshot_id != 0)
      {
        slot.page_snapshot_id = 0;
        slot.frame = -1;
      }
    }
    m_dirty_page_tracking.reset();
  }
  if (!m_dirty_page_tracking)
  {
    m_dirty_page_tracking = DirtyPages::ArmForSystem(system, this, m_force_full_scan,
                                                     m_keep_jit_tracking, m_audit_untracked_writes);
    *armed_now = *m_dirty_page_tracking;
  }
  return *m_dirty_page_tracking;
}

SnapshotRing::Slot* SnapshotRing::Find(s64 frame)
{
  for (Slot& slot : m_slots)
  {
    if (slot.frame == frame)
      return &slot;
  }
  return nullptr;
}

const SnapshotRing::Slot* SnapshotRing::Find(s64 frame) const
{
  for (const Slot& slot : m_slots)
  {
    if (slot.frame == frame)
      return &slot;
  }
  return nullptr;
}

bool SnapshotRing::Has(s64 frame) const
{
  const Slot* slot = Find(frame);
  return slot && (slot->page_snapshot_id == 0 || DirtyPages::Has(slot->page_snapshot_id));
}

void SnapshotRing::ExpandSlots(std::size_t slots)
{
  if (slots > m_slots.size())
    m_slots.resize(slots);
}

std::span<const u8> SnapshotRing::LastState() const
{
  const Slot& slot = m_slots[m_last];
  return {slot.state.data(), slot.state_size};
}

const State::RollbackStateLayout& SnapshotRing::LastStateLayout() const
{
  return m_slots[m_last].state_layout;
}

std::span<const u8> SnapshotRing::StateForFrame(s64 frame) const
{
  const Slot* slot = Find(frame);
  return slot ? std::span<const u8>{slot->state.data(), slot->state_size} : std::span<const u8>{};
}

const EFBRollbackAuditCapture* SnapshotRing::EFBAuditForFrame(s64 frame) const
{
  const Slot* slot = Find(frame);
  return slot && slot->audit_efb.valid ? &slot->audit_efb : nullptr;
}

void SnapshotRing::Reset(Core::System& system)
{
  for (Slot& slot : m_slots)
  {
    slot.frame = -1;
    slot.page_snapshot_id = 0;
  }
  DirtyPages::Disarm(this);
  m_dirty_page_tracking.reset();
  m_next = 0;
  m_initial_trace_events = 0;
  m_replay_mismatch_reported = false;
  m_state_byte_details_reported = false;
  m_efb_mismatch_reported = false;
  m_efb_match_reported = false;
  if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system))
    nand->JournalStop();
}

bool SnapshotRing::Save(Core::System& system, s64 frame)
{
  const bool trace = m_trace_snapshots && m_initial_trace_events++ < 16;
  if (trace)
    NOTICE_LOG_FMT(CORE, "Rollback trace: save frame {} begin", frame);
  m_last_save_timings = {};
  SnapshotExecutionBarrier execution_barrier(system);
  m_last_save_timings.execution_barrier_ms = execution_barrier.WaitMilliseconds();
  // Re-saving a frame after a rewind reuses its slot; otherwise take the next one.
  Slot* slot = Find(frame);
  const bool compare_replayed_frame =
      slot && m_audit_untracked_writes && Rollback::IsResimulating();
  std::size_t original_state_size = 0;
  State::RollbackStateLayout original_state_layout{};
  u64 original_l1_hash = 0;
  u64 original_nand_mark = 0;
  if (compare_replayed_frame)
  {
    original_state_size = slot->state_size;
    if (m_state_replay_audit.size() < original_state_size)
      m_state_replay_audit.reset(original_state_size);
    std::memcpy(m_state_replay_audit.data(), slot->state.data(), original_state_size);
    original_state_layout = slot->state_layout;
    m_replay_mem1_audit = slot->audit_mem1_pages;
    m_replay_mem2_audit = slot->audit_mem2_pages;
    original_l1_hash = XXH3_64bits(slot->l1_cache.data(), slot->l1_cache.size());
    original_nand_mark = slot->nand_journal_mark;
  }
  if (!slot)
  {
    slot = &m_slots[m_next];
    m_next = (m_next + 1) % m_slots.size();
  }
  m_last = static_cast<std::size_t>(slot - m_slots.data());
  Forget(slot);
  bool armed_now;
  const bool dirty_page_tracking = UseDirtyPageTracking(system, &armed_now);
  if (trace)
    NOTICE_LOG_FMT(CORE, "Rollback trace: save frame {} tracking ready (dirty={}, armed={})", frame,
                   dirty_page_tracking, armed_now);
  const auto buffers = [slot] {
    return std::tuple{slot->state.data(), slot->mem1.capacity(), slot->mem2.capacity()};
  };
  const auto buffers_before = buffers();

  // Non-RAM state first, so anything DoState writes into RAM is in the RAM snapshot.
  {
    const auto phase_start = std::chrono::steady_clock::now();
    SnapshotScope scope;
    slot->state_size = State::SaveToBufferForRollback(system, slot->state, &slot->state_layout);
    if (trace)
      NOTICE_LOG_FMT(CORE, "Rollback trace: save frame {} state done ({} bytes)", frame,
                     slot->state_size);
    m_last_save_timings.state_ms = MillisecondsSince(phase_start);
    m_last_save_timings.state_bytes = slot->state_size;
  }

  auto& memory = system.GetMemory();
  // Dirty-page tracking: commit the RAM written since the last snapshot and open this snapshot's
  // undo log. A 0 id means tracking stopped, so fall back to a full copy.
  if (dirty_page_tracking)
  {
    const auto phase_start = std::chrono::steady_clock::now();
    slot->page_snapshot_id = DirtyPages::Snapshot(frame);
    const DirtyPages::CommitTimings commit_timings = DirtyPages::GetLastCommitTimings();
    m_last_save_timings.dirty_bitmap_scan_ms = commit_timings.bitmap_scan_ms;
    m_last_save_timings.dirty_compare_ms = commit_timings.compare_ms;
    m_last_save_timings.dirty_undo_log_ms = commit_timings.undo_log_ms;
    m_last_save_timings.dirty_mirror_copy_ms = commit_timings.mirror_copy_ms;
    m_last_save_timings.dirty_marked_pages = commit_timings.marked_pages;
    m_last_save_timings.dirty_changed_pages = commit_timings.changed_pages;
    if (trace)
      NOTICE_LOG_FMT(CORE, "Rollback trace: save frame {} dirty commit done (id={})", frame,
                     slot->page_snapshot_id);
    m_last_save_timings.dirty_pages_ms = MillisecondsSince(phase_start);
  }
  if (slot->page_snapshot_id == 0)
  {
    const auto phase_start = std::chrono::steady_clock::now();
    slot->mem1.assign(memory.GetRAM(), memory.GetRAM() + memory.GetRamSize());
    if (memory.GetEXRAM())
      slot->mem2.assign(memory.GetEXRAM(), memory.GetEXRAM() + memory.GetExRamSize());
    else
      slot->mem2.clear();
    m_last_save_timings.dirty_pages_ms = MillisecondsSince(phase_start);
  }
  if (m_audit_untracked_writes)
  {
    HashRamPages(memory.GetRAM(), memory.GetRamSize(), &slot->audit_mem1_pages);
    if (memory.GetEXRAM())
      HashRamPages(memory.GetEXRAM(), memory.GetExRamSize(), &slot->audit_mem2_pages);
    else
      slot->audit_mem2_pages.clear();
  }
  const auto l1_start = std::chrono::steady_clock::now();
  slot->l1_cache.assign(memory.GetL1Cache(), memory.GetL1Cache() + memory.GetL1CacheSize());
  m_last_save_timings.l1_ms = MillisecondsSince(l1_start);
  slot->audit_efb.valid = false;
  const bool video_audit_checkpoint = frame == 0 || (frame > 0 && (frame - 1) % 60 == 0);
  if (m_audit_video && video_audit_checkpoint && g_framebuffer_manager &&
      !g_framebuffer_manager->CaptureEFBRollbackAudit(&slot->audit_efb))
  {
    ERROR_LOG_FMT(CORE, "Rollback EFB audit: frame {} save readback failed", frame);
  }
  slot->frame = frame;

  // NAND changes older than the oldest snapshot can never be undone, so trim them.
  if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system))
  {
    const auto phase_start = std::chrono::steady_clock::now();
    slot->nand_journal_mark = nand->LastJournalMark();
    u64 oldest = slot->nand_journal_mark;
    for (const Slot& other : m_slots)
    {
      if (other.frame >= 0)
        oldest = std::min(oldest, other.nand_journal_mark);
    }
    nand->JournalTrim(oldest);
    m_last_save_timings.journal_ms = MillisecondsSince(phase_start);
  }

  if (compare_replayed_frame)
  {
    const std::span<const u8> original_state{m_state_replay_audit.data(), original_state_size};
    const std::span<const u8> replayed_state{slot->state.data(), slot->state_size};
    const std::size_t mem1_pages = CountDifferentPageHashes(
        m_replay_mem1_audit, slot->audit_mem1_pages, 0, frame, "MEM1", false);
    const std::size_t mem2_pages = CountDifferentPageHashes(
        m_replay_mem2_audit, slot->audit_mem2_pages, 0x10000000u, frame, "MEM2", false);
    const u64 replayed_l1_hash = XXH3_64bits(slot->l1_cache.data(), slot->l1_cache.size());
    const bool state_differs = DeterministicStateDiffers(original_state, original_state_layout,
                                                         replayed_state, slot->state_layout);
    const bool l1_differs = original_l1_hash != replayed_l1_hash;
    const bool nand_differs = original_nand_mark != slot->nand_journal_mark;
    if (!m_replay_mismatch_reported &&
        (mem1_pages != 0 || mem2_pages != 0 || state_differs || l1_differs || nand_differs))
    {
      m_replay_mismatch_reported = true;
      ERROR_LOG_FMT(CORE,
                    "Rollback replay audit: FIRST original/replay mismatch at snapshot frame {}: "
                    "MEM1_pages={} MEM2_pages={} deterministic_state={} L1={} NAND_mark={}/{}",
                    frame, mem1_pages, mem2_pages, state_differs, l1_differs, original_nand_mark,
                    slot->nand_journal_mark);
      CountDifferentPageHashes(m_replay_mem1_audit, slot->audit_mem1_pages, 0, frame, "MEM1", true);
      CountDifferentPageHashes(m_replay_mem2_audit, slot->audit_mem2_pages, 0x10000000u, frame,
                               "MEM2", true);
      AuditStateRoundTrip(original_state, original_state_layout, replayed_state, slot->state_layout,
                          frame, "replay audit", true);
    }
  }

  if (trace)
    NOTICE_LOG_FMT(CORE, "Rollback trace: save frame {} complete", frame);

  // Arming allocates and initializes the RAM mirrors; that one-off cost counts as an allocation.
  return armed_now || buffers() != buffers_before;
}

bool SnapshotRing::Load(Core::System& system, s64 frame)
{
  return LoadSlot(system, frame, false);
}

std::optional<u64> SnapshotRing::NandJournalMarkForFrame(s64 frame) const
{
  const Slot* slot = Find(frame);
  if (!slot)
    return std::nullopt;
  return slot->nand_journal_mark;
}

bool SnapshotRing::LoadSlot(Core::System& system, s64 frame, bool redisplay)
{
  const bool trace = m_trace_snapshots && m_initial_trace_events++ < 16;
  if (trace)
    NOTICE_LOG_FMT(CORE, "Rollback trace: load frame {} begin", frame);
  m_last_load_timings = {};
  SnapshotExecutionBarrier execution_barrier(system);
  m_last_load_timings.execution_barrier_ms = execution_barrier.WaitMilliseconds();
  Slot* slot = Find(frame);
  if (!slot || slot->state_size == 0)
  {
    ERROR_LOG_FMT(CORE, "Rollback: no snapshot for frame {}", frame);
    return false;
  }

  // Restore RAM before device state: some devices (emulated Bluetooth) re-parse pending IOS
  // requests from RAM while loading, and live RAM may already hold a newer reply. The device-state
  // load writes no RAM in a session, since EFB copies stay on the GPU.
  auto& memory = system.GetMemory();
  const auto dirty_start = std::chrono::steady_clock::now();
  m_changed_blocks.clear();
  m_changed_ranges.clear();
  if (slot->page_snapshot_id != 0)
  {
    // The undo logs of this snapshot and every newer one list exactly the pages written since.
    if (!DirtyPages::Restore(
            slot->page_snapshot_id,
            [this](u32 physical, u32 length) {
              m_changed_ranges.push_back({physical, length});
              const u32 page = physical & ~static_cast<u32>(RESTORE_PAGE - 1);
              if (m_changed_blocks.empty() || m_changed_blocks.back() != page)
                m_changed_blocks.push_back(page);
            },
            m_tracked_bitmap_clear, frame))
    {
      ERROR_LOG_FMT(CORE, "Rollback: the dirty-page snapshot for frame {} is gone", frame);
      return false;
    }
    if (trace)
      NOTICE_LOG_FMT(CORE, "Rollback trace: load frame {} dirty restore done", frame);
  }
  else
  {
    RestoreRam(memory.GetRAM(), slot->mem1, MEM1_VIRTUAL, &m_changed_blocks, &m_changed_ranges);
    if (memory.GetEXRAM() && !slot->mem2.empty())
      RestoreRam(memory.GetEXRAM(), slot->mem2, MEM2_VIRTUAL, &m_changed_blocks, &m_changed_ranges);
  }
  if (m_audit_untracked_writes)
  {
    AuditRestoredRamPages(memory.GetRAM(), memory.GetRamSize(), slot->audit_mem1_pages, 0, frame,
                          "RAM restore");
    if (memory.GetEXRAM())
      AuditRestoredRamPages(memory.GetEXRAM(), memory.GetExRamSize(), slot->audit_mem2_pages,
                            0x10000000u, frame, "RAM restore");
  }
  std::sort(m_changed_blocks.begin(), m_changed_blocks.end());
  m_changed_blocks.erase(std::unique(m_changed_blocks.begin(), m_changed_blocks.end()),
                         m_changed_blocks.end());
  m_last_load_timings.dirty_pages_ms = MillisecondsSince(dirty_start);
  m_last_load_timings.changed_blocks = m_changed_blocks.size();
  m_last_load_timings.changed_cache_lines = m_changed_ranges.size();
  // Standalone ARAM is not CPU executable memory. FakeVMEM's canonical bitmap range maps back to
  // its physical fastmem address for invalidation.
  auto& jit = system.GetJitInterface();
  const std::size_t jit_blocks_before = jit.GetBlockCount();
  const auto jit_start = std::chrono::steady_clock::now();
  for (const ChangedMemoryRange& range : m_changed_ranges)
  {
    const u32 physical = range.physical_address;
    if (physical >= DirtyPages::GC_ARAM_PHYSICAL &&
        physical < DirtyPages::GC_ARAM_PHYSICAL + DSP::ARAM_SIZE)
      continue;
    const bool fake_vmem = physical >= DirtyPages::GC_FAKE_VMEM_PHYSICAL &&
                           physical < DirtyPages::GC_FAKE_VMEM_PHYSICAL + memory.GetFakeVMemSize();
    if (fake_vmem)
      jit.InvalidateICache(physical | 0x60000000u, range.length, true);
    else
      jit.EraseBlocksForRestoredRange(physical, range.length);
  }
  ForgetLearnedJitAddresses(system, m_changed_blocks);
  m_last_load_timings.jit_ms = MillisecondsSince(jit_start);
  const std::size_t jit_blocks_after = jit.GetBlockCount();
  m_last_load_timings.jit_blocks_invalidated =
      jit_blocks_before > jit_blocks_after ? jit_blocks_before - jit_blocks_after : 0;
  const auto l1_start = std::chrono::steady_clock::now();
  std::memcpy(memory.GetL1Cache(), slot->l1_cache.data(), slot->l1_cache.size());
  m_last_load_timings.l1_ms = MillisecondsSince(l1_start);

  bool ok;
  {
    const auto phase_start = std::chrono::steady_clock::now();
    SnapshotScope scope;
    VideoCommon_SetSnapshotRedisplays(redisplay);
    ok = State::LoadFromBufferForRollback(system,
                                          std::span<u8>(slot->state.data(), slot->state_size));
    if (trace)
      NOTICE_LOG_FMT(CORE, "Rollback trace: load frame {} state done (ok={})", frame, ok);
    VideoCommon_SetSnapshotRedisplays(false);
    m_last_load_timings.state_ms = MillisecondsSince(phase_start);
    m_last_load_timings.state_bytes = slot->state_size;
  }
  if (!ok)
  {
    ERROR_LOG_FMT(CORE, "Rollback: snapshot for frame {} did not load", frame);
    return false;
  }
  if (m_audit_untracked_writes || m_audit_video)
  {
    if (m_audit_untracked_writes)
    {
      AuditRestoredRamPages(memory.GetRAM(), memory.GetRamSize(), slot->audit_mem1_pages, 0, frame,
                            "state restore");
      if (memory.GetEXRAM())
        AuditRestoredRamPages(memory.GetEXRAM(), memory.GetExRamSize(), slot->audit_mem2_pages,
                              0x10000000u, frame, "state restore");
    }

    if (m_audit_video && g_framebuffer_manager && slot->audit_efb.valid)
    {
      if (!g_framebuffer_manager->CaptureEFBRollbackAudit(&m_efb_audit_scratch))
      {
        ERROR_LOG_FMT(CORE, "Rollback EFB audit: frame {} post-load readback failed", frame);
      }
      else
      {
        const bool report_details = !m_efb_mismatch_reported;
        const bool color_differs = AuditEFBImage(slot->audit_efb.color, m_efb_audit_scratch.color,
                                                 frame, "color", report_details);
        const bool depth_differs = AuditEFBImage(slot->audit_efb.depth, m_efb_audit_scratch.depth,
                                                 frame, "depth", report_details);
        if (color_differs || depth_differs)
          m_efb_mismatch_reported = true;
        else if (!m_efb_match_reported)
        {
          m_efb_match_reported = true;
          NOTICE_LOG_FMT(
              CORE,
              "Rollback EFB audit: frame {} saved/post-load color and depth match "
              "(color={} bytes {:016x}, depth={} bytes {:016x}, {}x{}x{})",
              frame, slot->audit_efb.color.bytes.size(),
              XXH3_64bits(slot->audit_efb.color.bytes.data(), slot->audit_efb.color.bytes.size()),
              slot->audit_efb.depth.bytes.size(),
              XXH3_64bits(slot->audit_efb.depth.bytes.data(), slot->audit_efb.depth.bytes.size()),
              slot->audit_efb.color.width, slot->audit_efb.color.height,
              slot->audit_efb.color.layers);
        }
      }
    }

    // Compare the state that was just loaded with an immediate reserialization. The rollback
    // which later reaches a diagnostic mismatch may have started several frames earlier.
    if (m_audit_video && slot->audit_efb.valid && frame >= 0)
    {
      State::RollbackStateLayout loaded_layout;
      std::size_t loaded_size;
      {
        SnapshotScope scope;
        loaded_size =
            State::SaveToBufferForRollback(system, m_state_roundtrip_audit, &loaded_layout);
      }
      if (loaded_size == 0)
      {
        ERROR_LOG_FMT(CORE, "Rollback state roundtrip: frame {} could not reserialize", frame);
      }
      else
      {
        const bool differs = AuditStateRoundTrip(
            std::span<const u8>(slot->state.data(), slot->state_size), slot->state_layout,
            std::span<const u8>(m_state_roundtrip_audit.data(), loaded_size), loaded_layout, frame,
            "state roundtrip", !m_state_byte_details_reported);
        if (differs)
          m_state_byte_details_reported = true;
      }
    }
  }
  // The NAND journal must have undone every change since this snapshot.
  const auto journal_start = std::chrono::steady_clock::now();
  if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system); nand && nand->TakeJournalError())
  {
    ERROR_LOG_FMT(CORE, "Rollback: the NAND could not be restored to frame {}", frame);
    return false;
  }
  m_last_load_timings.journal_ms = MillisecondsSince(journal_start);

  if (trace)
    NOTICE_LOG_FMT(CORE, "Rollback trace: load frame {} complete", frame);

  // Later frames are about to be re-run and re-saved, so drop their snapshots.
  for (Slot& other : m_slots)
  {
    if (other.frame > frame ||
        (other.page_snapshot_id != 0 && !DirtyPages::Has(other.page_snapshot_id)))
      Forget(&other);
  }
  m_next = (static_cast<std::size_t>(slot - m_slots.data()) + 1) % m_slots.size();
  return true;
}

bool SnapshotRing::Capture(Core::System& system, MachineImage* image, bool stop_journal)
{
  SnapshotExecutionBarrier execution_barrier(system);
  Common::UniqueBuffer<u8> state;
  std::size_t state_size;
  {
    SnapshotScope scope;
    state_size = State::SaveToBufferForRollback(system, state);
  }
  if (state_size == 0)
    return false;
  image->state.assign(state.data(), state.data() + state_size);
  auto& memory = system.GetMemory();
  image->mem1.assign(memory.GetRAM(), memory.GetRAM() + memory.GetRamSize());
  if (memory.GetEXRAM())
    image->mem2.assign(memory.GetEXRAM(), memory.GetEXRAM() + memory.GetExRamSize());
  else
    image->mem2.clear();
  image->l1_cache.assign(memory.GetL1Cache(), memory.GetL1Cache() + memory.GetL1CacheSize());
  if (stop_journal)
  {
    if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system))
      nand->JournalStop();
  }
  return true;
}

bool SnapshotRing::LoadImage(Core::System& system, MachineImage image, s64 frame)
{
  auto& memory = system.GetMemory();
  if (image.state.empty() || image.mem1.size() != memory.GetRamSize() ||
      image.mem2.size() != (memory.GetEXRAM() ? memory.GetExRamSize() : 0) ||
      image.l1_cache.size() != memory.GetL1CacheSize())
  {
    ERROR_LOG_FMT(CORE, "Rollback: keyframe for frame {} doesn't fit this machine", frame);
    return false;
  }
  Slot* slot = Find(frame);
  if (!slot)
  {
    slot = &m_slots[m_next];
    m_next = (m_next + 1) % m_slots.size();
  }
  // Store as a full-copy slot (page_snapshot_id 0), so LoadSlot restores mem1/mem2 directly.
  Forget(slot);
  slot->state.reset(image.state.size());
  std::memcpy(slot->state.data(), image.state.data(), image.state.size());
  slot->state_size = image.state.size();
  slot->mem1 = std::move(image.mem1);
  slot->mem2 = std::move(image.mem2);
  slot->l1_cache = std::move(image.l1_cache);
  slot->frame = frame;
  slot->nand_journal_mark = 0;
  // A joining machine has nothing on screen yet, so show the keyframe's frame.
  return LoadSlot(system, frame, true);
}

}  // namespace Rollback
