// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/RollbackVideoTiming.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <utility>

#include "Common/Logging/Log.h"
#include "VideoCommon/VideoState.h"

namespace RollbackVideoTiming
{
namespace
{
constexpr u64 SAMPLE_PERIOD = 16;
// Low bit enables collection; the remaining bits distinguish sessions, including consecutive
// sessions on a reused CPU thread. Configuration can be changed from the session-control thread.
std::atomic<u64> s_configuration{0};
struct Measurement
{
  u64 calls = 0;
  u64 inclusive_ns = 0;
  u64 exclusive_ns = 0;
  u64 max_ns = 0;
};
struct Collector
{
  u64 configuration = 0;
  u64 sequence = 0;
  u64 normal_frames = 0;
  u64 sampled_frames = 0;
  bool sample_normal = false;
  Scope* parent = nullptr;
  std::array<Measurement, static_cast<std::size_t>(Phase::Count)> phases{};
};
thread_local Collector s_collector;
}  // namespace

thread_local bool g_scope_collection_enabled = false;

void Configure(bool enabled)
{
  const u64 next = (s_configuration.load(std::memory_order_relaxed) + 2) & ~u64{1};
  s_configuration.store(next | static_cast<u64>(enabled), std::memory_order_release);
}

void BeginFrame(bool replay)
{
  const u64 configuration = s_configuration.load(std::memory_order_acquire);
  if (s_collector.configuration != configuration)
  {
    s_collector = {};
    s_collector.configuration = configuration;
  }
  g_scope_collection_enabled = (configuration & 1) != 0;
  s_collector.sample_normal = false;
  if (!(configuration & 1) || replay)
    return;
  ++s_collector.normal_frames;
  s_collector.sample_normal = s_collector.sequence++ % SAMPLE_PERIOD == 0;
  s_collector.sampled_frames += s_collector.sample_normal;
}

void Scope::Activate()
{
  m_configuration = s_collector.configuration;
  if (m_phase >= Phase::NormalFifo &&
      (!s_collector.sample_normal || VideoCommon_IsRollbackSnapshot() ||
       VideoCommon_IsSkippingRender()))
    return;
  m_active = true;
  m_parent = std::exchange(s_collector.parent, this);
  m_start = std::chrono::steady_clock::now();
}

void Scope::Finish()
{
  const u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - m_start)
                                      .count());
  s_collector.parent = m_parent;
  if (m_parent)
    m_parent->m_child_ns += ns;
  if (m_configuration != s_configuration.load(std::memory_order_relaxed))
    return;
  auto& value = s_collector.phases[static_cast<std::size_t>(m_phase)];
  ++value.calls;
  value.inclusive_ns += ns;
  value.exclusive_ns += ns - std::min(ns, m_child_ns);
  value.max_ns = std::max(value.max_ns, ns);
}

void ReportAndReset()
{
  const u64 configuration = s_configuration.load(std::memory_order_acquire);
  if (!(configuration & 1) || s_collector.configuration != configuration)
    return;
  constexpr std::array<const char*, 6> snapshot_names = {
      "fifo-copy save",      "fifo-copy replay-save",      "fifo-copy load",
      "backend-submit save", "backend-submit replay-save", "backend-submit load"};
  for (std::size_t i = 0; i < snapshot_names.size(); ++i)
  {
    const auto& value = s_collector.phases[i];
    NOTICE_LOG_FMT(CORE, "Rollback video timing: {} {:.4f}/{:.4f} ms avg/max, {} calls",
                   snapshot_names[i], value.calls ? value.inclusive_ns / (1e6 * value.calls) : 0.0,
                   value.max_ns / 1e6, value.calls);
  }
  NOTICE_LOG_FMT(
      CORE,
      "Rollback video timing: sampled {}/{} normal frames (1/{}); CPU wall time, "
      "not GPU completion; inclusive times overlap, exclusive times exclude nested scopes",
      s_collector.sampled_frames, s_collector.normal_frames, SAMPLE_PERIOD);
  constexpr std::array<const char*, 5> normal_names = {"fifo-service", "vertex-decode",
                                                       "vertex-flush", "texture-load", "present"};
  for (std::size_t i = 0; i < normal_names.size(); ++i)
  {
    const auto& value = s_collector.phases[static_cast<std::size_t>(Phase::NormalFifo) + i];
    const double denominator = 1e6 * s_collector.sampled_frames;
    NOTICE_LOG_FMT(CORE,
                   "Rollback video timing: normal {} {:.4f}/{:.4f} ms inclusive/exclusive per "
                   "sampled frame; {} calls, {:.4f} ms max call",
                   normal_names[i], denominator ? value.inclusive_ns / denominator : 0.0,
                   denominator ? value.exclusive_ns / denominator : 0.0, value.calls,
                   value.max_ns / 1e6);
  }
  s_collector.phases = {};
  s_collector.normal_frames = 0;
  s_collector.sampled_frames = 0;
}
}  // namespace RollbackVideoTiming
