// Copyright 2026 Project+ Rollback Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/GekkoRollback.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_M_X86_64) || defined(__x86_64__)
#include <xmmintrin.h>
#elif defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM64EC))
#include <arm64intr.h>
#endif

#include <fmt/format.h>
#include <gekkonet.h>
#include <xxh3.h>

#include "Common/Config/Config.h"
#include "Common/ENet.h"
#include "Common/FPURoundMode.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/Timer.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/HW/DSP.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/HW/WiimoteEmu/DesiredWiimoteState.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/Uids.h"
#include "Core/PatchEngine.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/DirtyBitmap.h"
#include "Core/Rollback/DirtyPages.h"
#include "Core/Rollback/Rollback.h"
#include "Core/System.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/GCAdapter.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VideoState.h"

namespace Rollback
{
namespace
{
// All physical controller buttons plus the adapter's synchronized calibration event.
constexpr u16 PAD_WIRE_BUTTONS = 0x1F7F | PAD_GET_ORIGIN;
constexpr int MAX_PORTS = 4;
struct WireInput
{
  u8 controller_type = static_cast<u8>(ControllerType::GameCube);
  WirePad pad{};
  u8 wiimote_length = 0;
  std::array<u8, 30> wiimote_data{};
};
static_assert(sizeof(WireInput) == 40);
// Gekko exposes up to 10 rollback frames in the lobby. Keep two additional snapshots so the
// current and confirmed boundary states cannot alias the oldest rollback state in the ring.
constexpr int RING_SNAPSHOT_SLOTS = 12;
constexpr int REMOTE_RAM_MATCH_SNAPSHOT_SLOTS = 64;
constexpr int WAIT_SLEEP_US = 100;
constexpr auto SIMULATED_P2_LATENCY = std::chrono::milliseconds(40);
constexpr u64 STRESS_INPUT_PERIOD_FRAMES = 1;
constexpr u64 P2_INPUT_SPAM_START_FRAME = 500;
// GekkoNet's serialized MsgHeader starts with its u8 PacketType. Artificial rollback stress must
// delay only Inputs packets; delaying InputAck/session-health traffic can stall the remote peer.
constexpr u8 GEKKO_INPUT_PACKET_TYPE = 1;
constexpr std::string_view SIMULATED_P1_ADDRESS = "in-process-p1";

std::mutex s_presentation_change_mutex;
std::function<void()> s_pending_presentation_change;

void RunPendingPresentationChange()
{
  std::function<void()> change;
  {
    std::lock_guard lock(s_presentation_change_mutex);
    change = std::move(s_pending_presentation_change);
    s_pending_presentation_change = {};
  }

  if (!change)
    return;

  NOTICE_LOG_FMT(VIDEO, "Running local presentation change at rollback frame boundary");
  change();
  NOTICE_LOG_FMT(VIDEO, "Local presentation change completed at rollback frame boundary");
}
constexpr std::string_view SIMULATED_P2_ADDRESS = "in-process-p2";
constexpr u32 BRAWL_FRAME_HOOK_ADDR = 0x80017504;
constexpr u32 BRAWL_EXPECTED_OPCODE = 0x90170100;  // stw r0, 0x100(r23)
constexpr std::array<u8, 4> RAM_DIAGNOSTIC_MAGIC{'R', 'M', 'D', 'G'};
constexpr std::array<char, 8> NETPLAY_REPLAY_MAGIC{'B', 'B', 'N', 'P', 'R', '\0', '\r', '\n'};
constexpr u32 NETPLAY_REPLAY_VERSION = 2;
constexpr u8 NETPLAY_REPLAY_HAS_MEM1_PAGE_HASHES = 1;
constexpr u8 NETPLAY_REPLAY_HAS_SYSCONF = 2;
constexpr u8 NETPLAY_REPLAY_MEM1_PAGE_SHIFT = 12;
constexpr std::size_t NETPLAY_REPLAY_MEM1_PAGE_SIZE = std::size_t{1}
                                                      << NETPLAY_REPLAY_MEM1_PAGE_SHIFT;
constexpr std::size_t NETPLAY_REPLAY_SYSCONF_SIZE = 0x4000;
constexpr u8 RAM_DIAGNOSTIC_VERSION = 8;
constexpr u8 RAM_DIAGNOSTIC_DIGEST = 1;
constexpr u8 RAM_DIAGNOSTIC_MISMATCH = 2;
constexpr u8 VIDEO_DIAGNOSTIC_MISMATCH = 3;
constexpr u8 RAM_DIAGNOSTIC_ENABLE = 4;
constexpr u8 RAM_DIAGNOSTIC_ACK = 5;
constexpr u8 DIAGNOSTIC_HAS_RAM = 1;
constexpr u8 DIAGNOSTIC_HAS_VIDEO = 2;
constexpr u8 DIAGNOSTIC_HAS_EFB = 4;
constexpr u8 DIAGNOSTIC_REMOTE_RAM_MATCH = 8;
constexpr std::size_t DIAGNOSTIC_VIDEO_OFFSET =
    112 + State::ROLLBACK_STATE_SECTION_COUNT * sizeof(u64);
constexpr std::size_t DIAGNOSTIC_VIDEO_SECTIONS_OFFSET = DIAGNOSTIC_VIDEO_OFFSET + 2 * sizeof(u64);
constexpr std::size_t DIAGNOSTIC_PPC_SECTIONS_OFFSET =
    DIAGNOSTIC_VIDEO_SECTIONS_OFFSET + State::ROLLBACK_VIDEO_SECTION_COUNT * sizeof(u64);
constexpr std::size_t DIAGNOSTIC_HW_SECTIONS_OFFSET =
    DIAGNOSTIC_PPC_SECTIONS_OFFSET + State::ROLLBACK_PPC_SECTION_COUNT * sizeof(u64);
constexpr std::size_t DIAGNOSTIC_CORE_TIMING_SECTIONS_OFFSET =
    DIAGNOSTIC_HW_SECTIONS_OFFSET + State::ROLLBACK_HW_SECTION_COUNT * sizeof(u64);
constexpr std::size_t DIAGNOSTIC_CPU_FIELDS_OFFSET =
    DIAGNOSTIC_CORE_TIMING_SECTIONS_OFFSET +
    State::ROLLBACK_CORE_TIMING_SECTION_COUNT * sizeof(u64);
constexpr std::size_t RAM_DIAGNOSTIC_PACKET_SIZE = DIAGNOSTIC_CPU_FIELDS_OFFSET + 4 * sizeof(u32);
constexpr std::array<std::string_view, State::ROLLBACK_STATE_SECTION_COUNT>
    ROLLBACK_STATE_SECTION_NAMES{"Movie",   "VideoBackend", "CoreTiming", "HW",
                                 "PowerPC", "Wiimote",      "Gecko"};
constexpr std::array<std::string_view, State::ROLLBACK_VIDEO_SECTION_COUNT>
    ROLLBACK_VIDEO_SECTION_NAMES{"BP",
                                 "CP",
                                 "XF",
                                 "TexMem",
                                 "TMEM",
                                 "FIFO",
                                 "Command",
                                 "PixelEngine",
                                 "PixelShader",
                                 "VertexShader",
                                 "GeometryShader",
                                 "VertexManager",
                                 "Framebuffer",
                                 "TextureCache",
                                 "Presenter",
                                 "BoundingBox",
                                 "Widescreen",
                                 "XFState"};
// These sections are guest-visible emulated GPU state. The remaining sections are host-side
// renderer caches, dirty flags, or presentation history. A peer which rolls back intentionally
// skips host rendering, so those derived sections may follow a different history without the
// emulated machine diverging.
constexpr std::array<bool, State::ROLLBACK_VIDEO_SECTION_COUNT>
    ROLLBACK_VIDEO_SECTION_AUTHORITATIVE{true,  true,  true,  true,  false, true,
                                         true,  true,  false, false, false, true,
                                         false, false, false, false, false, false};
constexpr std::array<std::string_view, State::ROLLBACK_PPC_SECTION_COUNT>
    ROLLBACK_PPC_SECTION_NAMES{"GPR", "PC",        "CR",      "Scalar", "FPR",    "SR",  "SPR",
                               "TLB", "PageTable", "Reserve", "ICache", "DCache", "MMU", "JIT"};
constexpr std::array<std::string_view, State::ROLLBACK_HW_SECTION_COUNT> ROLLBACK_HW_SECTION_NAMES{
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
constexpr std::array<std::string_view, State::ROLLBACK_CORE_TIMING_SECTION_COUNT>
    ROLLBACK_CORE_TIMING_SECTION_NAMES{"Data", "Events"};

bool HandleRamDiagnosticDatagram(const Common::ENet::RollbackDatagram& datagram);

constexpr std::string_view FrameBoundaryName(FrameBoundary boundary)
{
  switch (boundary)
  {
  case FrameBoundary::BrawlHook:
    return "brawl_hook";
  case FrameBoundary::VIBeginField:
    return "vi_begin_field";
  case FrameBoundary::VIEndField:
    return "vi_end_field";
  case FrameBoundary::VINewField:
    return "vi_new_field";
  case FrameBoundary::VIPreNewField:
    return "vi_pre_new_field";
  case FrameBoundary::VIPreSIPoll:
    return "vi_pre_si_poll";
  }
  return "unknown";
}

// Host floating point scope to protect guest FPU rounding modes
u64 HostFloatControl()
{
#if defined(_M_X86_64) || defined(__x86_64__)
  return _mm_getcsr();
#elif defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM64EC))
  return _ReadStatusReg(ARM64_FPCR);
#elif defined(__aarch64__)
  u64 fpcr;
  __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
  return fpcr;
#else
  return 0;
#endif
}

class HostFloatScope
{
public:
  explicit HostFloatScope(PowerPC::PowerPCState& ppc_state) : m_ppc_state(ppc_state)
  {
    Common::FPU::LoadDefaultSIMDState();
  }
  ~HostFloatScope() { PowerPC::RoundingModeUpdated(m_ppc_state); }
  HostFloatScope(const HostFloatScope&) = delete;
  HostFloatScope& operator=(const HostFloatScope&) = delete;

private:
  PowerPC::PowerPCState& m_ppc_state;
};

struct QueuedEvent
{
  GekkoGameEventType type = GekkoEmptyGameEvent;
  int frame = -1;
  std::vector<unsigned char> inputs;
  unsigned int* checksum = nullptr;
  unsigned int* state_len = nullptr;
  bool rolling_back = false;
  bool running_ahead = false;
};

struct SimulatedPacket
{
  std::chrono::steady_clock::time_point delivery_time;
  u64 delivery_frame = 0;
  bool frame_delayed = false;
  std::vector<char> data;
};

struct DelayedNativePacket
{
  u64 delivery_frame = 0;
  Common::ENet::RollbackDatagram datagram;
};

struct SampledLocalInput
{
  s64 frame = 0;
  WireInput input{};
  u64 sample_time_ns = 0;
};

struct InputLatencyFrame
{
  int frame = -1;
  u64 sample_time_ns = 0;
  u64 selected_time_ns = 0;
  u64 si_relatched_time_ns = 0;
  u64 guest_resume_time_ns = 0;
};

std::deque<SimulatedPacket> s_packets_to_main;
std::deque<SimulatedPacket> s_packets_to_fake;
std::vector<GekkoNetResult*> s_main_results;
std::vector<GekkoNetResult*> s_fake_results;
std::vector<GekkoNetResult*> s_shared_results;
std::deque<DelayedNativePacket> s_delayed_native_packets;
u64 s_simulated_link_frame = 0;
u64 s_last_stress_peer_update_frame = 0;
u64 s_stress_packet_delay_frames = 0;
bool s_stress_mode = false;
bool s_stress_frame_delay_active = false;

bool ParsePeerEndpoint(std::string_view endpoint, ENetAddress* address)
{
  const size_t colon = endpoint.rfind(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 == endpoint.size())
    return false;

  unsigned int port = 0;
  const char* port_begin = endpoint.data() + colon + 1;
  const char* port_end = endpoint.data() + endpoint.size();
  const auto [end, error] = std::from_chars(port_begin, port_end, port);
  if (error != std::errc{} || end != port_end || port == 0 || port > 65535)
    return false;

  const std::string host(endpoint.substr(0, colon));
  address->port = static_cast<u16>(port);
  return enet_address_set_host(address, host.c_str()) == 0;
}

void SharedAdapterSend(GekkoNetAddress* address, const char* data, int length)
{
  if (!address || address->size != sizeof(ENetAddress) || length < 0)
    return;

  ENetAddress endpoint{};
  std::memcpy(&endpoint, address->data, sizeof(endpoint));
  if (!Common::ENet::SendRollbackDatagram(endpoint, data, static_cast<size_t>(length)))
    WARN_LOG_FMT(CORE, "GekkoNet: failed to send shared-socket datagram");
}

GekkoNetResult** SharedAdapterReceive(int* length)
{
  s_shared_results.clear();
  for (Common::ENet::RollbackDatagram& datagram : Common::ENet::DrainRollbackDatagrams())
  {
    if (HandleRamDiagnosticDatagram(datagram))
      continue;

    const bool is_input_packet =
        !datagram.payload.empty() && datagram.payload.front() == GEKKO_INPUT_PACKET_TYPE;
    if (s_stress_mode && s_stress_frame_delay_active && is_input_packet)
    {
      s_delayed_native_packets.push_back(
          {s_simulated_link_frame + s_stress_packet_delay_frames, std::move(datagram)});
      continue;
    }

    auto* result = static_cast<GekkoNetResult*>(std::malloc(sizeof(GekkoNetResult)));
    result->addr.size = sizeof(ENetAddress);
    result->addr.data = std::malloc(sizeof(ENetAddress));
    std::memcpy(result->addr.data, &datagram.address, sizeof(ENetAddress));
    result->data_len = static_cast<unsigned int>(datagram.payload.size());
    result->data = std::malloc(datagram.payload.size());
    std::memcpy(result->data, datagram.payload.data(), datagram.payload.size());
    s_shared_results.push_back(result);
  }

  while (!s_delayed_native_packets.empty() &&
         s_delayed_native_packets.front().delivery_frame <= s_simulated_link_frame)
  {
    Common::ENet::RollbackDatagram datagram = std::move(s_delayed_native_packets.front().datagram);
    s_delayed_native_packets.pop_front();
    auto* result = static_cast<GekkoNetResult*>(std::malloc(sizeof(GekkoNetResult)));
    result->addr.size = sizeof(ENetAddress);
    result->addr.data = std::malloc(sizeof(ENetAddress));
    std::memcpy(result->addr.data, &datagram.address, sizeof(ENetAddress));
    result->data_len = static_cast<unsigned int>(datagram.payload.size());
    result->data = std::malloc(datagram.payload.size());
    std::memcpy(result->data, datagram.payload.data(), datagram.payload.size());
    s_shared_results.push_back(result);
  }
  *length = static_cast<int>(s_shared_results.size());
  return s_shared_results.data();
}

void SharedAdapterFree(void* data)
{
  std::free(data);
}

GekkoNetAdapter s_shared_adapter{SharedAdapterSend, SharedAdapterReceive, SharedAdapterFree};

void QueueSimulatedPacket(std::deque<SimulatedPacket>* destination, const char* data, int length)
{
  SimulatedPacket packet;
  packet.data.assign(data, data + length);
  if (s_stress_mode && s_stress_frame_delay_active)
  {
    packet.delivery_frame = s_simulated_link_frame + s_stress_packet_delay_frames;
    packet.frame_delayed = true;
  }
  else
  {
    // Stress mode keeps handshake traffic immediate. Frame-delayed delivery begins only after
    // Gekko reports that the session has started, avoiding a frame-zero synchronization deadlock.
    packet.delivery_time = std::chrono::steady_clock::now() +
                           (s_stress_mode ? std::chrono::milliseconds(0) : SIMULATED_P2_LATENCY);
  }
  destination->push_back(std::move(packet));
}

void MainAdapterSend(GekkoNetAddress*, const char* data, int length)
{
  QueueSimulatedPacket(&s_packets_to_fake, data, length);
}

void FakeAdapterSend(GekkoNetAddress*, const char* data, int length)
{
  QueueSimulatedPacket(&s_packets_to_main, data, length);
}

GekkoNetResult** ReceiveSimulatedPackets(std::deque<SimulatedPacket>* inbox,
                                         std::vector<GekkoNetResult*>* results,
                                         std::string_view sender, int* length)
{
  results->clear();
  const auto now = std::chrono::steady_clock::now();
  while (!inbox->empty())
  {
    const SimulatedPacket& front = inbox->front();
    const bool ready = front.frame_delayed ? s_simulated_link_frame >= front.delivery_frame :
                                             front.delivery_time <= now;
    if (!ready)
      break;
    SimulatedPacket packet = std::move(inbox->front());
    inbox->pop_front();

    auto* result = static_cast<GekkoNetResult*>(std::malloc(sizeof(GekkoNetResult)));
    result->addr.size = static_cast<unsigned int>(sender.size());
    result->addr.data = std::malloc(sender.size());
    std::memcpy(result->addr.data, sender.data(), sender.size());
    result->data_len = static_cast<unsigned int>(packet.data.size());
    result->data = std::malloc(packet.data.size());
    std::memcpy(result->data, packet.data.data(), packet.data.size());
    results->push_back(result);
  }
  *length = static_cast<int>(results->size());
  return results->data();
}

GekkoNetResult** MainAdapterReceive(int* length)
{
  return ReceiveSimulatedPackets(&s_packets_to_main, &s_main_results, SIMULATED_P2_ADDRESS, length);
}

GekkoNetResult** FakeAdapterReceive(int* length)
{
  return ReceiveSimulatedPackets(&s_packets_to_fake, &s_fake_results, SIMULATED_P1_ADDRESS, length);
}

void SimulatedAdapterFree(void* data)
{
  std::free(data);
}

GekkoNetAdapter s_main_simulated_adapter{MainAdapterSend, MainAdapterReceive, SimulatedAdapterFree};
GekkoNetAdapter s_fake_simulated_adapter{FakeAdapterSend, FakeAdapterReceive, SimulatedAdapterFree};

void ResetSimulatedLink()
{
  s_packets_to_main.clear();
  s_packets_to_fake.clear();
  s_main_results.clear();
  s_fake_results.clear();
  s_delayed_native_packets.clear();
  s_simulated_link_frame = 0;
  s_last_stress_peer_update_frame = 0;
  s_stress_packet_delay_frames = 0;
  s_stress_mode = false;
  s_stress_frame_delay_active = false;
}

struct RamDigest
{
  int frame = -1;
  bool ram_valid = false;
  u64 root = 0;
  u64 mem1 = 0;
  u64 mem2 = 0;
  u64 aram = 0;
  u64 fake_vmem = 0;
  u64 l1 = 0;
  u64 inputs = 0;
  u64 state = 0;
  u64 core_ticks = 0;
  u64 timebase = 0;
  u64 fake_tb_start_value = 0;
  u64 fake_tb_start_ticks = 0;
  u32 decrementer = 0;
  std::array<u64, State::ROLLBACK_STATE_SECTION_COUNT> state_sections{};
  bool video_valid = false;
  bool efb_valid = false;
  u64 efb_color = 0;
  u64 efb_depth = 0;
  std::array<u64, State::ROLLBACK_VIDEO_SECTION_COUNT> video_sections{};
  std::array<u64, State::ROLLBACK_PPC_SECTION_COUNT> ppc_sections{};
  std::array<u64, State::ROLLBACK_HW_SECTION_COUNT> hw_sections{};
  std::array<u64, State::ROLLBACK_CORE_TIMING_SECTION_COUNT> core_timing_sections{};
  u32 pc = 0;
  u32 npc = 0;
  u32 exceptions = 0;
  s32 downcount = 0;
  int sends = 0;
  u32 acknowledged_players = 0;
};

struct DiagnosticEvent
{
  GekkoGameEventType type = GekkoEmptyGameEvent;
  int frame = -1;
  bool rolling_back = false;
  bool running_ahead = false;
  u64 inputs = 0;
  u64 ticks = 0;
  u64 nand_mark = 0;
};

struct HostMemoryWriteEvent
{
  std::string_view source;
  u32 address = 0;
  std::size_t size = 0;
  int frame = -1;
  bool resimulating = false;
  u64 ticks = 0;
  u64 data_hash = 0;
  u64 source_offset = 0;
};

struct GekkoManager
{
  std::recursive_mutex mutex;
  std::mutex input_latency_mutex;
  GekkoSession* session = nullptr;
  GekkoSession* simulated_peer_session = nullptr;
  std::unique_ptr<SnapshotRing> ring;

  int players = 2;
  int local_player = 1;
  int local_handle = -1;
  int simulated_peer_p2_handle = -1;
  int remote_handle = -1;
  bool native_adapter_active = false;
  bool local_uses_gc_adapter = false;
  std::array<ControllerType, MAX_PORTS> controller_types{};
  std::vector<BrawlControllerProfile> brawl_controller_profiles;
  bool debug_p2_cstick = false;
  bool simulate_remote_p2 = false;
  bool stress_test = false;
  bool enable_random_inputs = false;
  bool disable_project_plus_music = false;
  bool native_stress = false;
  u32 stress_interval = 1;
  u32 stress_rollback_frames = 0;
  bool tracked_bitmap_clear = false;
  bool full_scan_benchmark = false;
  bool full_scan_keep_tracking = false;
  bool compare_confirmed_ram = false;
  // Lets host-I/O integrations avoid taking the manager mutex in ordinary sessions.
  std::atomic<bool> track_host_memory_writes{false};
  bool remote_ram_match = false;
  u32 remote_ram_match_activation_acked_players = 0;
  bool compare_confirmed_video = false;
  bool stop_ram_compare_on_mismatch = false;
  bool ram_diagnostic_dumped = false;
  bool video_diagnostic_dumped = false;
  FrameBoundary frame_boundary = FrameBoundary::VIEndField;
  std::atomic<bool> vi_boundary_pending{false};
  std::atomic<bool> precise_vi_tail{false};
  std::atomic<bool> precise_vi_guard_requested{false};
  bool late_vi_guard = false;
  int configured_local_delay = 0;
  int configured_prediction_window = 0;
  u32 session_id = 0;
  u64 local_input_frame = 0;
  std::vector<int> player_handles;
  std::vector<ENetAddress> remote_endpoints;
  std::deque<QueuedEvent> pending_events;
  std::deque<SampledLocalInput> sampled_local_inputs;
  std::deque<InputLatencyFrame> pending_input_latency_frames;
  std::array<double, 300> perf_input_poll_to_select_ms{};
  std::array<double, 300> perf_input_select_to_si_ms{};
  std::array<double, 300> perf_input_si_to_resume_ms{};
  std::array<double, 300> perf_input_resume_to_present_ms{};
  std::array<double, 300> perf_input_poll_to_present_ms{};
  std::size_t perf_input_latency_count = 0;
  u64 perf_input_latency_dropped = 0;
  u64 perf_input_latency_unmatched_presents = 0;
  std::array<GCPadStatus, MAX_PORTS> latched_pads{};
  std::array<WiimoteEmu::SerializedWiimoteState, MAX_PORTS> latched_wiimotes{};
  WiimoteEmu::SerializedWiimoteState pending_local_wiimote{};
  std::atomic<bool> active{false};
  std::atomic<bool> stop_requested{false};

  std::atomic<bool> netplay_replay_recording{false};
  std::atomic<bool> netplay_replay_playing{false};
  bool netplay_replay_session = false;
  int netplay_replay_players = 0;
  FrameBoundary netplay_replay_boundary = FrameBoundary::VIEndField;
  std::string netplay_replay_game_id;
  std::string netplay_replay_game_key;
  std::map<s32, std::vector<u8>> netplay_replay_frames;
  std::vector<std::pair<s32, std::vector<u8>>> netplay_replay_playback_frames;
  std::size_t netplay_replay_playback_index = 0;
  std::vector<std::pair<s32, WirePad>> replay_local_inputs;
  std::vector<std::pair<s32, std::vector<u8>>> replay_expected_inputs;
  bool replay_input_mismatch_reported = false;
  bool replay_input_environment_active = false;
  u64 netplay_replay_initial_rtc = 0;
  RamDigest netplay_replay_initial_digest{};
  bool netplay_replay_initial_digest_valid = false;
  std::vector<u64> netplay_replay_initial_mem1_page_hashes;
  std::vector<u8> netplay_replay_sysconf;

  std::map<int, RamDigest> ram_digests;
  std::map<std::pair<int, int>, RamDigest> remote_ram_digests;
  std::optional<int> first_ram_mismatch_frame;
  std::optional<int> first_video_mismatch_frame;
  std::optional<int> first_derived_video_mismatch_frame;
  std::optional<int> first_non_ram_video_mismatch_frame;
  std::optional<int> ram_mismatch_details_frame;
  std::optional<int> first_powerpc_mismatch_frame;
  u64 last_advance_input_hash = 0;
  bool last_advance_has_start = false;
  bool last_sampled_start = false;
  std::deque<DiagnosticEvent> diagnostic_events;
  std::deque<HostMemoryWriteEvent> host_memory_writes;
  bool restore_mismatch_reported = false;
  u64 perf_restore_checks = 0;
  u64 perf_restore_mismatches = 0;

  // Pacing
  double speed_scale = 1.0;
  double target_scale = 1.0;
  int timesync_counter = 0;
  bool visible_frame_pacing_pending = false;
  bool deferred_visible_follows_rollback = false;
  std::optional<std::pair<s64, TimePoint>> throttle_reference_before_load;
  Common::PrecisionTimer wait_timer;

  // Rolling diagnostics. These distinguish emulator snapshot cost from time spent waiting for
  // GekkoNet to authorize the next frame.
  std::chrono::steady_clock::time_point perf_window_start{};
  DirtyPages::Counters perf_dirty_pages_start{};
  u64 perf_real_frames = 0;
  u64 perf_replay_frames = 0;
  u64 perf_save_count = 0;
  u64 perf_load_count = 0;
  u64 perf_pump_count = 0;
  double perf_save_ms = 0.0;
  double perf_save_max_ms = 0.0;
  double perf_load_ms = 0.0;
  double perf_load_max_ms = 0.0;
  double perf_pump_ms = 0.0;
  double perf_pump_max_ms = 0.0;
  SnapshotPhaseTimings perf_save_phases{};
  SnapshotPhaseTimings perf_load_phases{};
  std::optional<WireInput> last_local_wire;
  u64 perf_local_input_changes = 0;
  std::array<u64, sizeof(WireInput)> perf_local_byte_changes{};
  std::optional<WireInput> last_applied_local_wire;
  u64 perf_delay_checks = 0;
  u64 perf_delay_matches = 0;
  u64 perf_delay_mismatches = 0;
  u64 perf_delay_missing = 0;
  u64 perf_delay_transitions = 0;
  u64 perf_delay_transition_frames = 0;
  u64 perf_delay_transition_min = 0;
  u64 perf_delay_transition_max = 0;
  std::optional<std::chrono::steady_clock::time_point> frame_execution_start;
  bool frame_execution_resim = false;
  int frame_execution_gekko_frame = -1;
  ResimJitCompileStats frame_execution_jit_start{};
  ResimEventStats frame_execution_event_start{};
  u64 perf_replay_exec_count = 0;
  double perf_replay_exec_ms = 0.0;
  double perf_replay_exec_max_ms = 0.0;
  u64 perf_resim_enter_sync_count = 0;
  double perf_resim_enter_sync_ms = 0.0;
  double perf_resim_enter_sync_max_ms = 0.0;
  u64 perf_resim_exit_sync_count = 0;
  double perf_resim_exit_sync_ms = 0.0;
  double perf_resim_exit_sync_max_ms = 0.0;
  u64 perf_replay_jit_blocks = 0;
  u64 perf_replay_jit_nanoseconds = 0;
  ResimEventStats perf_replay_events{};
  u64 perf_replay_save_count = 0;
  double perf_replay_save_ms = 0.0;
  double perf_replay_save_max_ms = 0.0;
  std::optional<std::chrono::steady_clock::time_point> rollback_burst_start;
  u64 perf_rollback_burst_count = 0;
  double perf_rollback_burst_ms = 0.0;
  double perf_rollback_burst_max_ms = 0.0;
  double perf_burst_load_ms = 0.0;
  double perf_burst_load_max_ms = 0.0;
  double perf_burst_exec_ms = 0.0;
  double perf_burst_exec_max_ms = 0.0;
  double perf_burst_save_ms = 0.0;
  double perf_burst_save_max_ms = 0.0;
  double perf_burst_other_ms = 0.0;
  double perf_burst_other_max_ms = 0.0;
  double current_burst_load_ms = 0.0;
  double current_burst_exec_ms = 0.0;
  double current_burst_save_ms = 0.0;
  std::size_t current_burst_jit_blocks_invalidated = 0;
  u64 current_rollback_replays = 0;
  u64 perf_rollback_depth_total = 0;
  u64 perf_rollback_depth_max = 0;
  std::optional<std::chrono::steady_clock::time_point> last_real_advance;
  u64 perf_real_interval_count = 0;
  double perf_real_interval_ms = 0.0;
  double perf_real_interval_max_ms = 0.0;
  u64 perf_real_gaps_over_20_ms = 0;
  u64 perf_real_gaps_over_25_ms = 0;
  u64 perf_real_gaps_over_33_ms = 0;
  u64 perf_real_gaps_over_50_ms = 0;
};

#pragma pack(push, 1)
struct NetplayReplayHeader
{
  std::array<char, 8> magic{};
  u32 version = NETPLAY_REPLAY_VERSION;
  u32 players = 0;
  u32 frame_boundary = 0;
  u32 bytes_per_frame = 0;
  u64 frame_count = 0;
  std::array<char, 6> game_id{};
  std::array<char, 24> game_key{};
  std::array<u8, 2> reserved{};
  u64 initial_rtc = 0;
  u64 initial_root = 0;
  u64 initial_mem1 = 0;
  u64 initial_mem2 = 0;
  u64 initial_aram = 0;
  u64 initial_fake_vmem = 0;
  u64 initial_l1 = 0;
  s64 initial_core_ticks = 0;
  u64 initial_timebase = 0;
  u64 initial_fake_tb_start_value = 0;
  s64 initial_fake_tb_start_ticks = 0;
  u32 initial_decrementer = 0;
  s32 first_frame = -1;
};
#pragma pack(pop)
static_assert(sizeof(NetplayReplayHeader) == 160);

GekkoManager g_manager;

void LogStartFrameStateDigest(Core::System& system, int frame)
{
  if (!g_manager.compare_confirmed_ram || !g_manager.ring)
    return;

  const std::span<const u8> state = g_manager.ring->LastState();
  const State::RollbackStateLayout& layout = g_manager.ring->LastStateLayout();
  std::array<u64, State::ROLLBACK_STATE_SECTION_COUNT> sections{};
  std::size_t section_start = 0;
  for (std::size_t i = 0; i < sections.size(); ++i)
  {
    const std::size_t section_end = std::min(layout.section_ends[i], state.size());
    if (section_end < section_start)
      break;
    sections[i] = XXH3_64bits(state.data() + section_start, section_end - section_start);
    section_start = section_end;
  }

  if (g_manager.last_advance_has_start)
  {
    const auto& ppc = system.GetPPCState();
    NOTICE_LOG_FMT(CORE,
                   "GekkoNet state trace: START save frame {} state={:016x} sections "
                   "Movie={:016x},Video={:016x},CoreTiming={:016x},HW={:016x},PowerPC={:016x},"
                   "Wiimote={:016x},Gecko={:016x}; ticks={}, timebase={}, pc={:08x}, npc={:08x}",
                   frame, XXH3_64bits(state.data(), state.size()), sections[0], sections[1],
                   sections[2], sections[3], sections[4], sections[5], sections[6],
                   system.GetCoreTiming().GetTicks(), system.GetPowerPC().ReadFullTimeBaseValue(),
                   ppc.pc, ppc.npc);
  }

  if (frame >= 278 && frame <= 281)
  {
    static constexpr std::array<std::string_view, State::ROLLBACK_HW_SECTION_COUNT> names{
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
    std::string hashes;
    std::size_t start = layout.section_ends[2];
    for (std::size_t i = 0; i < names.size(); ++i)
    {
      const std::size_t end = std::min(layout.hw_section_ends[i], state.size());
      if (end < start)
        break;
      hashes += fmt::format("{}{}={:016x}[{}..{}]", hashes.empty() ? "" : ",", names[i],
                            XXH3_64bits(state.data() + start, end - start), start, end);
      start = end;
    }
    NOTICE_LOG_FMT(CORE, "Rollback HW state trace: frame {} {}", frame, hashes);
  }

  if ((frame >= 0 && frame <= 10) || (frame >= 278 && frame <= 281))
  {
    static constexpr std::array<std::string_view, State::ROLLBACK_PPC_SECTION_COUNT> names{
        "GPR", "PC",        "CR",      "Scalar", "FPR",    "SR",  "SPR",
        "TLB", "PageTable", "Reserve", "ICache", "DCache", "MMU", "JIT"};
    std::string hashes;
    std::size_t start = layout.section_ends[3];
    for (std::size_t i = 0; i < names.size(); ++i)
    {
      const std::size_t end = std::min(layout.ppc_section_ends[i], state.size());
      if (end < start)
        break;
      hashes += fmt::format("{}{}={:016x}[{}..{}]", hashes.empty() ? "" : ",", names[i],
                            XXH3_64bits(state.data() + start, end - start), start, end);
      start = end;
    }
    NOTICE_LOG_FMT(CORE, "Rollback PowerPC state trace: frame {} {}", frame, hashes);
  }
}

IOS::HLE::FS::HostFileSystem* HostNand(Core::System& system)
{
  IOS::HLE::EmulationKernel* ios = system.GetIOS();
  return ios ? static_cast<IOS::HLE::FS::HostFileSystem*>(ios->GetFS().get()) : nullptr;
}

std::string_view DiagnosticEventName(GekkoGameEventType type)
{
  switch (type)
  {
  case GekkoSaveEvent:
    return "save";
  case GekkoLoadEvent:
    return "load";
  case GekkoAdvanceEvent:
    return "advance";
  default:
    return "other";
  }
}

void RecordDiagnosticEvent(Core::System& system, const QueuedEvent& event)
{
  if (!g_manager.compare_confirmed_ram)
    return;

  DiagnosticEvent diagnostic;
  diagnostic.type = event.type;
  diagnostic.frame = event.frame;
  diagnostic.rolling_back = event.rolling_back;
  diagnostic.running_ahead = event.running_ahead;
  diagnostic.inputs =
      event.inputs.empty() ? 0 : XXH3_64bits(event.inputs.data(), event.inputs.size());
  diagnostic.ticks = system.GetCoreTiming().GetTicks();
  if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system))
    diagnostic.nand_mark = nand->LastJournalMark();
  g_manager.diagnostic_events.push_back(diagnostic);
  while (g_manager.diagnostic_events.size() > 16)
    g_manager.diagnostic_events.pop_front();
}

void LogDiagnosticEventHistory()
{
  std::string history;
  for (const DiagnosticEvent& event : g_manager.diagnostic_events)
  {
    if (!history.empty())
      history += ", ";
    history += fmt::format("{}(f={},rb={},ra={},in={:016x},ticks={},nand={})",
                           DiagnosticEventName(event.type), event.frame, event.rolling_back,
                           event.running_ahead, event.inputs, event.ticks, event.nand_mark);
  }
  NOTICE_LOG_FMT(CORE, "Rollback restore diagnostic: preceding events [{}]", history);

  std::string writes;
  for (const HostMemoryWriteEvent& write : g_manager.host_memory_writes)
  {
    if (g_manager.ram_mismatch_details_frame &&
        (write.frame < *g_manager.ram_mismatch_details_frame - 2 ||
         write.frame > *g_manager.ram_mismatch_details_frame + 2))
    {
      continue;
    }
    if (!writes.empty())
      writes += ", ";
    writes += fmt::format("{}(f={},rb={},src=0x{:x},addr=0x{:08x},size={},ticks={},data={:016x})",
                          write.source, write.frame, write.resimulating, write.source_offset,
                          write.address, write.size, write.ticks, write.data_hash);
  }
  NOTICE_LOG_FMT(CORE, "Rollback host-write diagnostic: preceding writes [{}]", writes);
}

void WriteU32(u8* out, u32 value)
{
  for (int i = 0; i < 4; ++i)
    out[i] = static_cast<u8>(value >> (24 - i * 8));
}

void WriteU64(u8* out, u64 value)
{
  for (int i = 0; i < 8; ++i)
    out[i] = static_cast<u8>(value >> (56 - i * 8));
}

u32 ReadU32(const u8* in)
{
  u32 value = 0;
  for (int i = 0; i < 4; ++i)
    value = (value << 8) | in[i];
  return value;
}

u64 ReadU64(const u8* in)
{
  u64 value = 0;
  for (int i = 0; i < 8; ++i)
    value = (value << 8) | in[i];
  return value;
}

std::array<u8, RAM_DIAGNOSTIC_PACKET_SIZE> MakeRamDiagnosticPacket(u8 type, const RamDigest& digest)
{
  std::array<u8, RAM_DIAGNOSTIC_PACKET_SIZE> packet{};
  std::copy(RAM_DIAGNOSTIC_MAGIC.begin(), RAM_DIAGNOSTIC_MAGIC.end(), packet.begin());
  packet[4] = RAM_DIAGNOSTIC_VERSION;
  packet[5] = type;
  packet[6] = static_cast<u8>(g_manager.local_player);
  packet[7] = (digest.ram_valid ? DIAGNOSTIC_HAS_RAM : 0) |
              (digest.video_valid ? DIAGNOSTIC_HAS_VIDEO : 0) |
              (digest.efb_valid ? DIAGNOSTIC_HAS_EFB : 0) |
              (g_manager.remote_ram_match ? DIAGNOSTIC_REMOTE_RAM_MATCH : 0);
  WriteU32(packet.data() + 8, static_cast<u32>(digest.frame));
  WriteU64(packet.data() + 12, digest.root);
  WriteU64(packet.data() + 20, digest.mem1);
  WriteU64(packet.data() + 28, digest.mem2);
  WriteU64(packet.data() + 36, digest.aram);
  WriteU64(packet.data() + 44, digest.fake_vmem);
  WriteU64(packet.data() + 52, digest.l1);
  WriteU64(packet.data() + 60, digest.inputs);
  WriteU64(packet.data() + 68, digest.state);
  WriteU64(packet.data() + 76, digest.core_ticks);
  WriteU64(packet.data() + 84, digest.timebase);
  WriteU64(packet.data() + 92, digest.fake_tb_start_value);
  WriteU64(packet.data() + 100, digest.fake_tb_start_ticks);
  WriteU32(packet.data() + 108, digest.decrementer);
  for (std::size_t i = 0; i < digest.state_sections.size(); ++i)
    WriteU64(packet.data() + 112 + i * sizeof(u64), digest.state_sections[i]);
  WriteU64(packet.data() + DIAGNOSTIC_VIDEO_OFFSET, digest.efb_color);
  WriteU64(packet.data() + DIAGNOSTIC_VIDEO_OFFSET + sizeof(u64), digest.efb_depth);
  for (std::size_t i = 0; i < digest.video_sections.size(); ++i)
  {
    WriteU64(packet.data() + DIAGNOSTIC_VIDEO_SECTIONS_OFFSET + i * sizeof(u64),
             digest.video_sections[i]);
  }
  for (std::size_t i = 0; i < digest.ppc_sections.size(); ++i)
    WriteU64(packet.data() + DIAGNOSTIC_PPC_SECTIONS_OFFSET + i * sizeof(u64),
             digest.ppc_sections[i]);
  for (std::size_t i = 0; i < digest.hw_sections.size(); ++i)
    WriteU64(packet.data() + DIAGNOSTIC_HW_SECTIONS_OFFSET + i * sizeof(u64),
             digest.hw_sections[i]);
  for (std::size_t i = 0; i < digest.core_timing_sections.size(); ++i)
    WriteU64(packet.data() + DIAGNOSTIC_CORE_TIMING_SECTIONS_OFFSET + i * sizeof(u64),
             digest.core_timing_sections[i]);
  WriteU32(packet.data() + DIAGNOSTIC_CPU_FIELDS_OFFSET, digest.pc);
  WriteU32(packet.data() + DIAGNOSTIC_CPU_FIELDS_OFFSET + sizeof(u32), digest.npc);
  WriteU32(packet.data() + DIAGNOSTIC_CPU_FIELDS_OFFSET + 2 * sizeof(u32), digest.exceptions);
  WriteU32(packet.data() + DIAGNOSTIC_CPU_FIELDS_OFFSET + 3 * sizeof(u32),
           static_cast<u32>(digest.downcount));
  return packet;
}

void SendRamDiagnosticPacket(const ENetAddress& endpoint, u8 type, const RamDigest& digest)
{
  const auto packet = MakeRamDiagnosticPacket(type, digest);
  Common::ENet::SendRollbackDatagram(endpoint, packet.data(), packet.size());
}

void CompareRamDigest(int remote_player, const ENetAddress* sender, const RamDigest& remote)
{
  if (!g_manager.compare_confirmed_ram || !remote.ram_valid)
    return;

  const auto local_it = g_manager.ram_digests.find(remote.frame);
  if (local_it == g_manager.ram_digests.end())
    return;

  const RamDigest& local = local_it->second;
  const auto describe_hash_differences = [](const auto& local_hashes, const auto& remote_hashes,
                                            const auto& names) {
    std::string differences;
    for (std::size_t i = 0; i < local_hashes.size(); ++i)
    {
      if (local_hashes[i] == remote_hashes[i])
        continue;
      if (!differences.empty())
        differences += ", ";
      differences += fmt::format("{}={:016x}/{:016x}", names[i], local_hashes[i], remote_hashes[i]);
    }
    return differences.empty() ? std::string{"none"} : differences;
  };
  const std::string ppc_differences = describe_hash_differences(
      local.ppc_sections, remote.ppc_sections, ROLLBACK_PPC_SECTION_NAMES);
  const std::string core_timing_differences = describe_hash_differences(
      local.core_timing_sections, remote.core_timing_sections, ROLLBACK_CORE_TIMING_SECTION_NAMES);
  const std::string hw_differences =
      describe_hash_differences(local.hw_sections, remote.hw_sections, ROLLBACK_HW_SECTION_NAMES);
  constexpr std::size_t POWERPC_SECTION = 4;
  if (local.state_sections[POWERPC_SECTION] != remote.state_sections[POWERPC_SECTION] &&
      local.inputs == remote.inputs && local.core_ticks == remote.core_ticks &&
      local.fake_tb_start_value == remote.fake_tb_start_value &&
      local.fake_tb_start_ticks == remote.fake_tb_start_ticks &&
      !g_manager.first_powerpc_mismatch_frame)
  {
    g_manager.first_powerpc_mismatch_frame = remote.frame;
    NOTICE_LOG_FMT(
        CORE,
        "Rollback CPU diagnostic: first PowerPC mismatch at frame {} versus player {}: section "
        "{:016x}/{:016x}; PPC fields [{}]; PC {:#010x}/{:#010x} NPC {:#010x}/{:#010x} "
        "Exceptions {:#010x}/{:#010x} downcount {}/{}; CoreTiming [{}]; HW [{}]; TB "
        "{:#x}/{:#x}; DEC {:#x}/{:#x}; inputs match; RAM {}",
        remote.frame, remote_player, local.state_sections[POWERPC_SECTION],
        remote.state_sections[POWERPC_SECTION], ppc_differences, local.pc, remote.pc, local.npc,
        remote.npc, local.exceptions, remote.exceptions, local.downcount, remote.downcount,
        core_timing_differences, hw_differences, local.timebase, remote.timebase, local.decrementer,
        remote.decrementer, local.root == remote.root ? "match" : "DIFFER");
  }
  const bool ram_matches = local.root == remote.root;
  // PointerWrap serializes some trivially-copyable structs with their alignment padding. For
  // example, IOS FSCore::Handle has two padding bytes between gid and uid whose values can differ
  // even though every meaningful field matches. Keep all state, timing, and section hashes in the
  // report, but only a confirmed RAM/L1 digest difference stops the full-replay verification.
  if (ram_matches)
    return;

  std::string differing_sections;
  for (std::size_t i = 0; i < local.state_sections.size(); ++i)
  {
    if (local.state_sections[i] == remote.state_sections[i])
      continue;
    if (!differing_sections.empty())
      differing_sections += ", ";
    differing_sections += fmt::format("{}={:016x}/{:016x}", ROLLBACK_STATE_SECTION_NAMES[i],
                                      local.state_sections[i], remote.state_sections[i]);
  }
  if (differing_sections.empty())
    differing_sections = "none";

  if (!g_manager.first_ram_mismatch_frame || remote.frame < *g_manager.first_ram_mismatch_frame)
    g_manager.first_ram_mismatch_frame = remote.frame;

  if (!g_manager.ram_mismatch_details_frame || remote.frame < *g_manager.ram_mismatch_details_frame)
  {
    g_manager.ram_mismatch_details_frame = remote.frame;
    NOTICE_LOG_FMT(
        CORE,
        "Rollback RAM diagnostic: first confirmed mismatch at frame {} versus player {}: root "
        "local={:016x} remote={:016x}; MEM1 {:016x}/{:016x}; MEM2 {:016x}/{:016x}; "
        "ARAM {:016x}/{:016x}; FakeVMEM {:016x}/{:016x}; L1 {:016x}/{:016x}; "
        "state {:016x}/{:016x}; inputs {} ({:016x}/{:016x}); "
        "timing ticks {}/{} TB {:#x}/{:#x} fake-TB-value {:#x}/{:#x} fake-TB-ticks {}/{} "
        "DEC {:#x}/{:#x}; differing state sections [{}]; PPC [{}]; CoreTiming [{}]; HW [{}]",
        remote.frame, remote_player, local.root, remote.root, local.mem1, remote.mem1, local.mem2,
        remote.mem2, local.aram, remote.aram, local.fake_vmem, remote.fake_vmem, local.l1,
        remote.l1, local.state, remote.state, local.inputs == remote.inputs ? "match" : "DIFFER",
        local.inputs, remote.inputs, local.core_ticks, remote.core_ticks, local.timebase,
        remote.timebase, local.fake_tb_start_value, remote.fake_tb_start_value,
        local.fake_tb_start_ticks, remote.fake_tb_start_ticks, local.decrementer,
        remote.decrementer, differing_sections, ppc_differences, core_timing_differences,
        hw_differences);
    LogDiagnosticEventHistory();
  }

  // Tell the other peer explicitly so it pauses and preserves the same frame even if its copy of
  // the corresponding digest datagram was lost.
  if (sender)
  {
    for (int retry = 0; retry < 3; ++retry)
      SendRamDiagnosticPacket(*sender, RAM_DIAGNOSTIC_MISMATCH, local);
  }
  else
  {
    for (const ENetAddress& endpoint : g_manager.remote_endpoints)
    {
      for (int retry = 0; retry < 3; ++retry)
        SendRamDiagnosticPacket(endpoint, RAM_DIAGNOSTIC_MISMATCH, local);
    }
  }

  if (g_manager.stop_ram_compare_on_mismatch)
  {
    g_manager.compare_confirmed_ram = false;
    g_manager.track_host_memory_writes.store(false, std::memory_order_release);
    OSD::AddTypedMessage(
        OSD::MessageType::RollbackVerification,
        fmt::format("RAM mismatch at frame {} - verification disabled", remote.frame),
        std::numeric_limits<u32>::max(), OSD::Color::RED);
    NOTICE_LOG_FMT(CORE,
                   "Rollback RAM diagnostic: mismatch recorded at frame {}; verification "
                   "disabled and live emulation will continue",
                   remote.frame);
  }
}

void CompareVideoDigest(int remote_player, const ENetAddress*, const RamDigest& remote)
{
  if (!g_manager.compare_confirmed_video || !remote.video_valid)
    return;

  const auto local_it = g_manager.ram_digests.find(remote.frame);
  if (local_it == g_manager.ram_digests.end() || !local_it->second.video_valid)
    return;

  const RamDigest& local = local_it->second;
  std::string differing_sections;
  bool authoritative_video_differs = false;
  bool derived_video_differs = false;
  for (std::size_t i = 0; i < local.video_sections.size(); ++i)
  {
    if (local.video_sections[i] == remote.video_sections[i])
      continue;
    if (ROLLBACK_VIDEO_SECTION_AUTHORITATIVE[i])
      authoritative_video_differs = true;
    else
      derived_video_differs = true;
    if (!differing_sections.empty())
      differing_sections += ", ";
    differing_sections += fmt::format("{}={:016x}/{:016x}", ROLLBACK_VIDEO_SECTION_NAMES[i],
                                      local.video_sections[i], remote.video_sections[i]);
  }

  const bool comparable_efb = local.efb_valid && remote.efb_valid;
  const bool efb_differs = comparable_efb && (local.efb_color != remote.efb_color ||
                                              local.efb_depth != remote.efb_depth);
  if (!authoritative_video_differs && !derived_video_differs && !efb_differs)
  {
    return;
  }

  // Renderer caches and dirty flags legitimately differ after one peer has replayed frames with
  // rendering suppressed. Preserve the evidence, but do not stop either emulator or send a
  // mismatch notification for cache history alone.
  if (!authoritative_video_differs && !efb_differs)
  {
    if (!g_manager.first_derived_video_mismatch_frame ||
        remote.frame < *g_manager.first_derived_video_mismatch_frame)
    {
      g_manager.first_derived_video_mismatch_frame = remote.frame;
      NOTICE_LOG_FMT(
          CORE,
          "Rollback video diagnostic: first derived-cache difference at frame {} versus player "
          "{}; continuing verification; sections [{}]; RAM comparison {}",
          remote.frame, remote_player, differing_sections,
          local.ram_valid && remote.ram_valid ? (local.root == remote.root ? "match" : "DIFFER") :
                                                "disabled");
    }
    return;
  }

  if (g_manager.first_non_ram_video_mismatch_frame &&
      *g_manager.first_non_ram_video_mismatch_frame <= remote.frame)
  {
    return;
  }
  g_manager.first_non_ram_video_mismatch_frame = remote.frame;
  const char* const classification =
      authoritative_video_differs ?
          (efb_differs ? "AUTHORITATIVE_STATE_AND_EFB" : "AUTHORITATIVE_STATE_ONLY") :
          "EFB_ONLY";
  NOTICE_LOG_FMT(
      CORE,
      "Rollback video diagnostic: first non-RAM difference at frame {} versus player {}; "
      "continuing RAM verification: "
      "class={}; video sections [{}]; EFB {} color {:016x}/{:016x} depth "
      "{:016x}/{:016x}; RAM comparison {}",
      remote.frame, remote_player, classification,
      differing_sections.empty() ? "none" : differing_sections,
      comparable_efb ? (efb_differs ? "DIFFER" : "match") : "not sampled", local.efb_color,
      remote.efb_color, local.efb_depth, remote.efb_depth,
      local.ram_valid && remote.ram_valid ? (local.root == remote.root ? "match" : "DIFFER") :
                                            "disabled");
}

bool HandleRamDiagnosticDatagram(const Common::ENet::RollbackDatagram& datagram)
{
  const auto& payload = datagram.payload;
  if (payload.size() < RAM_DIAGNOSTIC_MAGIC.size() ||
      !std::equal(RAM_DIAGNOSTIC_MAGIC.begin(), RAM_DIAGNOSTIC_MAGIC.end(), payload.begin()))
  {
    return false;
  }

  // Always consume our private packet type so GekkoNet never tries to parse it as protocol data.
  if (payload.size() < 12 || payload[4] != RAM_DIAGNOSTIC_VERSION)
    return true;
  const u8 modes = payload[7];
  const u8 type = payload[5];
  const int remote_player = payload[6];
  const int packet_frame = static_cast<int>(ReadU32(payload.data() + 8));
  if (remote_player < 1 || remote_player > g_manager.players ||
      remote_player == g_manager.local_player)
  {
    return true;
  }
  if ((modes & DIAGNOSTIC_REMOTE_RAM_MATCH) != 0 && !g_manager.remote_ram_match)
  {
    g_manager.remote_ram_match = true;
    g_manager.compare_confirmed_ram = true;
    g_manager.track_host_memory_writes.store(true, std::memory_order_release);
    g_manager.stop_ram_compare_on_mismatch = false;
    if (g_manager.ring)
      g_manager.ring->ExpandSlots(REMOTE_RAM_MATCH_SNAPSHOT_SLOTS);
    NOTICE_LOG_FMT(CORE,
                   "Remote RAM Match: forced on by player {}; full confirmed-frame RAM "
                   "comparison is now active on this peer",
                   remote_player);
    OSD::AddTypedMessage(OSD::MessageType::RollbackVerification, "Remote RAM Match enabled by peer",
                         std::numeric_limits<u32>::max(), OSD::Color::CYAN);
  }
  if ((modes & DIAGNOSTIC_HAS_RAM) != 0 && !g_manager.compare_confirmed_ram)
  {
    if (g_manager.stop_ram_compare_on_mismatch)
      return true;
    g_manager.compare_confirmed_ram = true;
    g_manager.track_host_memory_writes.store(true, std::memory_order_release);
    NOTICE_LOG_FMT(CORE,
                   "Rollback RAM diagnostic: enabled by peer; confirmed snapshots will now be "
                   "compared");
  }

  if (type == RAM_DIAGNOSTIC_ENABLE)
  {
    RamDigest acknowledgement{};
    acknowledgement.frame = -1;
    SendRamDiagnosticPacket(datagram.address, RAM_DIAGNOSTIC_ACK, acknowledgement);
    return true;
  }
  if (type == RAM_DIAGNOSTIC_ACK)
  {
    if (packet_frame == -1)
    {
      g_manager.remote_ram_match_activation_acked_players |= 1u << remote_player;
    }
    else if (const auto it = g_manager.ram_digests.find(packet_frame);
             it != g_manager.ram_digests.end())
    {
      it->second.acknowledged_players |= 1u << remote_player;
    }
    return true;
  }

  RamDigest remote{};
  remote.frame = packet_frame;
  remote.ram_valid = (modes & DIAGNOSTIC_HAS_RAM) != 0;
  remote.video_valid = (modes & DIAGNOSTIC_HAS_VIDEO) != 0;
  remote.efb_valid = (modes & DIAGNOSTIC_HAS_EFB) != 0;
  if (type == RAM_DIAGNOSTIC_MISMATCH)
  {
    if (!g_manager.first_ram_mismatch_frame || remote.frame < *g_manager.first_ram_mismatch_frame)
      g_manager.first_ram_mismatch_frame = remote.frame;
    if (g_manager.stop_ram_compare_on_mismatch)
    {
      g_manager.compare_confirmed_ram = false;
      g_manager.track_host_memory_writes.store(false, std::memory_order_release);
      OSD::AddTypedMessage(
          OSD::MessageType::RollbackVerification,
          fmt::format("RAM mismatch at frame {} - verification disabled", remote.frame),
          std::numeric_limits<u32>::max(), OSD::Color::RED);
      NOTICE_LOG_FMT(CORE,
                     "Rollback RAM diagnostic: player {} reported mismatch frame {}; "
                     "verification disabled and live emulation will continue",
                     remote_player, remote.frame);
    }
    else
    {
      NOTICE_LOG_FMT(CORE,
                     "Rollback RAM diagnostic: player {} reported mismatch frame {}; "
                     "pausing to restore and dump it",
                     remote_player, remote.frame);
    }
    return true;
  }
  if (type == VIDEO_DIAGNOSTIC_MISMATCH)
  {
    if (g_manager.compare_confirmed_video && (!g_manager.first_video_mismatch_frame ||
                                              remote.frame < *g_manager.first_video_mismatch_frame))
    {
      g_manager.first_video_mismatch_frame = remote.frame;
      NOTICE_LOG_FMT(CORE,
                     "Rollback video diagnostic: player {} reported mismatch frame {}; pausing "
                     "to dump the confirmed video checkpoint",
                     remote_player, remote.frame);
    }
    return true;
  }
  if (type != RAM_DIAGNOSTIC_DIGEST || payload.size() != RAM_DIAGNOSTIC_PACKET_SIZE ||
      remote_player == g_manager.local_player)
  {
    return true;
  }

  remote.root = ReadU64(payload.data() + 12);
  remote.mem1 = ReadU64(payload.data() + 20);
  remote.mem2 = ReadU64(payload.data() + 28);
  remote.aram = ReadU64(payload.data() + 36);
  remote.fake_vmem = ReadU64(payload.data() + 44);
  remote.l1 = ReadU64(payload.data() + 52);
  remote.inputs = ReadU64(payload.data() + 60);
  remote.state = ReadU64(payload.data() + 68);
  remote.core_ticks = ReadU64(payload.data() + 76);
  remote.timebase = ReadU64(payload.data() + 84);
  remote.fake_tb_start_value = ReadU64(payload.data() + 92);
  remote.fake_tb_start_ticks = ReadU64(payload.data() + 100);
  remote.decrementer = ReadU32(payload.data() + 108);
  for (std::size_t i = 0; i < remote.state_sections.size(); ++i)
    remote.state_sections[i] = ReadU64(payload.data() + 112 + i * sizeof(u64));
  remote.efb_color = ReadU64(payload.data() + DIAGNOSTIC_VIDEO_OFFSET);
  remote.efb_depth = ReadU64(payload.data() + DIAGNOSTIC_VIDEO_OFFSET + sizeof(u64));
  for (std::size_t i = 0; i < remote.video_sections.size(); ++i)
  {
    remote.video_sections[i] =
        ReadU64(payload.data() + DIAGNOSTIC_VIDEO_SECTIONS_OFFSET + i * sizeof(u64));
  }
  for (std::size_t i = 0; i < remote.ppc_sections.size(); ++i)
    remote.ppc_sections[i] =
        ReadU64(payload.data() + DIAGNOSTIC_PPC_SECTIONS_OFFSET + i * sizeof(u64));
  for (std::size_t i = 0; i < remote.hw_sections.size(); ++i)
    remote.hw_sections[i] =
        ReadU64(payload.data() + DIAGNOSTIC_HW_SECTIONS_OFFSET + i * sizeof(u64));
  for (std::size_t i = 0; i < remote.core_timing_sections.size(); ++i)
  {
    remote.core_timing_sections[i] =
        ReadU64(payload.data() + DIAGNOSTIC_CORE_TIMING_SECTIONS_OFFSET + i * sizeof(u64));
  }
  remote.pc = ReadU32(payload.data() + DIAGNOSTIC_CPU_FIELDS_OFFSET);
  remote.npc = ReadU32(payload.data() + DIAGNOSTIC_CPU_FIELDS_OFFSET + sizeof(u32));
  remote.exceptions = ReadU32(payload.data() + DIAGNOSTIC_CPU_FIELDS_OFFSET + 2 * sizeof(u32));
  remote.downcount =
      static_cast<s32>(ReadU32(payload.data() + DIAGNOSTIC_CPU_FIELDS_OFFSET + 3 * sizeof(u32)));
  g_manager.remote_ram_digests[{remote_player, remote.frame}] = remote;
  if (g_manager.remote_ram_match)
  {
    RamDigest acknowledgement{};
    acknowledgement.frame = remote.frame;
    SendRamDiagnosticPacket(datagram.address, RAM_DIAGNOSTIC_ACK, acknowledgement);
  }
  return true;
}

void FillRamRegionHashes(Core::System& system, RamDigest* digest)
{
  auto& memory = system.GetMemory();
  digest->mem1 = XXH3_64bits(memory.GetRAM(), memory.GetRamSize());
  digest->mem2 = memory.GetEXRAM() ? XXH3_64bits(memory.GetEXRAM(), memory.GetExRamSize()) : 0;
  if (!system.IsWii())
  {
    auto& dsp = system.GetDSP();
    digest->aram = XXH3_64bits(dsp.GetARAMPtr(), dsp.GetARAMSize());
  }
  digest->fake_vmem =
      memory.GetFakeVMEM() ? XXH3_64bits(memory.GetFakeVMEM(), memory.GetFakeVMemSize()) : 0;
  digest->l1 = XXH3_64bits(memory.GetL1Cache(), memory.GetL1CacheSize());
  const std::array<u64, 5> regions{digest->mem1, digest->mem2, digest->aram, digest->fake_vmem,
                                   digest->l1};
  digest->root = XXH3_64bits(regions.data(), sizeof(regions));
}

std::vector<u64> HashMem1Pages(Core::System& system)
{
  auto& memory = system.GetMemory();
  const u8* const ram = memory.GetRAM();
  const std::size_t ram_size = memory.GetRamSize();
  const std::size_t page_count =
      (ram_size + NETPLAY_REPLAY_MEM1_PAGE_SIZE - 1) / NETPLAY_REPLAY_MEM1_PAGE_SIZE;
  std::vector<u64> hashes(page_count);
  for (std::size_t page = 0; page < page_count; ++page)
  {
    const std::size_t offset = page * NETPLAY_REPLAY_MEM1_PAGE_SIZE;
    const std::size_t size = std::min(NETPLAY_REPLAY_MEM1_PAGE_SIZE, ram_size - offset);
    hashes[page] = XXH3_64bits(ram + offset, size);
  }
  return hashes;
}

std::vector<u8> ReadReplaySysconf(Core::System& system)
{
  auto* const ios = system.GetIOS();
  if (!ios)
    return {};
  const auto fs = ios->GetFS();
  const auto file = fs->OpenFile(IOS::PID_KERNEL, IOS::PID_KERNEL, "/shared2/sys/SYSCONF",
                                 IOS::HLE::FS::Mode::Read);
  if (!file)
    return {};
  const auto status = file->GetStatus();
  if (!status || status->size != NETPLAY_REPLAY_SYSCONF_SIZE)
    return {};
  std::vector<u8> bytes(NETPLAY_REPLAY_SYSCONF_SIZE);
  if (!file->Read(bytes.data(), bytes.size()))
    return {};
  return bytes;
}

void LogNetplayReplayMem1PageDifferences(Core::System& system,
                                         const std::vector<u64>& expected_hashes)
{
  if (expected_hashes.empty())
    return;

  const std::vector<u64> actual_hashes = HashMem1Pages(system);
  if (actual_hashes.size() != expected_hashes.size())
  {
    NOTICE_LOG_FMT(CORE, "Netplay Replay MEM1 page map size mismatch: expected={} actual={}",
                   expected_hashes.size(), actual_hashes.size());
    return;
  }

  std::size_t mismatch_count = 0;
  std::size_t range_start = 0;
  bool in_range = false;
  std::vector<std::pair<std::size_t, std::size_t>> ranges;
  for (std::size_t page = 0; page <= expected_hashes.size(); ++page)
  {
    const bool mismatch =
        page < expected_hashes.size() && expected_hashes[page] != actual_hashes[page];
    if (mismatch)
    {
      ++mismatch_count;
      if (!in_range)
      {
        range_start = page;
        in_range = true;
      }
    }
    else if (in_range)
    {
      ranges.emplace_back(range_start, page - 1);
      in_range = false;
    }
  }

  std::string range_text;
  constexpr std::size_t MAX_LOGGED_RANGES = 32;
  for (std::size_t i = 0; i < std::min(ranges.size(), MAX_LOGGED_RANGES); ++i)
  {
    const u32 start =
        Memory::MEM1_BASE_ADDR + static_cast<u32>(ranges[i].first * NETPLAY_REPLAY_MEM1_PAGE_SIZE);
    const u32 end = Memory::MEM1_BASE_ADDR +
                    static_cast<u32>((ranges[i].second + 1) * NETPLAY_REPLAY_MEM1_PAGE_SIZE - 1);
    if (!range_text.empty())
      range_text += ", ";
    range_text += fmt::format("{:#010x}-{:#010x}", start, end);
  }
  if (ranges.size() > MAX_LOGGED_RANGES)
    range_text += fmt::format(", ... {} more ranges", ranges.size() - MAX_LOGGED_RANGES);

  NOTICE_LOG_FMT(CORE, "Netplay Replay MEM1 page comparison: mismatched_pages={}/{} ranges={} [{}]",
                 mismatch_count, expected_hashes.size(), ranges.size(), range_text);
}

void VerifyRestoredRam(Core::System& system, int frame)
{
  if (!g_manager.compare_confirmed_ram)
    return;

  const auto expected_it = g_manager.ram_digests.find(frame);
  if (expected_it == g_manager.ram_digests.end())
    return;

  ++g_manager.perf_restore_checks;
  RamDigest actual{};
  actual.frame = frame;
  FillRamRegionHashes(system, &actual);
  const RamDigest& expected = expected_it->second;
  if (actual.root == expected.root)
    return;

  ++g_manager.perf_restore_mismatches;
  if (g_manager.restore_mismatch_reported)
    return;
  g_manager.restore_mismatch_reported = true;

  const s64 snapshot_key = static_cast<s64>(frame) + 1;
  const std::optional<u64> expected_nand = g_manager.ring->NandJournalMarkForFrame(snapshot_key);
  IOS::HLE::FS::HostFileSystem* nand = HostNand(system);
  const std::optional<u64> actual_nand =
      nand ? std::optional<u64>(nand->LastJournalMark()) : std::nullopt;
  NOTICE_LOG_FMT(
      CORE,
      "Rollback restore diagnostic: RESTORE MISMATCH frame {} key {} root {:016x}/{:016x}; "
      "MEM1 {:016x}/{:016x}; MEM2 {:016x}/{:016x}; ARAM {:016x}/{:016x}; "
      "FakeVMEM {:016x}/{:016x}; L1 {:016x}/{:016x}; NAND mark {}/{}",
      frame, snapshot_key, expected.root, actual.root, expected.mem1, actual.mem1, expected.mem2,
      actual.mem2, expected.aram, actual.aram, expected.fake_vmem, actual.fake_vmem, expected.l1,
      actual.l1, expected_nand ? fmt::format("{}", *expected_nand) : "unavailable",
      actual_nand ? fmt::format("{}", *actual_nand) : "unavailable");
  LogDiagnosticEventHistory();
}

void RecordRamDigest(Core::System& system, int frame)
{
  if (!g_manager.compare_confirmed_ram && !g_manager.compare_confirmed_video)
    return;

  RamDigest digest{};
  digest.frame = frame;
  if (g_manager.compare_confirmed_ram)
  {
    digest.ram_valid = true;
    FillRamRegionHashes(system, &digest);
  }
  digest.inputs = g_manager.last_advance_input_hash;
  const std::span<const u8> state = g_manager.ring->LastState();
  digest.state = XXH3_64bits(state.data(), state.size());
  const State::RollbackStateLayout& layout = g_manager.ring->LastStateLayout();
  std::size_t section_start = 0;
  for (std::size_t i = 0; i < digest.state_sections.size(); ++i)
  {
    const std::size_t section_end = std::min(layout.section_ends[i], state.size());
    if (section_end < section_start)
      break;
    digest.state_sections[i] =
        XXH3_64bits(state.data() + section_start, section_end - section_start);
    section_start = section_end;
  }

  const auto hash_subsections = [&state](auto* hashes, const auto& ends,
                                         std::size_t subsection_start) {
    for (std::size_t i = 0; i < hashes->size(); ++i)
    {
      const std::size_t subsection_end = std::min(ends[i], state.size());
      if (subsection_end < subsection_start)
        break;
      (*hashes)[i] =
          XXH3_64bits(state.data() + subsection_start, subsection_end - subsection_start);
      subsection_start = subsection_end;
    }
  };
  hash_subsections(&digest.core_timing_sections, layout.core_timing_section_ends,
                   layout.section_ends[1]);
  hash_subsections(&digest.hw_sections, layout.hw_section_ends, layout.section_ends[2]);
  hash_subsections(&digest.ppc_sections, layout.ppc_section_ends, layout.section_ends[3]);
  const auto& ppc = system.GetPPCState();
  digest.pc = ppc.pc;
  digest.npc = ppc.npc;
  digest.exceptions = ppc.Exceptions;
  digest.downcount = ppc.downcount;
  if (g_manager.compare_confirmed_video)
  {
    digest.video_valid = true;
    std::size_t video_begin = layout.section_ends[0];
    for (std::size_t i = 0; i < digest.video_sections.size(); ++i)
    {
      const std::size_t video_end = std::min(layout.video_section_ends[i], state.size());
      if (video_end < video_begin)
        break;
      digest.video_sections[i] = XXH3_64bits(state.data() + video_begin, video_end - video_begin);
      video_begin = video_end;
    }

    if (const EFBRollbackAuditCapture* efb =
            g_manager.ring->EFBAuditForFrame(static_cast<s64>(frame) + 1))
    {
      digest.efb_valid = true;
      digest.efb_color = XXH3_64bits(efb->color.bytes.data(), efb->color.bytes.size());
      digest.efb_depth = XXH3_64bits(efb->depth.bytes.data(), efb->depth.bytes.size());
      NOTICE_LOG_FMT(CORE,
                     "Rollback video checkpoint: frame {} color={:016x}/{} depth={:016x}/{} "
                     "layout={}x{}x{} stride={}/{}",
                     frame, digest.efb_color, efb->color.bytes.size(), digest.efb_depth,
                     efb->depth.bytes.size(), efb->color.width, efb->color.height,
                     efb->color.layers, efb->color.stride, efb->depth.stride);

      std::string video_sections;
      for (std::size_t i = 0; i < ROLLBACK_VIDEO_SECTION_NAMES.size(); ++i)
      {
        if (!video_sections.empty())
          video_sections += ',';
        const std::size_t video_section_start =
            i == 0 ? layout.section_ends[0] : layout.video_section_ends[i - 1];
        const std::size_t video_section_end = layout.video_section_ends[i];
        video_sections +=
            fmt::format("{}={:016x}/{}", ROLLBACK_VIDEO_SECTION_NAMES[i], digest.video_sections[i],
                        video_section_end - video_section_start);
      }
      NOTICE_LOG_FMT(CORE, "Rollback video sections: frame {} {}", frame, video_sections);
    }
  }
  auto& core_timing = system.GetCoreTiming();
  digest.core_ticks = core_timing.GetTicks();
  digest.timebase = system.GetPowerPC().ReadFullTimeBaseValue();
  digest.fake_tb_start_value = core_timing.GetFakeTBStartValue();
  digest.fake_tb_start_ticks = core_timing.GetFakeTBStartTicks();
  digest.decrementer = system.GetSystemTimers().GetFakeDecrementer();
  if (frame == -1)
  {
    NOTICE_LOG_FMT(CORE,
                   "Rollback startup diagnostic: frame -1 root={:016x} state={:016x} ticks={} "
                   "TB={:#x} fake-TB-value={:#x} fake-TB-ticks={} DEC={:#x}",
                   digest.root, digest.state, digest.core_ticks, digest.timebase,
                   digest.fake_tb_start_value, digest.fake_tb_start_ticks, digest.decrementer);
  }
  g_manager.ram_digests[frame] = digest;
  while (g_manager.ram_digests.size() > 128)
    g_manager.ram_digests.erase(g_manager.ram_digests.begin());
  while (g_manager.remote_ram_digests.size() > 512)
    g_manager.remote_ram_digests.erase(g_manager.remote_ram_digests.begin());
}

void LogSparseStateCheckpoint(int frame)
{
  if (!g_manager.compare_confirmed_ram || frame < 0 || frame % 60 != 0 || !g_manager.ring)
    return;

  const auto digest_it = g_manager.ram_digests.find(frame);
  if (digest_it == g_manager.ram_digests.end())
    return;
  const RamDigest& digest = digest_it->second;

  NOTICE_LOG_FMT(
      CORE,
      "Rollback sparse checkpoint: frame {} resim={} input={:016x} MEM1={:016x} MEM2={:016x} "
      "L1={:016x} state={:016x} sections Movie={:016x},Video={:016x},CoreTiming={:016x},"
      "HW={:016x},PowerPC={:016x},Wiimote={:016x},Gecko={:016x}",
      frame, Rollback::IsResimulating(), g_manager.last_advance_input_hash, digest.mem1,
      digest.mem2, digest.l1, digest.state, digest.state_sections[0], digest.state_sections[1],
      digest.state_sections[2], digest.state_sections[3], digest.state_sections[4],
      digest.state_sections[5], digest.state_sections[6]);
}

void CompareSettledRamDigests()
{
  if (!g_manager.compare_confirmed_ram && !g_manager.compare_confirmed_video)
    return;
  for (const auto& [key, remote] : g_manager.remote_ram_digests)
  {
    CompareRamDigest(key.first, nullptr, remote);
    CompareVideoDigest(key.first, nullptr, remote);
  }
}

void SendConfirmedRamDigests()
{
  if ((!g_manager.compare_confirmed_ram && !g_manager.compare_confirmed_video) ||
      !g_manager.session || g_manager.remote_endpoints.empty())
  {
    return;
  }

  u32 expected_acknowledgements = 0;
  for (int player = 1; player <= g_manager.players; ++player)
  {
    if (player != g_manager.local_player)
      expected_acknowledgements |= 1u << player;
  }
  if (g_manager.remote_ram_match && (g_manager.remote_ram_match_activation_acked_players &
                                     expected_acknowledgements) != expected_acknowledgements)
  {
    RamDigest enable{};
    enable.frame = -1;
    for (const ENetAddress& endpoint : g_manager.remote_endpoints)
      SendRamDiagnosticPacket(endpoint, RAM_DIAGNOSTIC_ENABLE, enable);
  }

  int confirmed = std::numeric_limits<int>::max();
  for (const int handle : g_manager.player_handles)
    confirmed = std::min(confirmed, gekko_last_received_frame(g_manager.session, handle));
  if (confirmed < -1)
    return;

  for (auto& [frame, digest] : g_manager.ram_digests)
  {
    if (frame > confirmed ||
        (g_manager.remote_ram_match ? (digest.acknowledged_players & expected_acknowledgements) ==
                                          expected_acknowledgements :
                                      digest.sends >= 3))
      continue;
    if (!digest.ram_valid && !digest.video_valid)
      continue;
    for (const ENetAddress& endpoint : g_manager.remote_endpoints)
      SendRamDiagnosticPacket(endpoint, RAM_DIAGNOSTIC_DIGEST, digest);
    ++digest.sends;
  }
}

bool WriteRamRegion(const std::string& path, const u8* data, std::size_t size)
{
  return File::IOFile(path, "wb").WriteBytes(data, size);
}

bool DumpFirstRamMismatch(Core::System& system)
{
  if (!g_manager.compare_confirmed_ram || g_manager.ram_diagnostic_dumped ||
      !g_manager.first_ram_mismatch_frame || !g_manager.ring)
  {
    return false;
  }

  const int frame = *g_manager.first_ram_mismatch_frame;
  const int previous_frame = frame - 1;
  const std::string directory =
      fmt::format("{}remote-ram-match-session-{}-frame-{}-p{}{}", File::GetUserPath(D_LOGS_IDX),
                  g_manager.session_id, frame, g_manager.local_player, '/');
  File::CreateFullPath(directory);

  const auto dump_frame = [&](int dump_frame_number, std::string_view label) {
    const s64 snapshot_key = static_cast<s64>(dump_frame_number) + 1;
    if (!g_manager.ring->Load(system, snapshot_key))
      return false;

    auto& memory = system.GetMemory();
    const std::string base = directory + std::string(label);
    bool ok = WriteRamRegion(base + "-mem1.bin", memory.GetRAM(), memory.GetRamSize());
    ok = (!memory.GetEXRAM() ||
          WriteRamRegion(base + "-mem2.bin", memory.GetEXRAM(), memory.GetExRamSize())) &&
         ok;
    if (!system.IsWii())
    {
      auto& dsp = system.GetDSP();
      ok = WriteRamRegion(base + "-aram.bin", dsp.GetARAMPtr(), dsp.GetARAMSize()) && ok;
    }
    ok = (!memory.GetFakeVMEM() ||
          WriteRamRegion(base + "-fakevmem.bin", memory.GetFakeVMEM(), memory.GetFakeVMemSize())) &&
         ok;
    ok = WriteRamRegion(base + "-l1.bin", memory.GetL1Cache(), memory.GetL1CacheSize()) && ok;
    const std::span<const u8> state = g_manager.ring->StateForFrame(snapshot_key);
    ok = (!state.empty() && WriteRamRegion(base + "-state.bin", state.data(), state.size())) && ok;
    return ok;
  };

  // Orca's undo history can only move backward. Capture the newer mismatching frame before
  // restoring the preceding frame, otherwise loading last-good discards first-bad's undo log.
  const bool mismatch_ok = dump_frame(frame, "first-bad");
  const bool previous_ok = previous_frame >= -1 && dump_frame(previous_frame, "last-good");

  const auto local_it = g_manager.ram_digests.find(frame);
  const RamDigest* local = local_it == g_manager.ram_digests.end() ? nullptr : &local_it->second;
  const RamDigest* remote = nullptr;
  int remote_player = 0;
  for (const auto& [key, digest] : g_manager.remote_ram_digests)
  {
    if (key.second == frame)
    {
      remote_player = key.first;
      remote = &digest;
      break;
    }
  }
  const auto hash_or_zero = [](const RamDigest* digest, u64 RamDigest::* member) {
    return digest ? digest->*member : 0;
  };
  const std::string manifest = fmt::format(
      "{{\n  \"format\": 1,\n  \"session_id\": {},\n  \"local_player\": {},\n"
      "  \"remote_player\": {},\n  \"first_bad_frame\": {},\n"
      "  \"last_good_frame\": {},\n  \"game_id\": \"{}\",\n  \"revision\": {},\n"
      "  \"frame_boundary\": \"{}\",\n  \"previous_dump_ok\": {},\n"
      "  \"mismatch_dump_ok\": {},\n  \"local\": {{\"root\": \"{:016x}\", "
      "\"mem1\": \"{:016x}\", \"mem2\": \"{:016x}\", \"l1\": \"{:016x}\", "
      "\"inputs\": \"{:016x}\", \"state\": \"{:016x}\"}},\n"
      "  \"remote\": {{\"root\": \"{:016x}\", \"mem1\": \"{:016x}\", "
      "\"mem2\": \"{:016x}\", \"l1\": \"{:016x}\", \"inputs\": \"{:016x}\", "
      "\"state\": \"{:016x}\"}}\n}}\n",
      g_manager.session_id, g_manager.local_player, remote_player, frame, previous_frame,
      SConfig::GetInstance().GetGameID(), SConfig::GetInstance().GetRevision(),
      FrameBoundaryName(g_manager.frame_boundary), previous_ok, mismatch_ok,
      hash_or_zero(local, &RamDigest::root), hash_or_zero(local, &RamDigest::mem1),
      hash_or_zero(local, &RamDigest::mem2), hash_or_zero(local, &RamDigest::l1),
      hash_or_zero(local, &RamDigest::inputs), hash_or_zero(local, &RamDigest::state),
      hash_or_zero(remote, &RamDigest::root), hash_or_zero(remote, &RamDigest::mem1),
      hash_or_zero(remote, &RamDigest::mem2), hash_or_zero(remote, &RamDigest::l1),
      hash_or_zero(remote, &RamDigest::inputs), hash_or_zero(remote, &RamDigest::state));
  const bool manifest_ok = File::IOFile(directory + "manifest.json", "wb").WriteString(manifest);

  g_manager.ram_diagnostic_dumped = true;
  NOTICE_LOG_FMT(CORE,
                 "Remote RAM Match: captured last-good frame {} and first-bad frame {} for P{} "
                 "in {} (previous_ok={}, mismatch_ok={}, manifest_ok={}); emulation paused",
                 previous_frame, frame, g_manager.local_player, directory, previous_ok, mismatch_ok,
                 manifest_ok);
  OSD::AddTypedMessage(OSD::MessageType::RollbackVerification,
                       fmt::format("Remote RAM mismatch at frame {} - capture saved", frame),
                       std::numeric_limits<u32>::max(), OSD::Color::RED);
  Core::SetState(system, Core::State::Paused);
  return true;
}

bool DumpFirstVideoMismatch(Core::System& system)
{
  if (!g_manager.compare_confirmed_video || g_manager.video_diagnostic_dumped ||
      !g_manager.first_video_mismatch_frame || !g_manager.ring)
  {
    return false;
  }

  const int frame = *g_manager.first_video_mismatch_frame;
  const s64 snapshot_key = static_cast<s64>(frame) + 1;
  const EFBRollbackAuditCapture* efb = g_manager.ring->EFBAuditForFrame(snapshot_key);
  const std::span<const u8> state = g_manager.ring->StateForFrame(snapshot_key);
  if (state.empty())
  {
    NOTICE_LOG_FMT(CORE,
                   "Rollback video diagnostic: confirmed frame {} fell out of the snapshot ring; "
                   "pausing without a state dump",
                   frame);
    g_manager.video_diagnostic_dumped = true;
    Core::SetState(system, Core::State::Paused);
    return true;
  }

  const std::string base =
      fmt::format("{}rollback-video-session-{}-frame-{}-p{}", File::GetUserPath(D_LOGS_IDX),
                  g_manager.session_id, frame, g_manager.local_player);
  const bool color_ok = !efb || WriteRamRegion(base + "-efb-color.bin", efb->color.bytes.data(),
                                               efb->color.bytes.size());
  const bool depth_ok = !efb || WriteRamRegion(base + "-efb-depth.bin", efb->depth.bytes.data(),
                                               efb->depth.bytes.size());
  const bool state_ok = WriteRamRegion(base + "-state.bin", state.data(), state.size());
  g_manager.video_diagnostic_dumped = true;
  if (efb)
  {
    NOTICE_LOG_FMT(CORE,
                   "Rollback video diagnostic: dumped confirmed frame {} P{} to {}-[efb-color|"
                   "efb-depth|state].bin; layout={}x{}x{} color_stride={} color_format={} "
                   "depth_stride={} depth_format={} write_ok={}/{}/{}; emulation paused",
                   frame, g_manager.local_player, base, efb->color.width, efb->color.height,
                   efb->color.layers, efb->color.stride, static_cast<u32>(efb->color.format),
                   efb->depth.stride, static_cast<u32>(efb->depth.format), color_ok, depth_ok,
                   state_ok);
  }
  else
  {
    NOTICE_LOG_FMT(CORE,
                   "Rollback video diagnostic: dumped confirmed frame {} P{} serialized state "
                   "to {}-state.bin; EFB was not sampled on this frame; write_ok={}; emulation "
                   "paused",
                   frame, g_manager.local_player, base, state_ok);
  }
  Core::SetState(system, Core::State::Paused);
  return true;
}

}  // namespace

WirePad EncodePad(const GCPadStatus& status)
{
  WirePad pad{};
  const u16 buttons = status.button & PAD_WIRE_BUTTONS;
  pad.data[0] = static_cast<u8>(buttons >> 8);
  pad.data[1] = static_cast<u8>(buttons & 0xFF);
  pad.data[2] = static_cast<u8>(status.stickX - GCPadStatus::MAIN_STICK_CENTER_X);
  pad.data[3] = static_cast<u8>(status.stickY - GCPadStatus::MAIN_STICK_CENTER_Y);
  pad.data[4] = static_cast<u8>(status.substickX - GCPadStatus::C_STICK_CENTER_X);
  pad.data[5] = static_cast<u8>(status.substickY - GCPadStatus::C_STICK_CENTER_Y);
  pad.data[6] = status.triggerLeft;
  pad.data[7] = status.triggerRight;
  return pad;
}

bool WirePadHasStart(const WirePad& pad)
{
  const u16 buttons = static_cast<u16>((pad.data[0] << 8) | pad.data[1]);
  return (buttons & PAD_BUTTON_START) != 0;
}

GCPadStatus DecodePad(const WirePad& pad)
{
  GCPadStatus status{};
  status.button = static_cast<u16>((pad.data[0] << 8) | pad.data[1]) & PAD_WIRE_BUTTONS;
  status.stickX = static_cast<u8>(pad.data[2] + GCPadStatus::MAIN_STICK_CENTER_X);
  status.stickY = static_cast<u8>(pad.data[3] + GCPadStatus::MAIN_STICK_CENTER_Y);
  status.substickX = static_cast<u8>(pad.data[4] + GCPadStatus::C_STICK_CENTER_X);
  status.substickY = static_cast<u8>(pad.data[5] + GCPadStatus::C_STICK_CENTER_Y);
  status.triggerLeft = pad.data[6];
  status.triggerRight = pad.data[7];
  status.analogA = 0;
  status.analogB = 0;
  status.isConnected = true;
  return status;
}

static WireInput EncodeGameCubeInput(const WirePad& pad)
{
  WireInput input{};
  input.controller_type = static_cast<u8>(ControllerType::GameCube);
  input.pad = pad;
  return input;
}

static WireInput EncodeWiimoteInput(const WiimoteEmu::SerializedWiimoteState& state)
{
  WireInput input{};
  input.controller_type = static_cast<u8>(ControllerType::Wiimote);
  input.wiimote_length =
      std::min<u8>(state.length, static_cast<u8>(input.wiimote_data.size()));
  std::copy_n(state.data.begin(), input.wiimote_length, input.wiimote_data.begin());
  return input;
}

static WiimoteEmu::SerializedWiimoteState DecodeWiimoteInput(const WireInput& input)
{
  WiimoteEmu::SerializedWiimoteState state{};
  state.length = std::min<u8>(input.wiimote_length, static_cast<u8>(state.data.size()));
  std::copy_n(input.wiimote_data.begin(), state.length, state.data.begin());
  return state;
}

std::optional<GCPadStatus> GetRollbackPad(int port)
{
  if (!g_manager.active.load(std::memory_order_relaxed))
    return std::nullopt;

  if (port >= 0 && port < MAX_PORTS)
  {
    std::lock_guard lk(g_manager.mutex);
    if (g_manager.controller_types[port] != ControllerType::GameCube)
      return std::nullopt;
    return g_manager.latched_pads[port];
  }
  return std::nullopt;
}

void CaptureLocalWiimoteState(const WiimoteEmu::SerializedWiimoteState& state)
{
  if (!g_manager.active.load(std::memory_order_relaxed))
    return;
  std::lock_guard lk(g_manager.mutex);
  if (g_manager.local_player >= 1 && g_manager.local_player <= MAX_PORTS &&
      g_manager.controller_types[g_manager.local_player - 1] == ControllerType::Wiimote)
  {
    g_manager.pending_local_wiimote = state;
  }
}

std::optional<WiimoteEmu::SerializedWiimoteState> GetRollbackWiimoteState(int port)
{
  if (!g_manager.active.load(std::memory_order_relaxed) || port < 0 || port >= MAX_PORTS)
    return std::nullopt;
  std::lock_guard lk(g_manager.mutex);
  if (g_manager.controller_types[port] != ControllerType::Wiimote)
    return std::nullopt;
  return g_manager.latched_wiimotes[port];
}

bool BeginNetplayReplayRecording(const std::string& game_key)
{
  std::lock_guard lk(g_manager.mutex);
  if (!g_manager.active.load(std::memory_order_relaxed) || !g_manager.session ||
      g_manager.players < 1 || g_manager.players > MAX_PORTS)
  {
    return false;
  }
  if (std::any_of(g_manager.controller_types.begin(),
                  g_manager.controller_types.begin() + g_manager.players,
                  [](ControllerType type) { return type == ControllerType::Wiimote; }))
  {
    WARN_LOG_FMT(CORE, "Netplay Replay: Wii Remote recording is not supported by NPR v2");
    return false;
  }

  g_manager.netplay_replay_frames.clear();
  g_manager.netplay_replay_players = g_manager.players;
  g_manager.netplay_replay_boundary = g_manager.frame_boundary;
  g_manager.netplay_replay_game_id = SConfig::GetInstance().GetGameID();
  g_manager.netplay_replay_game_key = game_key;
  g_manager.netplay_replay_initial_rtc = ExpansionInterface::CEXIIPL::NetPlay_GetEmulatedTime();
  g_manager.netplay_replay_initial_digest = {};
  g_manager.netplay_replay_initial_digest_valid = false;
  g_manager.netplay_replay_initial_mem1_page_hashes.clear();
  g_manager.netplay_replay_sysconf.clear();
  g_manager.netplay_replay_recording = true;
  return true;
}

bool SaveNetplayReplay(const std::string& path)
{
  std::lock_guard lk(g_manager.mutex);
  if (!g_manager.netplay_replay_recording.load(std::memory_order_relaxed) ||
      g_manager.netplay_replay_frames.empty())
    return false;

  File::IOFile file(path, "wb");
  if (!file)
    return false;

  NetplayReplayHeader header{};
  header.magic = NETPLAY_REPLAY_MAGIC;
  header.players = static_cast<u32>(g_manager.netplay_replay_players);
  header.frame_boundary = static_cast<u32>(g_manager.netplay_replay_boundary);
  header.bytes_per_frame = header.players * sizeof(WirePad);
  header.frame_count = g_manager.netplay_replay_frames.size();
  const std::string& game_id = g_manager.netplay_replay_game_id;
  std::memcpy(header.game_id.data(), game_id.data(),
              std::min(game_id.size(), header.game_id.size()));
  std::memcpy(header.game_key.data(), g_manager.netplay_replay_game_key.data(),
              std::min(g_manager.netplay_replay_game_key.size(), header.game_key.size()));
  const RamDigest& initial = g_manager.netplay_replay_initial_digest;
  header.initial_rtc = g_manager.netplay_replay_initial_rtc;
  header.initial_root = initial.root;
  header.initial_mem1 = initial.mem1;
  header.initial_mem2 = initial.mem2;
  header.initial_aram = initial.aram;
  header.initial_fake_vmem = initial.fake_vmem;
  header.initial_l1 = initial.l1;
  header.initial_core_ticks = initial.core_ticks;
  header.initial_timebase = initial.timebase;
  header.initial_fake_tb_start_value = initial.fake_tb_start_value;
  header.initial_fake_tb_start_ticks = initial.fake_tb_start_ticks;
  header.initial_decrementer = initial.decrementer;
  header.first_frame = g_manager.netplay_replay_frames.begin()->first;
  if (!g_manager.netplay_replay_initial_mem1_page_hashes.empty())
  {
    header.reserved[0] |= NETPLAY_REPLAY_HAS_MEM1_PAGE_HASHES;
    header.reserved[1] = NETPLAY_REPLAY_MEM1_PAGE_SHIFT;
  }
  if (g_manager.netplay_replay_sysconf.size() == NETPLAY_REPLAY_SYSCONF_SIZE)
    header.reserved[0] |= NETPLAY_REPLAY_HAS_SYSCONF;

  if (!file.WriteArray(&header, 1))
    return false;
  if ((header.reserved[0] & NETPLAY_REPLAY_HAS_MEM1_PAGE_HASHES) != 0)
  {
    const u32 page_count =
        static_cast<u32>(g_manager.netplay_replay_initial_mem1_page_hashes.size());
    if (!file.WriteArray(&page_count, 1) ||
        !file.WriteArray(g_manager.netplay_replay_initial_mem1_page_hashes.data(), page_count))
    {
      return false;
    }
  }
  if ((header.reserved[0] & NETPLAY_REPLAY_HAS_SYSCONF) != 0)
  {
    const u32 sysconf_size = static_cast<u32>(g_manager.netplay_replay_sysconf.size());
    if (!file.WriteArray(&sysconf_size, 1) ||
        !file.WriteBytes(g_manager.netplay_replay_sysconf.data(), sysconf_size))
    {
      return false;
    }
  }
  for (const auto& [frame, inputs] : g_manager.netplay_replay_frames)
  {
    if (inputs.size() != header.bytes_per_frame || !file.WriteArray(&frame, 1) ||
        !file.WriteBytes(inputs.data(), inputs.size()))
    {
      return false;
    }
  }
  return true;
}

namespace
{
struct ParsedNetplayReplay
{
  NetplayReplayHeader header;
  std::vector<u64> initial_mem1_page_hashes;
  std::vector<u8> sysconf;
  std::vector<std::pair<s32, std::vector<u8>>> frames;
};

bool ReadNetplayReplay(const std::string& path, ParsedNetplayReplay* replay)
{
  File::IOFile file(path, "rb");
  NetplayReplayHeader& header = replay->header;
  if (!file || !file.ReadArray(&header, 1) || header.magic != NETPLAY_REPLAY_MAGIC ||
      header.version != NETPLAY_REPLAY_VERSION || header.players < 1 ||
      header.players > MAX_PORTS ||
      header.frame_boundary > static_cast<u32>(FrameBoundary::VIPreSIPoll) ||
      header.bytes_per_frame != header.players * sizeof(WirePad) || header.frame_count > 10'000'000)
  {
    return false;
  }

  if ((header.reserved[0] & NETPLAY_REPLAY_HAS_MEM1_PAGE_HASHES) != 0)
  {
    if (header.reserved[1] != NETPLAY_REPLAY_MEM1_PAGE_SHIFT)
      return false;
    u32 page_count = 0;
    constexpr u32 MAX_MEM1_PAGE_COUNT = Memory::MEM1_SIZE_MAX / NETPLAY_REPLAY_MEM1_PAGE_SIZE;
    if (!file.ReadArray(&page_count, 1) || page_count == 0 || page_count > MAX_MEM1_PAGE_COUNT)
      return false;
    replay->initial_mem1_page_hashes.resize(page_count);
    if (!file.ReadArray(replay->initial_mem1_page_hashes.data(), page_count))
      return false;
  }

  if ((header.reserved[0] & NETPLAY_REPLAY_HAS_SYSCONF) != 0)
  {
    u32 sysconf_size = 0;
    if (!file.ReadArray(&sysconf_size, 1) || sysconf_size != NETPLAY_REPLAY_SYSCONF_SIZE)
      return false;
    replay->sysconf.resize(sysconf_size);
    if (!file.ReadBytes(replay->sysconf.data(), replay->sysconf.size()))
      return false;
  }

  replay->frames.reserve(static_cast<std::size_t>(header.frame_count));
  for (u64 i = 0; i < header.frame_count; ++i)
  {
    s32 frame = 0;
    std::vector<u8> inputs(header.bytes_per_frame);
    if (!file.ReadArray(&frame, 1) || !file.ReadBytes(inputs.data(), inputs.size()))
      return false;
    if (!replay->frames.empty() && frame <= replay->frames.back().first)
      return false;
    replay->frames.emplace_back(frame, std::move(inputs));
  }
  return true;
}

bool InstallNetplayReplay(ParsedNetplayReplay replay)
{
  const NetplayReplayHeader& header = replay.header;

  std::lock_guard lk(g_manager.mutex);
  g_manager.netplay_replay_recording = false;
  g_manager.netplay_replay_playing = true;
  g_manager.netplay_replay_players = static_cast<int>(header.players);
  g_manager.netplay_replay_boundary = static_cast<FrameBoundary>(header.frame_boundary);
  const auto game_id_end = std::find(header.game_id.begin(), header.game_id.end(), '\0');
  g_manager.netplay_replay_game_id.assign(
      header.game_id.data(), static_cast<std::size_t>(game_id_end - header.game_id.begin()));
  const auto game_key_end = std::find(header.game_key.begin(), header.game_key.end(), '\0');
  g_manager.netplay_replay_game_key.assign(
      header.game_key.data(), static_cast<std::size_t>(game_key_end - header.game_key.begin()));
  g_manager.netplay_replay_playback_frames = std::move(replay.frames);
  g_manager.netplay_replay_playback_index = 0;
  g_manager.netplay_replay_initial_rtc = header.initial_rtc;
  g_manager.netplay_replay_initial_digest.frame = header.first_frame;
  g_manager.netplay_replay_initial_digest.root = header.initial_root;
  g_manager.netplay_replay_initial_digest.mem1 = header.initial_mem1;
  g_manager.netplay_replay_initial_digest.mem2 = header.initial_mem2;
  g_manager.netplay_replay_initial_digest.aram = header.initial_aram;
  g_manager.netplay_replay_initial_digest.fake_vmem = header.initial_fake_vmem;
  g_manager.netplay_replay_initial_digest.l1 = header.initial_l1;
  g_manager.netplay_replay_initial_digest.core_ticks = header.initial_core_ticks;
  g_manager.netplay_replay_initial_digest.timebase = header.initial_timebase;
  g_manager.netplay_replay_initial_digest.fake_tb_start_value = header.initial_fake_tb_start_value;
  g_manager.netplay_replay_initial_digest.fake_tb_start_ticks = header.initial_fake_tb_start_ticks;
  g_manager.netplay_replay_initial_digest.decrementer = header.initial_decrementer;
  g_manager.netplay_replay_initial_digest_valid = true;
  g_manager.netplay_replay_initial_mem1_page_hashes = std::move(replay.initial_mem1_page_hashes);
  g_manager.netplay_replay_sysconf = std::move(replay.sysconf);
  g_manager.latched_pads.fill(GCPadStatus{});
  g_manager.late_vi_guard = true;
  g_manager.vi_boundary_pending.store(false, std::memory_order_relaxed);
  g_manager.precise_vi_tail.store(false, std::memory_order_relaxed);
  g_manager.precise_vi_guard_requested.store(false, std::memory_order_relaxed);
  NOTICE_LOG_FMT(CORE, "Loaded Netplay Replay with {} frames, {} players, boundary={}",
                 header.frame_count, header.players,
                 FrameBoundaryName(g_manager.netplay_replay_boundary));
  return true;
}
}  // namespace

bool LoadNetplayReplay(const std::string& path)
{
  ParsedNetplayReplay replay{};
  return ReadNetplayReplay(path, &replay) && InstallNetplayReplay(std::move(replay));
}

bool IsNetplayReplayRecording()
{
  std::lock_guard lk(g_manager.mutex);
  return g_manager.netplay_replay_recording.load(std::memory_order_relaxed);
}

std::string GetNetplayReplayGameID()
{
  std::lock_guard lk(g_manager.mutex);
  return g_manager.netplay_replay_game_id;
}

int GetNetplayReplayPlayerCount()
{
  std::lock_guard lk(g_manager.mutex);
  return g_manager.netplay_replay_players;
}

u64 GetNetplayReplayInitialRTC()
{
  std::lock_guard lk(g_manager.mutex);
  return g_manager.netplay_replay_initial_rtc;
}

std::vector<u8> GetNetplayReplaySysconf()
{
  std::lock_guard lk(g_manager.mutex);
  return g_manager.netplay_replay_sysconf;
}

bool IsNetplayReplayPlaying()
{
  std::lock_guard lk(g_manager.mutex);
  return g_manager.netplay_replay_playing.load(std::memory_order_relaxed);
}

bool IsNetplayReplayInputActive()
{
  std::lock_guard lk(g_manager.mutex);
  return g_manager.replay_input_environment_active;
}

void StopNetplayReplay()
{
  std::lock_guard lk(g_manager.mutex);
  g_manager.netplay_replay_recording = false;
  g_manager.netplay_replay_playing = false;
  g_manager.netplay_replay_frames.clear();
  g_manager.netplay_replay_playback_frames.clear();
  g_manager.netplay_replay_playback_index = 0;
  g_manager.netplay_replay_initial_rtc = 0;
  g_manager.netplay_replay_initial_digest = {};
  g_manager.netplay_replay_initial_digest_valid = false;
  g_manager.netplay_replay_initial_mem1_page_hashes.clear();
  g_manager.netplay_replay_sysconf.clear();
  g_manager.netplay_replay_game_id.clear();
  g_manager.netplay_replay_game_key.clear();
}

bool IsGekkoSessionActive()
{
  return g_manager.active.load(std::memory_order_relaxed);
}

bool IsGekkoFrameBoundary(FrameBoundary boundary)
{
  return g_manager.active.load(std::memory_order_acquire) && g_manager.frame_boundary == boundary;
}

bool QueueLocalPresentationChange(std::function<void()> change)
{
  if (!change || !g_manager.active.load(std::memory_order_acquire))
    return false;

  std::lock_guard lock(s_presentation_change_mutex);
  if (!g_manager.active.load(std::memory_order_acquire))
    return false;

  // A second window-state notification before the next frame supersedes the first one. Only the
  // final requested presentation state matters, and executing both would recreate the surface
  // twice at consecutive boundaries.
  s_pending_presentation_change = std::move(change);
  return true;
}

void SetGekkoLocalDelay(int local_delay)
{
  const int clamped_delay = std::clamp(local_delay, 0, 60);
  std::lock_guard lk(g_manager.mutex);
  if (!g_manager.session || g_manager.local_handle < 0)
    return;

  const int previous_delay = g_manager.configured_local_delay;
  if (previous_delay == clamped_delay)
    return;

  gekko_set_local_delay(g_manager.session, g_manager.local_handle,
                        static_cast<unsigned char>(clamped_delay));
  g_manager.configured_local_delay = clamped_delay;
  g_manager.perf_delay_checks = 0;
  g_manager.perf_delay_matches = 0;
  g_manager.perf_delay_mismatches = 0;
  g_manager.perf_delay_missing = 0;
  g_manager.perf_delay_transitions = 0;
  g_manager.perf_delay_transition_frames = 0;
  g_manager.perf_delay_transition_min = 0;
  g_manager.perf_delay_transition_max = 0;
  NOTICE_LOG_FMT(CORE, "GekkoNet: Live local input delay changed {} -> {} at sample frame {}",
                 previous_delay, clamped_delay, g_manager.local_input_frame);
}

static bool StartGekkoSessionImpl(const std::string& game_name, u32 session_id, int players,
                                  int local_player,
                                  const std::vector<std::string>& player_endpoints, int local_delay,
                                  int prediction_window, const GekkoSessionOptions& options,
                                  bool replay_playback)
{
  StopGekkoSession();

  std::lock_guard lk(g_manager.mutex);

  if (players < 1 || players > 4 || local_player < 1 || local_player > players)
  {
    ERROR_LOG_FMT(CORE, "GekkoNet: Invalid session parameters (players={}, local={})", players,
                  local_player);
    return false;
  }
  if (!options.controller_types.empty() &&
      options.controller_types.size() != static_cast<std::size_t>(players))
  {
    ERROR_LOG_FMT(CORE, "GekkoNet: Controller roster has {} entries for {} players",
                  options.controller_types.size(), players);
    return false;
  }
  if (!options.brawl_controller_profiles.empty() &&
      options.brawl_controller_profiles.size() != static_cast<std::size_t>(players))
  {
    ERROR_LOG_FMT(CORE, "GekkoNet: Controller profile roster has {} entries for {} players",
                  options.brawl_controller_profiles.size(), players);
    return false;
  }
  if (replay_playback && (!g_manager.netplay_replay_playing.load(std::memory_order_relaxed) ||
                          g_manager.netplay_replay_playback_frames.empty() ||
                          players != g_manager.netplay_replay_players))
  {
    ERROR_LOG_FMT(CORE, "GekkoNet: Invalid Netplay Replay session state");
    return false;
  }
  if (options.simulate_remote_p2 && (players != 2 || local_player != 1))
  {
    ERROR_LOG_FMT(CORE, "GekkoNet: simulated remote player 2 requires a two-player host session");
    return false;
  }
  if (options.native_stress &&
      (!options.stress_test || options.simulate_remote_p2 || players != 1 || local_player != 1))
  {
    ERROR_LOG_FMT(CORE, "GekkoNet: native stress requires a one-player stress session without a "
                        "simulated peer");
    return false;
  }

  std::vector<std::pair<s32, WirePad>> replay_local_inputs;
  std::vector<std::pair<s32, std::vector<u8>>> replay_expected_inputs;
  u64 replay_input_initial_rtc = 0;
  std::vector<u8> replay_input_sysconf;
  if (!replay_playback && !options.replay_input_path.empty())
  {
    ParsedNetplayReplay replay{};
    if (!ReadNetplayReplay(options.replay_input_path, &replay) ||
        replay.header.players != static_cast<u32>(players) ||
        replay.header.frame_boundary != static_cast<u32>(options.frame_boundary))
    {
      ERROR_LOG_FMT(CORE, "GekkoNet: Invalid local NPR input source {}", options.replay_input_path);
      return false;
    }

    const std::size_t input_offset = static_cast<std::size_t>(local_player - 1) * sizeof(WirePad);
    replay_local_inputs.reserve(replay.frames.size());
    replay_expected_inputs.reserve(replay.frames.size());
    replay_input_initial_rtc = replay.header.initial_rtc;
    replay_input_sysconf = replay.sysconf;
    s32 source_frame = replay.header.first_frame;
    s32 logical_frame = 0;
    for (const auto& [frame, inputs] : replay.frames)
    {
      if (frame != source_frame || inputs.size() < input_offset + sizeof(WirePad))
      {
        ERROR_LOG_FMT(CORE, "GekkoNet: Local NPR input source is missing recorded frame {}",
                      source_frame);
        return false;
      }
      WirePad wire{};
      std::memcpy(&wire, inputs.data() + input_offset, sizeof(wire));
      replay_local_inputs.emplace_back(logical_frame, wire);
      std::vector<u8> expanded_inputs(static_cast<std::size_t>(players) * sizeof(WireInput));
      for (int port = 0; port < players; ++port)
      {
        WirePad recorded_pad{};
        std::memcpy(&recorded_pad,
                    inputs.data() + static_cast<std::size_t>(port) * sizeof(WirePad),
                    sizeof(recorded_pad));
        const WireInput expanded = EncodeGameCubeInput(recorded_pad);
        std::memcpy(expanded_inputs.data() + static_cast<std::size_t>(port) * sizeof(WireInput),
                    &expanded, sizeof(expanded));
      }
      replay_expected_inputs.emplace_back(logical_frame, std::move(expanded_inputs));
      ++source_frame;
      ++logical_frame;
    }
  }
  if (!gekko_create(&g_manager.session,
                    options.native_stress ? GekkoStressSession : GekkoGameSession))
  {
    ERROR_LOG_FMT(CORE, "GekkoNet: Failed to create session");
    return false;
  }

  const int clamped_delay = std::clamp(local_delay, 0, 60);
  const int clamped_rollback_window = std::clamp(prediction_window, 1, 10);
  // Gekko allows prediction only while prediction_window > frames_already_predicted. The stress
  // transport therefore gets one internal prediction slot beyond the selected rollback distance,
  // or both simulated peers can reach the cap and stop the emulated clock that releases packets.
  const int clamped_prediction =
      options.stress_test ? clamped_rollback_window + clamped_delay + 2 : clamped_rollback_window;

  GekkoConfig config{};
  config.num_players = static_cast<unsigned char>(players);
  config.max_spectators = 0;
  config.input_prediction_window = static_cast<unsigned char>(clamped_prediction);
  config.spectator_delay = 0;
  config.input_size = static_cast<unsigned int>(sizeof(WireInput));
  config.state_size = 0;  // State is maintained internally in Dolphin's SnapshotRing
  // GekkoNet's limited-saving mode rebuilds confirmed snapshots by loading and replaying frames.
  // That is counterproductive for whole-emulator states, whose loads and replay frames are much
  // more expensive than a small game's state copy. Keep per-frame snapshots until GekkoNet has an
  // Orca-style sparse forward-snapshot policy.
  config.limited_saving = false;
  // Dolphin's rollback snapshots deliberately do not provide hashes to GekkoNet, so its checksum
  // comparison cannot produce meaningful desync results.
  config.desync_detection = false;
  // StressSession loads the state at current-check_distance, then advances from the following
  // frame through current-1. Therefore its replay count is check_distance-1; add one so the
  // harness's RollbackFrames value means the number of frames actually re-executed.
  config.check_distance =
      options.native_stress ? static_cast<unsigned int>(clamped_rollback_window + 1) : 10;
  config.stress_interval = std::max(1u, options.stress_interval);

  gekko_start(g_manager.session, &config);

  GekkoNetAdapter* adapter = nullptr;
  if (replay_playback || options.native_stress)
  {
    // Local-only Gekko sessions do not need a transport adapter. Replay playback supplies every
    // actor's input from the NPR stream; native stress generates its input locally.
  }
  else if (options.simulate_remote_p2)
    adapter = &s_main_simulated_adapter;
  else
  {
    if (!Common::ENet::StartRollbackDatagrams(session_id))
    {
      ERROR_LOG_FMT(CORE, "GekkoNet: no shared NetPlay UDP socket is registered");
      gekko_destroy(&g_manager.session);
      return false;
    }
    adapter = &s_shared_adapter;
  }

  if (!replay_playback && !options.native_stress && !adapter)
  {
    ERROR_LOG_FMT(CORE, "GekkoNet: Failed to create network adapter");
    gekko_destroy(&g_manager.session);
    return false;
  }
  g_manager.native_adapter_active =
      !replay_playback && !options.native_stress && !options.simulate_remote_p2;
  if (adapter)
    gekko_net_adapter_set(g_manager.session, adapter);
  const auto destroy_failed_session = [&] {
    gekko_destroy(&g_manager.session);
    if (g_manager.native_adapter_active)
    {
      Common::ENet::StopRollbackDatagrams();
      g_manager.native_adapter_active = false;
    }
  };

  gekko_set_runahead(g_manager.session, 0);

  g_manager.players = players;
  g_manager.local_player = local_player;
  g_manager.controller_types.fill(ControllerType::GameCube);
  g_manager.brawl_controller_profiles = options.brawl_controller_profiles;
  if (!replay_playback)
  {
    std::copy(options.controller_types.begin(), options.controller_types.end(),
              g_manager.controller_types.begin());
  }
  // Capture the first local device before NetPlay's boot layer remaps local SI devices to their
  // in-game seats. This remains port 0 on every client regardless of its network player number.
  g_manager.local_uses_gc_adapter =
      !replay_playback &&
      Config::Get(Config::GetInfoForSIDevice(0)) == SerialInterface::SIDEVICE_WIIU_ADAPTER;
  g_manager.netplay_replay_session = replay_playback;
  g_manager.debug_p2_cstick = options.debug_p2_cstick;
  g_manager.simulate_remote_p2 = options.simulate_remote_p2;
  g_manager.stress_test = options.stress_test;
  g_manager.enable_random_inputs = options.enable_random_inputs;
  g_manager.disable_project_plus_music = options.disable_project_plus_music;
  g_manager.native_stress = options.native_stress;
  g_manager.stress_interval = std::max(1u, options.stress_interval);
  g_manager.stress_rollback_frames =
      options.stress_test ? static_cast<u32>(clamped_rollback_window) : 0;
  g_manager.remote_ram_match =
      options.remote_ram_match && !options.simulate_remote_p2 && players > 1;
  g_manager.remote_ram_match_activation_acked_players = 0;
  g_manager.compare_confirmed_ram = (options.compare_confirmed_ram || g_manager.remote_ram_match) &&
                                    !options.simulate_remote_p2 && players > 1;
  g_manager.track_host_memory_writes.store(g_manager.compare_confirmed_ram,
                                           std::memory_order_release);
  g_manager.compare_confirmed_video =
      options.compare_confirmed_video && !options.simulate_remote_p2 && players > 1;
  g_manager.stop_ram_compare_on_mismatch =
      options.stop_ram_compare_on_mismatch && g_manager.compare_confirmed_ram;
  SetDetailedResimProfilingEnabled(options.deep_logging);
  DirtyPages::SetCommitProfilingEnabled(options.deep_logging);
  DirtyPages::SetParallelCompareEnabled(options.parallel_dirty_compare);
  SetCoarseFifoTiming(options.coarse_fifo_timing);
  g_manager.late_vi_guard = options.late_vi_guard;
  g_manager.ram_diagnostic_dumped = false;
  g_manager.video_diagnostic_dumped = false;
  g_manager.frame_boundary = options.frame_boundary;
  g_manager.vi_boundary_pending.store(false, std::memory_order_relaxed);
  g_manager.configured_local_delay = clamped_delay;
  g_manager.configured_prediction_window = clamped_prediction;
  g_manager.session_id = session_id;
  g_manager.local_input_frame = 0;
  g_manager.replay_local_inputs = std::move(replay_local_inputs);
  g_manager.replay_expected_inputs = std::move(replay_expected_inputs);
  g_manager.replay_input_mismatch_reported = false;
  g_manager.replay_input_environment_active = !g_manager.replay_local_inputs.empty();
  if (g_manager.replay_input_environment_active)
  {
    g_manager.netplay_replay_initial_rtc = replay_input_initial_rtc;
    g_manager.netplay_replay_sysconf = std::move(replay_input_sysconf);
  }
  // GetRollbackPad is active throughout boot, before Gekko emits the first Advance event. Never
  // expose pads left latched at the end of the previous session to the next game's SI startup.
  g_manager.latched_pads.fill(GCPadStatus{});
  const auto neutral_wiimote = WiimoteEmu::SerializeDesiredState({});
  g_manager.latched_wiimotes.fill(neutral_wiimote);
  g_manager.pending_local_wiimote = neutral_wiimote;
  g_manager.local_handle = -1;
  g_manager.simulated_peer_p2_handle = -1;
  g_manager.remote_handle = -1;
  g_manager.player_handles.assign(players, -1);
  g_manager.remote_endpoints.clear();
  g_manager.pending_events.clear();
  g_manager.sampled_local_inputs.clear();
  {
    std::lock_guard latency_lk(g_manager.input_latency_mutex);
    g_manager.pending_input_latency_frames.clear();
    g_manager.perf_input_latency_count = 0;
    g_manager.perf_input_latency_dropped = 0;
    g_manager.perf_input_latency_unmatched_presents = 0;
  }
  g_manager.ram_digests.clear();
  g_manager.remote_ram_digests.clear();
  g_manager.first_ram_mismatch_frame.reset();
  g_manager.first_video_mismatch_frame.reset();
  g_manager.first_derived_video_mismatch_frame.reset();
  g_manager.first_non_ram_video_mismatch_frame.reset();
  g_manager.ram_mismatch_details_frame.reset();
  g_manager.first_powerpc_mismatch_frame.reset();
  g_manager.last_advance_input_hash = 0;
  g_manager.last_advance_has_start = false;
  g_manager.last_sampled_start = false;
  g_manager.diagnostic_events.clear();
  g_manager.host_memory_writes.clear();
  g_manager.restore_mismatch_reported = false;
  g_manager.perf_restore_checks = 0;
  g_manager.perf_restore_mismatches = 0;
  g_manager.perf_window_start = {};
  g_manager.perf_dirty_pages_start = DirtyPages::GetCounters();
  g_manager.perf_real_frames = 0;
  g_manager.perf_replay_frames = 0;
  g_manager.perf_save_count = 0;
  g_manager.perf_load_count = 0;
  g_manager.perf_pump_count = 0;
  g_manager.perf_save_ms = 0.0;
  g_manager.perf_save_max_ms = 0.0;
  g_manager.perf_load_ms = 0.0;
  g_manager.perf_load_max_ms = 0.0;
  g_manager.perf_pump_ms = 0.0;
  g_manager.perf_pump_max_ms = 0.0;
  g_manager.perf_save_phases = {};
  g_manager.perf_load_phases = {};
  g_manager.last_local_wire.reset();
  g_manager.perf_local_input_changes = 0;
  g_manager.perf_local_byte_changes.fill(0);
  g_manager.last_applied_local_wire.reset();
  g_manager.perf_delay_checks = 0;
  g_manager.perf_delay_matches = 0;
  g_manager.perf_delay_mismatches = 0;
  g_manager.perf_delay_missing = 0;
  g_manager.perf_delay_transitions = 0;
  g_manager.perf_delay_transition_frames = 0;
  g_manager.perf_delay_transition_min = 0;
  g_manager.perf_delay_transition_max = 0;
  g_manager.frame_execution_start.reset();
  g_manager.frame_execution_resim = false;
  g_manager.frame_execution_gekko_frame = -1;
  if (IsDetailedResimProfilingEnabled())
  {
    g_manager.frame_execution_jit_start = GetResimJitCompileStats();
    g_manager.frame_execution_event_start = GetResimEventStats();
  }
  g_manager.perf_replay_exec_count = 0;
  g_manager.perf_replay_exec_ms = 0.0;
  g_manager.perf_replay_exec_max_ms = 0.0;
  g_manager.perf_resim_enter_sync_count = 0;
  g_manager.perf_resim_enter_sync_ms = 0.0;
  g_manager.perf_resim_enter_sync_max_ms = 0.0;
  g_manager.perf_resim_exit_sync_count = 0;
  g_manager.perf_resim_exit_sync_ms = 0.0;
  g_manager.perf_resim_exit_sync_max_ms = 0.0;
  g_manager.perf_replay_jit_blocks = 0;
  g_manager.perf_replay_jit_nanoseconds = 0;
  g_manager.perf_replay_events = {};
  g_manager.perf_replay_save_count = 0;
  g_manager.perf_replay_save_ms = 0.0;
  g_manager.perf_replay_save_max_ms = 0.0;
  g_manager.rollback_burst_start.reset();
  g_manager.perf_rollback_burst_count = 0;
  g_manager.perf_rollback_burst_ms = 0.0;
  g_manager.perf_rollback_burst_max_ms = 0.0;
  g_manager.perf_burst_load_ms = 0.0;
  g_manager.perf_burst_load_max_ms = 0.0;
  g_manager.perf_burst_exec_ms = 0.0;
  g_manager.perf_burst_exec_max_ms = 0.0;
  g_manager.perf_burst_save_ms = 0.0;
  g_manager.perf_burst_save_max_ms = 0.0;
  g_manager.perf_burst_other_ms = 0.0;
  g_manager.perf_burst_other_max_ms = 0.0;
  g_manager.current_burst_load_ms = 0.0;
  g_manager.current_burst_exec_ms = 0.0;
  g_manager.current_burst_save_ms = 0.0;
  g_manager.current_burst_jit_blocks_invalidated = 0;
  g_manager.current_rollback_replays = 0;
  g_manager.perf_rollback_depth_total = 0;
  g_manager.perf_rollback_depth_max = 0;
  g_manager.last_real_advance.reset();
  g_manager.perf_real_interval_count = 0;
  g_manager.perf_real_interval_ms = 0.0;
  g_manager.perf_real_interval_max_ms = 0.0;
  g_manager.perf_real_gaps_over_20_ms = 0;
  g_manager.perf_real_gaps_over_25_ms = 0;
  g_manager.perf_real_gaps_over_33_ms = 0;
  g_manager.perf_real_gaps_over_50_ms = 0;
  g_manager.visible_frame_pacing_pending = false;
  g_manager.deferred_visible_follows_rollback = false;
  g_manager.throttle_reference_before_load.reset();

  for (int p = 1; p <= players; ++p)
  {
    if (replay_playback || p == local_player)
    {
      const int handle = gekko_add_actor(g_manager.session, GekkoLocalPlayer, nullptr);
      if (handle < 0)
      {
        ERROR_LOG_FMT(CORE, "GekkoNet: Failed to add local actor");
        destroy_failed_session();
        return false;
      }
      if (p == local_player)
        g_manager.local_handle = handle;
      g_manager.player_handles[p - 1] = handle;
      gekko_set_local_delay(g_manager.session, handle,
                            static_cast<unsigned char>(replay_playback ? 0 : clamped_delay));
    }
    else if (options.simulate_remote_p2 && p == 2)
    {
      GekkoNetAddress addr{const_cast<char*>(SIMULATED_P2_ADDRESS.data()),
                           static_cast<unsigned int>(SIMULATED_P2_ADDRESS.size())};
      const int handle = gekko_add_actor(g_manager.session, GekkoRemotePlayer, &addr);
      if (handle < 0)
      {
        ERROR_LOG_FMT(CORE, "GekkoNet: Failed to add simulated remote player 2 actor");
        destroy_failed_session();
        return false;
      }
      g_manager.player_handles[p - 1] = handle;
      g_manager.remote_handle = handle;
    }
    else
    {
      if (p >= static_cast<int>(player_endpoints.size()) || player_endpoints[p].empty())
      {
        ERROR_LOG_FMT(CORE, "GekkoNet: Missing native endpoint for player {}", p);
        destroy_failed_session();
        return false;
      }
      NOTICE_LOG_FMT(CORE, "GekkoNet: Native endpoint configured for player {}", p);
      ENetAddress endpoint{};
      if (!ParsePeerEndpoint(player_endpoints[p], &endpoint))
      {
        ERROR_LOG_FMT(CORE, "GekkoNet: Invalid native endpoint for player {}", p);
        destroy_failed_session();
        return false;
      }
      GekkoNetAddress addr{&endpoint, sizeof(endpoint)};
      const int handle = gekko_add_actor(g_manager.session, GekkoRemotePlayer, &addr);
      if (handle < 0)
      {
        ERROR_LOG_FMT(CORE, "GekkoNet: Failed to add remote actor");
        destroy_failed_session();
        return false;
      }
      if (g_manager.remote_handle < 0)
        g_manager.remote_handle = handle;
      g_manager.player_handles[p - 1] = handle;
      g_manager.remote_endpoints.push_back(endpoint);
    }
  }

  ResetSimulatedLink();
  // For a real two-process benchmark, delay only the P2 -> P1 receive path. Delaying both
  // directions also delays acknowledgements and can make Gekko batch traffic instead of exposing
  // a clean, repeatable remote-input misprediction.
  s_stress_mode = !options.native_stress && options.stress_test &&
                  (options.simulate_remote_p2 || local_player == 1);
  s_stress_packet_delay_frames = options.stress_test && !options.native_stress ?
                                     static_cast<u64>(clamped_rollback_window + clamped_delay + 1) :
                                     0;

  if (options.simulate_remote_p2)
  {
    if (!gekko_create(&g_manager.simulated_peer_session, GekkoGameSession))
    {
      ERROR_LOG_FMT(CORE, "GekkoNet: Failed to create simulated player 2 peer session");
      destroy_failed_session();
      return false;
    }
    gekko_start(g_manager.simulated_peer_session, &config);
    gekko_net_adapter_set(g_manager.simulated_peer_session, &s_fake_simulated_adapter);
    gekko_set_runahead(g_manager.simulated_peer_session, 0);

    GekkoNetAddress p1_addr{const_cast<char*>(SIMULATED_P1_ADDRESS.data()),
                            static_cast<unsigned int>(SIMULATED_P1_ADDRESS.size())};
    const int fake_remote_p1 =
        gekko_add_actor(g_manager.simulated_peer_session, GekkoRemotePlayer, &p1_addr);
    const int fake_local_p2 =
        gekko_add_actor(g_manager.simulated_peer_session, GekkoLocalPlayer, nullptr);
    if (fake_remote_p1 < 0 || fake_local_p2 < 0)
    {
      ERROR_LOG_FMT(CORE, "GekkoNet: Failed to add actors to simulated player 2 peer");
      gekko_destroy(&g_manager.simulated_peer_session);
      destroy_failed_session();
      return false;
    }
    g_manager.simulated_peer_p2_handle = fake_local_p2;
    gekko_set_local_delay(g_manager.simulated_peer_session, fake_local_p2,
                          static_cast<unsigned char>(options.stress_test ? 0 : clamped_delay));
  }

  g_manager.tracked_bitmap_clear = options.tracked_bitmap_clear;
  g_manager.full_scan_benchmark = options.full_scan_benchmark;
  g_manager.full_scan_keep_tracking = options.full_scan_keep_tracking;
  const int snapshot_slots =
      g_manager.remote_ram_match ? REMOTE_RAM_MATCH_SNAPSHOT_SLOTS : RING_SNAPSHOT_SLOTS;
  g_manager.ring = std::make_unique<SnapshotRing>(
      snapshot_slots, g_manager.tracked_bitmap_clear, g_manager.full_scan_benchmark,
      g_manager.full_scan_keep_tracking, options.compare_confirmed_ram,
      options.compare_confirmed_video,
      options.deep_logging || g_manager.compare_confirmed_ram || g_manager.compare_confirmed_video);
  g_manager.stop_requested.store(false, std::memory_order_relaxed);
  g_manager.vi_boundary_pending.store(false, std::memory_order_relaxed);
  g_manager.precise_vi_tail.store(false, std::memory_order_relaxed);
  g_manager.precise_vi_guard_requested.store(false, std::memory_order_relaxed);
  g_manager.active.store(true, std::memory_order_release);

  NOTICE_LOG_FMT(
      CORE,
      "GekkoNet: Started {} shared NetPlay UDP session (players={}, local={}, session={}, "
      "delay={}, "
      "rollback_window={}, gekko_prediction_window={}, local_input_source={}, "
      "debug_p2_cstick={}, simulate_remote_p2={}, simulated_one_way_latency_ms={}, "
      "stress_enabled={}, native_stress={}, random_inputs={}, disable_project_plus_music={}, "
      "stress_input_period_frames={}, "
      "stress_rollback_frames={}, "
      "tracked_bitmap_clear={}, "
      "full_scan_benchmark={}, full_scan_keep_tracking={}, parallel_dirty_compare={}, "
      "compare_confirmed_ram={}, remote_ram_match={}, compare_confirmed_video={}, "
      "frame_boundary={})",
      game_name, players, local_player, session_id, clamped_delay, clamped_rollback_window,
      clamped_prediction,
      g_manager.replay_local_inputs.empty() ?
          (g_manager.local_uses_gc_adapter ? "gc_adapter" : "emulated_pad") :
          "npr",
      options.debug_p2_cstick, options.simulate_remote_p2,
      options.simulate_remote_p2 && !options.stress_test ? SIMULATED_P2_LATENCY.count() : 0,
      options.stress_test, options.native_stress, options.enable_random_inputs,
      options.disable_project_plus_music,
      options.native_stress ? std::max(1u, options.stress_interval) : STRESS_INPUT_PERIOD_FRAMES,
      clamped_rollback_window, g_manager.tracked_bitmap_clear, g_manager.full_scan_benchmark,
      g_manager.full_scan_keep_tracking, options.parallel_dirty_compare,
      g_manager.compare_confirmed_ram, g_manager.remote_ram_match,
      g_manager.compare_confirmed_video, FrameBoundaryName(g_manager.frame_boundary));
  NOTICE_LOG_FMT(CORE, "GekkoNet optimization settings: coarse_fifo_timing={}, late_vi_guard={}",
                 UseCoarseFifoTiming(), g_manager.late_vi_guard);
  return true;
}

bool StartGekkoSession(const std::string& game_name, u32 session_id, int players, int local_player,
                       const std::vector<std::string>& player_endpoints, int local_delay,
                       int prediction_window, const GekkoSessionOptions& options)
{
  return StartGekkoSessionImpl(game_name, session_id, players, local_player, player_endpoints,
                               local_delay, prediction_window, options, false);
}

bool StartNetplayReplaySession()
{
  int players = 0;
  FrameBoundary boundary = FrameBoundary::VIEndField;
  {
    std::lock_guard lk(g_manager.mutex);
    if (!g_manager.netplay_replay_playing.load(std::memory_order_relaxed) ||
        g_manager.netplay_replay_playback_frames.empty())
    {
      return false;
    }
    players = g_manager.netplay_replay_players;
    boundary = g_manager.netplay_replay_boundary;
  }

  GekkoSessionOptions options{};
  options.frame_boundary = boundary;
  options.late_vi_guard = true;
  return StartGekkoSessionImpl("Netplay Replay", 0, players, 1, {}, 0, 10, options, true);
}

void StopGekkoSession()
{
  // DirtyPages::Disarm clears active-session JIT blocks.  Stop requests normally originate on the
  // UI or NetPlay thread, so pause the emulated CPU before taking the manager lock and tearing the
  // tracker down.  Clearing the block cache while the CPU is executing it can make the host execute
  // the cache's breakpoint fill bytes (STATUS_BREAKPOINT on Windows).
  Core::CPUThreadGuard cpu_guard(Core::System::GetInstance());
  const auto stop_started = std::chrono::steady_clock::now();
  const auto elapsed_ms = [&] {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                     stop_started)
        .count();
  };
  NOTICE_LOG_FMT(CORE, "Shutdown lifecycle: StopGekkoSession waiting for manager mutex");
  std::lock_guard lk(g_manager.mutex);
  NOTICE_LOG_FMT(CORE, "Shutdown lifecycle: StopGekkoSession acquired manager mutex ({:.3f} ms)",
                 elapsed_ms());
  const bool had_session = g_manager.active.load(std::memory_order_relaxed) || g_manager.session ||
                           g_manager.simulated_peer_session || g_manager.native_adapter_active;

