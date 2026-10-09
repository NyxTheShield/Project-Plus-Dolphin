// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/WiiRoot.h"

#include <optional>
#include <filesystem>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Core/Boot/Boot.h"
#include "Core/CommonTitles.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/HW/WiiSave.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/Network/KD/VFF/VFFUtil.h"
#include "Core/IOS/Uids.h"
#include "Core/Movie.h"
#include "Core/NetPlayClient.h"
#include "Core/SysConf.h"
#include "Core/System.h"
namespace Core
{
namespace FS = IOS::HLE::FS;

static std::string s_temp_wii_root;
static std::string s_temp_redirect_root;
static bool s_wii_root_initialized = false;
static std::vector<IOS::HLE::FS::NandRedirect> s_nand_redirects;

// When Temp NAND + Redirects are both active, we need to keep track of where each redirect path
// should be copied back to after a successful session finish.
struct TempRedirectPath
{
  std::string real_path;
  std::string temp_path;
};
static std::vector<TempRedirectPath> s_temp_nand_redirects;

const std::vector<IOS::HLE::FS::NandRedirect>& GetActiveNandRedirects()
{
  return s_nand_redirects;
}

static bool CopyBackupFile(const std::string& path_from, const std::string& path_to)
{
  if (!File::Exists(path_from))
    return false;

  File::CreateFullPath(path_to);

  return File::CopyRegularFile(path_from, path_to);
}

static void DeleteBackupFile(const std::string& file_name)
{
  File::Delete(File::GetUserPath(D_BACKUP_IDX) + file_name);
}

static void BackupFile(const std::string& path_in_nand)
{
  const std::string file_name = PathToFileName(path_in_nand);
  const std::string original_path = File::GetUserPath(D_WIIROOT_IDX) + path_in_nand;
  const std::string backup_path = File::GetUserPath(D_BACKUP_IDX) + file_name;

  CopyBackupFile(original_path, backup_path);
}

static void RestoreFile(const std::string& path_in_nand)
{
  const std::string file_name = PathToFileName(path_in_nand);
  const std::string original_path = File::GetUserPath(D_WIIROOT_IDX) + path_in_nand;
  const std::string backup_path = File::GetUserPath(D_BACKUP_IDX) + file_name;

  if (CopyBackupFile(backup_path, original_path))
    DeleteBackupFile(file_name);
}

static void CopySave(FS::FileSystem* source, FS::FileSystem* dest, const u64 title_id)
{
  dest->CreateFullPath(IOS::PID_KERNEL, IOS::PID_KERNEL, Common::GetTitleDataPath(title_id) + '/',
                       0, {FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::ReadWrite});
  const auto source_save = WiiSave::MakeNandStorage(source, title_id);
  const auto dest_save = WiiSave::MakeNandStorage(dest, title_id);
  WiiSave::Copy(source_save.get(), dest_save.get());
}

static bool CopyNandFile(FS::FileSystem* source_fs, const std::string& source_file,
                         FS::FileSystem* dest_fs, const std::string& dest_file)
{
  auto source_handle =
      source_fs->OpenFile(IOS::PID_KERNEL, IOS::PID_KERNEL, source_file, IOS::HLE::FS::Mode::Read);
  // If the source file doesn't exist, there is nothing more to do.
  // This function must not create an empty file on the destination filesystem.
  if (!source_handle)
    return true;

  dest_fs->CreateFullPath(IOS::PID_KERNEL, IOS::PID_KERNEL, dest_file, 0,
                          {FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::ReadWrite});

  auto dest_handle =
      dest_fs->CreateAndOpenFile(IOS::PID_KERNEL, IOS::PID_KERNEL, source_file,
                                 {IOS::HLE::FS::Mode::ReadWrite, IOS::HLE::FS::Mode::ReadWrite,
                                  IOS::HLE::FS::Mode::ReadWrite});
  if (!dest_handle)
    return false;

  std::vector<u8> buffer(source_handle->GetStatus()->size);
  if (!source_handle->Read(buffer.data(), buffer.size()))
    return false;
  if (!dest_handle->Write(buffer.data(), buffer.size()))
    return false;

  return true;
}

static void InitializeDeterministicWiiSaves(FS::FileSystem* session_fs,
                                            const BootSessionData& boot_session_data)
{
  auto& movie = Core::System::GetInstance().GetMovie();
  const u64 title_id = SConfig::GetInstance().GetTitleID();
  const auto configured_fs = FS::MakeFileSystem(FS::Location::Configured);
  if (movie.IsRecordingInput())
  {
    if (NetPlay::IsNetPlayRunning() && !SConfig::GetInstance().bCopyWiiSaveNetplay)
    {
      movie.SetClearSave(true);
    }
    else
    {
      // TODO: Check for the actual save data
      const std::string path = Common::GetTitleDataPath(title_id) + "/banner.bin";
      movie.SetClearSave(!configured_fs->GetMetadata(IOS::PID_KERNEL, IOS::PID_KERNEL, path));
    }
  }

  if ((NetPlay::IsNetPlayRunning() && SConfig::GetInstance().bCopyWiiSaveNetplay) ||
      (movie.IsMovieActive() && !movie.IsStartingFromClearSave()))
  {
    auto* sync_fs = boot_session_data.GetWiiSyncFS();
    auto& sync_titles = boot_session_data.GetWiiSyncTitles();

    auto* source_fs = sync_fs ? sync_fs : configured_fs.get();
    INFO_LOG_FMT(CORE, "Wii Save Init: Copying from {} to session_fs.",
                 sync_fs ? "sync_fs" : "configured_fs");

    // Copy the current user's save to the Blank NAND
    if (movie.IsMovieActive() && !NetPlay::IsNetPlayRunning())
    {
      INFO_LOG_FMT(CORE, "Wii Save Init: Copying {0:016x}.", title_id);
      CopySave(source_fs, session_fs, title_id);
    }
    else
    {
      for (const u64 title : sync_titles)
      {
        INFO_LOG_FMT(CORE, "Wii Save Init: Copying {0:016x}.", title);
        CopySave(source_fs, session_fs, title);
      }
    }

    // Copy Mii data
    if (!CopyNandFile(source_fs, Common::GetMiiDatabasePath(), session_fs,
                      Common::GetMiiDatabasePath()))
    {
      WARN_LOG_FMT(CORE, "Failed to copy Mii database to the NAND");
    }

    const auto& netplay_redirect_folder = boot_session_data.GetWiiSyncRedirectFolder();
    if (!netplay_redirect_folder.empty())
    {
      File::CreateDirs(s_temp_redirect_root);
      File::Copy(netplay_redirect_folder, s_temp_redirect_root);
    }
  }
}

static void MoveToBackupIfExists(const std::string& path)
{
  if (File::Exists(path))
  {
    const std::string backup_path = path.substr(0, path.size() - 1) + ".backup";
    WARN_LOG_FMT(IOS_FS, "Temporary directory at {} exists, moving to backup...", path);

    // If backup exists, delete it as we don't want a mess
    if (File::Exists(backup_path))
    {
      WARN_LOG_FMT(IOS_FS, "Temporary backup directory at {} exists, deleting...", backup_path);
      File::DeleteDirRecursively(backup_path);
    }

    File::MoveWithOverwrite(path, backup_path);
  }
}

void InitializeWiiRoot(bool use_temporary)
{
  ASSERT(!s_wii_root_initialized);

  const std::string replay_file = Config::Get(Config::MAIN_REPLAY_PLAYBACK_FILE);
  use_temporary = use_temporary || !replay_file.empty();

  if (use_temporary)
  {
    s_temp_wii_root = File::GetUserPath(D_USER_IDX) + "WiiSession" DIR_SEP;
    s_temp_redirect_root = File::GetUserPath(D_USER_IDX) + "RedirectSession" DIR_SEP;
    WARN_LOG_FMT(IOS_FS, "Using temporary directory {} for minimal Wii FS", s_temp_wii_root);
    WARN_LOG_FMT(IOS_FS, "Using temporary directory {} for redirected saves", s_temp_redirect_root);

    // If directory exists, make a backup
    MoveToBackupIfExists(s_temp_wii_root);
    MoveToBackupIfExists(s_temp_redirect_root);

    File::SetUserPath(D_SESSION_WIIROOT_IDX, s_temp_wii_root);
	File::Copy(File::GetSysDirectory() + NETPLAY_SAVE_DIR, s_temp_wii_root);

    if (!replay_file.empty())
    {
      std::vector<u8> replay_bytes;
      File::IOFile source(replay_file, "rb");
      if (source.IsOpen())
      {
        replay_bytes.resize(static_cast<size_t>(source.GetSize()));
        if (source.ReadBytes(replay_bytes.data(), replay_bytes.size()))
        {
          const std::string replay_name = "/rp/" +
                                          std::filesystem::path(replay_file).filename().string();
          const auto fs = std::make_shared<FS::HostFileSystem>(s_temp_wii_root);
          const auto result = IOS::HLE::NWC24::WriteToVFF(
              "/title/00010000/52534245/data/collect.vff", replay_name, fs, replay_bytes);
          if (result != IOS::HLE::NWC24::WC24_OK)
            ERROR_LOG_FMT(IOS_FS, "Could not insert replay {} into the temporary NAND", replay_file);
        }
      }
    }
  }
  else
  {
    File::SetUserPath(D_SESSION_WIIROOT_IDX, File::GetUserPath(D_WIIROOT_IDX));
  }

  s_nand_redirects.clear();
  s_wii_root_initialized = true;
}

void ShutdownWiiRoot()
{
  if (Config::Get(Config::MAIN_SAVE_NETPLAY_REPLAYS))
  {
    const std::string brawl_temp_save =
        File::GetUserPath(D_USER_IDX) + "WiiSession" DIR_SEP "title" DIR_SEP
                                            "00010000" DIR_SEP "52534245" DIR_SEP "data" DIR_SEP;
    const std::string configured_replay_root =
        Config::Get(Config::MAIN_NETPLAY_REPLAY_DIRECTORY);
    const std::string replay_data = configured_replay_root.empty() ?
                                        File::GetUserPath(D_USER_IDX) + "ReplayData" DIR_SEP :
                                        configured_replay_root + DIR_SEP;
    const std::string game_directory =
        replay_data + Config::Get(Config::MAIN_NETPLAY_REPLAY_GAME) + DIR_SEP;

    if (File::Exists(brawl_temp_save + "collect.vff"))
    {
      time_t rawtime;
      struct tm* timeinfo;
      char buffer[80];

      time(&rawtime);
      timeinfo = localtime(&rawtime);

      strftime(buffer, sizeof(buffer), "%Y-%m-%d %H_%M_%S", timeinfo);
      std::string date(buffer);
      const std::string session_directory = game_directory + date.substr(0, 7) + DIR_SEP;
      File::CreateFullPath(session_directory);

      const std::string players = Config::Get(Config::MAIN_NETPLAY_REPLAY_PLAYERS);
      const std::string vff_path = brawl_temp_save + "collect.vff";

      // Extract individual .bin replays directly from the VFF FAT16 container
      std::vector<u8> vff_bytes;
      bool extracted_any = false;
      File::IOFile vff_file(vff_path, "rb");
      if (vff_file.IsOpen())
      {
        const u64 file_len = vff_file.GetSize();
        if (file_len >= 32)
        {
          vff_bytes.resize(static_cast<size_t>(file_len));
          if (vff_file.ReadBytes(vff_bytes.data(), vff_bytes.size()))
          {
            if (std::memcmp(vff_bytes.data(), "VFF ", 4) == 0)
            {
              constexpr u32 SYSECT = 233;
              const auto cluster_offset = [](u32 c) -> u64 {
                return static_cast<u64>(SYSECT + (c - 2)) * 512 - 480;
              };

              // /rp (replays) directory is cluster 3
              const u64 rp_offset = cluster_offset(3);
              if (rp_offset + 512 <= vff_bytes.size())
              {
                const u8* rp_dir = vff_bytes.data() + rp_offset;
                std::u16string lfn_parts;

                for (size_t i = 0; i < 512; i += 32)
                {
                  const u8* entry = rp_dir + i;
                  if (entry[0] == 0)
                    break;

                  const u8 attr = entry[11];
                  if (attr == 0x0F)
                  {
                    // LFN entry
                    std::u16string part;
                    const auto append_u16 = [&](size_t offset, size_t count) {
                      for (size_t c = 0; c < count; ++c)
                      {
                        const u16 ch = static_cast<u16>(entry[offset + c * 2]) |
                                       (static_cast<u16>(entry[offset + c * 2 + 1]) << 8);
                        if (ch == 0 || ch == 0xFFFF)
                          return;
                        part.push_back(static_cast<char16_t>(ch));
                      }
                    };
                    append_u16(1, 5);
                    append_u16(14, 6);
                    append_u16(28, 2);
                    lfn_parts = part + lfn_parts;
                  }
                  else
                  {
                    std::string entry_name;
                    if (!lfn_parts.empty())
                    {
                      for (char16_t ch : lfn_parts)
                        entry_name.push_back(static_cast<char>(ch));
                      lfn_parts.clear();
                    }
                    else
                    {
                      for (size_t c = 0; c < 11; ++c)
                      {
                        if (entry[c] != ' ' && entry[c] != 0)
                          entry_name.push_back(static_cast<char>(entry[c]));
                      }
                    }

                    const u16 start_clst = static_cast<u16>(entry[26]) |
                                           (static_cast<u16>(entry[27]) << 8);
                    const u32 size = static_cast<u32>(entry[28]) |
                                     (static_cast<u32>(entry[29]) << 8) |
                                     (static_cast<u32>(entry[30]) << 16) |
                                     (static_cast<u32>(entry[31]) << 24);

                    if ((attr & 0x10) == 0 && size > 0 && start_clst >= 2)
                    {
                      const u64 file_offset = cluster_offset(start_clst);
                      if (file_offset + size <= vff_bytes.size())
                      {
                        std::string out_name;
                        if (!players.empty())
                          out_name = fmt::format("{} {} {}", players, date, entry_name);
                        else
                          out_name = fmt::format("{} {}", date, entry_name);

                        if (!out_name.ends_with(".bin"))
                          out_name += ".bin";

                        const std::string out_path = session_directory + out_name;
                        File::IOFile out_file(out_path, "wb");
                        if (out_file.IsOpen() &&
                            out_file.WriteBytes(vff_bytes.data() + file_offset, size))
                        {
                          INFO_LOG_FMT(IOS_FS, "Extracted replay save file to {}", out_path);
                          extracted_any = true;
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }

      if (!extracted_any)
        WARN_LOG_FMT(IOS_FS, "No new .bin replay was found in collect.vff");
    }
  }
  
  if (WiiRootIsTemporary())
  {
    File::DeleteDirRecursively(s_temp_wii_root);
    s_temp_wii_root.clear();
    File::DeleteDirRecursively(s_temp_redirect_root);
    s_temp_redirect_root.clear();
    s_temp_nand_redirects.clear();
  }

  s_nand_redirects.clear();
  s_wii_root_initialized = false;
}

bool WiiRootIsInitialized()
{
  return s_wii_root_initialized;
}

bool WiiRootIsTemporary()
{
  return !s_temp_wii_root.empty();
}

void BackupWiiSettings()
{
  // Back up files which Dolphin can modify at boot, so that we can preserve the original contents.
  // For SYSCONF, the backup is only needed in case Dolphin crashes or otherwise exists unexpectedly
  // during emulation, since the config system will restore the SYSCONF settings at emulation end.
  // For setting.txt, there is no other code that restores the original values for us.

  BackupFile(Common::GetTitleDataPath(Titles::SYSTEM_MENU) + "/" WII_SETTING);
  BackupFile("/shared2/sys/SYSCONF");
}

void RestoreWiiSettings(RestoreReason reason)
{
  RestoreFile(Common::GetTitleDataPath(Titles::SYSTEM_MENU) + "/" WII_SETTING);

  // We must not restore the SYSCONF backup when ending emulation cleanly, since the user may have
  // edited the SYSCONF file in the NAND using the emulated software (e.g. the Wii Menu settings).
  if (reason == RestoreReason::CrashRecovery)
    RestoreFile("/shared2/sys/SYSCONF");
  else
    DeleteBackupFile("SYSCONF");
}

/// Copy a directory from host_source_path (on the host FS) to nand_target_path on the NAND.
///
/// Both paths should not have trailing slashes. To specify the NAND root, use "".
static bool CopySysmenuFilesToFS(FS::FileSystem* fs, const std::string& host_source_path,
                                 const std::string& nand_target_path)
{
  const auto entries = File::ScanDirectoryTree(host_source_path, false);
  for (const File::FSTEntry& entry : entries.children)
  {
    const std::string host_path = host_source_path + '/' + entry.virtualName;
    const std::string nand_path = nand_target_path + '/' + entry.virtualName;
    constexpr FS::Modes public_modes{FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::ReadWrite};

    if (entry.isDirectory)
    {
      fs->CreateDirectory(IOS::SYSMENU_UID, IOS::SYSMENU_GID, nand_path, 0, public_modes);
      if (!CopySysmenuFilesToFS(fs, host_path, nand_path))
        return false;
    }
    else
    {
      // Do not overwrite any existing files.
      if (fs->GetMetadata(IOS::SYSMENU_UID, IOS::SYSMENU_UID, nand_path).has_value())
        continue;

      File::IOFile host_file{host_path, "rb"};
      std::vector<u8> file_data(host_file.GetSize());
      if (!host_file.ReadBytes(file_data.data(), file_data.size()))
        return false;

      const auto nand_file =
          fs->CreateAndOpenFile(IOS::SYSMENU_UID, IOS::SYSMENU_GID, nand_path, public_modes);
      if (!nand_file || !nand_file->Write(file_data.data(), file_data.size()))
        return false;
    }
  }
  return true;
}

void InitializeWiiFileSystemContents(
    std::optional<DiscIO::Riivolution::SavegameRedirect> save_redirect,
    const BootSessionData& boot_session_data)
{
  const auto fs = Core::System::GetInstance().GetIOS()->GetFS();

  // Some games (such as Mario Kart Wii) assume that NWC24 files will always be present
  // even upon the first launch as they are normally created by the system menu.
  // Because we do not require the system menu to be run, WiiConnect24 files must be copied
  // to the NAND manually.
  if (!CopySysmenuFilesToFS(fs.get(), File::GetSysDirectory() + WII_USER_DIR, ""))
    WARN_LOG_FMT(CORE, "Failed to copy initial System Menu files to the NAND");

  const bool is_temp_nand = WiiRootIsTemporary();
  if (is_temp_nand)
  {
    // Generate a SYSCONF with default settings for the temporary Wii NAND.
    SysConf sysconf{fs};
    sysconf.Save();

    InitializeDeterministicWiiSaves(fs.get(), boot_session_data);
  }

  if (save_redirect)
  {
    const u64 title_id = SConfig::GetInstance().GetTitleID();
    std::string source_path = Common::GetTitleDataPath(title_id);

    if (is_temp_nand)
    {
      // remember the actual path for copying back on shutdown and redirect to a temp folder instead
      s_temp_nand_redirects.emplace_back(
          TempRedirectPath{save_redirect->m_target_path, s_temp_redirect_root});
      save_redirect->m_target_path = s_temp_redirect_root;
    }

    if (!File::IsDirectory(save_redirect->m_target_path))
    {
      File::CreateDirs(save_redirect->m_target_path);
      if (save_redirect->m_clone)
      {
        File::Copy(Common::GetTitleDataPath(title_id, Common::FromWhichRoot::Session),
                   save_redirect->m_target_path);
      }
    }
    s_nand_redirects.emplace_back(IOS::HLE::FS::NandRedirect{
        std::move(source_path), std::move(save_redirect->m_target_path)});
    fs->SetNandRedirects(s_nand_redirects);
  }
}

void CleanUpWiiFileSystemContents(const BootSessionData& boot_session_data)
{
  // In TAS mode, copy back always.
  // In Netplay, only copy back when we're the host and writing back is enabled.
  const bool wii_root_is_temporary = WiiRootIsTemporary();
  const auto* netplay_settings = boot_session_data.GetNetplaySettings();
  const bool is_netplay_write = netplay_settings && netplay_settings->savedata_write;
  const bool is_netplay_host = netplay_settings && netplay_settings->is_hosting;
  const bool cleanup_required =
      wii_root_is_temporary && (!netplay_settings || (is_netplay_write && is_netplay_host));

  INFO_LOG_FMT(CORE,
               "Wii FS Cleanup: cleanup_required = {} (wii_root_is_temporary = {}, "
               "is netplay = {}, is_netplay_write = {}, is_netplay_host = {})",
               cleanup_required, wii_root_is_temporary, !!netplay_settings, is_netplay_write,
               is_netplay_host);

  if (!cleanup_required)
    return;

  INFO_LOG_FMT(CORE, "Wii FS Cleanup: Copying from temporary FS to configured_fs.");

  // copy back the temp nand redirected files to where they should normally be redirected to
  for (const auto& redirect : s_temp_nand_redirects)
  {
    File::CreateFullPath(redirect.real_path);
    File::MoveWithOverwrite(redirect.temp_path, redirect.real_path);
  }

  IOS::HLE::EmulationKernel* ios = Core::System::GetInstance().GetIOS();

  // clear the redirects in the session FS, otherwise the back-copy might grab redirected files
  s_nand_redirects.clear();
  ios->GetFS()->SetNandRedirects({});

  const auto configured_fs = FS::MakeFileSystem(FS::Location::Configured);

  // Copy back Mii data
  if (!CopyNandFile(ios->GetFS().get(), Common::GetMiiDatabasePath(), configured_fs.get(),
                    Common::GetMiiDatabasePath()))
  {
    WARN_LOG_FMT(CORE, "Failed to copy Mii database to the NAND");
  }

  // If we started by copying only certain saves, we also only want to copy back those exact saves.
  // This prevents a situation where you change game and create a new save that was not loaded from
  // the real NAND during netplay, and that then overwrites your existing local save during this
  // cleanup process.
  const bool copy_all = !netplay_settings || netplay_settings->savedata_sync_all_wii;
  for (const u64 title_id :
       (copy_all ? ios->GetESCore().GetInstalledTitles() : boot_session_data.GetWiiSyncTitles()))
  {
    INFO_LOG_FMT(CORE, "Wii FS Cleanup: Copying {0:016x}.", title_id);

    const auto session_save = WiiSave::MakeNandStorage(ios->GetFS().get(), title_id);

    // FS won't write the save if the directory doesn't exist
    const std::string title_path = Common::GetTitleDataPath(title_id);
    configured_fs->CreateFullPath(IOS::PID_KERNEL, IOS::PID_KERNEL, title_path + '/', 0,
                                  {FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::ReadWrite});

    const auto user_save = WiiSave::MakeNandStorage(configured_fs.get(), title_id);

    const std::string backup_path =
        fmt::format("{}/{:016x}.bin", File::GetUserPath(D_BACKUP_IDX), title_id);
    const auto backup_save = WiiSave::MakeDataBinStorage(&ios->GetIOSC(), backup_path, "w+b");

    // Backup the existing save just in case it's still needed.
    WiiSave::Copy(user_save.get(), backup_save.get());
    WiiSave::Copy(session_save.get(), user_save.get());
  }
}
}  // namespace Core
