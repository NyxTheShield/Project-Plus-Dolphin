// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QMainWindow>
#include <QStringList>

#include <array>
#include <memory>
#include <optional>
#include <string>

#ifdef USE_RETRO_ACHIEVEMENTS
#include "Common/Config/Config.h"
#endif  // USE_RETRO_ACHIEVEMENTS

#include "Core/Boot/Boot.h"
#include "DolphinQt/FIFO/FIFOPlayerWindow.h"

class QMenu;
class QStackedWidget;
class QString;

class AchievementsWindow;
class AssemblerWidget;
class BreakpointWidget;
struct BootParameters;
class CheatsManager;
class CodeWidget;
class DiscordHandler;
class DragEnterEvent;
class FreeLookWindow;
class GameCount;
class GameList;
class GBATASInputWindow;
class GCTASInputWindow;
class HotkeyScheduler;
class InfinityBaseWindow;
class JITWidget;
class LogConfigWidget;
class LogWidget;
class MappingWindow;
class MemoryWidget;
class MenuBar;
class NetPlayDialog;
class NetPlaySetupDialog;
class NetworkWidget;
class RegisterWidget;
class RenderWidget;
class SearchBar;
class SettingsWindow;
class SkylanderPortalWindow;
class ThreadWidget;
class ToolBar;
class WatchWidget;
class WiiTASInputWindow;
class WiiSpeakWindow;
class LogitechMicWindow;
struct WindowSystemInfo;

namespace Core
{
class System;
}

namespace DiscIO
{
enum class Region;
}

namespace UICommon
{
class GameFile;
}

namespace X11Utils
{
class XRRConfiguration;
}

class MainWindow final : public QMainWindow
{
  Q_OBJECT

public:
  explicit MainWindow(Core::System& system, std::unique_ptr<BootParameters> boot_parameters,
                      const std::string& movie_path, const std::string& brawlback_ticket_path);
  ~MainWindow() override;

  WindowSystemInfo GetWindowSystemInfo() const;

  bool eventFilter(QObject* object, QEvent* event) override;
  QMenu* createPopupMenu() override;

  void ShowTriforceWindow();

signals:
  void ReadOnlyModeChanged(bool read_only);
  void RecordingStatusChanged(bool recording);

private:
  void Open();
  void RefreshGameList();
  void Play(const std::optional<std::string>& savestate_path = {});
  void Pause();
  void TogglePause();

  // May ask for confirmation. Returns whether or not it actually stopped.
  bool RequestStop();
  void ForceManagedProcessExit(bool notify_peers);
  void ForceStop();
  void Reset();
  void FrameAdvance();
  void StateLoad();
  void StateSave();
  void StateLoadSlot();
  void StateSaveSlot();
  void StateLoadSlotAt(int slot);
  void StateSaveSlotAt(int slot);
  void StateLoadLastSavedAt(int slot);
  void StateLoadUndo();
  void StateSaveUndo();
  void StateSaveOldest();
  void SetStateSlot(int slot);
  void IncrementSelectedStateSlot();
  void DecrementSelectedStateSlot();
  void BootWiiSystemMenu();

  void PerformOnlineUpdate(const std::string& region);

  void SetFullScreenResolution(bool fullscreen);

  void FullScreen();
  void UnlockCursor();
  void ScreenShot();

  void CreateComponents();

  void ConnectGameList();
  void ConnectHost();
  void ConnectHotkeys();
  void ConnectMenuBar();
  void ConnectRenderWidget();
  void ConnectStack();
  void ConnectToolBar();

  void InitControllers();
  void ShutdownControllers();

  void InitCoreCallbacks();

  enum class ScanForSecondDisc
  {
    Yes,
    No,
  };