  g_manager.stop_requested.store(true, std::memory_order_relaxed);
  g_manager.active.store(false, std::memory_order_release);
  {
    std::lock_guard presentation_lock(s_presentation_change_mutex);
    s_pending_presentation_change = {};
  }
  g_manager.netplay_replay_session = false;
  g_manager.vi_boundary_pending.store(false, std::memory_order_relaxed);
  g_manager.precise_vi_tail.store(false, std::memory_order_relaxed);
  g_manager.precise_vi_guard_requested.store(false, std::memory_order_relaxed);

  if (g_manager.session)
  {
    NOTICE_LOG_FMT(CORE, "Shutdown lifecycle: destroying primary Gekko session ({:.3f} ms)",
                   elapsed_ms());
    gekko_destroy(&g_manager.session);
    g_manager.session = nullptr;
    NOTICE_LOG_FMT(CORE, "Shutdown lifecycle: primary Gekko session destroyed ({:.3f} ms)",
                   elapsed_ms());
  }
  if (g_manager.simulated_peer_session)
  {
    NOTICE_LOG_FMT(CORE, "Shutdown lifecycle: destroying simulated Gekko peer ({:.3f} ms)",
                   elapsed_ms());
    gekko_destroy(&g_manager.simulated_peer_session);
    g_manager.simulated_peer_session = nullptr;
    NOTICE_LOG_FMT(CORE, "Shutdown lifecycle: simulated Gekko peer destroyed ({:.3f} ms)",
                   elapsed_ms());
  }
  if (g_manager.native_adapter_active)
  {
    NOTICE_LOG_FMT(CORE, "Shutdown lifecycle: stopping rollback datagrams ({:.3f} ms)",
                   elapsed_ms());
    Common::ENet::StopRollbackDatagrams();
    g_manager.native_adapter_active = false;
    NOTICE_LOG_FMT(CORE, "Shutdown lifecycle: rollback datagrams stopped ({:.3f} ms)",
                   elapsed_ms());
  }
  ResetSimulatedLink();
  g_manager.pending_events.clear();
  g_manager.replay_local_inputs.clear();
  g_manager.replay_expected_inputs.clear();
  g_manager.replay_input_mismatch_reported = false;
  g_manager.replay_input_environment_active = false;
  g_manager.visible_frame_pacing_pending = false;
  g_manager.deferred_visible_follows_rollback = false;
  g_manager.throttle_reference_before_load.reset();
  if (g_manager.ring)
  {
    NOTICE_LOG_FMT(CORE, "Shutdown lifecycle: resetting rollback snapshot ring ({:.3f} ms)",
                   elapsed_ms());
    g_manager.ring->Reset(Core::System::GetInstance());
    NOTICE_LOG_FMT(CORE, "Shutdown lifecycle: rollback snapshot ring reset ({:.3f} ms)",
                   elapsed_ms());
  }
  g_manager.ring.reset();
  g_manager.track_host_memory_writes.store(false, std::memory_order_release);
  Rollback::SetResimulating(false);
  Rollback::SetDetailedResimProfilingEnabled(false);
  DirtyPages::SetCommitProfilingEnabled(false);
  DirtyPages::SetParallelCompareEnabled(false);
  Rollback::SetCoarseFifoTiming(false);
  if (had_session)
    NOTICE_LOG_FMT(CORE, "GekkoNet: Session stopped ({:.3f} ms shutdown)", elapsed_ms());
  NOTICE_LOG_FMT(CORE, "Shutdown lifecycle: StopGekkoSession completed ({:.3f} ms)", elapsed_ms());
}

