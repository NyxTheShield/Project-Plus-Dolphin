// Copyright 2026 Brawlback Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/ManagedLaunch/ManagedLaunchSchema.h"

#include <string>

#include <gtest/gtest.h>
#include <picojson.h>

namespace Core::ManagedLaunch
{
namespace
{
picojson::value Parse(const std::string& json)
{
  picojson::value value;
  const std::string error = picojson::parse(value, json);
  EXPECT_TRUE(error.empty()) << error;
  return value;
}

constexpr const char* VALID_TICKET = R"json({
  "schema_version": 1,
  "mode": "netplay",
  "match_id": "match-fixture",
  "player_id": "player-one",
  "seat": 1,
  "players": [{
    "player_id": "player-one",
    "display_name": "Player One",
    "seat": 1,
    "controller_type": "gamecube",
    "controller_tag": "P1",
    "controller_profile": "001122"
  }],
  "rendezvous_addr": "match.example.invalid:9000",
  "rendezvous_token": "fixture-token",
  "expires_at": "2026-10-19T12:00:00Z",
  "test_solo": true,
  "game": "project-plus",
  "input_delay_mode": "safe",
  "enable_deep_logging": false,
  "enable_netplay_replays": true,
  "enable_random_inputs": false,
  "remote_ram_match": false,
  "aspect_ratio": 0,
  "video_backend": "D3D",
  "internal_resolution": 1,
  "session_fingerprint": "0000000000000000000000000000000000000000000000000000000000000000",
  "status_path": "fixture-status.json",
  "frame_boundary": 1,
  "rollback_window_override": 5,
  "gecko_codes_path": "",
  "wii_save_data_bin_path": "",
  "disable_project_plus_music": false,
  "brawl_iso_path": "fixtures/brawl.iso",
  "project_plus_sd_path": "fixtures/sd.raw",
  "sd_card_path": "",
  "game_path": "",
  "game_id": "RSBE01",
  "game_revision": 0,
  "game_platform": 2,
  "replay_output_path": "fixtures/replays",
  "save_replays": true,
  "start_fullscreen": false,
  "gamecube_memory_card_path": "",
  "wii_nand_path": "fixtures/nand"
})json";

constexpr const char* VALID_REPLAY_TICKET = R"json({
  "schema_version": 1,
  "mode": "netplay_replay",
  "match_id": "replay-fixture",
  "game": "Project+",
  "status_path": "fixture-status.json",
  "brawl_iso_path": "fixtures/brawl.iso",
  "project_plus_sd_path": "fixtures/sd.raw",
  "sd_card_path": "",
  "game_path": "",
  "replay_path": "fixtures/replay.npr",
  "aspect_ratio": 0,
  "video_backend": "D3D",
  "internal_resolution": 1,
  "disable_project_plus_music": true,
  "start_fullscreen": false,
  "gamecube_memory_card_path": "",
  "wii_nand_path": "",
  "save_replays": false
})json";
}  // namespace

TEST(ManagedLaunchSchema, AcceptsCanonicalNetplayTicket)
{
  const ValidationResult result = ValidateTicket(Parse(VALID_TICKET));
  EXPECT_TRUE(result) << result.error;
}

TEST(ManagedLaunchSchema, ReplayRequiresExplicitMusicSetting)
{
  EXPECT_TRUE(ValidateTicket(Parse(VALID_REPLAY_TICKET)));

  picojson::value missing_music = Parse(VALID_REPLAY_TICKET);
  missing_music.get<picojson::object>().erase("disable_project_plus_music");
  EXPECT_FALSE(ValidateTicket(missing_music));
}

TEST(ManagedLaunchSchema, NormalizedSummaryDoesNotExposeSecretsOrPaths)
{
  const std::string summary = NormalizedSummary(Parse(VALID_TICKET));
  EXPECT_EQ(summary,
            "schema=1 mode=netplay game=project-plus players=1 seat=1 rollback=5 boundary=1");
  EXPECT_EQ(summary.find("fixture-token"), std::string::npos);
  EXPECT_EQ(summary.find("fixtures/brawl.iso"), std::string::npos);
}

TEST(ManagedLaunchSchema, FormattedLogShowsTicketAndRedactsRendezvousToken)
{
  const std::string formatted = FormatTicketForLog(Parse(VALID_TICKET));
  EXPECT_NE(formatted.find("\n"), std::string::npos);
  EXPECT_EQ(formatted.find("fixture-token"), std::string::npos);

  picojson::value reparsed;
  const std::string error = picojson::parse(reparsed, formatted);
  ASSERT_TRUE(error.empty()) << error << '\n' << formatted;
  ASSERT_TRUE(reparsed.is<picojson::object>()) << formatted;
  const picojson::object& logged = reparsed.get<picojson::object>();
  ASSERT_TRUE(logged.contains("match_id")) << formatted;
  ASSERT_TRUE(logged.contains("brawl_iso_path")) << formatted;
  ASSERT_TRUE(logged.contains("rendezvous_token")) << formatted;
  EXPECT_EQ(logged.at("match_id").get<std::string>(), "match-fixture");
  EXPECT_EQ(logged.at("brawl_iso_path").get<std::string>(), "fixtures/brawl.iso");
  EXPECT_EQ(logged.at("rendezvous_token").get<std::string>(), "<redacted>");
}

TEST(ManagedLaunchSchema, RejectsMissingVersionAndUnknownFields)
{
  EXPECT_FALSE(ValidateTicket(Parse(R"json({"mode":"netplay"})json")));
  std::string polluted = VALID_TICKET;
  polluted.insert(polluted.rfind('}'), R"json(,"legacy_hack":true)json");
  EXPECT_FALSE(ValidateTicket(Parse(polluted)));
}

TEST(ManagedLaunchSchema, RejectsFractionalIntegersAndDuplicateSeats)
{
  std::string fractional = VALID_TICKET;
  const std::string integer_boundary = R"json("frame_boundary": 1)json";
  fractional.replace(fractional.find(integer_boundary), integer_boundary.size(),
                     R"json("frame_boundary": 1.5)json");
  EXPECT_FALSE(ValidateTicket(Parse(fractional)));

  picojson::value duplicate = Parse(VALID_TICKET);
  const std::string player_json = R"json({
    "player_id":"player-two","display_name":"Player Two","seat":1,
    "controller_type":"gamecube","controller_tag":"","controller_profile":""})json";
  duplicate.get<picojson::object>()["players"].get<picojson::array>().push_back(Parse(player_json));
  EXPECT_FALSE(ValidateTicket(duplicate));
}
}  // namespace Core::ManagedLaunch
