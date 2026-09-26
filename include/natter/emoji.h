// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace natter::emoji {

// The Unicode (UTF-8) for a standard Slack shortcode, without colons:
// "smile", "+1", "thumbsup". A Slack skin-tone suffix is understood:
// "wave::skin-tone-3" and "wave:skin-tone-3". Custom workspace emoji are not
// in this table (see Store::customEmoji).
std::optional<std::string> lookup(std::string_view name);

// Apply a Slack skin tone (2 = light .. 6 = dark, as in ":skin-tone-2:");
// returns the plain form when the emoji takes no tone or tone is out of range.
std::optional<std::string> lookup(std::string_view name, int skinTone);

// True when the emoji accepts a skin tone.
bool takesSkinTone(std::string_view name);

// "skin-tone-4" -> 4; 0 when the name is not a tone modifier.
int skinToneFromName(std::string_view name);

// Split "thumbsup::skin-tone-2" (reaction names) into name and tone.
void splitSkinTone(std::string_view full, std::string& name, int& tone);

size_t count();   // number of short names in the table

}  // namespace natter::emoji