static void ApplyDebugCStickPattern(GCPadStatus* status, u64 frame)
{
  // Change to the next of eight directions every two frames. Diagonals are normalized to
  // approximately the same radius as cardinal directions.
  constexpr int DIRECTION_FRAMES = 2;
  constexpr std::array<std::pair<int, int>, 8> DIRECTIONS{{
      {90, 0},
      {64, -64},
      {0, -90},
      {-64, -64},
      {-90, 0},
      {-64, 64},
      {0, 90},
      {64, 64},
  }};

  const auto [x, y] = DIRECTIONS[(frame / DIRECTION_FRAMES) % DIRECTIONS.size()];
  status->substickX = static_cast<u8>(GCPadStatus::C_STICK_CENTER_X + x);
  status->substickY = static_cast<u8>(GCPadStatus::C_STICK_CENTER_Y + y);

  // Mash both triggers on even frames and release them on odd frames. Override both the digital
  // click and analog pressure so the wire input changes unambiguously every frame.
  constexpr u16 TRIGGER_BUTTONS = PAD_TRIGGER_L | PAD_TRIGGER_R;
  const bool triggers_pressed = (frame & 1) == 0;
  if (triggers_pressed)
    status->button |= TRIGGER_BUTTONS;
  else
    status->button &= ~TRIGGER_BUTTONS;
  status->triggerLeft = triggers_pressed ? 255 : 0;
  status->triggerRight = triggers_pressed ? 255 : 0;
}

