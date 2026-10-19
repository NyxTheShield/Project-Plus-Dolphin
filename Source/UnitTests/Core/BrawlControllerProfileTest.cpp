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
}  // namespace Rollback
