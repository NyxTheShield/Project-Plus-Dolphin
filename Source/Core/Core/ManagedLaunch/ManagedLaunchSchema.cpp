// Copyright 2026 Brawlback Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/ManagedLaunch/ManagedLaunchSchema.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <initializer_list>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace Core::ManagedLaunch
{
namespace
{
ValidationResult Invalid(std::string error)
{
  return {.valid = false, .error = std::move(error)};
}

bool IsInteger(const picojson::value& value, double minimum, double maximum)
{
  if (!value.is<double>())
    return false;
  const double number = value.get<double>();
  return std::isfinite(number) && std::floor(number) == number && number >= minimum &&
         number <= maximum;
}

bool HasString(const picojson::object& object, std::string_view name, bool allow_empty = false)
{
  const auto it = object.find(std::string{name});
  return it != object.end() && it->second.is<std::string>() &&
         (allow_empty || !it->second.get<std::string>().empty());
}

bool HasInteger(const picojson::object& object, std::string_view name, double minimum,
                double maximum)
{
  const auto it = object.find(std::string{name});
  return it != object.end() && IsInteger(it->second, minimum, maximum);
}

bool HasBool(const picojson::object& object, std::string_view name)
{
  const auto it = object.find(std::string{name});
  return it != object.end() && it->second.is<bool>();
}

ValidationResult ValidateOptionalTypes(const picojson::object& ticket)
{
  static const std::unordered_set<std::string> string_fields{
      "brawl_iso_path",       "project_plus_sd_path", "sd_card_path",
      "game_path",            "game_id",              "status_path",
      "replay_output_path",   "replay_input_path",     "replay_path",
      "gecko_codes_path",     "wii_save_data_bin_path", "video_backend",
      "gamecube_memory_card_path", "wii_nand_path",
      "session_fingerprint",
  };
  static const std::unordered_set<std::string> bool_fields{
      "stress_test",
      "simulate_remote_p2",
      "enable_random_inputs",
      "disable_project_plus_music",
      "native_stress",
      "full_scan_benchmark",
      "full_scan_keep_tracking",
      "parallel_dirty_compare",
      "tracked_bitmap_clear",
      "compare_confirmed_ram",
      "remote_ram_match",
      "compare_confirmed_video",
      "stop_ram_compare_on_mismatch",
      "enable_deep_logging",
      "enable_netplay_replays",
      "coarse_fifo_timing",
      "late_vi_guard",
      "save_replays",
      "start_fullscreen",
  };
  for (const auto& [name, value] : ticket)
  {
    if (string_fields.contains(name) && !value.is<std::string>())
      return Invalid("Field '" + name + "' must be a string");
    if (bool_fields.contains(name) && !value.is<bool>())
      return Invalid("Field '" + name + "' must be a boolean");
  }
  return {.valid = true};
}

ValidationResult ValidatePlayers(const picojson::value& value, int local_seat)
{
  if (!value.is<picojson::array>())
    return Invalid("Field 'players' must be an array");
  const picojson::array& players = value.get<picojson::array>();
  if (players.empty() || players.size() > 4 || local_seat < 1 ||
      local_seat > static_cast<int>(players.size()))
  {
    return Invalid("The player roster size or local seat is invalid");
  }

  std::unordered_set<int> seats;
  std::unordered_set<std::string> player_ids;
  for (const picojson::value& player_value : players)
  {
    if (!player_value.is<picojson::object>())
      return Invalid("Every player entry must be an object");
    const picojson::object& player = player_value.get<picojson::object>();
    static const std::unordered_set<std::string> allowed{
        "player_id", "display_name", "seat", "controller_type", "controller_tag",
        "controller_profile"};
    for (const auto& [name, unused] : player)
    {
      if (!allowed.contains(name))
        return Invalid("Unknown player field '" + name + "'");
    }
    if (!HasString(player, "player_id") || !HasString(player, "display_name") ||
        !HasInteger(player, "seat", 1, 4) || !HasString(player, "controller_type") ||
        !HasString(player, "controller_tag", true) ||
        !HasString(player, "controller_profile", true))
    {
      return Invalid("A player entry is incomplete or malformed");
    }
    const int seat = static_cast<int>(player.at("seat").get<double>());
    const std::string& player_id = player.at("player_id").get<std::string>();
    const std::string& controller = player.at("controller_type").get<std::string>();
    if (!seats.insert(seat).second || !player_ids.insert(player_id).second)
      return Invalid("Player seats and IDs must be unique");
    if (controller != "gamecube" && controller != "wiimote")
      return Invalid("A player has an unsupported controller type");
  }
  for (int seat = 1; seat <= static_cast<int>(players.size()); ++seat)
  {
    if (!seats.contains(seat))
      return Invalid("Player seats must be contiguous starting at one");
  }
  return {.valid = true};
}
}  // namespace

ValidationResult ValidateTicket(const picojson::value& root)
{
  if (!root.is<picojson::object>())
    return Invalid("Managed launch ticket must be a JSON object");
  const picojson::object& ticket = root.get<picojson::object>();
  static const std::unordered_set<std::string> allowed_fields{
      "schema_version", "mode", "match_id", "player_id", "seat", "players",
      "rendezvous_addr", "rendezvous_token", "expires_at", "test_solo", "game",
      "input_delay_mode", "input_delay_override", "rollback_window_override",
      "enable_deep_logging", "enable_netplay_replays", "enable_random_inputs",
      "remote_ram_match", "aspect_ratio", "video_backend", "internal_resolution",
      "frame_boundary", "gecko_codes_path", "wii_save_data_bin_path",
      "disable_project_plus_music", "brawl_iso_path", "project_plus_sd_path", "sd_card_path",
      "game_path", "game_id", "game_revision", "game_platform", "status_path",
      "replay_output_path", "replay_input_path", "replay_path", "stress_test",
      "simulate_remote_p2", "native_stress", "stress_interval", "full_scan_benchmark",
      "full_scan_keep_tracking", "parallel_dirty_compare", "tracked_bitmap_clear",
      "compare_confirmed_ram", "compare_confirmed_video", "stop_ram_compare_on_mismatch",
      "coarse_fifo_timing", "late_vi_guard", "save_replays", "start_fullscreen",
      "gamecube_memory_card_path", "wii_nand_path", "session_fingerprint"};
  for (const auto& [name, unused] : ticket)
  {
    if (!allowed_fields.contains(name))
      return Invalid("Unknown managed launch field '" + name + "'");
  }
  if (!HasInteger(ticket, "schema_version", 1, 1) || !HasString(ticket, "mode") ||
      !HasString(ticket, "match_id") || !HasString(ticket, "game") ||
      !HasString(ticket, "status_path"))
  {
    return Invalid("Managed launch identity fields are missing or malformed");
  }
  if (const ValidationResult result = ValidateOptionalTypes(ticket); !result)
    return result;

  const std::string& mode = ticket.at("mode").get<std::string>();
  if (mode == "netplay")
  {
    if (!HasString(ticket, "player_id") || !HasString(ticket, "rendezvous_addr") ||
        !HasString(ticket, "rendezvous_token") || !HasString(ticket, "expires_at") ||
        !HasInteger(ticket, "seat", 1, 4) || !HasBool(ticket, "test_solo") ||
        !HasString(ticket, "input_delay_mode") || !HasBool(ticket, "enable_deep_logging") ||
        !HasBool(ticket, "enable_netplay_replays") || !HasBool(ticket, "enable_random_inputs") ||
        !HasBool(ticket, "remote_ram_match") || !HasInteger(ticket, "aspect_ratio", 0, 5) ||
        !HasString(ticket, "video_backend") || !HasInteger(ticket, "internal_resolution", 1, 12) ||
        !HasInteger(ticket, "frame_boundary", 0, 5) ||
        !HasString(ticket, "gecko_codes_path", true) ||
        !HasString(ticket, "wii_save_data_bin_path", true) ||
        !HasBool(ticket, "disable_project_plus_music") ||
        !HasString(ticket, "brawl_iso_path", true) ||
        !HasString(ticket, "project_plus_sd_path", true) ||
        !HasString(ticket, "sd_card_path", true) || !HasString(ticket, "game_path", true) ||
        !HasString(ticket, "game_id", true) || !HasInteger(ticket, "game_revision", 0, 65535) ||
        !HasInteger(ticket, "game_platform", 0, 255) ||
        !HasString(ticket, "replay_output_path", true) || !HasBool(ticket, "save_replays") ||
        !HasBool(ticket, "start_fullscreen") ||
        !HasString(ticket, "gamecube_memory_card_path", true) ||
        !HasString(ticket, "wii_nand_path", true))
    {
      return Invalid("Managed netplay fields are missing or malformed");
    }
    if (!HasString(ticket, "session_fingerprint"))
      return Invalid("The managed session fingerprint is missing");
    const std::string& fingerprint = ticket.at("session_fingerprint").get<std::string>();
    if (fingerprint.size() != 64 ||
        !std::all_of(fingerprint.begin(), fingerprint.end(),
                     [](unsigned char c) { return std::isdigit(c) || (c >= 'a' && c <= 'f'); }))
    {
      return Invalid("The managed session fingerprint is malformed");
    }
    const std::string& delay_mode = ticket.at("input_delay_mode").get<std::string>();
    if (delay_mode != "safe" && delay_mode != "normal" && delay_mode != "aggressive")
      return Invalid("The input delay mode is invalid");
    const std::string& video_backend = ticket.at("video_backend").get<std::string>();
    if (video_backend != "D3D" && video_backend != "D3D12" && video_backend != "Vulkan" &&
        video_backend != "OGL")
    {
      return Invalid("The video backend is invalid");
    }
    if (!ticket.contains("players"))
      return Invalid("The player roster is missing");
    if (const ValidationResult result =
            ValidatePlayers(ticket.at("players"), static_cast<int>(ticket.at("seat").get<double>()));
        !result)
    {
      return result;
    }
    const size_t player_count = ticket.at("players").get<picojson::array>().size();
    if (ticket.at("test_solo").get<bool>() != (player_count == 1))
      return Invalid("The solo-session flag does not match the player roster");

    const bool has_memcard = !ticket.at("gamecube_memory_card_path").get<std::string>().empty();
    const bool has_nand = !ticket.at("wii_nand_path").get<std::string>().empty();
    if (has_memcard && has_nand)
      return Invalid("GameCube memory-card and Wii NAND overrides are mutually exclusive");

    const bool stress = ticket.contains("stress_test") && ticket.at("stress_test").get<bool>();
    const bool native_stress =
        ticket.contains("native_stress") && ticket.at("native_stress").get<bool>();
    const bool simulated_peer =
        ticket.contains("simulate_remote_p2") && ticket.at("simulate_remote_p2").get<bool>();
    if ((native_stress || simulated_peer) && (player_count != 1 ||
                                               ticket.at("seat").get<double>() != 1.0))
      return Invalid("Solo rollback diagnostics require a one-player seat-one roster");
    if (native_stress && (!stress || simulated_peer))
      return Invalid("Native rollback stress settings are inconsistent");
    if (stress && !ticket.contains("rollback_window_override"))
      return Invalid("Rollback stress requires an explicit rollback window");
  }
  else if (mode == "netplay_replay" || mode == "replay")
  {
    if (!HasString(ticket, "brawl_iso_path", true) ||
        !HasString(ticket, "project_plus_sd_path", true) ||
        !HasString(ticket, "sd_card_path", true) || !HasString(ticket, "game_path", true) ||
        !HasString(ticket, "replay_path") || !HasInteger(ticket, "aspect_ratio", 0, 5) ||
        !HasString(ticket, "video_backend") ||
        !HasInteger(ticket, "internal_resolution", 1, 12) ||
        !HasBool(ticket, "disable_project_plus_music") ||
        !HasBool(ticket, "start_fullscreen") ||
        !HasString(ticket, "gamecube_memory_card_path", true) ||
        !HasString(ticket, "wii_nand_path", true) || !HasBool(ticket, "save_replays"))
    {
      return Invalid("Managed replay fields are missing or malformed");
    }
    const std::string& video_backend = ticket.at("video_backend").get<std::string>();
    if (video_backend != "D3D" && video_backend != "D3D12" && video_backend != "Vulkan" &&
        video_backend != "OGL")
    {
      return Invalid("The video backend is invalid");
    }
    if (!ticket.at("gamecube_memory_card_path").get<std::string>().empty() &&
        !ticket.at("wii_nand_path").get<std::string>().empty())
    {
      return Invalid("GameCube memory-card and Wii NAND overrides are mutually exclusive");
    }
  }
  else
  {
    return Invalid("The managed launch mode is unsupported");
  }

  const auto validate_optional_integer = [&ticket](std::string_view name, double minimum,
                                                    double maximum) -> ValidationResult {
    const auto it = ticket.find(std::string{name});
    if (it != ticket.end() && !IsInteger(it->second, minimum, maximum))
      return Invalid("Field '" + std::string{name} + "' is not a valid integer");
    return {.valid = true};
  };
  for (const auto& [name, minimum, maximum] :
       {std::tuple{"input_delay_override", 0.0, 60.0},
        std::tuple{"rollback_window_override", 1.0, 10.0},
        std::tuple{"frame_boundary", 0.0, 5.0}, std::tuple{"aspect_ratio", 0.0, 5.0},
        std::tuple{"internal_resolution", 1.0, 12.0},
        std::tuple{"game_revision", 0.0, 65535.0}, std::tuple{"game_platform", 0.0, 255.0},
        std::tuple{"stress_interval", 1.0, 600.0}})
  {
    if (const ValidationResult result = validate_optional_integer(name, minimum, maximum); !result)
      return result;
  }
  return {.valid = true};
}

ParseResult ParseTicket(const picojson::value& root)
{
  const ValidationResult validation = ValidateTicket(root);
  if (!validation)
    return {.ticket = std::nullopt, .error = validation.error};
  return {.ticket = Ticket{root.get<picojson::object>()}, .error = {}};
}

std::string NormalizedSummary(const picojson::value& root)
{
  const ParseResult parsed = ParseTicket(root);
  if (!parsed)
    return "invalid: " + parsed.error;

  const picojson::object& ticket = parsed.ticket->GetFields();
  const auto string_value = [&ticket](std::string_view name) {
    const auto it = ticket.find(std::string{name});
    return it != ticket.end() && it->second.is<std::string>() ? it->second.get<std::string>() :
                                                                std::string{};
  };
  const auto integer_value = [&ticket](std::string_view name) -> std::string {
    const auto it = ticket.find(std::string{name});
    if (it == ticket.end() || !it->second.is<double>())
      return "none";
    return std::to_string(static_cast<int>(it->second.get<double>()));
  };
  size_t player_count = 0;
  if (const auto it = ticket.find("players"); it != ticket.end() && it->second.is<picojson::array>())
    player_count = it->second.get<picojson::array>().size();

  return "schema=1 mode=" + string_value("mode") + " game=" + string_value("game") +
         " players=" + std::to_string(player_count) + " seat=" + integer_value("seat") +
         " rollback=" + integer_value("rollback_window_override") +
         " boundary=" + integer_value("frame_boundary");
}

std::string FormatTicketForLog(const picojson::value& root)
{
  picojson::value formatted = root;
  if (formatted.is<picojson::object>())
  {
    picojson::object& ticket = formatted.get<picojson::object>();
    if (auto it = ticket.find("rendezvous_token"); it != ticket.end())
      it->second = picojson::value{std::string{"<redacted>"}};
  }
  return formatted.serialize(true);
}
}  // namespace Core::ManagedLaunch