static void ApplyStressCStickPattern(GCPadStatus* status, u64 frame)
{
  // Four distinct values prevent a two-frame-delayed input from matching the repeat-last
  // prediction by parity, which would happen with a simple left/right alternation.
  constexpr std::array<std::pair<s8, s8>, 4> CARDINALS{{{-90, 0}, {0, 90}, {90, 0}, {0, -90}}};
  const auto [x, y] = CARDINALS[(frame / STRESS_INPUT_PERIOD_FRAMES) % CARDINALS.size()];
  status->substickX = static_cast<u8>(GCPadStatus::C_STICK_CENTER_X + x);
  status->substickY = static_cast<u8>(GCPadStatus::C_STICK_CENTER_Y + y);
}

static bool SubmitNetplayReplayInputs();

static bool SubmitLocalInput()
{
  if (g_manager.netplay_replay_session)
    return SubmitNetplayReplayInputs();

  WireInput wire{};
  const u64 sample_time_ns = static_cast<u64>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
          .count());
  if (!g_manager.replay_local_inputs.empty())
  {
    // NPR stores the input applied to each logical frame. Read ahead by the configured local delay
    // so Gekko's normal delay places that recorded input back onto the same target frame.
    const u64 source_frame = g_manager.local_input_frame + g_manager.configured_local_delay;
    if (source_frame >= g_manager.replay_local_inputs.size())
    {
      NOTICE_LOG_FMT(CORE, "GekkoNet: Local NPR input stream completed at frame {}",
                     g_manager.local_input_frame);
      return false;
    }
    const auto& [recorded_frame, recorded_wire] = g_manager.replay_local_inputs[source_frame];
    if (recorded_frame != static_cast<s32>(source_frame))
    {
      ERROR_LOG_FMT(CORE, "GekkoNet: Local NPR input expected frame {}, found {}", source_frame,
                    recorded_frame);
      return false;
    }
    wire = EncodeGameCubeInput(recorded_wire);
  }
  else if (g_manager.controller_types[g_manager.local_player - 1] == ControllerType::Wiimote)
  {
    wire = EncodeWiimoteInput(g_manager.pending_local_wiimote);
  }
  else
  {
    g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::SerialInterface);
    g_controller_interface.UpdateInput();

    // The network seat is not a local controller index. Every client samples its first local pad.
    constexpr int LOCAL_PAD = 0;
    GCPadStatus local_status =
        g_manager.local_uses_gc_adapter ? GCAdapter::Input(LOCAL_PAD) : Pad::GetStatus(LOCAL_PAD);
    const bool legacy_input_spam_active =
        g_manager.local_input_frame >= P2_INPUT_SPAM_START_FRAME;
    if (g_manager.debug_p2_cstick && legacy_input_spam_active)
    {
      ApplyDebugCStickPattern(&local_status,
                              g_manager.local_input_frame - P2_INPUT_SPAM_START_FRAME);
    }
    // The client-facing random-input toggle is an explicit replacement for this client's own
    // sampled pad. It must not inherit the legacy P2 stress diagnostic's 500-frame warm-up.
    if (g_manager.enable_random_inputs)
    {
      ApplyStressCStickPattern(&local_status, g_manager.local_input_frame);
    }
    wire = EncodeGameCubeInput(EncodePad(local_status));
  }

  const bool sampled_start =
      wire.controller_type == static_cast<u8>(ControllerType::GameCube) && WirePadHasStart(wire.pad);
  if (sampled_start != g_manager.last_sampled_start)
  {
    if (g_manager.compare_confirmed_ram)
    {
      NOTICE_LOG_FMT(CORE, "GekkoNet input trace: local P{} sampled START {} at input frame {}",
                     g_manager.local_player, sampled_start ? "DOWN" : "UP",
                     g_manager.local_input_frame);
    }
    g_manager.last_sampled_start = sampled_start;
  }

  if (g_manager.last_local_wire)
  {
    bool changed = false;
    const auto* wire_bytes = reinterpret_cast<const u8*>(&wire);
    const auto* previous_bytes = reinterpret_cast<const u8*>(&*g_manager.last_local_wire);
    for (std::size_t i = 0; i < sizeof(WireInput); ++i)
    {
      if (wire_bytes[i] != previous_bytes[i])
      {
        changed = true;
        ++g_manager.perf_local_byte_changes[i];
      }
    }
    if (changed)
      ++g_manager.perf_local_input_changes;
  }
  g_manager.last_local_wire = wire;

  // Keep the raw host sample beside the Gekko frame on which it was submitted. When Gekko later
  // emits the local player's input for an Advance event, this lets diagnostics verify the actual
  // delay behavior instead of merely reporting the configured value.
  g_manager.sampled_local_inputs.push_back(
      {static_cast<s64>(g_manager.local_input_frame), wire, sample_time_ns});

  gekko_add_local_input(g_manager.session, g_manager.local_handle, &wire);

  if (g_manager.simulated_peer_session && g_manager.simulated_peer_p2_handle >= 0)
  {
    GCPadStatus simulated_p2{};
    if (g_manager.local_input_frame >= P2_INPUT_SPAM_START_FRAME)
    {
      const u64 spam_frame = g_manager.local_input_frame - P2_INPUT_SPAM_START_FRAME;
      if (g_manager.stress_test)
        ApplyStressCStickPattern(&simulated_p2, spam_frame);
      else
        ApplyDebugCStickPattern(&simulated_p2, spam_frame);
    }
    WireInput simulated_wire = EncodeGameCubeInput(EncodePad(simulated_p2));
    gekko_add_local_input(g_manager.simulated_peer_session, g_manager.simulated_peer_p2_handle,
                          &simulated_wire);
  }
  ++g_manager.local_input_frame;
  s_simulated_link_frame = g_manager.local_input_frame;
  return true;
}