  void ScanForSecondDiscAndStartGame(const UICommon::GameFile& game,
                                     std::unique_ptr<BootSessionData> boot_session_data = nullptr);
  void StartGame(const QString& path, ScanForSecondDisc scan,
                 std::unique_ptr<BootSessionData> boot_session_data = nullptr);
  void StartGame(const std::string& path, ScanForSecondDisc scan,
                 std::unique_ptr<BootSessionData> boot_session_data = nullptr);
  void StartGame(const std::vector<std::string>& paths,
                 std::unique_ptr<BootSessionData> boot_session_data = nullptr);
  void StartGame(std::unique_ptr<BootParameters>&& parameters);
  void BootGame(std::unique_ptr<BootParameters>&& parameters);
  void ConnectFullscreenBootSignals();
  void OnFullscreenBootStateChanged(bool fullscreen);
  void OnFullscreenBootSizeChanged(int width, int height);
  void TryCompleteFullscreenBoot();
  void ShowRenderWidget();
  void HideRenderWidget(bool reinit = true, bool is_exit = false);

  void ShowSettingsWindow();
  void ShowGeneralWindow();
  void ShowAudioWindow();
  void ShowControllersWindow();
  void ShowGraphicsWindow();
  void ShowFreeLookWindow();
  void ShowAboutDialog();
  void ShowHotkeyDialog();
  void ShowNetPlaySetupDialog();
  void ShowNetPlayBrowser();
  void ShowFIFOPlayer();
  void ShowSkylanderPortal();
  void ShowInfinityBase();
  void ShowWiiSpeakWindow();
  void ShowLogitechMicWindow();
  void ShowMemcardManager();
  void ShowResourcePackManager();
  void ShowCheatsManager();
  void ShowRiivolutionBootWidget(const UICommon::GameFile& game);

#ifdef USE_RETRO_ACHIEVEMENTS
  void ShowAchievementsWindow();
  void ShowAchievementSettings();
  void OnHardcoreChanged();
#endif  // USE_RETRO_ACHIEVEMENTS

  void NetPlayInit();
  bool NetPlayJoin();
  bool NetPlayHost(const UICommon::GameFile& game);
  void NetPlayQuit();
  void StartManagedNetPlay();
  void TryStartManagedNetplayReplay();
  void PollManagedNetPlayReady();
  void ReportManagedStatus(const std::string& state, const std::string& reason = {});
  void FailManagedSession(const std::string& reason);

  void OnBootGameCubeIPL(DiscIO::Region region);
  void OnImportNANDBackup();
  void OnConnectWiiRemote(int id);

#if defined(__unix__) || defined(__unix) || defined(__APPLE__)
  void OnSignal();
#endif

  void OnPlayRecording();
  void OnStartRecording();
  void OnStopRecording();
  void OnExportRecording();
  void OnActivateChat();
  void OnCollapseChat();
  void OnExpandChat();
  void OnRequestGolfControl();
  void ShowTASInput();
  void ShowOSDWindow();

  void ChangeDisc();
  void EjectDisc();

  void OpenUserFolder();
  void OpenConfigFolder();
  void OpenCacheFolder();

  QStringList PromptFileNames();

  void UpdateScreenSaverInhibition();

  void OnStopComplete();
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;
  QSize sizeHint() const override;

  Core::System& m_system;

#ifdef HAVE_XRANDR
  std::unique_ptr<X11Utils::XRRConfiguration> m_xrr_config;
#endif

  QStackedWidget* m_stack;
  ToolBar* m_tool_bar;
  MenuBar* m_menu_bar;
  SearchBar* m_search_bar;
  GameList* m_game_list;
  GameCount* m_game_count;
  RenderWidget* m_render_widget = nullptr;
  bool m_rendering_to_main;
  bool m_stop_confirm_showing = false;
  bool m_stop_requested = false;
  bool m_exit_requested = false;
  bool m_is_screensaver_inhibited = false;
  u32 m_state_slot = 1;
  std::unique_ptr<BootParameters> m_pending_boot;
  std::unique_ptr<BootParameters> m_pending_fullscreen_boot;
  bool m_fullscreen_boot_state_observed = false;
  bool m_fullscreen_boot_size_observed = false;
  bool m_fullscreen_boot_completion_scheduled = false;
  int m_fullscreen_boot_width = 0;
  int m_fullscreen_boot_height = 0;

