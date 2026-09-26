// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#include "TextLayout.h"

#include "ImageCache.h"

#include <Bitmap.h>
#include <View.h>

#include <algorithm>
#include <string_view>

namespace natter::ui {

namespace {

constexpr float kQuoteIndent = 12;
constexpr float kListIndent = 18;
constexpr float kLineGap = 2;

const BFont* FontFor(uint32_t style, const Theme& theme)
{
	if (style & (kStyleCode | kStyleCodeBlock))
		return &theme.code;
	bool bold = style & kStyleBold, italic = style & kStyleItalic;
	if (bold && italic)
		return &theme.boldItalic;
	if (bold)
		return &theme.bold;
	if (italic)
		return &theme.italic;
	return &theme.plain;
}

rgb_color ColorFor(RunKind kind, const Theme& theme)
{
	switch (kind) {
	case RunKind::Link:
	case RunKind::UserMention:
	case RunKind::ChannelMention:
	case RunKind::UserGroupMention:
	case RunKind::Broadcast:
		return theme.link;
	default:
		return theme.text;
	}
}

// The byte length of the UTF-8 character starting at text[index].
size_t CharacterLength(const std::string& text, size_t index)
{
	unsigned char lead = text[index];
	size_t length = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 1;
	return std::min(length, text.size() - index);
}

}  // namespace

void TextLayout::Layout(const FormattedText& text, float width, const EmojiUrl& emojiUrl)
{
	const Theme& theme = Theme::Current();
	fFragments.clear();
	fBoxes.clear();
	fWidth = std::max(40.0f, width);
	float y = 0;
	float lineStart = 0;
	float x = 0;
	size_t lineFirst = 0;
	bool lineQuote = false;
	bool lineCode = false;
	int listIndent = 0;
	bool listEnding = false;

	auto finishLine = [&](bool force) {
		if (!force && lineFirst == fFragments.size())
			return;
		font_height metrics;
		theme.plain.GetHeight(&metrics);
		float ascent = metrics.ascent, descent = metrics.descent + metrics.leading;
		for (size_t index = lineFirst; index < fFragments.size(); index++) {
			font_height fragment;
			fFragments[index].font->GetHeight(&fragment);
			ascent = std::max(ascent, fragment.ascent);
			descent = std::max(descent, fragment.descent + fragment.leading);
			if (fFragments[index].kind == RunKind::CustomEmoji)
				ascent = std::max(ascent, fFragments[index].frame.Height() - descent);
		}
		float height = std::ceil(ascent + descent);
		for (size_t index = lineFirst; index < fFragments.size(); index++) {
			Fragment& fragment = fFragments[index];
			float fragmentWidth = fragment.frame.Width();
			fragment.frame.top = y;
			fragment.frame.bottom = y + height;
			fragment.frame.right = fragment.frame.left + fragmentWidth;
			fragment.baseline = y + ascent;
		}
		if (lineCode)
			fBoxes.push_back({ BRect(0, y - 1, fWidth, y + height + kLineGap), Box::CodeBlock });
		if (lineQuote)
			fBoxes.push_back({ BRect(0, y, 3, y + height + kLineGap), Box::Quote });
		y += height + kLineGap;
		lineFirst = fFragments.size();
		lineQuote = false;
		lineCode = false;
		lineStart = listIndent * kListIndent;
		x = lineStart;
	};

	auto place = [&](const Run& run, const std::string& token, const BFont* font, bool breakable) {
		bool space = token.find_first_not_of(' ') == std::string::npos;
		bool code = run.style & kStyleCodeBlock;
		if (x == lineStart && (run.style & kStyleQuote) && !lineQuote) {
			lineQuote = true;
			lineStart += kQuoteIndent;
			x = lineStart;
		}
		if (code) {
			lineCode = true;
			if (x == lineStart) {
				lineStart += 6;
				x = lineStart;
			}
		}
		if (space && x == lineStart && !code)
			return;
		float tokenWidth = font->StringWidth(token.c_str());
		float limit = fWidth - (code ? 6 : 0);
		if (x + tokenWidth > limit && x > lineStart && !space) {
			finishLine(false);
			if (run.style & kStyleQuote) {
				lineQuote = true;
				lineStart += kQuoteIndent;
				x = lineStart;
			}
			if (code) {
				lineCode = true;
				lineStart += 6;
				x = lineStart;
			}
		}
		// A word longer than the line (a URL) is broken between characters.
		if (breakable && x + tokenWidth > limit && !space) {
			std::string piece;
			for (size_t index = 0; index < token.size();) {
				size_t length = CharacterLength(token, index);
				std::string next = piece + token.substr(index, length);
				if (x + font->StringWidth(next.c_str()) > limit && !piece.empty()) {
					Fragment fragment;
					fragment.text = piece;
					fragment.font = font;
					fragment.color = ColorFor(run.kind, Theme::Current());
					fragment.frame = BRect(x, 0, x + font->StringWidth(piece.c_str()), 0);
					fragment.style = run.style;
					fragment.kind = run.kind;
					fragment.target = run.target;
					fragment.highlight = run.highlight;
					fFragments.push_back(fragment);
					finishLine(false);
					piece = token.substr(index, length);
				} else {
					piece = next;
				}
				index += length;
			}
			if (!piece.empty()) {
				Fragment fragment;
				fragment.text = piece;
				fragment.font = font;
				fragment.color = ColorFor(run.kind, Theme::Current());
				fragment.frame = BRect(x, 0, x + font->StringWidth(piece.c_str()), 0);
				fragment.style = run.style;
				fragment.kind = run.kind;
				fragment.target = run.target;
				fragment.highlight = run.highlight;
				fFragments.push_back(fragment);
				x = fragment.frame.right;
			}
			return;
		}
		Fragment fragment;
		fragment.text = token;
		fragment.font = font;
		fragment.color = ColorFor(run.kind, Theme::Current());
		fragment.frame = BRect(x, 0, x + tokenWidth, 0);
		fragment.style = run.style;
		fragment.kind = run.kind;
		fragment.target = run.target;
		fragment.highlight = run.highlight;
		fFragments.push_back(fragment);
		x += tokenWidth;
	};

	for (const Run& run : text.runs) {
		const BFont* font = FontFor(run.style, theme);
		switch (run.kind) {
		case RunKind::LineBreak:
			finishLine(true);
			// The list goes on only if the next line is another item.
			listEnding = listIndent > 0;
			continue;
		case RunKind::ListMarker:
			listEnding = false;
			listIndent = std::max(1, run.indent + 1);
			if (x > lineStart)
				finishLine(false);
			lineStart = (listIndent - 1) * kListIndent;
			x = lineStart;
			place(run, run.text, font, false);
			lineStart = listIndent * kListIndent;
			x = std::max(x, lineStart);
			continue;
		case RunKind::CustomEmoji: {
			std::string url = emojiUrl ? emojiUrl(run.target) : std::string();
			if (url.empty())
				break;
			float size = std::ceil(theme.plain.Size() * 1.4f);
			if (x + size > fWidth && x > lineStart)
				finishLine(false);
			Fragment fragment;
			fragment.text = run.text;
			fragment.font = font;
			fragment.color = theme.text;
			fragment.frame = BRect(x, 0, x + size, size);
			fragment.kind = run.kind;
			fragment.target = run.target;
			fragment.imageUrl = url;
			fFragments.push_back(fragment);
			x += size + 1;
			continue;
		}
		default:
			break;
		}
		// A line that does not start with an item ends the list.
		if (listEnding && !run.text.empty()) {
			listEnding = false;
			listIndent = 0;
			lineStart = 0;
			x = 0;
		}
		// Split into words and spaces, and on newlines inside the text.
		const std::string value = run.kind == RunKind::Emoji ? DrawableEmoji(run.text) : run.text;
		size_t start = 0;
		while (start < value.size()) {
			if (value[start] == '\n') {
				finishLine(true);
				start++;
				continue;
			}
			bool space = value[start] == ' ';
			size_t end = start;
			while (end < value.size() && value[end] != '\n' && (value[end] == ' ') == space)
				end++;
			place(run, value.substr(start, end - start), font, true);
			start = end;
		}
	}
	finishLine(false);
	fHeight = y > 0 ? y - kLineGap : 0;
}

void TextLayout::Draw(BView* view, BPoint origin, BMessenger imageTarget) const
{
	const Theme& theme = Theme::Current();
	for (const Box& box : fBoxes) {
		BRect frame = box.frame.OffsetByCopy(origin);
		if (box.type == Box::CodeBlock) {
			view->SetHighColor(theme.codeBackground);
			view->FillRect(frame);
		} else {
			view->SetHighColor(theme.quoteBar);
			view->FillRect(frame);
		}
	}
	for (const Fragment& fragment : fFragments) {
		BRect frame = fragment.frame.OffsetByCopy(origin);
		if (!fragment.imageUrl.empty()) {
			const BBitmap* bitmap = ImageCache::Shared().Get(fragment.imageUrl, imageTarget);
			float size = fragment.frame.Width();
			BRect target(frame.left, frame.bottom - size, frame.left + size, frame.bottom);
			if (bitmap) {
				view->SetDrawingMode(B_OP_ALPHA);
				view->DrawBitmap(bitmap, bitmap->Bounds(), target, B_FILTER_BITMAP_BILINEAR);
				view->SetDrawingMode(B_OP_COPY);
			} else {
				view->SetHighColor(theme.muted);
				view->SetFont(&theme.small);
				view->DrawString(fragment.text.c_str(), BPoint(frame.left, origin.y + fragment.baseline));
			}
			continue;
		}
		bool mention = fragment.kind == RunKind::UserMention || fragment.kind == RunKind::Broadcast
			|| fragment.kind == RunKind::UserGroupMention;
		if (fragment.highlight || mention || (fragment.style & kStyleCode)) {
			BRect back = frame;
			back.InsetBy(-1, 1);
			view->SetHighColor(fragment.highlight ? theme.mention
				: (fragment.style & kStyleCode) ? theme.codeBackground : theme.mentionOther);
			view->FillRoundRect(back, 3, 3);
		}
		view->SetFont(fragment.font);
		view->SetHighColor(fragment.color);
		BPoint baseline(frame.left, origin.y + fragment.baseline);
		view->DrawString(fragment.text.c_str(), baseline);
		if (fragment.style & kStyleStrike) {
			float middle = baseline.y - fragment.font->Size() * 0.3f;
			view->StrokeLine(BPoint(frame.left, middle), BPoint(frame.right, middle));
		}
	}
}

const TextLayout::Fragment* TextLayout::FragmentAt(BPoint point) const
{
	for (const Fragment& fragment : fFragments) {
		if (fragment.frame.Contains(point))
			return &fragment;
	}
	return nullptr;
}

std::string DrawableEmoji(const std::string& text)
{
	std::string result;
	result.reserve(text.size());
	for (size_t index = 0; index < text.size();) {
		unsigned char lead = text[index];
		size_t length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
		std::string_view character(text.data() + index, std::min(length, text.size() - index));
		bool drop = character == "\xEF\xB8\x8F"   // U+FE0F variation selector
			|| character == "\xE2\x80\x8D"            // U+200D zero-width joiner
			|| (character.size() == 4 && character.substr(0, 3) == "\xF0\x9F\x8F"
				&& static_cast<unsigned char>(character[3]) >= 0xbb
				&& static_cast<unsigned char>(character[3]) <= 0xbf);   // U+1F3FB..U+1F3FF skin tones
		if (!drop)
			result.append(character);
		index += length;
	}
	return result.empty() ? text : result;
}

}  // namespace natter::ui