static void PumpSimulatedPeer()
{
  if (!g_manager.simulated_peer_session)
    return;

  gekko_network_poll(g_manager.simulated_peer_session);
  // Once deterministic frame delay is active, advance the hidden protocol peer exactly once per
  // visible real frame. Repeated calls in the 100 us wait loop still poll packets but cannot make
  // the hidden session race ahead and change the requested rollback depth.
  if (g_manager.stress_test && s_stress_frame_delay_active &&
      s_last_stress_peer_update_frame == s_simulated_link_frame)
  {
    return;
  }
  if (g_manager.stress_test && s_stress_frame_delay_active)
    s_last_stress_peer_update_frame = s_simulated_link_frame;

  int event_count = 0;
  GekkoGameEvent** events = gekko_update_session(g_manager.simulated_peer_session, &event_count);
  for (int i = 0; i < event_count; ++i)
  {
    GekkoGameEvent* event = events[i];
    if (!event)
      continue;
    // The hidden peer exists only to exercise GekkoNet's real remote-input path. Its emulation
    // state is the visible Dolphin instance, so acknowledge peer-side state events without making
    // a second, non-authoritative save/load implementation.
    if (event->type == GekkoSaveEvent)
    {
      if (event->data.save.checksum)
        *event->data.save.checksum = 0;
      if (event->data.save.state_len)
        *event->data.save.state_len = 0;
    }
  }
  int session_event_count = 0;
  gekko_session_events(g_manager.simulated_peer_session, &session_event_count);
}

