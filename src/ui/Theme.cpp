// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#include "Theme.h"

#include <InterfaceDefs.h>

namespace natter::ui {

rgb_color Mix(rgb_color a, rgb_color b, float amount)
{
	auto blend = [amount](uint8 x, uint8 y) { return static_cast<uint8>(x + (y - x) * amount); };
	return { blend(a.red, b.red), blend(a.green, b.green), blend(a.blue, b.blue), 255 };
}

static Theme sTheme;
static bool sLoaded = false;

static bool IsDark(rgb_color color)
{
	return color.red * 0.299 + color.green * 0.587 + color.blue * 0.114 < 128;
}

void Theme::Reload()
{
	Theme& theme = sTheme;
	theme.background = ui_color(B_DOCUMENT_BACKGROUND_COLOR);
	theme.text = ui_color(B_DOCUMENT_TEXT_COLOR);
	theme.dark = IsDark(theme.background);
	theme.muted = Mix(theme.text, theme.background, 0.45f);
	theme.link = ui_color(B_LINK_TEXT_COLOR);
	if (theme.link == theme.text || (theme.link.red == 0 && theme.link.green == 0 && theme.link.blue == 0))
		theme.link = theme.dark ? rgb_color{ 110, 170, 255, 255 } : rgb_color{ 18, 100, 163, 255 };
	theme.hover = Mix(theme.background, theme.text, 0.04f);
	theme.codeBackground = Mix(theme.background, theme.text, 0.06f);
	theme.codeBorder = Mix(theme.background, theme.text, 0.18f);
	theme.quoteBar = Mix(theme.background, theme.text, 0.3f);
	theme.mention = theme.dark ? rgb_color{ 92, 74, 26, 255 } : rgb_color{ 255, 242, 196, 255 };
	theme.mentionOther = theme.dark ? rgb_color{ 30, 60, 92, 255 } : rgb_color{ 226, 238, 250, 255 };
	theme.reaction = Mix(theme.background, theme.text, 0.07f);
	theme.reactionMine = theme.dark ? rgb_color{ 28, 64, 104, 255 } : rgb_color{ 218, 234, 252, 255 };
	// The sidebar takes the control colours, as panels do in Haiku apps.
	theme.sidebar = ui_color(B_PANEL_BACKGROUND_COLOR);
	theme.sidebarText = ui_color(B_PANEL_TEXT_COLOR);
	theme.sidebarMuted = Mix(theme.sidebarText, theme.sidebar, 0.4f);
	theme.sidebarSelection = ui_color(B_LIST_SELECTED_BACKGROUND_COLOR);
	theme.badge = { 224, 30, 90, 255 };
	theme.badgeText = { 255, 255, 255, 255 };
	theme.presenceActive = { 43, 172, 118, 255 };
	theme.separator = Mix(theme.background, theme.text, 0.12f);

	theme.plain = *be_plain_font;
	theme.bold = *be_bold_font;
	theme.bold.SetSize(theme.plain.Size());
	theme.italic = theme.plain;
	theme.italic.SetFace(B_ITALIC_FACE);
	theme.boldItalic = theme.bold;
	theme.boldItalic.SetFace(B_BOLD_FACE | B_ITALIC_FACE);
	theme.code = *be_fixed_font;
	theme.code.SetSize(theme.plain.Size() * 0.92f);
	theme.small = theme.plain;
	theme.small.SetSize(theme.plain.Size() * 0.85f);
	sLoaded = true;
}

const Theme& Theme::Current()
{
	if (!sLoaded)
		Reload();
	return sTheme;
}

}  // namespace natter::ui
