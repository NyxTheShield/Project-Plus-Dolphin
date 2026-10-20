// Copyright 2026 Project+ Rollback Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Rollback/BrawlControllerProfile.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

namespace WiimoteEmu
{
struct SerializedWiimoteState;
}

namespace Rollback
{
enum class FrameBoundary : u8
{
  BrawlHook = 0,
  VIBeginField = 1,
  VIEndField = 2,
  VINewField = 3,
  VIPreNewField = 4,
  VIPreSIPoll = 5,
};

enum class ControllerType : u8
{
  GameCube = 0,
  Wiimote = 1,
};

// Per-session diagnostics and experimental switches. These are supplied by the launch ticket for
// managed Brawlback sessions, or by current-run command-line configuration for manual NetPlay.
// They are deliberately not discovered from persistent configuration inside the rollback core.
struct GekkoSessionOptions
{
  bool debug_p2_cstick = false;
  bool simulate_remote_p2 = false;
  bool stress_test = false;
  bool enable_random_inputs = false;
  bool disable_project_plus_music = false;
  bool native_stress = false;
  u32 stress_interval = 1;
  bool compare_confirmed_ram = false;
  bool remote_ram_match = false;
  bool compare_confirmed_video = false;
  bool stop_ram_compare_on_mismatch = false;
  bool deep_logging = false;
  bool coarse_fifo_timing = false;
  bool late_vi_guard = true;
  FrameBoundary frame_boundary = FrameBoundary::VIPreSIPoll;
  bool tracked_bitmap_clear = true;
  bool full_scan_benchmark = false;
  bool full_scan_keep_tracking = false;
  bool parallel_dirty_compare = true;
  std::vector<ControllerType> controller_types;
  std::vector<BrawlControllerProfile> brawl_controller_profiles;
  // Optional NPR source for this process's own local player. The recorded seat matching
  // local_player is submitted through the normal networked Gekko actor.
  std::string replay_input_path;
};

// Wire format for GameCube controller input over GekkoNet (8 bytes)
// [Buttons Hi, Buttons Lo, MainStick X, MainStick Y, CStick X, CStick Y, Trigger L, Trigger R]
struct WirePad
{
  u8 data[8];
};

WirePad EncodePad(const GCPadStatus& status);
GCPadStatus DecodePad(const WirePad& pad);

// Rollback input query from SI controller polling
std::optional<GCPadStatus> GetRollbackPad(int port);
void CaptureLocalWiimoteState(const WiimoteEmu::SerializedWiimoteState& state);
std::optional<WiimoteEmu::SerializedWiimoteState> GetRollbackWiimoteState(int port);

// Boundary callback called by the Brawl HLE hook at 0x80017504.
void OnFrameBoundary(const Core::CPUThreadGuard& guard);

// VI callbacks latch a candidate boundary. CPU dispatchers run it only after CoreTiming::Advance
// has returned, so a rollback load never replaces timing state beneath an active Advance call.
void SignalVIBoundary(FrameBoundary boundary);
bool IsVIBoundaryPending();
void RunPendingVIBoundary(Core::System& system);

// The JIT may run past a CoreTiming event by the remainder of its current block. Execute the final
// half-line before the selected VI boundary in the interpreter so it is reached at an
// instruction-precise point independent of each client's JIT cache and block layout.
void UpdatePreciseVITailArm(bool next_is_begin_field, bool next_is_end_field,
                            bool next_is_new_field, bool next_is_si_poll);
bool ConsumePreciseVIGuardRequest();
void ArmPreciseVITailFromGuard();
bool IsPreciseVITailArmed();
bool IsDeterminismDiagnosticsEnabled();
// Keeps a bounded history of host-subsystem writes into guest RAM for mismatch diagnostics.
void RecordHostMemoryWrite(Core::System& system, std::string_view source, u32 address,
                           std::size_t size, u64 source_offset = 0);

// Session management. Dolphin negotiates endpoints in the lobby; GekkoNet owns gameplay traffic.
bool StartGekkoSession(const std::string& game_name, u32 session_id, int players, int local_player,
                       const std::vector<std::string>& player_endpoints, int local_delay,
                       int prediction_window, const GekkoSessionOptions& options);
void StopGekkoSession();
bool IsGekkoSessionActive();
bool IsGekkoFrameBoundary(FrameBoundary boundary);
// Queues a local presentation-only change for the next stable rollback boundary. The callback runs
// on the CPU thread, allowing Dolphin's normal GPU-thread quiescence path to stop FIFO safely.
bool QueueLocalPresentationChange(std::function<void()> change);
void SetGekkoLocalDelay(int local_delay);
// Deep-logging probe: pairs the next host presentation with the local input frame selected by
// GekkoNet. The timestamp uses steady_clock nanoseconds and never enters emulated state.
void RecordGekkoInputPresentation(u64 present_time_ns);

// Netplay replays store the port-ordered inputs emitted by GekkoAdvanceEvent once per logical
// frame. They deliberately do not use Dolphin's SI-poll-based DTM stream.
bool BeginNetplayReplayRecording(const std::string& game_key);
bool SaveNetplayReplay(const std::string& path);
bool LoadNetplayReplay(const std::string& path);
bool StartNetplayReplaySession(bool disable_project_plus_music);
std::string GetNetplayReplayGameID();
int GetNetplayReplayPlayerCount();
std::vector<ControllerType> GetNetplayReplayControllerTypes();
u64 GetNetplayReplayInitialRTC();
std::vector<u8> GetNetplayReplaySysconf();
std::vector<BrawlControllerProfile> GetNetplayReplayBrawlControllerProfiles();
bool IsNetplayReplayRecording();
bool IsNetplayReplayPlaying();
bool IsNetplayReplayInputActive();
void StopNetplayReplay();

}  // namespace Rollback