static void LatchInputs(const unsigned char* raw_inputs)
{
  if (!raw_inputs)
    return;

  for (int p = 0; p < g_manager.players; ++p)
  {
    const int handle = g_manager.player_handles[p];
    if (handle >= 0 && handle < g_manager.players)
    {
      WireInput wire{};
      std::memcpy(&wire, raw_inputs + (handle * sizeof(WireInput)), sizeof(WireInput));
      if (g_manager.controller_types[p] == ControllerType::Wiimote)
        g_manager.latched_wiimotes[p] = DecodeWiimoteInput(wire);
      else
        g_manager.latched_pads[p] = DecodePad(wire.pad);
    }
  }
}

static void VerifyConfirmedReplayInputs(const QueuedEvent& event)
{
  if (g_manager.replay_expected_inputs.empty() || g_manager.replay_input_mismatch_reported ||
      event.frame < 0 ||
      static_cast<std::size_t>(event.frame) >= g_manager.replay_expected_inputs.size())
  {
    return;
  }

  // Predictions may differ transiently. Compare only once every actor has supplied this frame.
  for (const int handle : g_manager.player_handles)
  {
    if (handle < 0 || gekko_last_received_frame(g_manager.session, handle) < event.frame)
      return;
  }

  const auto& [expected_frame, expected] =
      g_manager.replay_expected_inputs[static_cast<std::size_t>(event.frame)];
  if (expected_frame != event.frame)
    return;

  const std::size_t compared_size = std::min(expected.size(), event.inputs.size());
  std::size_t mismatch = compared_size;
  for (std::size_t i = 0; i < compared_size; ++i)
  {
    if (expected[i] != event.inputs[i])
    {
      mismatch = i;
      break;
    }
  }
  if (mismatch == compared_size && expected.size() == event.inputs.size())
    return;

  g_manager.replay_input_mismatch_reported = true;
  ERROR_LOG_FMT(CORE,
                "GekkoNet: confirmed NPR input mismatch at frame {} byte {} (expected_size={}, "
                "actual_size={})",
                event.frame, mismatch, expected.size(), event.inputs.size());
}

static void RecordNetplayReplayFrame(Core::System& system, s32 frame)
{
  if (!g_manager.netplay_replay_recording.load(std::memory_order_relaxed))
    return;

  if (!g_manager.netplay_replay_initial_digest_valid)
  {
    RamDigest& initial = g_manager.netplay_replay_initial_digest;
    initial = {};
    initial.frame = frame;
    FillRamRegionHashes(system, &initial);
    auto& core_timing = system.GetCoreTiming();
    initial.core_ticks = core_timing.GetTicks();
    initial.timebase = system.GetPowerPC().ReadFullTimeBaseValue();
    initial.fake_tb_start_value = core_timing.GetFakeTBStartValue();
    initial.fake_tb_start_ticks = core_timing.GetFakeTBStartTicks();
    initial.decrementer = system.GetSystemTimers().GetFakeDecrementer();
    g_manager.netplay_replay_initial_mem1_page_hashes = HashMem1Pages(system);
    g_manager.netplay_replay_sysconf = ReadReplaySysconf(system);
    g_manager.netplay_replay_initial_digest_valid = true;
    NOTICE_LOG_FMT(CORE,
                   "Netplay Replay base recorded: frame={} root={:016x} MEM1={:016x} "
                   "MEM2={:016x} L1={:016x} ticks={} TB={:#x} RTC={} SYSCONF={}",
                   frame, initial.root, initial.mem1, initial.mem2, initial.l1, initial.core_ticks,
                   initial.timebase, g_manager.netplay_replay_initial_rtc,
                   g_manager.netplay_replay_sysconf.size());
  }

  std::vector<u8> inputs(static_cast<std::size_t>(g_manager.netplay_replay_players) *
                         sizeof(WirePad));
  for (int port = 0; port < g_manager.netplay_replay_players; ++port)
  {
    const WirePad wire = EncodePad(g_manager.latched_pads[port]);
    std::memcpy(inputs.data() + static_cast<std::size_t>(port) * sizeof(WirePad), &wire,
                sizeof(wire));
  }

  // A corrected resimulation emits the same logical frame again. Assignment deliberately replaces
  // the predicted version, leaving the file with one final port-ordered input set per frame.
  g_manager.netplay_replay_frames[frame] = std::move(inputs);
}

static bool InputsEqual(const WireInput& lhs, const WireInput& rhs)
{
  return std::memcmp(&lhs, &rhs, sizeof(WireInput)) == 0;
}

static std::optional<u64> MeasureLocalInputDelay(int advance_frame,
                                                 const std::vector<unsigned char>& inputs)
{
  if (g_manager.local_handle < 0)
    return std::nullopt;

  std::optional<u64> applied_sample_time_ns;

  const std::size_t input_offset =
      static_cast<std::size_t>(g_manager.local_handle) * sizeof(WireInput);
  if (inputs.size() < input_offset + sizeof(WireInput))
  {
    ++g_manager.perf_delay_missing;
    return std::nullopt;
  }

  WireInput applied{};
  std::memcpy(&applied, inputs.data() + input_offset, sizeof(WireInput));

  const s64 expected_sample_frame =
      static_cast<s64>(advance_frame) - g_manager.configured_local_delay;
  if (expected_sample_frame >= 0)
  {
    const auto expected =
        std::find_if(g_manager.sampled_local_inputs.begin(), g_manager.sampled_local_inputs.end(),
                     [expected_sample_frame](const SampledLocalInput& sample) {
                       return sample.frame == expected_sample_frame;
                     });
    if (expected == g_manager.sampled_local_inputs.end())
    {
      ++g_manager.perf_delay_missing;
    }
    else
    {
      applied_sample_time_ns = expected->sample_time_ns;
      ++g_manager.perf_delay_checks;
      if (InputsEqual(applied, expected->input))
        ++g_manager.perf_delay_matches;
      else
        ++g_manager.perf_delay_mismatches;
    }
  }

  // An applied transition can be matched to the newest identical sampled transition. This is an
  // independent observation of the effective delay and will report zero if the host bypasses the
  // configured Gekko delay. It is intentionally transition-based because an unchanged held input
  // cannot reveal how many frames it was delayed.
  if (g_manager.last_applied_local_wire &&
      !InputsEqual(applied, *g_manager.last_applied_local_wire))
  {
    for (std::size_t i = g_manager.sampled_local_inputs.size(); i-- > 1;)
    {
      const SampledLocalInput& sample = g_manager.sampled_local_inputs[i];
      const SampledLocalInput& previous = g_manager.sampled_local_inputs[i - 1];
      if (sample.frame > advance_frame || !InputsEqual(sample.input, applied) ||
          InputsEqual(sample.input, previous.input))
      {
        continue;
      }

      const u64 observed_delay = static_cast<u64>(advance_frame - sample.frame);
      ++g_manager.perf_delay_transitions;
      g_manager.perf_delay_transition_frames += observed_delay;
      if (g_manager.perf_delay_transitions == 1)
        g_manager.perf_delay_transition_min = observed_delay;
      else
        g_manager.perf_delay_transition_min =
            std::min(g_manager.perf_delay_transition_min, observed_delay);
      g_manager.perf_delay_transition_max =
          std::max(g_manager.perf_delay_transition_max, observed_delay);
      break;
    }
  }
  g_manager.last_applied_local_wire = applied;

  constexpr s64 SAMPLE_HISTORY_FRAMES = 64;
  const s64 oldest_needed = static_cast<s64>(advance_frame) - SAMPLE_HISTORY_FRAMES;
  while (g_manager.sampled_local_inputs.size() > 2 &&
         g_manager.sampled_local_inputs[1].frame < oldest_needed)
  {
    g_manager.sampled_local_inputs.pop_front();
  }
  return applied_sample_time_ns;
}

static u64 SteadyNowNs()
{
  return static_cast<u64>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
          .count());
}

static void RecordGekkoInputGuestResume()
{
  if (!IsDetailedResimProfilingEnabled())
    return;

  std::lock_guard lk(g_manager.input_latency_mutex);
  if (!g_manager.pending_input_latency_frames.empty())
  {
    InputLatencyFrame& frame = g_manager.pending_input_latency_frames.back();
    if (frame.guest_resume_time_ns == 0)
      frame.guest_resume_time_ns = SteadyNowNs();
  }
}

void RecordGekkoInputPresentation(u64 present_time_ns)
{
  if (!IsDetailedResimProfilingEnabled() || !g_manager.active.load(std::memory_order_relaxed))
  {
    return;
  }

  std::lock_guard lk(g_manager.input_latency_mutex);
  if (g_manager.pending_input_latency_frames.empty())
  {
    ++g_manager.perf_input_latency_unmatched_presents;
    return;
  }

  // Presentation is not guaranteed to run once for every emulated frame. Startup, frame skips,
  // and rollback catch-up can all advance several logical frames without presenting them. Pair
  // this presentation with the newest frame that actually returned to guest execution, and
  // discard older superseded samples instead of allowing a permanent FIFO offset to accumulate.
  const auto newest_ready =
      std::find_if(g_manager.pending_input_latency_frames.rbegin(),
                   g_manager.pending_input_latency_frames.rend(),
                   [](const InputLatencyFrame& frame) { return frame.guest_resume_time_ns != 0; });
  if (newest_ready == g_manager.pending_input_latency_frames.rend())
  {
    ++g_manager.perf_input_latency_unmatched_presents;
    return;
  }

  const auto ready = std::prev(newest_ready.base());
  const InputLatencyFrame frame = *ready;
  g_manager.perf_input_latency_dropped +=
      static_cast<u64>(std::distance(g_manager.pending_input_latency_frames.begin(), ready));
  g_manager.pending_input_latency_frames.erase(g_manager.pending_input_latency_frames.begin(),
                                               std::next(ready));
  if (frame.guest_resume_time_ns == 0 || present_time_ns < frame.guest_resume_time_ns ||
      frame.guest_resume_time_ns < frame.si_relatched_time_ns ||
      frame.si_relatched_time_ns < frame.selected_time_ns ||
      frame.selected_time_ns < frame.sample_time_ns)
  {
    ++g_manager.perf_input_latency_dropped;
    return;
  }

  constexpr double NS_TO_MS = 1.0 / 1'000'000.0;
  const std::size_t index = g_manager.perf_input_latency_count++;
  g_manager.perf_input_poll_to_select_ms[index] =
      static_cast<double>(frame.selected_time_ns - frame.sample_time_ns) * NS_TO_MS;
  g_manager.perf_input_select_to_si_ms[index] =
      static_cast<double>(frame.si_relatched_time_ns - frame.selected_time_ns) * NS_TO_MS;
  g_manager.perf_input_si_to_resume_ms[index] =
      static_cast<double>(frame.guest_resume_time_ns - frame.si_relatched_time_ns) * NS_TO_MS;
  g_manager.perf_input_resume_to_present_ms[index] =
      static_cast<double>(present_time_ns - frame.guest_resume_time_ns) * NS_TO_MS;
  g_manager.perf_input_poll_to_present_ms[index] =
      static_cast<double>(present_time_ns - frame.sample_time_ns) * NS_TO_MS;

  if (g_manager.perf_input_latency_count != g_manager.perf_input_poll_to_select_ms.size())
    return;

  const auto summarize = [](const std::array<double, 300>& values) {
    std::array<double, 300> sorted = values;
    std::ranges::sort(sorted);
    return std::array<double, 3>{sorted[149], sorted[284], sorted.back()};
  };
  const auto poll_select = summarize(g_manager.perf_input_poll_to_select_ms);
  const auto select_si = summarize(g_manager.perf_input_select_to_si_ms);
  const auto si_resume = summarize(g_manager.perf_input_si_to_resume_ms);
  const auto resume_present = summarize(g_manager.perf_input_resume_to_present_ms);
  const auto poll_present = summarize(g_manager.perf_input_poll_to_present_ms);
  NOTICE_LOG_FMT(CORE,
                 "GekkoNet input age: p50/p95/max ms poll->select {:.3f}/{:.3f}/{:.3f}, "
                 "select->SI {:.3f}/{:.3f}/{:.3f}, SI->resume {:.3f}/{:.3f}/{:.3f}, "
                 "resume->present {:.3f}/{:.3f}/{:.3f}, poll->present {:.3f}/{:.3f}/{:.3f}; "
                 "dropped={}, unmatched_presents={}",
                 poll_select[0], poll_select[1], poll_select[2], select_si[0], select_si[1],
                 select_si[2], si_resume[0], si_resume[1], si_resume[2], resume_present[0],
                 resume_present[1], resume_present[2], poll_present[0], poll_present[1],
                 poll_present[2], g_manager.perf_input_latency_dropped,
                 g_manager.perf_input_latency_unmatched_presents);
  g_manager.perf_input_latency_count = 0;
  g_manager.perf_input_latency_dropped = 0;
  g_manager.perf_input_latency_unmatched_presents = 0;
}

static void QueueEvents(GekkoGameEvent** events, int event_count)
{
  for (int i = 0; i < event_count; ++i)
  {
    const GekkoGameEvent* event = events[i];
    if (!event)
      continue;

    QueuedEvent queued;
    queued.type = event->type;
    switch (event->type)
    {
    case GekkoSaveEvent:
      queued.frame = event->data.save.frame;
      queued.checksum = event->data.save.checksum;
      queued.state_len = event->data.save.state_len;
      break;
    case GekkoLoadEvent:
      queued.frame = event->data.load.frame;
      break;
    case GekkoAdvanceEvent:
      queued.frame = event->data.adv.frame;
      queued.rolling_back = event->data.adv.rolling_back;
      queued.running_ahead = event->data.adv.running_ahead;
      if (event->data.adv.inputs && event->data.adv.input_len != 0)
      {
        queued.inputs.assign(event->data.adv.inputs,
                             event->data.adv.inputs + event->data.adv.input_len);
      }
      break;
    default:
      break;
    }
    g_manager.pending_events.emplace_back(std::move(queued));
  }
}

static double ElapsedMs(std::chrono::steady_clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
      .count();
}

static void AddSnapshotPhases(SnapshotPhaseTimings* total, const SnapshotPhaseTimings& sample)
{
  total->execution_barrier_ms += sample.execution_barrier_ms;
  total->state_ms += sample.state_ms;
  total->dirty_pages_ms += sample.dirty_pages_ms;
  total->dirty_bitmap_scan_ms += sample.dirty_bitmap_scan_ms;
  total->dirty_compare_ms += sample.dirty_compare_ms;
  total->dirty_undo_log_ms += sample.dirty_undo_log_ms;
  total->dirty_mirror_copy_ms += sample.dirty_mirror_copy_ms;
  total->l1_ms += sample.l1_ms;
  total->jit_ms += sample.jit_ms;
  total->journal_ms += sample.journal_ms;
  total->state_bytes += sample.state_bytes;
  total->changed_blocks += sample.changed_blocks;
  total->changed_cache_lines += sample.changed_cache_lines;
  total->jit_blocks_invalidated += sample.jit_blocks_invalidated;
  total->dirty_marked_pages += sample.dirty_marked_pages;
  total->dirty_changed_pages += sample.dirty_changed_pages;
}

// Native GekkoNet uses frame -1 for the snapshot immediately before frame 0. SnapshotRing uses
// -1 as its empty-slot sentinel, so offset every Gekko frame by one without changing what Orca
// saves or restores.
static s64 SnapshotFrameKey(int gekko_frame)
{
  return static_cast<s64>(gekko_frame) + 1;
}

static void RecordPumpTime(std::chrono::steady_clock::time_point start)
{
  const double elapsed = ElapsedMs(start);
  ++g_manager.perf_pump_count;
  g_manager.perf_pump_ms += elapsed;
  g_manager.perf_pump_max_ms = std::max(g_manager.perf_pump_max_ms, elapsed);
}

