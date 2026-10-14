// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VideoState.h"

#include "Common/ChunkFile.h"
#include "Core/System.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/BPStructs.h"
#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/FrameDumper.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/TMEM.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/Widescreen.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/XFStateManager.h"
#include "VideoCommon/VideoBackendBase.h"

#include <atomic>
#include "Common/Logging/Log.h"

static std::atomic<bool> s_skip_render{false};
static std::atomic<bool> s_skip_present{false};
static std::atomic<bool> s_rollback_snapshot{false};
static std::atomic<bool> s_snapshot_redisplays{false};

void VideoCommon_SetRollbackSnapshot(bool snapshot)
{
  s_rollback_snapshot.store(snapshot, std::memory_order_relaxed);
}

bool VideoCommon_IsRollbackSnapshot()
{
  return s_rollback_snapshot.load(std::memory_order_relaxed);
}

void VideoCommon_SetSnapshotRedisplays(bool redisplays)
{
  s_snapshot_redisplays.store(redisplays, std::memory_order_relaxed);
}

bool VideoCommon_LoadRedisplays()
{
  return !s_rollback_snapshot.load(std::memory_order_relaxed) ||
         s_snapshot_redisplays.load(std::memory_order_relaxed);
}

void VideoCommon_SetSkipRender(bool skip)
{
  s_skip_present.store(skip, std::memory_order_relaxed);
  s_skip_render.store(skip, std::memory_order_relaxed);
}

bool VideoCommon_IsSkippingRender()
{
  return s_skip_render.load(std::memory_order_relaxed);
}

bool VideoCommon_IsSkippingPresent()
{
  return s_skip_present.load(std::memory_order_relaxed);
}

void VideoCommon_DoState(PointerWrap& p, std::array<std::size_t, 18>* rollback_section_ends,
                         u8* rollback_layout_base)
{
  const auto note_section_end = [&](std::size_t section) {
    if (rollback_section_ends && rollback_layout_base)
      (*rollback_section_ends)[section] = p.GetOffsetFromPreviousPosition(rollback_layout_base);
  };
  bool software = false;
  p.Do(software);

  if (p.IsReadMode() && software == true)
  {
    // change mode to abort load of incompatible save state.
    p.SetVerifyMode();
  }

  // Orca: on a load, draw any buffered vertices first, while the old registers are still in
  // place. Flushed after the restore, they would be drawn with the snapshot's matrices and TEV
  // state and show up as garbage triangles. See ORCA.md, "Rollback and snapshots".
  if (p.IsReadMode())
    g_vertex_manager->Flush();

  // BP Memory
  p.Do(bpmem);
  p.DoMarker("BP Memory");
  note_section_end(0);

  // CP Memory
  // We don't save g_preprocess_cp_state separately because the GPU should be
  // synced around state save/load.
  p.Do(g_main_cp_state);
  p.DoMarker("CP Memory");
  note_section_end(1);
  if (p.IsReadMode())
    CopyPreprocessCPStateFromMain();

  // XF Memory
  p.Do(xfmem);
  p.DoMarker("XF Memory");
  note_section_end(2);

  // Texture decoder
  p.DoArray(s_tex_mem);
  p.DoMarker("texMem");
  note_section_end(3);

  // TMEM
  TMEM::DoState(p);
  p.DoMarker("TMEM");
  note_section_end(4);

  // FIFO
  auto& system = Core::System::GetInstance();
  system.GetFifo().DoState(p);
  p.DoMarker("Fifo");
  note_section_end(5);

  auto& command_processor = system.GetCommandProcessor();
  command_processor.DoState(p);
  p.DoMarker("CommandProcessor");
  note_section_end(6);

  system.GetPixelEngine().DoState(p);
  p.DoMarker("PixelEngine");
  note_section_end(7);

  // the old way of replaying current bpmem as writes to push side effects to pixel shader manager
  // doesn't really work.
  system.GetPixelShaderManager().DoState(p);
  p.DoMarker("PixelShaderManager");
  note_section_end(8);

  system.GetVertexShaderManager().DoState(p);
  p.DoMarker("VertexShaderManager");
  note_section_end(9);

  system.GetGeometryShaderManager().DoState(p);
  p.DoMarker("GeometryShaderManager");
  note_section_end(10);

  // Orca: always saved, even for rollback: it holds emulated GX state (z slope, cached
  // normal/tangent/binormal). Its flush on load is a no-op after the flush above.
  g_vertex_manager->DoState(p);
  p.DoMarker("VertexManager");
  note_section_end(11);

  if (!s_rollback_snapshot.load(std::memory_order_relaxed))
    g_framebuffer_manager->DoState(p);
  p.DoMarker("FramebufferManager");
  note_section_end(12);

  if (!s_rollback_snapshot.load(std::memory_order_relaxed))
    g_texture_cache->DoState(p);
  p.DoMarker("TextureCache");
  note_section_end(13);

  g_presenter->DoState(p);
  g_frame_dumper->DoState(p);
  p.DoMarker("Presenter");
  note_section_end(14);

  if (!s_rollback_snapshot.load(std::memory_order_relaxed))
    g_bounding_box->DoState(p);
  p.DoMarker("Bounding Box");
  note_section_end(15);

  g_widescreen->DoState(p);
  p.DoMarker("Widescreen");
  note_section_end(16);

  system.GetXFStateManager().DoState(p);
  p.DoMarker("XFStateManager");
  note_section_end(17);

  // Refresh state.
  if (p.IsReadMode())
  {
    // Inform backend of new state from registers.
    BPReload();
    VertexLoaderManager::MarkAllDirty();
  }

  // Orca: a full save submits GPU work through its EFB readback, but a rollback snapshot skips
  // that. Submit encoded work here (no wait): some backends (Metal) otherwise submit only on
  // present, and re-run frames present nothing.
  if (s_rollback_snapshot.load(std::memory_order_relaxed) && !p.IsMeasureMode())
    g_gfx->Flush();
}
