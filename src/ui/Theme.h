// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#pragma once

#include <Font.h>
#include <GraphicsDefs.h>

namespace natter::ui {

// Colours and fonts, derived from the user's Appearance preferences so that
// the app follows light and dark themes.
struct Theme {
	rgb_color background;       // the conversation
	rgb_color text;
	rgb_color muted;            // times, "(edited)"
	rgb_color link;
	rgb_color hover;            // the message under the pointer
	rgb_color codeBackground;
	rgb_color codeBorder;
	rgb_color quoteBar;
	rgb_color mention;          // background of a mention of you
	rgb_color mentionOther;     // background of other mentions
	rgb_color reaction;         // reaction chip
	rgb_color reactionMine;     // a reaction you added
	rgb_color sidebar;
	rgb_color sidebarText;
	rgb_color sidebarMuted;
	rgb_color sidebarSelection;
	rgb_color badge;
	rgb_color badgeText;
	rgb_color presenceActive;
	rgb_color separator;
	bool dark = false;

	BFont plain;
	BFont bold;
	BFont italic;
	BFont boldItalic;
	BFont code;
	BFont small;

	static const Theme& Current();
	static void Reload();       // after B_COLORS_UPDATED / B_FONTS_UPDATED
};

rgb_color Mix(rgb_color a, rgb_color b, float amount);

}  // namespace natter::ui