static void MaybeLogPerformance(Core::System& system)
{
  constexpr u64 REPORT_FRAMES = 300;
  if (g_manager.perf_real_frames < REPORT_FRAMES)
    return;

  const auto now = std::chrono::steady_clock::now();
  const double wall_ms =
      std::chrono::duration<double, std::milli>(now - g_manager.perf_window_start).count();
  const double fps = wall_ms > 0.0 ? g_manager.perf_real_frames * 1000.0 / wall_ms : 0.0;
  const auto average = [](double total, u64 count) { return count ? total / count : 0.0; };
  const DirtyPages::Counters dirty_pages = DirtyPages::GetCounters();
  const Common::ENet::RollbackDatagramStats udp = Common::ENet::GetRollbackDatagramStats();
  const ResimJitMemoryStats jit_memory = TakeResimJitMemoryStats();
  const std::vector<ResimGuestHotspot> guest_hotspots = TakeResimGuestHotspots(8);
  const CoreTiming::RollbackThrottleStats throttle =
      system.GetCoreTiming().TakeRollbackThrottleStats();

  DEBUG_LOG_FMT(
      CORE,
      "GekkoNet perf: {:.1f} fps over {} real frames; save {:.3f}/{:.3f} ms avg/max ({}); "
      "pump {:.3f}/{:.3f} ms ({}); load {:.3f}/{:.3f} ms ({}); replays {}; "
      "replay_exec {:.3f}/{:.3f} ms ({}); replay_jit {:.3f} ms/frame, {:.2f} blocks/frame; "
      "replay_subsystems cpu_jit {:.3f} unaccounted {:.3f} boundary_setup {:.3f} "
      "scheduler {:.3f} fifo {:.3f} vi {:.3f} dsp_audio {:.3f} ios_ipc {:.3f} devices {:.3f} "
      "events_other {:.3f} ms/frame; "
      "replay_save {:.3f}/{:.3f} ms ({}); "
      "burst {:.3f}/{:.3f} ms ({}); burst_parts load {:.3f}/{:.3f} exec {:.3f}/{:.3f} "
      "save {:.3f}/{:.3f} other {:.3f}/{:.3f} ms avg/max; "
      "real_interval {:.3f}/{:.3f} ms ({}); input_changes {} bytes [{},{},{},{},{},{},{},{}]; "
      "delay_measure cfg={} verified={}/{} mismatch={} missing={} observed "
      "{:.2f}/{}/{} frames avg/min/max ({} transitions); "
      "rollback_depth {:.2f}/{} avg/max; RAM dirty/saved/unchanged pages {}/{}/{}; "
      "udp tx/rx/reject {}/{}/{}; ahead={:.2f}; stress_enabled={}; "
      "stress_input_period_frames={}; stress_rollback_frames={}; "
      "tracked_bitmap_clear={}; full_scan_benchmark={}",
      fps, g_manager.perf_real_frames, average(g_manager.perf_save_ms, g_manager.perf_save_count),
      g_manager.perf_save_max_ms, g_manager.perf_save_count,
      average(g_manager.perf_pump_ms, g_manager.perf_pump_count), g_manager.perf_pump_max_ms,
      g_manager.perf_pump_count, average(g_manager.perf_load_ms, g_manager.perf_load_count),
      g_manager.perf_load_max_ms, g_manager.perf_load_count, g_manager.perf_replay_frames,
      average(g_manager.perf_replay_exec_ms, g_manager.perf_replay_exec_count),
      g_manager.perf_replay_exec_max_ms, g_manager.perf_replay_exec_count,
      average(static_cast<double>(g_manager.perf_replay_jit_nanoseconds) / 1'000'000.0,
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_jit_blocks),
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.nanoseconds[static_cast<size_t>(
                  ResimEventCategory::GuestCpu)]) /
                  1'000'000.0,
              g_manager.perf_replay_exec_count),
      average(std::max(0.0, g_manager.perf_replay_exec_ms -
                                static_cast<double>(std::accumulate(
                                    g_manager.perf_replay_events.nanoseconds.begin(),
                                    g_manager.perf_replay_events.nanoseconds.end(), u64{0})) /
                                    1'000'000.0),
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.nanoseconds[static_cast<size_t>(
                  ResimEventCategory::FrameSetup)]) /
                  1'000'000.0,
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.nanoseconds[static_cast<size_t>(
                  ResimEventCategory::CoreTiming)]) /
                  1'000'000.0,
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events
                                      .nanoseconds[static_cast<size_t>(ResimEventCategory::Fifo)]) /
                  1'000'000.0,
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.nanoseconds[static_cast<size_t>(
                  ResimEventCategory::VideoInterface)]) /
                  1'000'000.0,
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.nanoseconds[static_cast<size_t>(
                  ResimEventCategory::DspAudio)]) /
                  1'000'000.0,
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.nanoseconds[static_cast<size_t>(
                  ResimEventCategory::IosIpc)]) /
                  1'000'000.0,
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.nanoseconds[static_cast<size_t>(
                  ResimEventCategory::Devices)]) /
                  1'000'000.0,
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.nanoseconds[static_cast<size_t>(
                  ResimEventCategory::Other)]) /
                  1'000'000.0,
              g_manager.perf_replay_exec_count),
      average(g_manager.perf_replay_save_ms, g_manager.perf_replay_save_count),
      g_manager.perf_replay_save_max_ms, g_manager.perf_replay_save_count,
      average(g_manager.perf_rollback_burst_ms, g_manager.perf_rollback_burst_count),
      g_manager.perf_rollback_burst_max_ms, g_manager.perf_rollback_burst_count,
      average(g_manager.perf_burst_load_ms, g_manager.perf_rollback_burst_count),
      g_manager.perf_burst_load_max_ms,
      average(g_manager.perf_burst_exec_ms, g_manager.perf_rollback_burst_count),
      g_manager.perf_burst_exec_max_ms,
      average(g_manager.perf_burst_save_ms, g_manager.perf_rollback_burst_count),
      g_manager.perf_burst_save_max_ms,
      average(g_manager.perf_burst_other_ms, g_manager.perf_rollback_burst_count),
      g_manager.perf_burst_other_max_ms,
      average(g_manager.perf_real_interval_ms, g_manager.perf_real_interval_count),
      g_manager.perf_real_interval_max_ms, g_manager.perf_real_interval_count,
      g_manager.perf_local_input_changes, g_manager.perf_local_byte_changes[0],
      g_manager.perf_local_byte_changes[1], g_manager.perf_local_byte_changes[2],
      g_manager.perf_local_byte_changes[3], g_manager.perf_local_byte_changes[4],
      g_manager.perf_local_byte_changes[5], g_manager.perf_local_byte_changes[6],
      g_manager.perf_local_byte_changes[7], g_manager.configured_local_delay,
      g_manager.perf_delay_matches, g_manager.perf_delay_checks, g_manager.perf_delay_mismatches,
      g_manager.perf_delay_missing,
      average(static_cast<double>(g_manager.perf_delay_transition_frames),
              g_manager.perf_delay_transitions),
      g_manager.perf_delay_transition_min, g_manager.perf_delay_transition_max,
      g_manager.perf_delay_transitions,
      average(static_cast<double>(g_manager.perf_rollback_depth_total),
              g_manager.perf_rollback_burst_count),
      g_manager.perf_rollback_depth_max,
      dirty_pages.dirty_pages - g_manager.perf_dirty_pages_start.dirty_pages,
      dirty_pages.pages_recorded - g_manager.perf_dirty_pages_start.pages_recorded,
      (dirty_pages.dirty_pages - g_manager.perf_dirty_pages_start.dirty_pages) -
          (dirty_pages.pages_recorded - g_manager.perf_dirty_pages_start.pages_recorded),
      udp.sent, udp.received, udp.rejected, gekko_frames_ahead(g_manager.session),
      g_manager.stress_test,
      g_manager.native_stress ? g_manager.stress_interval : STRESS_INPUT_PERIOD_FRAMES,
      g_manager.native_stress ? g_manager.stress_rollback_frames :
      g_manager.stress_test   ? s_stress_packet_delay_frames -
                                  static_cast<u64>(g_manager.configured_local_delay) - 1 :
                              0,
      g_manager.tracked_bitmap_clear, g_manager.full_scan_benchmark);

  NOTICE_LOG_FMT(
      CORE,
      "GekkoNet perf: {:.1f} fps/{} frames; save {:.3f}/{:.3f} ms ({}); pump "
      "{:.3f}/{:.3f} ms; load {:.3f}/{:.3f} ms ({}); replay {} frames, exec "
      "{:.3f}/{:.3f} ms, save {:.3f}/{:.3f} ms; bursts {} at {:.3f}/{:.3f} ms, depth "
      "{:.2f}/{}; interval {:.3f}/{:.3f} ms; gaps >20/>25/>33/>50 {}/{}/{}/{}; "
      "ahead {:.2f}",
      fps, g_manager.perf_real_frames, average(g_manager.perf_save_ms, g_manager.perf_save_count),
      g_manager.perf_save_max_ms, g_manager.perf_save_count,
      average(g_manager.perf_pump_ms, g_manager.perf_pump_count), g_manager.perf_pump_max_ms,
      average(g_manager.perf_load_ms, g_manager.perf_load_count), g_manager.perf_load_max_ms,
      g_manager.perf_load_count, g_manager.perf_replay_frames,
      average(g_manager.perf_replay_exec_ms, g_manager.perf_replay_exec_count),
      g_manager.perf_replay_exec_max_ms,
      average(g_manager.perf_replay_save_ms, g_manager.perf_replay_save_count),
      g_manager.perf_replay_save_max_ms, g_manager.perf_rollback_burst_count,
      average(g_manager.perf_rollback_burst_ms, g_manager.perf_rollback_burst_count),
      g_manager.perf_rollback_burst_max_ms,
      average(static_cast<double>(g_manager.perf_rollback_depth_total),
              g_manager.perf_rollback_burst_count),
      g_manager.perf_rollback_depth_max,
      average(g_manager.perf_real_interval_ms, g_manager.perf_real_interval_count),
      g_manager.perf_real_interval_max_ms, g_manager.perf_real_gaps_over_20_ms,
      g_manager.perf_real_gaps_over_25_ms, g_manager.perf_real_gaps_over_33_ms,
      g_manager.perf_real_gaps_over_50_ms, gekko_frames_ahead(g_manager.session));

  NOTICE_LOG_FMT(
      CORE,
      "GekkoNet resim transitions: enter SyncGPU {:.3f}/{:.3f} ms ({}); exit SyncGPU "
      "{:.3f}/{:.3f} ms ({})",
      average(g_manager.perf_resim_enter_sync_ms, g_manager.perf_resim_enter_sync_count),
      g_manager.perf_resim_enter_sync_max_ms, g_manager.perf_resim_enter_sync_count,
      average(g_manager.perf_resim_exit_sync_ms, g_manager.perf_resim_exit_sync_count),
      g_manager.perf_resim_exit_sync_max_ms, g_manager.perf_resim_exit_sync_count);

  if (IsDetailedResimProfilingEnabled())
  {
    const auto event_ms = [&](ResimEventCategory category) {
      return average(static_cast<double>(g_manager.perf_replay_events.nanoseconds[static_cast<
                         std::size_t>(category)]) /
                         1'000'000.0,
                     g_manager.perf_replay_exec_count);
    };
    const u64 categorized_event_ns = std::accumulate(
        g_manager.perf_replay_events.nanoseconds.begin(),
        g_manager.perf_replay_events.nanoseconds.end(), u64{0});
    const double unaccounted_ms =
        average(std::max(0.0, g_manager.perf_replay_exec_ms -
                                  static_cast<double>(categorized_event_ns) / 1'000'000.0),
                g_manager.perf_replay_exec_count);
    NOTICE_LOG_FMT(
        CORE,
        "GekkoNet replay subsystem times: cpu_jit {:.3f}, fifo {:.3f}, vi {:.3f}, "
        "scheduler {:.3f}, boundary_setup {:.3f}, dsp_audio {:.3f}, ios_ipc {:.3f}, "
        "devices {:.3f}, events_other {:.3f}, unaccounted {:.3f} ms/frame",
        event_ms(ResimEventCategory::GuestCpu), event_ms(ResimEventCategory::Fifo),
        event_ms(ResimEventCategory::VideoInterface), event_ms(ResimEventCategory::CoreTiming),
        event_ms(ResimEventCategory::FrameSetup), event_ms(ResimEventCategory::DspAudio),
        event_ms(ResimEventCategory::IosIpc), event_ms(ResimEventCategory::Devices),
        event_ms(ResimEventCategory::Other), unaccounted_ms);

    const auto region_calls = [&](bool write, ResimJitMemoryRegion region) {
      const auto& calls = write ? jit_memory.write_region_calls : jit_memory.read_region_calls;
      return average(static_cast<double>(calls[static_cast<std::size_t>(region)]),
                     g_manager.perf_replay_exec_count);
    };
    const auto region_ms = [&](bool write, ResimJitMemoryRegion region) {
      const auto& nanoseconds =
          write ? jit_memory.write_region_nanoseconds : jit_memory.read_region_nanoseconds;
      return average(static_cast<double>(nanoseconds[static_cast<std::size_t>(region)]) /
                         1'000'000.0,
                     g_manager.perf_replay_exec_count);
    };
    NOTICE_LOG_FMT(
        CORE,
        "GekkoNet replay slow-memory regions: writes calls/ms RAM {:.1f}/{:.3f}, gather "
        "{:.1f}/{:.3f}, MMIO {:.1f}/{:.3f}, other {:.1f}/{:.3f}; reads calls/ms RAM "
        "{:.1f}/{:.3f}, gather {:.1f}/{:.3f}, MMIO {:.1f}/{:.3f}, other {:.1f}/{:.3f} per frame",
        region_calls(true, ResimJitMemoryRegion::Ram),
        region_ms(true, ResimJitMemoryRegion::Ram),
        region_calls(true, ResimJitMemoryRegion::GatherPipe),
        region_ms(true, ResimJitMemoryRegion::GatherPipe),
        region_calls(true, ResimJitMemoryRegion::Mmio),
        region_ms(true, ResimJitMemoryRegion::Mmio),
        region_calls(true, ResimJitMemoryRegion::Other),
        region_ms(true, ResimJitMemoryRegion::Other),
        region_calls(false, ResimJitMemoryRegion::Ram),
        region_ms(false, ResimJitMemoryRegion::Ram),
        region_calls(false, ResimJitMemoryRegion::GatherPipe),
        region_ms(false, ResimJitMemoryRegion::GatherPipe),
        region_calls(false, ResimJitMemoryRegion::Mmio),
        region_ms(false, ResimJitMemoryRegion::Mmio),
        region_calls(false, ResimJitMemoryRegion::Other),
        region_ms(false, ResimJitMemoryRegion::Other));
  }

  NOTICE_LOG_FMT(CORE,
                 "GekkoNet presentation cadence: gaps >20/>25/>33.3/>50 ms = {}/{}/{}/{} of {} "
                 "real frames",
                 g_manager.perf_real_gaps_over_20_ms, g_manager.perf_real_gaps_over_25_ms,
                 g_manager.perf_real_gaps_over_33_ms, g_manager.perf_real_gaps_over_50_ms,
                 g_manager.perf_real_interval_count);

  NOTICE_LOG_FMT(
      CORE,
      "GekkoNet throttle: normal calls {}; requested {:.3f}/{:.3f} ms avg/max; actual "
      "{:.3f}/{:.3f} ms avg/max; first-after-burst calls {}; requested {:.3f}/{:.3f} ms "
      "avg/max; actual {:.3f}/{:.3f} ms avg/max",
      throttle.normal_calls,
      average(static_cast<double>(throttle.normal_requested_ns) / 1'000'000.0,
              throttle.normal_calls),
      static_cast<double>(throttle.normal_requested_max_ns) / 1'000'000.0,
      average(static_cast<double>(throttle.normal_actual_ns) / 1'000'000.0, throttle.normal_calls),
      static_cast<double>(throttle.normal_actual_max_ns) / 1'000'000.0, throttle.post_burst_calls,
      average(static_cast<double>(throttle.post_burst_requested_ns) / 1'000'000.0,
              throttle.post_burst_calls),
      static_cast<double>(throttle.post_burst_requested_max_ns) / 1'000'000.0,
      average(static_cast<double>(throttle.post_burst_actual_ns) / 1'000'000.0,
              throttle.post_burst_calls),
      static_cast<double>(throttle.post_burst_actual_max_ns) / 1'000'000.0);

  NOTICE_LOG_FMT(
      CORE,
      "GekkoNet replay callbacks: cpu_slices {:.2f}, boundary_setups {:.2f}, fifo {:.2f}, "
      "vi {:.2f}, dsp_audio {:.2f}, ios_ipc {:.2f}, devices {:.2f}, events_other {:.2f} "
      "callbacks/frame",
      average(static_cast<double>(g_manager.perf_replay_events.callbacks[static_cast<size_t>(
                  ResimEventCategory::GuestCpu)]),
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.callbacks[static_cast<size_t>(
                  ResimEventCategory::FrameSetup)]),
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events
                                      .callbacks[static_cast<size_t>(ResimEventCategory::Fifo)]),
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.callbacks[static_cast<size_t>(
                  ResimEventCategory::VideoInterface)]),
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events.callbacks[static_cast<size_t>(
                  ResimEventCategory::DspAudio)]),
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events
                                      .callbacks[static_cast<size_t>(ResimEventCategory::IosIpc)]),
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events
                                      .callbacks[static_cast<size_t>(ResimEventCategory::Devices)]),
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(g_manager.perf_replay_events
                                      .callbacks[static_cast<size_t>(ResimEventCategory::Other)]),
              g_manager.perf_replay_exec_count));

  NOTICE_LOG_FMT(
      CORE,
      "GekkoNet replay JIT memory helpers: reads {:.2f} calls/{:.3f} ms, writes {:.2f} "
      "calls/{:.3f} ms per replay frame",
      average(static_cast<double>(jit_memory.read_calls), g_manager.perf_replay_exec_count),
      average(static_cast<double>(jit_memory.read_nanoseconds) / 1'000'000.0,
              g_manager.perf_replay_exec_count),
      average(static_cast<double>(jit_memory.write_calls), g_manager.perf_replay_exec_count),
      average(static_cast<double>(jit_memory.write_nanoseconds) / 1'000'000.0,
              g_manager.perf_replay_exec_count));

  const u64 guest_total_nanoseconds =
      g_manager.perf_replay_events.nanoseconds[static_cast<size_t>(ResimEventCategory::GuestCpu)];
  for (const ResimGuestHotspot& hotspot : guest_hotspots)
  {
    const double percent = guest_total_nanoseconds == 0 ?
                               0.0 :
                               100.0 * static_cast<double>(hotspot.nanoseconds) /
                                   static_cast<double>(guest_total_nanoseconds);
    NOTICE_LOG_FMT(CORE,
                   "GekkoNet replay CPU hotspot: pc={:08x} '{}' {:.3f} ms/frame ({:.1f}% CPU/JIT), "
                   "{:.1f} slices/frame, {:.0f} guest cycles/frame",
                   hotspot.pc, system.GetPPCSymbolDB().GetDescription(hotspot.pc),
                   average(static_cast<double>(hotspot.nanoseconds) / 1'000'000.0,
                           g_manager.perf_replay_exec_count),
                   percent,
                   average(static_cast<double>(hotspot.slices), g_manager.perf_replay_exec_count),
                   average(static_cast<double>(hotspot.cycles), g_manager.perf_replay_exec_count));
  }

  const double save_known_ms =
      g_manager.perf_save_phases.execution_barrier_ms + g_manager.perf_save_phases.state_ms +
      g_manager.perf_save_phases.dirty_pages_ms + g_manager.perf_save_phases.l1_ms +
      g_manager.perf_save_phases.journal_ms;
  const double load_known_ms =
      g_manager.perf_load_phases.execution_barrier_ms + g_manager.perf_load_phases.dirty_pages_ms +
      g_manager.perf_load_phases.jit_ms + g_manager.perf_load_phases.l1_ms +
      g_manager.perf_load_phases.state_ms + g_manager.perf_load_phases.journal_ms;
  NOTICE_LOG_FMT(
      CORE,
      "GekkoNet snapshot phases: save barrier {:.3f} state {:.3f} dirty {:.3f} l1 {:.3f} "
      "journal {:.3f} other {:.3f} ms, state {:.1f} KiB; load barrier {:.3f} dirty {:.3f} "
      "jit {:.3f} l1 {:.3f} state {:.3f} journal {:.3f} other {:.3f} ms, changed {:.1f} "
      "pages/{:.1f} cache lines, invalidated {:.1f} JIT blocks",
      average(g_manager.perf_save_phases.execution_barrier_ms, g_manager.perf_save_count),
      average(g_manager.perf_save_phases.state_ms, g_manager.perf_save_count),
      average(g_manager.perf_save_phases.dirty_pages_ms, g_manager.perf_save_count),
      average(g_manager.perf_save_phases.l1_ms, g_manager.perf_save_count),
      average(g_manager.perf_save_phases.journal_ms, g_manager.perf_save_count),
      average(std::max(0.0, g_manager.perf_save_ms - save_known_ms), g_manager.perf_save_count),
      average(static_cast<double>(g_manager.perf_save_phases.state_bytes),
              g_manager.perf_save_count) /
          1024.0,
      average(g_manager.perf_load_phases.execution_barrier_ms, g_manager.perf_load_count),
      average(g_manager.perf_load_phases.dirty_pages_ms, g_manager.perf_load_count),
      average(g_manager.perf_load_phases.jit_ms, g_manager.perf_load_count),
      average(g_manager.perf_load_phases.l1_ms, g_manager.perf_load_count),
      average(g_manager.perf_load_phases.state_ms, g_manager.perf_load_count),
      average(g_manager.perf_load_phases.journal_ms, g_manager.perf_load_count),
      average(std::max(0.0, g_manager.perf_load_ms - load_known_ms), g_manager.perf_load_count),
      average(static_cast<double>(g_manager.perf_load_phases.changed_blocks),
              g_manager.perf_load_count),
      average(static_cast<double>(g_manager.perf_load_phases.changed_cache_lines),
              g_manager.perf_load_count),
      average(static_cast<double>(g_manager.perf_load_phases.jit_blocks_invalidated),
              g_manager.perf_load_count));

  if (g_manager.perf_save_phases.dirty_marked_pages != 0)
  {
    NOTICE_LOG_FMT(
        CORE,
        "GekkoNet dirty commit profile: bitmap/loop {:.3f} compare {:.3f} undo-log {:.3f} "
        "mirror-copy {:.3f} ms/save, marked {:.1f} changed {:.1f} pages/save",
        average(g_manager.perf_save_phases.dirty_bitmap_scan_ms, g_manager.perf_save_count),
        average(g_manager.perf_save_phases.dirty_compare_ms, g_manager.perf_save_count),
        average(g_manager.perf_save_phases.dirty_undo_log_ms, g_manager.perf_save_count),
        average(g_manager.perf_save_phases.dirty_mirror_copy_ms, g_manager.perf_save_count),
        average(static_cast<double>(g_manager.perf_save_phases.dirty_marked_pages),
                g_manager.perf_save_count),
        average(static_cast<double>(g_manager.perf_save_phases.dirty_changed_pages),
                g_manager.perf_save_count));
  }

  if (g_manager.compare_confirmed_ram)
  {
    NOTICE_LOG_FMT(CORE,
                   "Rollback restore diagnostic: verified {} snapshot loads; {} restore "
                   "mismatches in this window",
                   g_manager.perf_restore_checks, g_manager.perf_restore_mismatches);
  }

  g_manager.perf_window_start = now;
  g_manager.perf_dirty_pages_start = dirty_pages;
  g_manager.perf_real_frames = 0;
  g_manager.perf_replay_frames = 0;
  g_manager.perf_save_count = 0;
  g_manager.perf_load_count = 0;
  g_manager.perf_pump_count = 0;
  g_manager.perf_save_ms = 0.0;
  g_manager.perf_save_max_ms = 0.0;
  g_manager.perf_load_ms = 0.0;
  g_manager.perf_load_max_ms = 0.0;
  g_manager.perf_pump_ms = 0.0;
  g_manager.perf_pump_max_ms = 0.0;
  g_manager.perf_save_phases = {};
  g_manager.perf_load_phases = {};
  g_manager.perf_local_input_changes = 0;
  g_manager.perf_local_byte_changes.fill(0);
  g_manager.perf_delay_checks = 0;
  g_manager.perf_delay_matches = 0;
  g_manager.perf_delay_mismatches = 0;
  g_manager.perf_delay_missing = 0;
  g_manager.perf_delay_transitions = 0;
  g_manager.perf_delay_transition_frames = 0;
  g_manager.perf_delay_transition_min = 0;
  g_manager.perf_delay_transition_max = 0;
  g_manager.perf_replay_exec_count = 0;
  g_manager.perf_replay_exec_ms = 0.0;
  g_manager.perf_replay_exec_max_ms = 0.0;
  g_manager.perf_resim_enter_sync_count = 0;
  g_manager.perf_resim_enter_sync_ms = 0.0;
  g_manager.perf_resim_enter_sync_max_ms = 0.0;
  g_manager.perf_resim_exit_sync_count = 0;
  g_manager.perf_resim_exit_sync_ms = 0.0;
  g_manager.perf_resim_exit_sync_max_ms = 0.0;
  g_manager.perf_replay_jit_blocks = 0;
  g_manager.perf_replay_jit_nanoseconds = 0;
  g_manager.perf_replay_events = {};
  g_manager.perf_replay_save_count = 0;
  g_manager.perf_replay_save_ms = 0.0;
  g_manager.perf_replay_save_max_ms = 0.0;
  g_manager.perf_rollback_burst_count = 0;
  g_manager.perf_rollback_burst_ms = 0.0;
  g_manager.perf_rollback_burst_max_ms = 0.0;
  g_manager.perf_burst_load_ms = 0.0;
  g_manager.perf_burst_load_max_ms = 0.0;
  g_manager.perf_burst_exec_ms = 0.0;
  g_manager.perf_burst_exec_max_ms = 0.0;
  g_manager.perf_burst_save_ms = 0.0;
  g_manager.perf_burst_save_max_ms = 0.0;
  g_manager.perf_burst_other_ms = 0.0;
  g_manager.perf_burst_other_max_ms = 0.0;
  g_manager.perf_rollback_depth_total = 0;
  g_manager.perf_rollback_depth_max = 0;
  g_manager.perf_real_interval_count = 0;
  g_manager.perf_real_interval_ms = 0.0;
  g_manager.perf_real_interval_max_ms = 0.0;
  g_manager.perf_real_gaps_over_20_ms = 0;
  g_manager.perf_real_gaps_over_25_ms = 0;
  g_manager.perf_real_gaps_over_33_ms = 0;
  g_manager.perf_real_gaps_over_50_ms = 0;
  g_manager.perf_restore_checks = 0;
  g_manager.perf_restore_mismatches = 0;
}

static void FinishRollbackBurst(Core::System& system)
{
  const bool leaving_resimulation = Rollback::IsResimulating();
  const auto exit_sync_start = std::chrono::steady_clock::now();
  Rollback::SetResimulating(false);
  const auto exit_sync_end = std::chrono::steady_clock::now();
  if (leaving_resimulation)
  {
    const double exit_sync_ms =
        std::chrono::duration<double, std::milli>(exit_sync_end - exit_sync_start).count();
    ++g_manager.perf_resim_exit_sync_count;
    g_manager.perf_resim_exit_sync_ms += exit_sync_ms;
    g_manager.perf_resim_exit_sync_max_ms =
        std::max(g_manager.perf_resim_exit_sync_max_ms, exit_sync_ms);
  }
  if (g_manager.throttle_reference_before_load)
  {
    system.GetCoreTiming().SetThrottleReference(*g_manager.throttle_reference_before_load);
    g_manager.throttle_reference_before_load.reset();
  }

  if (!g_manager.rollback_burst_start)
    return;

  // Include the transition back to visible rendering in the complete burst cost. The transition
  // drains the dual-core FIFO and was previously absent from both burst and execution timings.
  const auto burst_duration = exit_sync_end - *g_manager.rollback_burst_start;
  const double elapsed = std::chrono::duration<double, std::milli>(burst_duration).count();
  ++g_manager.perf_rollback_burst_count;
  g_manager.perf_rollback_burst_ms += elapsed;
  g_manager.perf_rollback_burst_max_ms = std::max(g_manager.perf_rollback_burst_max_ms, elapsed);
  const double other_ms =
      std::max(0.0, elapsed - g_manager.current_burst_load_ms - g_manager.current_burst_exec_ms -
                        g_manager.current_burst_save_ms);
  const auto milliseconds_to_nanoseconds = [](double milliseconds) {
    return static_cast<u64>(std::max(0.0, milliseconds) * 1'000'000.0);
  };
  Rollback::RecordRollbackBurst(
      static_cast<u64>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(burst_duration).count()),
      milliseconds_to_nanoseconds(g_manager.current_burst_load_ms),
      milliseconds_to_nanoseconds(g_manager.current_burst_exec_ms),
      milliseconds_to_nanoseconds(g_manager.current_burst_save_ms),
      milliseconds_to_nanoseconds(other_ms), g_manager.current_rollback_replays);
  g_manager.perf_burst_load_ms += g_manager.current_burst_load_ms;
  g_manager.perf_burst_load_max_ms =
      std::max(g_manager.perf_burst_load_max_ms, g_manager.current_burst_load_ms);
  g_manager.perf_burst_exec_ms += g_manager.current_burst_exec_ms;
  g_manager.perf_burst_exec_max_ms =
      std::max(g_manager.perf_burst_exec_max_ms, g_manager.current_burst_exec_ms);
  g_manager.perf_burst_save_ms += g_manager.current_burst_save_ms;
  g_manager.perf_burst_save_max_ms =
      std::max(g_manager.perf_burst_save_max_ms, g_manager.current_burst_save_ms);
  g_manager.perf_burst_other_ms += other_ms;
  g_manager.perf_burst_other_max_ms = std::max(g_manager.perf_burst_other_max_ms, other_ms);
  g_manager.perf_rollback_depth_total += g_manager.current_rollback_replays;
  g_manager.perf_rollback_depth_max =
      std::max(g_manager.perf_rollback_depth_max, g_manager.current_rollback_replays);
  g_manager.current_rollback_replays = 0;
  g_manager.rollback_burst_start.reset();
}

// Consumes state operations up to one advance. Returning true means that the caller must return
// to the CPU so the selected frame can actually run. GekkoNet can emit a complete rollback as one
// batch (load, several replay advances/saves, then a real advance); Dolphin must spread that batch
// across real frame boundaries instead of merely relatching all of its inputs in one callback.
enum class PrepareFrameResult
{
  NoFrame,
  FrameReady,
  VisibleFrameDeferred,
};

static PrepareFrameResult PrepareQueuedFrame(Core::System& system,
                                             bool allow_visible_advance = true)
{
  while (!g_manager.pending_events.empty())
  {
    const QueuedEvent& next = g_manager.pending_events.front();
    if (!allow_visible_advance && next.type == GekkoAdvanceEvent && !next.rolling_back &&
        !next.running_ahead)
    {
      const bool follows_rollback = g_manager.rollback_burst_start.has_value();
      if (follows_rollback)
        FinishRollbackBurst(system);
      g_manager.deferred_visible_follows_rollback |= follows_rollback;
      return PrepareFrameResult::VisibleFrameDeferred;
    }

    QueuedEvent event = std::move(g_manager.pending_events.front());
    g_manager.pending_events.pop_front();
    RecordDiagnosticEvent(system, event);

    switch (event.type)
    {
    case GekkoSaveEvent:
      if (event.frame >= -1 && g_manager.ring)
      {
        const auto start = std::chrono::steady_clock::now();
        g_manager.ring->Save(system, SnapshotFrameKey(event.frame));
        LogStartFrameStateDigest(system, event.frame);
        RecordRamDigest(system, event.frame);
        LogSparseStateCheckpoint(event.frame);
        const double elapsed = ElapsedMs(start);
        AddSnapshotPhases(&g_manager.perf_save_phases, g_manager.ring->GetLastSaveTimings());
        ++g_manager.perf_save_count;
        g_manager.perf_save_ms += elapsed;
        g_manager.perf_save_max_ms = std::max(g_manager.perf_save_max_ms, elapsed);
        if (Rollback::IsResimulating() && g_manager.rollback_burst_start)
        {
          ++g_manager.perf_replay_save_count;
          g_manager.perf_replay_save_ms += elapsed;
          g_manager.perf_replay_save_max_ms = std::max(g_manager.perf_replay_save_max_ms, elapsed);
          g_manager.current_burst_save_ms += elapsed;
        }
        if (event.checksum)
          *event.checksum = 0;
        if (event.state_len)
          *event.state_len = 0;
      }
      break;

    case GekkoLoadEvent:
      if (event.frame >= -1 && g_manager.ring)
      {
        DEBUG_LOG_FMT(CORE, "GekkoNet: Loading rollback frame {}", event.frame);
        if (!Rollback::IsResimulating() && !g_manager.throttle_reference_before_load)
        {
          g_manager.throttle_reference_before_load = system.GetCoreTiming().GetThrottleReference();
        }
        if (!g_manager.rollback_burst_start)
        {
          g_manager.rollback_burst_start = std::chrono::steady_clock::now();
          g_manager.current_rollback_replays = 0;
          g_manager.current_burst_load_ms = 0.0;
          g_manager.current_burst_exec_ms = 0.0;
          g_manager.current_burst_save_ms = 0.0;
          g_manager.current_burst_jit_blocks_invalidated = 0;
        }
        const auto start = std::chrono::steady_clock::now();
        if (!g_manager.ring->Load(system, SnapshotFrameKey(event.frame)))
        {
          ERROR_LOG_FMT(CORE, "GekkoNet: Failed to restore rollback frame {}", event.frame);
          g_manager.stop_requested.store(true, std::memory_order_relaxed);
          return PrepareFrameResult::NoFrame;
        }
        VerifyRestoredRam(system, event.frame);
        if (g_manager.stress_test && event.frame < 10)
        {
          const auto& ppc = system.GetPPCState();
          NOTICE_LOG_FMT(CORE,
                         "GekkoNet trace: load event {} returned (pc={:08x}, npc={:08x}, "
                         "downcount={}, ticks={})",
                         event.frame, ppc.pc, ppc.npc, ppc.downcount,
                         system.GetCoreTiming().GetTicks());
        }
        const double elapsed = ElapsedMs(start);
        const SnapshotPhaseTimings& load_timings = g_manager.ring->GetLastLoadTimings();
        AddSnapshotPhases(&g_manager.perf_load_phases, load_timings);
        g_manager.current_burst_jit_blocks_invalidated += load_timings.jit_blocks_invalidated;
        ++g_manager.perf_load_count;
        g_manager.perf_load_ms += elapsed;
        g_manager.perf_load_max_ms = std::max(g_manager.perf_load_max_ms, elapsed);
        g_manager.current_burst_load_ms += elapsed;
      }
      break;

    case GekkoAdvanceEvent:
    {
      const bool resimulating = event.rolling_back || event.running_ahead;
      const u64 selected_time_ns =
          !resimulating && IsDetailedResimProfilingEnabled() ? SteadyNowNs() : 0;
      std::optional<u64> applied_sample_time_ns;
      std::string start_players;
      std::pair<u32, u32> si0_before{};
      std::pair<u32, u32> si1_before{};
      if (g_manager.compare_confirmed_ram)
      {
        g_manager.last_advance_input_hash = XXH3_64bits(event.inputs.data(), event.inputs.size());
        for (int p = 0; p < g_manager.players; ++p)
        {
          const int handle = g_manager.player_handles[p];
          if (handle < 0)
            continue;
          const std::size_t offset = static_cast<std::size_t>(handle) * sizeof(WireInput);
          if (event.inputs.size() < offset + sizeof(WireInput))
            continue;
          WireInput wire{};
          std::memcpy(&wire, event.inputs.data() + offset, sizeof(wire));
          if (wire.controller_type == static_cast<u8>(ControllerType::GameCube) &&
              WirePadHasStart(wire.pad))
            start_players += fmt::format("{}P{}", start_players.empty() ? "" : ",", p + 1);
        }
        si0_before = system.GetSerialInterface().GetInputRegisters(0);
        si1_before = system.GetSerialInterface().GetInputRegisters(1);
      }
      g_manager.last_advance_has_start = !start_players.empty();
      if (!resimulating)
        applied_sample_time_ns = MeasureLocalInputDelay(event.frame, event.inputs);
      VerifyConfirmedReplayInputs(event);
      LatchInputs(event.inputs.data());
      RecordNetplayReplayFrame(system, event.frame);
      const auto advance_now = std::chrono::steady_clock::now();
      if (resimulating)
      {
        const bool entering_resimulation = !Rollback::IsResimulating();
        const auto enter_sync_start = std::chrono::steady_clock::now();
        Rollback::SetResimulating(true);
        if (entering_resimulation)
        {
          const double enter_sync_ms = ElapsedMs(enter_sync_start);
          ++g_manager.perf_resim_enter_sync_count;
          g_manager.perf_resim_enter_sync_ms += enter_sync_ms;
          g_manager.perf_resim_enter_sync_max_ms =
              std::max(g_manager.perf_resim_enter_sync_max_ms, enter_sync_ms);
        }
      }
      else
      {
        const bool follows_rollback = g_manager.deferred_visible_follows_rollback ||
                                      g_manager.rollback_burst_start.has_value();
        FinishRollbackBurst(system);
        Rollback::RecordFramePacingBoundary(event.frame, follows_rollback);
        g_manager.deferred_visible_follows_rollback = false;
      }
      if (resimulating)
      {
        ++g_manager.perf_replay_frames;
        ++g_manager.current_rollback_replays;
      }
      else
      {
        if (g_manager.last_real_advance)
        {
          const double interval =
              std::chrono::duration<double, std::milli>(advance_now - *g_manager.last_real_advance)
                  .count();
          ++g_manager.perf_real_interval_count;
          g_manager.perf_real_interval_ms += interval;
          g_manager.perf_real_interval_max_ms =
              std::max(g_manager.perf_real_interval_max_ms, interval);
          g_manager.perf_real_gaps_over_20_ms += interval > 20.0;
          g_manager.perf_real_gaps_over_25_ms += interval > 25.0;
          g_manager.perf_real_gaps_over_33_ms += interval > (100.0 / 3.0);
          g_manager.perf_real_gaps_over_50_ms += interval > 50.0;
        }
        g_manager.last_real_advance = advance_now;
        if (g_manager.perf_real_frames == 0)
          g_manager.perf_window_start = std::chrono::steady_clock::now();
        ++g_manager.perf_real_frames;
      }
      g_manager.frame_execution_resim = resimulating;
      g_manager.frame_execution_gekko_frame = event.frame;
      if (IsDetailedResimProfilingEnabled())
        g_manager.frame_execution_event_start = GetResimEventStats();
      const auto frame_setup_start =
          resimulating ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      // Publish the inputs selected by GekkoNet before returning to guest execution. Waiting for
      // the next scheduled SI poll adds an implicit input stage for VI-based boundaries.
      system.GetSerialInterface().RelatchInputs();
      if (applied_sample_time_ns && IsDetailedResimProfilingEnabled())
      {
        std::lock_guard latency_lk(g_manager.input_latency_mutex);
        constexpr std::size_t MAX_PENDING_INPUT_LATENCY_FRAMES = 32;
        if (g_manager.pending_input_latency_frames.size() >= MAX_PENDING_INPUT_LATENCY_FRAMES)
        {
          g_manager.pending_input_latency_frames.pop_front();
          ++g_manager.perf_input_latency_dropped;
        }
        g_manager.pending_input_latency_frames.push_back(
            {event.frame, *applied_sample_time_ns, selected_time_ns, SteadyNowNs(), 0});
      }
      if (g_manager.compare_confirmed_ram)
      {
        NOTICE_LOG_FMT(CORE,
                       "Rollback input diagnostic: advance frame {} input={:016x} rollback={} "
                       "runahead={}",
                       event.frame, g_manager.last_advance_input_hash, event.rolling_back,
                       event.running_ahead);
      }
      if (g_manager.compare_confirmed_ram && !start_players.empty())
      {
        const auto si0_after = system.GetSerialInterface().GetInputRegisters(0);
        const auto si1_after = system.GetSerialInterface().GetInputRegisters(1);
        const auto& ppc = system.GetPPCState();
        NOTICE_LOG_FMT(
            CORE,
            "GekkoNet input trace: advance frame {} START [{}], rollback={}, runahead={}, "
            "hash={:016x}, ticks={}, timebase={}, vi_halfline={}, pc={:08x}, npc={:08x}, "
            "si0={:08x}/{:08x}->{:08x}/{:08x}, si1={:08x}/{:08x}->{:08x}/{:08x}",
            event.frame, start_players, event.rolling_back, event.running_ahead,
            g_manager.last_advance_input_hash, system.GetCoreTiming().GetTicks(),
            system.GetPowerPC().ReadFullTimeBaseValue(),
            system.GetVideoInterface().GetHalfLineCount(), ppc.pc, ppc.npc, si0_before.first,
            si0_before.second, si0_after.first, si0_after.second, si1_before.first,
            si1_before.second, si1_after.first, si1_after.second);
      }
      if (g_manager.stress_test && event.frame < 10)
      {
        NOTICE_LOG_FMT(CORE,
                       "GekkoNet trace: advance event {} returning to CPU (rollback={}, "
                       "runahead={})",
                       event.frame, event.rolling_back, event.running_ahead);
      }
      if (event.rolling_back || event.running_ahead)
      {
        DEBUG_LOG_FMT(CORE, "GekkoNet: Replaying frame {} (rollback={}, runahead={})", event.frame,
                      event.rolling_back, event.running_ahead);
      }
      if (resimulating && IsDetailedResimProfilingEnabled())
      {
        RecordResimEvent(ResimEventCategory::FrameSetup,
                         static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - frame_setup_start)
                                              .count()));
      }
      // Start execution timing after the resimulation transition and input/boundary setup. This
      // measures the guest frame itself instead of charging the first replay frame for SyncGPU.
      g_manager.frame_execution_start = std::chrono::steady_clock::now();
      if (IsDetailedResimProfilingEnabled())
      {
        g_manager.frame_execution_jit_start = GetResimJitCompileStats();
      }
      return PrepareFrameResult::FrameReady;
    }

    default:
      break;
    }
  }
  return PrepareFrameResult::NoFrame;
}