  struct ManagedSession
  {
    std::string ticket_path;
    std::string match_id;
    std::string player_id;
    std::string display_name;
    std::string rendezvous_address;
    std::string rendezvous_token;
    std::string game;
    std::string brawl_iso_path;
    std::string project_plus_sd_path;
    std::string sd_card_path;
    std::string game_path;
    std::string game_id;
    u16 game_revision = 0;
    u8 game_platform = 2;
    std::string status_path;
    std::string replay_output_path;
    std::string replay_input_path;
    int seat = 0;
    int player_count = 0;
    std::vector<std::string> player_ids;
    std::vector<std::string> player_names;
    std::vector<u8> controller_types;
    std::vector<std::string> controller_tags;
    std::vector<std::string> controller_profiles;
    bool test_solo = false;
    bool stress_test = false;
    bool simulate_remote_p2 = false;
    bool enable_random_inputs = false;
    bool disable_project_plus_music = false;
    bool native_stress = false;
    u32 stress_interval = 1;
    std::optional<u32> rollback_window_override;
    u32 frame_boundary = 2;
    std::string gecko_codes_path;
    std::string wii_save_data_bin_path;
    bool full_scan_benchmark = false;
    bool full_scan_keep_tracking = false;
    bool parallel_dirty_compare = true;
    bool tracked_bitmap_clear = true;
    std::optional<u32> input_delay_override;
    u32 input_delay_reduction = 2;
    std::string input_delay_mode = "safe";
    bool compare_confirmed_ram = false;
    bool remote_ram_match = false;
    bool compare_confirmed_video = false;
    bool stop_ram_compare_on_mismatch = false;
    bool deep_logging = false;
    bool enable_netplay_replays = false;
    bool netplay_replay_recording_started = false;
    bool coarse_fifo_timing = false;
    bool late_vi_guard = true;
    bool latency_sampling_started = false;
    u32 ready_poll_count = 0;
    bool start_requested = false;
    bool started_reported = false;
    bool failure_reported = false;
  };
  std::optional<ManagedSession> m_managed_session;

  SettingsWindow* m_settings_window = nullptr;
  // m_fifo_window doesn't set MainWindow as its parent so that the fifo can be focused without
  // raising the main window, so use a unique_ptr to make sure it gets destroyed.
  std::unique_ptr<FIFOPlayerWindow> m_fifo_window = nullptr;
  SkylanderPortalWindow* m_skylander_window = nullptr;
  InfinityBaseWindow* m_infinity_window = nullptr;
  WiiSpeakWindow* m_wii_speak_window = nullptr;
  LogitechMicWindow* m_logitech_mic_window = nullptr;
  MappingWindow* m_hotkey_window = nullptr;
  FreeLookWindow* m_freelook_window = nullptr;

  HotkeyScheduler* m_hotkey_scheduler;
  NetPlayDialog* m_netplay_dialog;
  DiscordHandler* m_netplay_discord;
  NetPlaySetupDialog* m_netplay_setup_dialog;
  static constexpr int num_gc_controllers = 4;
  std::array<GCTASInputWindow*, num_gc_controllers> m_gc_tas_input_windows{};
  std::array<GBATASInputWindow*, num_gc_controllers> m_gba_tas_input_windows{};
  static constexpr int num_wii_controllers = 4;
  std::array<WiiTASInputWindow*, num_wii_controllers> m_wii_tas_input_windows{};

#ifdef USE_RETRO_ACHIEVEMENTS
  AchievementsWindow* m_achievements_window = nullptr;
  Config::ConfigChangedCallbackID m_config_changed_callback_id;
  bool m_former_hardcore_setting = false;
#endif  // USE_RETRO_ACHIEVEMENTS

  AssemblerWidget* m_assembler_widget;
  BreakpointWidget* m_breakpoint_widget;
  CodeWidget* m_code_widget;
  JITWidget* m_jit_widget;
  LogWidget* m_log_widget;
  LogConfigWidget* m_log_config_widget;
  MemoryWidget* m_memory_widget;
  NetworkWidget* m_network_widget;
  RegisterWidget* m_register_widget;
  ThreadWidget* m_thread_widget;
  WatchWidget* m_watch_widget;
  CheatsManager* m_cheats_manager{};
  QByteArray m_render_widget_geometry;
};
