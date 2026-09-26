// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#pragma once

#include "Theme.h"
#include "natter/mrkdwn.h"

#include <Messenger.h>
#include <Rect.h>

#include <functional>
#include <string>
#include <vector>

class BView;

namespace natter::ui {

// Lays out a formatted message (natter::FormattedText) in a given width:
// words wrapped, styles mapped to fonts, code blocks and quotes decorated,
// custom emoji drawn from the image cache. Coordinates are relative to the
// layout's top left corner.
class TextLayout {
public:
	struct Fragment {
		std::string text;
		const BFont* font = nullptr;
		rgb_color color;
		BRect frame;
		float baseline = 0;
		uint32_t style = 0;
		RunKind kind = RunKind::Text;
		std::string target;
		bool highlight = false;
		std::string imageUrl;      // custom emoji
	};

	using EmojiUrl = std::function<std::string(const std::string& name)>;

	void Layout(const FormattedText& text, float width, const EmojiUrl& emojiUrl);
	float Height() const { return fHeight; }
	float Width() const { return fWidth; }
	void Draw(BView* view, BPoint origin, BMessenger imageTarget) const;
	// The fragment at a point, for links and mentions.
	const Fragment* FragmentAt(BPoint point) const;
	bool Empty() const { return fFragments.empty(); }

private:
	struct Box {
		BRect frame;
		enum { CodeBlock, Quote } type;
	};
	std::vector<Fragment> fFragments;
	std::vector<Box> fBoxes;
	float fHeight = 0;
	float fWidth = 0;
};

// Emoji as Haiku can draw them: its text engine does not shape sequences, so
// skin tones, variation selectors and joiners would each draw as a glyph (or
// a missing-glyph box) of their own. They are dropped.
std::string DrawableEmoji(const std::string& text);

}  // namespace natter::ui
