// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstddef>

#include "Common/CommonTypes.h"

namespace RollbackVideoTiming
{
// Updated once per emulated frame. Disabled scopes pay only a thread-local branch rather than an
// out-of-line call and an atomic load at every FIFO/vertex/texture operation.
extern thread_local bool g_scope_collection_enabled;

enum class Phase : std::size_t
{
  FifoSave,
  FifoReplaySave,
  FifoLoad,
  SubmitSave,
  SubmitReplaySave,
  SubmitLoad,
  NormalFifo,
  NormalVertices,
  NormalFlush,
  NormalTextures,
  NormalPresent,
  Count
};

// Session-scoped host diagnostics, never serialized. Normal rendering is sampled on the
// single CPU/GPU thread; snapshot copies/submissions are measured on every save/load.
void Configure(bool enabled);
void BeginFrame(bool replay);
void ReportAndReset();

class Scope final
{
public:
  explicit Scope(Phase phase, bool eligible = true) : m_phase(phase)
  {
    if (eligible && g_scope_collection_enabled)
      Activate();
  }
  ~Scope()
  {
    if (m_active)
      Finish();
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

private:
  void Activate();
  void Finish();
  Phase m_phase;
  bool m_active = false;
  u64 m_configuration = 0;
  u64 m_child_ns = 0;
  Scope* m_parent = nullptr;
  std::chrono::steady_clock::time_point m_start;
};
}  // namespace RollbackVideoTiming
