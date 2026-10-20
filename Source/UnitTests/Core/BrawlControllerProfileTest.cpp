// Copyright 2026 Brawlback Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/BrawlControllerProfile.h"

#include <gtest/gtest.h>

namespace Rollback
{
TEST(BrawlControllerProfile, EncodesBrawlKeyboardCharacters)
{
  EXPECT_EQ(EncodeBrawlTag("@()^;"), u"＠（）＾；");
  EXPECT_EQ(EncodeBrawlTag(":!?&%"), u"：！？＆％");
  EXPECT_EQ(EncodeBrawlTag(",./~"), u"，．／～");
  EXPECT_EQ(EncodeBrawlTag("Aaz09"), u"Ａａｚ０９");
  EXPECT_EQ(EncodeBrawlTag("'·£"), u"′・￡");
  EXPECT_EQ(EncodeBrawlTag("がぱヴ"), u"がぱヴ");
  EXPECT_EQ(EncodeBrawlTag("かなカナ"), u"かなカナ");
  EXPECT_EQ(EncodeBrawlTag("Œß€←"), u"Œß€←");
}

TEST(BrawlControllerProfile, RejectsCharactersAbsentFromBrawlsKeyboard)
{
  EXPECT_FALSE(EncodeBrawlTag("|").has_value());
  EXPECT_FALSE(EncodeBrawlTag("\\").has_value());
  EXPECT_FALSE(EncodeBrawlTag("[]{}").has_value());
  EXPECT_FALSE(EncodeBrawlTag("abcdef").has_value());
  EXPECT_FALSE(EncodeBrawlTag("😀").has_value());
  EXPECT_FALSE(EncodeBrawlTag("か\xCC\x99").has_value());
}

TEST(BrawlControllerProfile, ReplayMetadataRoundTrip)
{
  BrawlControllerProfile first;
  first.tag = *EncodeBrawlTag("Nyx");
  for (std::size_t i = 0; i < first.controls.size(); ++i)
    first.controls[i] = static_cast<u8>(i);
  std::vector<BrawlControllerProfile> profiles{first, BrawlControllerProfile{}};

  const std::vector<u8> bytes = SerializeBrawlControllerProfiles(profiles);
  const auto decoded = DeserializeBrawlControllerProfiles(bytes);
  ASSERT_TRUE(decoded.has_value());
  ASSERT_EQ(decoded->size(), profiles.size());
  EXPECT_EQ((*decoded)[0].tag, profiles[0].tag);
  EXPECT_EQ((*decoded)[0].controls, profiles[0].controls);
  EXPECT_FALSE((*decoded)[1].Enabled());
}

TEST(BrawlControllerProfile, RejectsMalformedReplayMetadata)
{
  EXPECT_FALSE(DeserializeBrawlControllerProfiles({}).has_value());
  EXPECT_FALSE(DeserializeBrawlControllerProfiles(std::array<u8, 2>{5, 0}).has_value());
  EXPECT_FALSE(DeserializeBrawlControllerProfiles(std::array<u8, 2>{1, 6}).has_value());
}
}  // namespace Rollback