static bool StartPreparedFrame(Core::System& system)
{
  PrepareFrameResult result = PrepareQueuedFrame(system, !g_manager.visible_frame_pacing_pending);
  if (result == PrepareFrameResult::FrameReady)
    return true;

  if (result != PrepareFrameResult::VisibleFrameDeferred)
    return false;

  // All rollback advances and their saves have completed, and Gekko's next visible advance is
  // waiting at the front of the queue. Pace against the original emulated deadline now, so the
  // rollback burst consumes the idle budget instead of being added after a full frame wait.
  auto& core_timing = system.GetCoreTiming();
  core_timing.ThrottleForRollbackBoundary(static_cast<s64>(core_timing.GetTicks()));
  g_manager.visible_frame_pacing_pending = false;

  result = PrepareQueuedFrame(system);
  ASSERT(result == PrepareFrameResult::FrameReady);
  return result == PrepareFrameResult::FrameReady;
}

static void ProcessFrameBoundary(const Core::CPUThreadGuard& guard, FrameBoundary boundary)
{
  auto& system = guard.GetSystem();
  HostFloatScope float_scope(system.GetPPCState());

  if (!g_manager.active.load(std::memory_order_acquire) || g_manager.frame_boundary != boundary)
    return;

  if (boundary == FrameBoundary::BrawlHook)
  {
    // A launcher installs this hook before Brawl is in RAM. Until the expected instruction is live,
    // whatever happens to execute at this address is not Brawl's frame boundary.
    const u32 opcode = system.GetMemory().Read_U32(BRAWL_FRAME_HOOK_ADDR & 0x1FFFFFFF);
    if (opcode != BRAWL_EXPECTED_OPCODE)
      return;

    // Inside a JIT block PC and NPC are stale. Pin both to the boundary so snapshots resume at the
    // hook. This is a Start hook, so the original instruction executes normally after this returns.
    auto& ppc_state = system.GetPPCState();
    ppc_state.pc = BRAWL_FRAME_HOOK_ADDR;
    ppc_state.npc = BRAWL_FRAME_HOOK_ADDR;
  }

  // Presentation changes are local host operations. Run them only at a complete emulated-frame
  // boundary and before taking the rollback manager lock. The callback uses Dolphin's existing
  // CPU-thread GPU quiescence path, so FIFO cannot consume guest RAM while the backend changes.
  RunPendingPresentationChange();

  PatchEngine::ApplyNetplayMusicPatchAtRollbackBoundary(guard,
                                                        g_manager.disable_project_plus_music);

  std::lock_guard lk(g_manager.mutex);
  if (!g_manager.session)
    return;

  if (g_manager.frame_execution_start)
  {
    const double elapsed = ElapsedMs(*g_manager.frame_execution_start);
    if (g_manager.frame_execution_resim)
    {
      u64 jit_blocks = 0;
      u64 jit_nanoseconds = 0;
      ++g_manager.perf_replay_exec_count;
      g_manager.perf_replay_exec_ms += elapsed;
      g_manager.perf_replay_exec_max_ms = std::max(g_manager.perf_replay_exec_max_ms, elapsed);
      if (IsDetailedResimProfilingEnabled())
      {
        const ResimJitCompileStats jit_now = GetResimJitCompileStats();
        jit_blocks = jit_now.blocks - g_manager.frame_execution_jit_start.blocks;
        jit_nanoseconds = jit_now.nanoseconds - g_manager.frame_execution_jit_start.nanoseconds;
        const ResimEventStats event_now = GetResimEventStats();
        g_manager.perf_replay_jit_blocks += jit_blocks;
        g_manager.perf_replay_jit_nanoseconds += jit_nanoseconds;
        for (std::size_t i = 0; i < g_manager.perf_replay_events.nanoseconds.size(); ++i)
        {
          g_manager.perf_replay_events.nanoseconds[i] +=
              event_now.nanoseconds[i] - g_manager.frame_execution_event_start.nanoseconds[i];
          g_manager.perf_replay_events.callbacks[i] +=
              event_now.callbacks[i] - g_manager.frame_execution_event_start.callbacks[i];
        }
      }
      if (g_manager.rollback_burst_start)
        g_manager.current_burst_exec_ms += elapsed;
      if (elapsed >= 10.0)
      {
        NOTICE_LOG_FMT(CORE,
                       "GekkoNet replay spike: frame {} exec {:.3f} ms, JIT compiled {} blocks in "
                       "{:.3f} ms after invalidating {} blocks in this burst",
                       g_manager.frame_execution_gekko_frame, elapsed, jit_blocks,
                       static_cast<double>(jit_nanoseconds) / 1'000'000.0,
                       g_manager.current_burst_jit_blocks_invalidated);
      }
    }
    else
    {
      g_manager.visible_frame_pacing_pending = true;
    }
    g_manager.frame_execution_start.reset();
  }

  MaybeLogPerformance(system);

  // A rollback batch is replayed one emulated frame at a time. State saves left after the prior
  // advance are intentionally handled now, after that frame has completed.
  if (StartPreparedFrame(system))
  {
    return;
  }

  // Only publish confirmed hashes after the previous Gekko event batch has completely drained.
  // A network poll can confirm an input before HandleRollback has emitted and executed the
  // correction, so publishing immediately after polling would compare stale predicted RAM.
  CompareSettledRamDigests();
  SendConfirmedRamDigests();
  if (DumpFirstRamMismatch(system))
    return;
  if (DumpFirstVideoMismatch(system))
    return;

  // Submit before polling so this boundary's inputs are eligible to be sent immediately. The
  // simulated peer uses the same GekkoNet path, with packets released after the configured delay.
  if (!SubmitLocalInput())
    return;
  PumpSimulatedPeer();
  if (!g_manager.netplay_replay_session)
    gekko_network_poll(g_manager.session);

  // A peer mismatch notification can arrive during the poll above. Restore and dump immediately
  // at this safe boundary, then pause instead of waiting for another Gekko advance.
  if (DumpFirstRamMismatch(system))
    return;
  if (DumpFirstVideoMismatch(system))
    return;

  // Symmetric timesync pacing (RMG-K parameters). Tight deadzone + fast lerp keeps
  // framesAhead near zero so both players share prediction/rollback load.
  static constexpr double kSymDeadzone = 0.20;
  static constexpr double kSymStrength = 0.015;
  static constexpr double kSymMinScale = 0.97;
  static constexpr double kSymMaxScale = 1.03;
  static constexpr double kSymLerp = 0.35;

  const float frames_ahead = gekko_frames_ahead(g_manager.session);

  double newTarget = 1.0;
  if (frames_ahead >= static_cast<float>(kSymDeadzone) ||
      frames_ahead <= -static_cast<float>(kSymDeadzone))
  {
    newTarget = 1.0 - (static_cast<double>(frames_ahead) * kSymStrength);
    newTarget = std::clamp(newTarget, kSymMinScale, kSymMaxScale);
  }
  g_manager.target_scale = newTarget;

  g_manager.speed_scale += (g_manager.target_scale - g_manager.speed_scale) * kSymLerp;
  Core::System::GetInstance().GetCoreTiming().SetTimesyncScale(
      static_cast<float>(g_manager.speed_scale));

  static u32 s_boundary_ticks = 0;
  if (++s_boundary_ticks % 60 == 1 && g_manager.compare_confirmed_ram)
  {
    NOTICE_LOG_FMT(CORE,
                   "GekkoNet: Frame boundary tick #{}, boundary={}, local_player={}, ahead={:.2f}, "
                   "target={:.4f}, scale={:.4f}",
                   s_boundary_ticks, FrameBoundaryName(boundary), g_manager.local_player,
                   frames_ahead, g_manager.target_scale, g_manager.speed_scale);
  }

  // Run GekkoNet event pump - must wait for an advance event each frame.
  const auto pump_start = std::chrono::steady_clock::now();
  const bool diagnose_pump_stalls = IsDetailedResimProfilingEnabled();
  const Common::ENet::RollbackDatagramStats pump_udp_start =
      diagnose_pump_stalls ? Common::ENet::GetRollbackDatagramStats() :
                             Common::ENet::RollbackDatagramStats{};
  std::array<int, MAX_PORTS> received_at_pump_start{};
  received_at_pump_start.fill(std::numeric_limits<int>::min());
  if (diagnose_pump_stalls)
  {
    for (int p = 0; p < g_manager.players; ++p)
    {
      const int handle = g_manager.player_handles[p];
      if (handle >= 0)
        received_at_pump_start[p] = gekko_last_received_frame(g_manager.session, handle);
    }
  }

  u64 pump_retries = 0;
  double pump_update_ms = 0.0;
  double pump_prepare_ms = 0.0;
  double pump_sleep_ms = 0.0;
  double pump_network_poll_ms = 0.0;
  int last_event_count = 0;
  int last_batch_saves = 0;
  int last_batch_loads = 0;
  int last_batch_advances = 0;
  u64 total_saves = 0;
  u64 total_loads = 0;
  u64 total_advances = 0;
  constexpr std::array<double, 4> PUMP_STALL_THRESHOLDS_MS{16.0, 33.0, 50.0, 100.0};
  std::size_t next_pump_stall_threshold = 0;

  const auto log_pump_stall = [&](std::string_view phase, double elapsed_ms) {
    int pending_saves = 0;
    int pending_loads = 0;
    int pending_advances = 0;
    int latest_pending_advance = -1;
    for (const QueuedEvent& event : g_manager.pending_events)
    {
      switch (event.type)
      {
      case GekkoSaveEvent:
        ++pending_saves;
        break;
      case GekkoLoadEvent:
        ++pending_loads;
        break;
      case GekkoAdvanceEvent:
        ++pending_advances;
        latest_pending_advance = event.frame;
        break;
      default:
        break;
      }
    }

    const int current_frame = gekko_current_frame(g_manager.session);
    int min_remote_received = std::numeric_limits<int>::max();
    std::string player_frames;
    for (int p = 0; p < g_manager.players; ++p)
    {
      const int handle = g_manager.player_handles[p];
      if (handle < 0)
        continue;
      const int received = gekko_last_received_frame(g_manager.session, handle);
      const bool local = handle == g_manager.local_handle;
      player_frames +=
          fmt::format("{}P{}:{}{}({:+d})", player_frames.empty() ? "" : ",", p + 1,
                      local ? "L" : "R", received, received - received_at_pump_start[p]);
      if (!local)
        min_remote_received = std::min(min_remote_received, received);
    }

    const int predicted_remote_frames = min_remote_received == std::numeric_limits<int>::max() ?
                                            0 :
                                            std::max(0, current_frame - min_remote_received - 1);
    std::string_view reason = "input_unavailable";
    if (pending_advances != 0)
      reason = "advance_queued_but_deferred";
    else if (min_remote_received != std::numeric_limits<int>::max() &&
             predicted_remote_frames >= g_manager.configured_prediction_window)
      reason = "remote_prediction_window_exhausted";
    else if (last_event_count == 0)
      reason = "no_game_events";
    else
      reason = "events_without_advance";

    const Common::ENet::RollbackDatagramStats udp = Common::ENet::GetRollbackDatagramStats();
    GekkoNetworkStats network_stats{};
    if (g_manager.remote_handle >= 0)
      gekko_network_stats(g_manager.session, g_manager.remote_handle, &network_stats);

    NOTICE_LOG_FMT(
        CORE,
        "GekkoNet pump stall: phase={} elapsed={:.3f} ms retries={} reason={}; "
        "parts update={:.3f} prepare={:.3f} sleep={:.3f} network_poll={:.3f} ms; "
        "frame current={} last_started={} latest_pending={} local_submitted={} ahead={:.2f}; "
        "remote_prediction={}/{} frames; received=[{}]; "
        "events last S/L/A={}/{}/{} total={}/{}/{} pending={}/{}/{}; "
        "transport rx_delta={} reject_delta={} ping_last/avg/jitter={}/{:.1f}/{:.1f} ms",
        phase, elapsed_ms, pump_retries, reason, pump_update_ms, pump_prepare_ms, pump_sleep_ms,
        pump_network_poll_ms, current_frame, g_manager.frame_execution_gekko_frame,
        latest_pending_advance, g_manager.local_input_frame, gekko_frames_ahead(g_manager.session),
        predicted_remote_frames, g_manager.configured_prediction_window, player_frames,
        last_batch_saves, last_batch_loads, last_batch_advances, total_saves, total_loads,
        total_advances, pending_saves, pending_loads, pending_advances,
        udp.received - pump_udp_start.received, udp.rejected - pump_udp_start.rejected,
        network_stats.last_ping, network_stats.avg_ping, network_stats.jitter);
  };

  const auto maybe_log_waiting_pump = [&] {
    if (!diagnose_pump_stalls)
      return;
    const double elapsed_ms = ElapsedMs(pump_start);
    while (next_pump_stall_threshold < PUMP_STALL_THRESHOLDS_MS.size() &&
           elapsed_ms >= PUMP_STALL_THRESHOLDS_MS[next_pump_stall_threshold])
    {
      log_pump_stall("waiting", elapsed_ms);
      ++next_pump_stall_threshold;
    }
  };

  for (;;)
  {
    if (g_manager.stop_requested.load(std::memory_order_relaxed))
      return;

    int event_count = 0;
    const auto update_start = std::chrono::steady_clock::now();
    GekkoGameEvent** events = gekko_update_session(g_manager.session, &event_count);
    if (diagnose_pump_stalls)
      pump_update_ms += ElapsedMs(update_start);

    last_event_count = event_count;
    last_batch_saves = 0;
    last_batch_loads = 0;
    last_batch_advances = 0;
    for (int i = 0; i < event_count; ++i)
    {
      if (!events[i])
        continue;
      switch (events[i]->type)
      {
      case GekkoSaveEvent:
        ++last_batch_saves;
        ++total_saves;
        break;
      case GekkoLoadEvent:
        ++last_batch_loads;
        ++total_loads;
        break;
      case GekkoAdvanceEvent:
        ++last_batch_advances;
        ++total_advances;
        break;
      default:
        break;
      }
    }

    // Check session lifecycle events
    int s_count = 0;
    GekkoSessionEvent** s_events = gekko_session_events(g_manager.session, &s_count);
    for (int s = 0; s < s_count; ++s)
    {
      const auto* sev = s_events[s];
      if (!sev)
        continue;
      switch (sev->type)
      {
      case GekkoPlayerSyncing:
        NOTICE_LOG_FMT(CORE, "GekkoNet: Player handle {} syncing ({}/{})", sev->data.syncing.handle,
                       sev->data.syncing.current, sev->data.syncing.max);
        break;
      case GekkoPlayerConnected:
        NOTICE_LOG_FMT(CORE, "GekkoNet: Player handle {} connected!", sev->data.connected.handle);
        break;
      case GekkoSessionStarted:
        NOTICE_LOG_FMT(CORE, "GekkoNet: Session started! Rollback active.");
        if (g_manager.stress_test && !s_stress_frame_delay_active)
        {
          s_stress_frame_delay_active = true;
          s_last_stress_peer_update_frame = 0;
          NOTICE_LOG_FMT(CORE,
                         "GekkoNet benchmark: deterministic {}-frame packets active; Player 2 "
                         "changes input every {} frame(s)",
                         s_stress_packet_delay_frames, STRESS_INPUT_PERIOD_FRAMES);
        }
        break;
      default:
        break;
      }
    }

    QueueEvents(events, event_count);
    const auto prepare_start = std::chrono::steady_clock::now();
    const bool frame_started = StartPreparedFrame(system);
    if (diagnose_pump_stalls)
      pump_prepare_ms += ElapsedMs(prepare_start);
    if (frame_started)
    {
      const double elapsed_ms = ElapsedMs(pump_start);
      RecordPumpTime(pump_start);
      if (diagnose_pump_stalls && elapsed_ms >= PUMP_STALL_THRESHOLDS_MS.front())
        log_pump_stall("resolved", elapsed_ms);
      return;
    }

    // No advance yet - poll network and retry.
    // A sub-millisecond std::this_thread::sleep_for can consume a full scheduler quantum on
    // Windows. That turns a brief GekkoNet wait into a lost emulated frame. Dolphin's precision
    // timer spins for this short interval and preserves the intended 100 us polling cadence.
    ++pump_retries;
    const auto sleep_start = std::chrono::steady_clock::now();
    g_manager.wait_timer.SleepUntil(Clock::now() + std::chrono::microseconds(WAIT_SLEEP_US));
    if (diagnose_pump_stalls)
      pump_sleep_ms += ElapsedMs(sleep_start);
    PumpSimulatedPeer();
    if (!g_manager.netplay_replay_session)
    {
      const auto network_poll_start = std::chrono::steady_clock::now();
      gekko_network_poll(g_manager.session);
      if (diagnose_pump_stalls)
        pump_network_poll_ms += ElapsedMs(network_poll_start);
    }
    maybe_log_waiting_pump();
  }
}

static bool SubmitNetplayReplayInputs()
{
  if (!g_manager.netplay_replay_session ||
      !g_manager.netplay_replay_playing.load(std::memory_order_relaxed))
    return false;

  if (g_manager.netplay_replay_playback_index >= g_manager.netplay_replay_playback_frames.size())
  {
    g_manager.netplay_replay_playing.store(false, std::memory_order_release);
    g_manager.stop_requested.store(true, std::memory_order_relaxed);
    auto& system = Core::System::GetInstance();
    system.GetCPU().Break();
    Core::DisplayMessage("Netplay Replay ended", 3000);
    return false;
  }

  auto& system = Core::System::GetInstance();
  const bool first_playback_frame = g_manager.netplay_replay_playback_index == 0;
  const auto& [frame, inputs] =
      g_manager.netplay_replay_playback_frames[g_manager.netplay_replay_playback_index];
  if (first_playback_frame && g_manager.netplay_replay_initial_digest_valid)
  {
    RamDigest actual{};
    actual.frame = frame;
    FillRamRegionHashes(system, &actual);
    auto& core_timing = system.GetCoreTiming();
    actual.core_ticks = core_timing.GetTicks();
    actual.timebase = system.GetPowerPC().ReadFullTimeBaseValue();
    actual.fake_tb_start_value = core_timing.GetFakeTBStartValue();
    actual.fake_tb_start_ticks = core_timing.GetFakeTBStartTicks();
    actual.decrementer = system.GetSystemTimers().GetFakeDecrementer();
    const RamDigest& expected = g_manager.netplay_replay_initial_digest;
    NOTICE_LOG_FMT(CORE,
                   "Netplay Replay base comparison: frame={}/{} root={:016x}/{:016x} match={} "
                   "MEM1={:016x}/{:016x} MEM2={:016x}/{:016x} ARAM={:016x}/{:016x} "
                   "FakeVMEM={:016x}/{:016x} L1={:016x}/{:016x} ticks={}/{} TB={:#x}/{:#x} "
                   "fake-TB-value={:#x}/{:#x} fake-TB-ticks={}/{} DEC={:#x}/{:#x} RTC={}",
                   expected.frame, frame, expected.root, actual.root, expected.root == actual.root,
                   expected.mem1, actual.mem1, expected.mem2, actual.mem2, expected.aram,
                   actual.aram, expected.fake_vmem, actual.fake_vmem, expected.l1, actual.l1,
                   expected.core_ticks, actual.core_ticks, expected.timebase, actual.timebase,
                   expected.fake_tb_start_value, actual.fake_tb_start_value,
                   expected.fake_tb_start_ticks, actual.fake_tb_start_ticks, expected.decrementer,
                   actual.decrementer, g_manager.netplay_replay_initial_rtc);
    LogNetplayReplayMem1PageDifferences(system, g_manager.netplay_replay_initial_mem1_page_hashes);
  }
  for (int port = 0; port < g_manager.netplay_replay_players; ++port)
  {
    WirePad recorded_pad{};
    std::memcpy(&recorded_pad, inputs.data() + static_cast<std::size_t>(port) * sizeof(WirePad),
                sizeof(recorded_pad));
    WireInput wire = EncodeGameCubeInput(recorded_pad);
    const int handle = g_manager.player_handles[port];
    if (handle < 0)
    {
      ERROR_LOG_FMT(CORE, "Netplay Replay: missing Gekko actor for port {}", port + 1);
      g_manager.stop_requested.store(true, std::memory_order_relaxed);
      return false;
    }
    gekko_add_local_input(g_manager.session, handle, &wire);
  }
  ++g_manager.netplay_replay_playback_index;
  ++g_manager.local_input_frame;
  DEBUG_LOG_FMT(CORE, "Netplay Replay: submitted recorded frame {} to GekkoNet ({}/{})", frame,
                g_manager.netplay_replay_playback_index,
                g_manager.netplay_replay_playback_frames.size());
  return true;
}

void OnFrameBoundary(const Core::CPUThreadGuard& guard)
{
  ProcessFrameBoundary(guard, FrameBoundary::BrawlHook);
  RecordGekkoInputGuestResume();
}

void SignalVIBoundary(FrameBoundary boundary)
{
  if (g_manager.active.load(std::memory_order_acquire) && g_manager.frame_boundary == boundary)
  {
    g_manager.precise_vi_tail.store(false, std::memory_order_release);
    g_manager.vi_boundary_pending.store(true, std::memory_order_release);
  }
}

bool IsVIBoundaryPending()
{
  return g_manager.vi_boundary_pending.load(std::memory_order_acquire);
}

void UpdatePreciseVITailArm(bool next_is_begin_field, bool next_is_end_field,
                            bool next_is_new_field, bool next_is_si_poll)
{
  const bool active = g_manager.active.load(std::memory_order_acquire);
  const FrameBoundary selected_boundary = g_manager.frame_boundary;
  const bool selected = (selected_boundary == FrameBoundary::VIBeginField && next_is_begin_field) ||
                        (selected_boundary == FrameBoundary::VIEndField && next_is_end_field) ||
                        (selected_boundary == FrameBoundary::VINewField && next_is_new_field) ||
                        (selected_boundary == FrameBoundary::VIPreNewField && next_is_new_field) ||
                        (selected_boundary == FrameBoundary::VIPreSIPoll && next_is_si_poll);
  if (active && selected && g_manager.late_vi_guard)
  {
    g_manager.precise_vi_tail.store(false, std::memory_order_release);
    g_manager.precise_vi_guard_requested.store(true, std::memory_order_release);
  }
  else
  {
    g_manager.precise_vi_guard_requested.store(false, std::memory_order_release);
    g_manager.precise_vi_tail.store(active && selected, std::memory_order_release);
  }
}

bool ConsumePreciseVIGuardRequest()
{
  return g_manager.precise_vi_guard_requested.exchange(false, std::memory_order_acq_rel);
}

void ArmPreciseVITailFromGuard()
{
  if (g_manager.active.load(std::memory_order_acquire) && g_manager.late_vi_guard)
    g_manager.precise_vi_tail.store(true, std::memory_order_release);
}

bool IsPreciseVITailArmed()
{
  return g_manager.precise_vi_tail.load(std::memory_order_acquire);
}

bool IsDeterminismDiagnosticsEnabled()
{
  return g_manager.compare_confirmed_ram;
}

void RecordHostMemoryWrite(Core::System& system, std::string_view source, u32 address,
                           std::size_t size, u64 source_offset)
{
  if (!g_manager.active.load(std::memory_order_acquire) ||
      !g_manager.track_host_memory_writes.load(std::memory_order_acquire))
    return;

  std::lock_guard lk(g_manager.mutex);
  if (!g_manager.compare_confirmed_ram)
    return;

  const u8* const data = system.GetMemory().GetPointerForRangeReadOnly(address, size);
  const u64 data_hash = data ? XXH3_64bits(data, size) : 0;
  g_manager.host_memory_writes.push_back(HostMemoryWriteEvent{
      source, address, size, g_manager.frame_execution_gekko_frame, Rollback::IsResimulating(),
      system.GetCoreTiming().GetTicks(), data_hash, source_offset});
  while (g_manager.host_memory_writes.size() > 512)
    g_manager.host_memory_writes.pop_front();
}

void RunPendingVIBoundary(Core::System& system)
{
  if (!g_manager.vi_boundary_pending.exchange(false, std::memory_order_acq_rel))
    return;

  // The first VI event can run from the JIT's initial CoreTiming::Advance before a guest block has
  // established NPC. Such a snapshot serializes successfully but cannot resume after a load.
  // Wait for the first genuine dispatcher boundary instead of exposing an invalid frame -1.
  const auto& initial_ppc = system.GetPPCState();
  if (initial_ppc.npc == 0)
  {
    NOTICE_LOG_FMT(CORE, "GekkoNet: Ignoring pre-execution VI boundary (pc={:08x}, npc={:08x})",
                   initial_ppc.pc, initial_ppc.npc);
    return;
  }

  const Core::CPUThreadGuard guard(system);
  ProcessFrameBoundary(guard, g_manager.frame_boundary);
  RecordGekkoInputGuestResume();
  static u32 s_early_returns = 0;
  if (g_manager.stress_test && s_early_returns++ < 16)
  {
    const auto& ppc = system.GetPPCState();
    NOTICE_LOG_FMT(CORE,
                   "GekkoNet trace: VI handler returned (pc={:08x}, npc={:08x}, downcount={}, "
                   "ticks={})",
                   ppc.pc, ppc.npc, ppc.downcount, system.GetCoreTiming().GetTicks());
  }
}

}  // namespace Rollback
