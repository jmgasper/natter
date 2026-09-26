// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#include "MessageView.h"

#include "ImageCache.h"
#include "TextLayout.h"
#include "Messages.h"
#include "natter/emoji.h"
#include "natter/session.h"
#include "natter/util.h"

#include <Bitmap.h>
#include <Cursor.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <ScrollBar.h>
#include <String.h>
#include <Window.h>

#include <algorithm>
#include <cmath>
#include <ctime>

namespace natter::ui {

namespace {

constexpr float kPadding = 10;
constexpr float kAvatar = 36;
constexpr float kGap = 10;
constexpr float kSeparator = 30;
constexpr int64_t kCompactSeconds = 5 * 60;

const char* kQuickReactions[][2] = {
	{ "thumbsup", "Thumbs up" }, { "heart", "Heart" }, { "joy", "Laughing" },
	{ "tada", "Celebrate" }, { "eyes", "Looking" }, { "pray", "Thanks" },
	{ "white_check_mark", "Done" }, { "fire", "Fire" },
};

std::string TimeOfDay(int64_t seconds)
{
	time_t value = static_cast<time_t>(seconds);
	struct tm local;
	localtime_r(&value, &local);
	char text[16];
	strftime(text, sizeof(text), "%H:%M", &local);
	return text;
}

std::string DayLabel(int64_t seconds)
{
	time_t value = static_cast<time_t>(seconds);
	struct tm day;
	localtime_r(&value, &day);
	time_t now = time(nullptr);
	struct tm today;
	localtime_r(&now, &today);
	if (day.tm_year == today.tm_year && day.tm_yday == today.tm_yday)
		return "Today";
	time_t yesterdayTime = now - 24 * 60 * 60;
	struct tm yesterday;
	localtime_r(&yesterdayTime, &yesterday);
	if (day.tm_year == yesterday.tm_year && day.tm_yday == yesterday.tm_yday)
		return "Yesterday";
	char text[64];
	strftime(text, sizeof(text), day.tm_year == today.tm_year ? "%A, %B %e" : "%A, %B %e, %Y", &day);
	return text;
}

bool SameDay(int64_t a, int64_t b)
{
	time_t first = static_cast<time_t>(a), second = static_cast<time_t>(b);
	struct tm x, y;
	localtime_r(&first, &x);
	localtime_r(&second, &y);
	return x.tm_year == y.tm_year && x.tm_yday == y.tm_yday;
}

std::string SizeText(int64_t bytes)
{
	char text[32];
	if (bytes >= 1024 * 1024)
		snprintf(text, sizeof(text), "%.1f MB", bytes / (1024.0 * 1024.0));
	else if (bytes >= 1024)
		snprintf(text, sizeof(text), "%.0f KB", bytes / 1024.0);
	else
		snprintf(text, sizeof(text), "%lld bytes", static_cast<long long>(bytes));
	return text;
}

// Unicode for a reaction name, or empty for a custom emoji.
std::string ReactionGlyph(const std::string& name)
{
	std::string base;
	int tone = 0;
	emoji::splitSkinTone(name, base, tone);
	auto found = tone ? emoji::lookup(base, tone) : emoji::lookup(base);
	return found ? DrawableEmoji(*found) : std::string();
}

}  // namespace

MessageView::MessageView(Session* session, bool thread)
	:
	BView(thread ? "thread" : "conversation", B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE),
	fSession(session),
	fThreadMode(thread)
{
	SetViewColor(B_TRANSPARENT_COLOR);
	// Without a layout a view's minimum is its current size, which would
	// keep the window from making room for the thread.
	SetExplicitMinSize(BSize(thread ? 200 : 260, 100));
	SetExplicitPreferredSize(BSize(thread ? 300 : 560, 400));
}

void MessageView::AttachedToWindow()
{
	BView::AttachedToWindow();
	// No B_POINTER_EVENTS mask: with it the view also gets the clicks that
	// choose items in its own context menu, and acts on what lies beneath.
}

float MessageView::TextLeft() const
{
	return kPadding + kAvatar + kGap;
}

float MessageView::TextWidth() const
{
	return std::max(60.0f, Bounds().Width() - TextLeft() - kPadding - 4);
}

void MessageView::SetConversation(const std::string& channel, const std::string& threadTs)
{
	fChannel = channel;
	fThread = threadTs;
	fItems.clear();
	fHover = -1;
	fRevealed.clear();
	fMoreOlder = !fThreadMode;   // a thread loads whole
	fLoadingOlder = false;
	fAtEnd = true;
	Rebuild(false);
	ScrollToEnd();
}

void MessageView::SetLoadingOlder(bool loading, bool more)
{
	fLoadingOlder = loading;
	fMoreOlder = more;
}

void MessageView::Reload()
{
	Rebuild(true);
}

void MessageView::Rebuild(bool keepPosition)
{
	// Remember what is at the top of the view, to keep it there when older
	// messages arrive above it.
	std::string anchorTs;
	float anchorOffset = 0;
	float scroll = Bounds().top;
	bool atEnd = fContentHeight - Bounds().Height() - scroll < 8 || fItems.empty();
	if (keepPosition && !atEnd) {
		for (const Item& item : fItems) {
			if (item.top + item.height > scroll) {
				anchorTs = item.message.ts;
				anchorOffset = scroll - item.top;
				break;
			}
		}
	}

	std::vector<Message> messages;
	if (!fChannel.empty()) {
		messages = fThreadMode ? fSession->store().thread(fChannel, fThread)
			: fSession->store().messages(fChannel, 0);
	}
	Store& store = fSession->store();
	std::string self = store.selfUserId();
	std::vector<Item> items;
	items.reserve(messages.size());
	const Item* previous = nullptr;
	for (Message& message : messages) {
		if (message.hidden)
			continue;
		if (!fThreadMode && message.isThreadReply() && !message.isBroadcastReply())
			continue;
		Item item;
		int64_t seconds = tsSeconds(message.ts);
		item.time = TimeOfDay(seconds);
		if (auto user = store.user(message.user)) {
			item.author = user->bestName();
			item.avatarUrl = user->avatarUrl(72);
		} else if (!message.username.empty()) {
			item.author = message.username;
		} else if (message.botProfile.is_object()) {
			item.author = jStr(message.botProfile, "name", "App");
		} else {
			item.author = message.user.empty() ? "Slack" : message.user;
		}
		if (item.avatarUrl.empty() && message.botProfile.is_object())
			item.avatarUrl = jStr(jObj(message.botProfile, "icons"), "image_72");
		item.mine = !self.empty() && message.user == self;
		if (!previous || !SameDay(tsSeconds(previous->message.ts), seconds))
			item.dayLabel = DayLabel(seconds);
		item.compact = previous && item.dayLabel.empty()
			&& previous->message.user == message.user && previous->message.username == message.username
			&& seconds - tsSeconds(previous->message.ts) < kCompactSeconds
			&& (fThreadMode || previous->message.replyCount == 0);
		item.message = std::move(message);
		items.push_back(std::move(item));
		previous = &items.back();
	}
	fItems = std::move(items);
	fHover = -1;
	LayoutItems();
	// The scroll bar first: it clamps scrolling to its range.
	UpdateScrollBar();
	if (keepPosition && atEnd) {
		ScrollToEnd();
	} else if (!anchorTs.empty()) {
		for (const Item& item : fItems) {
			if (item.message.ts == anchorTs) {
				BView::ScrollTo(BPoint(0, std::max(0.0f, item.top + anchorOffset)));
				break;
			}
		}
	}
	UpdateScrollBar();
	Invalidate();
}

void MessageView::LayoutItems()
{
	const Theme& theme = Theme::Current();
	FormatContext context = fSession->formatContext();
	float width = TextWidth();
	fLaidOutWidth = Bounds().Width();
	font_height plainHeight, boldHeight, smallHeight;
	theme.plain.GetHeight(&plainHeight);
	theme.bold.GetHeight(&boldHeight);
	theme.small.GetHeight(&smallHeight);
	float nameLine = std::ceil(boldHeight.ascent + boldHeight.descent + boldHeight.leading) + 2;
	float smallLine = std::ceil(smallHeight.ascent + smallHeight.descent + smallHeight.leading);
	auto emojiUrl = [this](const std::string& name) { return fSession->store().customEmojiUrl(name); };

	float y = kPadding;
	for (Item& item : fItems) {
		if (!item.dayLabel.empty())
			y += kSeparator;
		item.top = y;
		float cursor = y + 4;
		if (!item.compact)
			cursor += nameLine;
		item.textTop = cursor;
		FormattedText text = formatMessage(item.message, context);
		item.layout.Layout(text, width, emojiUrl);
		cursor += item.layout.Height();
		if (item.message.edited && !item.layout.Empty())
			cursor += 0;
		// Attachments: link previews and bot cards, as a quoted block.
		item.attachments.clear();
		if (item.message.attachments.is_array()) {
			for (const json& attachment : item.message.attachments) {
				std::string body;
				std::string title = jStr(attachment, "title");
				std::string link = jStr(attachment, "title_link");
				std::string pretext = jStr(attachment, "pretext");
				std::string value = jStr(attachment, "text", jStr(attachment, "fallback"));
				if (!pretext.empty())
					body += pretext + "\n";
				if (!title.empty())
					body += link.empty() ? "*" + title + "*\n" : "<" + link + "|" + title + ">\n";
				body += value;
				if (body.empty())
					continue;
				TextLayout layout;
				layout.Layout(formatMrkdwn(body, context), width - 12, emojiUrl);
				cursor += 4;
				BRect frame(TextLeft(), cursor, TextLeft() + width, cursor + layout.Height());
				item.attachments.emplace_back(frame, std::move(layout));
				cursor = frame.bottom + 2;
			}
		}
		// Files: image thumbnails, other files as a line each.
		item.files.clear();
		item.images.clear();
		for (const File& file : item.message.files) {
			if (file.mode == "tombstone")
				continue;
			std::string thumb = file.mimetype.rfind("image/", 0) == 0 ? file.thumbUrl(360) : std::string();
			if (!thumb.empty()) {
				float w = file.originalWidth > 0 ? file.originalWidth : 360;
				float h = file.originalHeight > 0 ? file.originalHeight : 240;
				float scale = std::min({ 1.0f, 360.0f / w, 240.0f / h, width / w });
				BRect frame(TextLeft(), cursor + 4, TextLeft() + std::floor(w * scale), cursor + 4 + std::floor(h * scale));
				item.images.emplace_back(frame, thumb);
				item.files.emplace_back(frame, file);
				cursor = frame.bottom + 2;
			} else {
				BRect frame(TextLeft(), cursor + 4, TextLeft() + width, cursor + 4 + smallLine + 8);
				item.files.emplace_back(frame, file);
				cursor = frame.bottom;
			}
		}
		// Reactions as chips, wrapped.
		item.reactions.clear();
		if (!item.message.reactions.empty()) {
			float x = TextLeft();
			float chipHeight = std::ceil(plainHeight.ascent + plainHeight.descent) + 6;
			float rowTop = cursor + 6;
			for (const Reaction& reaction : item.message.reactions) {
				std::string glyph = ReactionGlyph(reaction.name);
				std::string label = (glyph.empty() ? ":" + reaction.name + ":" : glyph) + " " + std::to_string(reaction.count);
				float chipWidth = theme.plain.StringWidth(label.c_str()) + 14;
				if (x + chipWidth > TextLeft() + width && x > TextLeft()) {
					x = TextLeft();
					rowTop += chipHeight + 4;
				}
				item.reactions.emplace_back(BRect(x, rowTop, x + chipWidth, rowTop + chipHeight), reaction.name);
				x += chipWidth + 6;
			}
			cursor = rowTop + chipHeight;
		}
		// "3 replies" under a thread's first message (not inside the thread).
		item.replies = BRect();
		if (!fThreadMode && item.message.replyCount > 0) {
			cursor += 4;
			item.replies = BRect(TextLeft(), cursor, TextLeft() + 220, cursor + smallLine + 2);
			cursor = item.replies.bottom;
		}
		item.height = cursor + 4 - y;
		if (!item.compact)
			item.height = std::max(item.height, kAvatar + 8);
		y += item.height;
	}
	fContentHeight = y + kPadding;
}

void MessageView::UpdateScrollBar()
{
	BScrollBar* bar = ScrollBar(B_VERTICAL);
	if (!bar)
		return;
	float visible = Bounds().Height();
	float range = std::max(0.0f, fContentHeight - visible);
	bar->SetRange(0, range);
	bar->SetProportion(fContentHeight > 0 ? std::min(1.0f, visible / fContentHeight) : 1.0f);
	bar->SetSteps(24, std::max(24.0f, visible - 40));
}

bool MessageView::Reveal(const std::string& ts)
{
	for (const Item& item : fItems) {
		if (item.message.ts != ts)
			continue;
		fRevealed = ts;
		UpdateScrollBar();
		// A third of the way down, with what led up to it above.
		float range = std::max(0.0f, fContentHeight - Bounds().Height());
		ScrollTo(BPoint(0, std::clamp(item.top - Bounds().Height() / 3, 0.0f, range)));
		Invalidate();
		return true;
	}
	return false;
}

void MessageView::ScrollToEnd()
{
	UpdateScrollBar();
	float range = std::max(0.0f, fContentHeight - Bounds().Height());
	BView::ScrollTo(BPoint(0, range));
	fAtEnd = true;
}

void MessageView::ScrollTo(BPoint where)
{
	BView::ScrollTo(where);
	// While the layout resizes the view, its scroll bar may scroll it before
	// FrameResized() lays the messages out again; that is not the reader
	// leaving the end.
	bool resizing = std::fabs(Bounds().Width() - fLaidOutWidth) > 0.5f;
	if (!resizing)
		fAtEnd = fContentHeight - Bounds().Height() - where.y < 8;
	if (!resizing && where.y < 300 && !fLoadingOlder && fMoreOlder && !fChannel.empty() && !fItems.empty()) {
		fLoadingOlder = true;
		BMessage message(kLoadOlder);
		message.AddString("channel", fChannel.c_str());
		if (fThreadMode)
			message.AddString("thread", fThread.c_str());
		SendToWindow(message);
	}
}

void MessageView::FrameResized(float width, float height)
{
	BView::FrameResized(width, height);
	bool atEnd = fAtEnd;
	if (std::fabs(Bounds().Width() - fLaidOutWidth) > 0.5f)
		LayoutItems();
	UpdateScrollBar();
	if (atEnd)
		ScrollToEnd();
	Invalidate();
}

void MessageView::Draw(BRect updateRect)
{
	const Theme& theme = Theme::Current();
	SetHighColor(theme.background);
	FillRect(updateRect);
	if (fItems.empty()) {
		SetFont(&theme.plain);
		SetHighColor(theme.muted);
		const char* text = fChannel.empty() ? "Choose a conversation." : "No messages yet.";
		float width = StringWidth(text);
		BRect bounds = Bounds();
		DrawString(text, BPoint(bounds.left + (bounds.Width() - width) / 2, bounds.top + bounds.Height() / 2));
		return;
	}
	for (size_t index = 0; index < fItems.size(); index++) {
		Item& item = fItems[index];
		float separatorTop = item.dayLabel.empty() ? item.top : item.top - kSeparator;
		if (item.top + item.height < updateRect.top || separatorTop > updateRect.bottom)
			continue;
		if (!item.dayLabel.empty()) {
			float middle = item.top - kSeparator / 2;
			SetHighColor(theme.separator);
			StrokeLine(BPoint(kPadding, middle), BPoint(Bounds().right - kPadding, middle));
			SetFont(&theme.bold);
			float width = StringWidth(item.dayLabel.c_str()) + 20;
			BRect pill(Bounds().Width() / 2 - width / 2, middle - 10, Bounds().Width() / 2 + width / 2, middle + 10);
			SetHighColor(theme.background);
			FillRoundRect(pill, 10, 10);
			SetHighColor(theme.separator);
			StrokeRoundRect(pill, 10, 10);
			SetHighColor(theme.text);
			DrawString(item.dayLabel.c_str(), BPoint(pill.left + 10, middle + 4));
		}
		if (!fRevealed.empty() && item.message.ts == fRevealed) {
			SetHighColor(Mix(theme.background, theme.mention, 0.6f));
			FillRect(BRect(0, item.top, Bounds().right, item.top + item.height));
		} else if (static_cast<int>(index) == fHover) {
			SetHighColor(theme.hover);
			FillRect(BRect(0, item.top, Bounds().right, item.top + item.height));
		}
		DrawItem(item, updateRect);
	}
	if (fLoadingOlder && fMoreOlder) {
		SetFont(&theme.small);
		SetHighColor(theme.muted);
		DrawString("Loading older messages…", BPoint(kPadding, kPadding + 8));
	}
}

void MessageView::DrawItem(Item& item, BRect updateRect)
{
	const Theme& theme = Theme::Current();
	BMessenger self(this);
	if (!item.compact) {
		BRect avatar(kPadding, item.top + 6, kPadding + kAvatar, item.top + 6 + kAvatar);
		const BBitmap* bitmap = ImageCache::Shared().Get(item.avatarUrl, self);
		if (bitmap) {
			SetDrawingMode(B_OP_ALPHA);
			DrawBitmap(bitmap, bitmap->Bounds(), avatar, B_FILTER_BITMAP_BILINEAR);
			SetDrawingMode(B_OP_COPY);
		} else {
			SetHighColor(Mix(theme.background, theme.link, 0.35f));
			FillRoundRect(avatar, 6, 6);
			SetHighColor(theme.background);
			SetFont(&theme.bold);
			std::string initial = item.author.empty() ? "?" : item.author.substr(0, 1);
			float width = StringWidth(initial.c_str());
			DrawString(initial.c_str(), BPoint(avatar.left + (kAvatar - width) / 2, avatar.top + kAvatar / 2 + 5));
		}
		font_height metrics;
		theme.bold.GetHeight(&metrics);
		float baseline = item.top + 4 + std::ceil(metrics.ascent);
		SetFont(&theme.bold);
		SetHighColor(theme.text);
		DrawString(item.author.c_str(), BPoint(TextLeft(), baseline));
		float x = TextLeft() + StringWidth(item.author.c_str()) + 8;
		SetFont(&theme.small);
		SetHighColor(theme.muted);
		DrawString(item.time.c_str(), BPoint(x, baseline));
		if (item.message.edited) {
			x += StringWidth(item.time.c_str()) + 6;
			DrawString("(edited)", BPoint(x, baseline));
		}
	} else if (static_cast<int>(&item - fItems.data()) == fHover) {
		SetFont(&theme.small);
		SetHighColor(theme.muted);
		font_height metrics;
		theme.plain.GetHeight(&metrics);
		DrawString(item.time.c_str(), BPoint(kPadding, item.textTop + std::ceil(metrics.ascent)));
	}
	item.layout.Draw(this, BPoint(TextLeft(), item.textTop), self);
	for (auto& [frame, layout] : item.attachments) {
		SetHighColor(theme.quoteBar);
		FillRect(BRect(frame.left, frame.top, frame.left + 3, frame.bottom));
		layout.Draw(this, BPoint(frame.left + 12, frame.top), self);
	}
	for (auto& [frame, url] : item.images) {
		const BBitmap* bitmap = ImageCache::Shared().Get(url, self);
		if (bitmap) {
			DrawBitmap(bitmap, bitmap->Bounds(), frame, B_FILTER_BITMAP_BILINEAR);
		} else {
			// Loading, or it could not be fetched: the file's name holds the place.
			SetHighColor(theme.codeBackground);
			FillRoundRect(frame, 4, 4);
			auto file = std::find_if(item.files.begin(), item.files.end(), [&](const auto& entry) { return entry.first == frame; });
			if (file != item.files.end()) {
				BString name((file->second.title.empty() ? file->second.name : file->second.title).c_str());
				SetFont(&theme.small);
				theme.small.TruncateString(&name, B_TRUNCATE_MIDDLE, frame.Width() - 16);
				font_height metrics;
				theme.small.GetHeight(&metrics);
				SetHighColor(theme.muted);
				DrawString(name.String(), BPoint(frame.left + (frame.Width() - theme.small.StringWidth(name.String())) / 2,
					frame.top + (frame.Height() + metrics.ascent - metrics.descent) / 2));
			}
		}
		SetHighColor(theme.codeBorder);
		StrokeRoundRect(frame, 4, 4);
	}
	for (auto& [frame, file] : item.files) {
		if (std::any_of(item.images.begin(), item.images.end(), [&](const auto& image) { return image.first == frame; }))
			continue;
		SetHighColor(theme.codeBackground);
		FillRoundRect(frame, 4, 4);
		SetHighColor(theme.codeBorder);
		StrokeRoundRect(frame, 4, 4);
		SetFont(&theme.plain);
		SetHighColor(theme.link);
		std::string label = "📎 " + (file.title.empty() ? file.name : file.title);
		font_height metrics;
		theme.plain.GetHeight(&metrics);
		float baseline = frame.top + 4 + std::ceil(metrics.ascent);
		DrawString(label.c_str(), BPoint(frame.left + 8, baseline));
		SetFont(&theme.small);
		SetHighColor(theme.muted);
		std::string detail = (file.prettyType.empty() ? file.filetype : file.prettyType)
			+ (file.size > 0 ? " · " + SizeText(file.size) : std::string());
		SetFont(&theme.plain);
		float labelWidth = StringWidth(label.c_str());
		SetFont(&theme.small);
		DrawString(detail.c_str(), BPoint(frame.left + 16 + labelWidth, baseline));
	}
	std::string me = fSession->store().selfUserId();
	for (auto& [frame, name] : item.reactions) {
		const Reaction* reaction = nullptr;
		for (const Reaction& candidate : item.message.reactions) {
			if (candidate.name == name)
				reaction = &candidate;
		}
		bool mine = reaction && std::find(reaction->users.begin(), reaction->users.end(), me) != reaction->users.end();
		SetHighColor(mine ? theme.reactionMine : theme.reaction);
		FillRoundRect(frame, 8, 8);
		SetHighColor(mine ? theme.link : theme.codeBorder);
		StrokeRoundRect(frame, 8, 8);
		std::string glyph = ReactionGlyph(name);
		std::string label = (glyph.empty() ? ":" + name + ":" : glyph) + " " + std::to_string(reaction ? reaction->count : 0);
		SetFont(&theme.plain);
		SetHighColor(theme.text);
		font_height metrics;
		theme.plain.GetHeight(&metrics);
		DrawString(label.c_str(), BPoint(frame.left + 7, frame.top + 3 + std::ceil(metrics.ascent)));
	}
	if (item.replies.IsValid()) {
		SetFont(&theme.bold);
		SetHighColor(theme.link);
		font_height metrics;
		theme.small.GetHeight(&metrics);
		std::string label = std::to_string(item.message.replyCount)
			+ (item.message.replyCount == 1 ? " reply" : " replies");
		BFont font = theme.bold;
		font.SetSize(theme.small.Size());
		SetFont(&font);
		DrawString(label.c_str(), BPoint(item.replies.left, item.replies.top + std::ceil(metrics.ascent)));
		if (!item.message.latestReply.empty()) {
			float x = item.replies.left + StringWidth(label.c_str()) + 8;
			SetFont(&theme.small);
			SetHighColor(theme.muted);
			std::string last = "Last reply " + TimeOfDay(tsSeconds(item.message.latestReply));
			DrawString(last.c_str(), BPoint(x, item.replies.top + std::ceil(metrics.ascent)));
		}
	}
}

int MessageView::ItemAt(BPoint where) const
{
	for (size_t index = 0; index < fItems.size(); index++) {
		const Item& item = fItems[index];
		if (where.y >= item.top && where.y < item.top + item.height)
			return static_cast<int>(index);
	}
	return -1;
}

void MessageView::SendToWindow(BMessage& message)
{
	if (Window())
		Window()->PostMessage(&message);
}

void MessageView::MouseDown(BPoint where)
{
	MakeFocus(true);
	int index = ItemAt(where);
	if (index < 0)
		return;
	uint32 buttons = 0;
	if (BMessage* current = Window()->CurrentMessage())
		current->FindInt32("buttons", reinterpret_cast<int32*>(&buttons));
	if (buttons & B_SECONDARY_MOUSE_BUTTON) {
		ShowContextMenu(index, where);
		return;
	}
	Item& item = fItems[index];
	if (item.replies.IsValid() && item.replies.Contains(where)) {
		BMessage message(kOpenThread);
		message.AddString("channel", fChannel.c_str());
		message.AddString("thread", item.message.ts.c_str());
		SendToWindow(message);
		return;
	}
	for (auto& [frame, name] : item.reactions) {
		if (frame.Contains(where)) {
			BMessage message(kToggleReaction);
			message.AddString("channel", fChannel.c_str());
			message.AddString("ts", item.message.ts.c_str());
			message.AddString("name", name.c_str());
			SendToWindow(message);
			return;
		}
	}
	for (auto& [frame, file] : item.files) {
		if (frame.Contains(where)) {
			BMessage message(kDownloadFile);
			message.AddString("url", (file.urlPrivateDownload.empty() ? file.urlPrivate : file.urlPrivateDownload).c_str());
			message.AddString("name", file.name.c_str());
			SendToWindow(message);
			return;
		}
	}
	const TextLayout::Fragment* fragment = item.layout.FragmentAt(where - BPoint(TextLeft(), item.textTop));
	for (auto& [frame, layout] : item.attachments) {
		if (!fragment && frame.Contains(where))
			fragment = layout.FragmentAt(where - BPoint(frame.left + 12, frame.top));
	}
	if (!fragment)
		return;
	if (fragment->kind == RunKind::Link) {
		BMessage message(kOpenLink);
		message.AddString("url", fragment->target.c_str());
		SendToWindow(message);
	} else if (fragment->kind == RunKind::ChannelMention) {
		BMessage message(kOpenChannel);
		message.AddString("channel", fragment->target.c_str());
		SendToWindow(message);
	} else if (fragment->kind == RunKind::UserMention) {
		BMessage message(kOpenUser);
		message.AddString("user", fragment->target.c_str());
		SendToWindow(message);
	}
}

void MessageView::MouseMoved(BPoint where, uint32 transit, const BMessage* dragged)
{
	int hover = transit == B_EXITED_VIEW ? -1 : ItemAt(where);
	if (hover != fHover) {
		auto invalidate = [this](int index) {
			if (index >= 0 && index < static_cast<int>(fItems.size()))
				Invalidate(BRect(0, fItems[index].top, Bounds().right, fItems[index].top + fItems[index].height));
		};
		invalidate(fHover);
		fHover = hover;
		invalidate(fHover);
	}
	bool clickable = false;
	if (hover >= 0) {
		const Item& item = fItems[hover];
		const TextLayout::Fragment* fragment = item.layout.FragmentAt(where - BPoint(TextLeft(), item.textTop));
		clickable = (fragment && fragment->kind != RunKind::Text && fragment->kind != RunKind::Emoji
				&& fragment->kind != RunKind::LineBreak)
			|| (item.replies.IsValid() && item.replies.Contains(where));
		for (auto& reaction : item.reactions)
			clickable |= reaction.first.Contains(where);
		for (auto& file : item.files)
			clickable |= file.first.Contains(where);
	}
	BCursor cursor(clickable ? B_CURSOR_ID_FOLLOW_LINK : B_CURSOR_ID_SYSTEM_DEFAULT);
	SetViewCursor(&cursor);
}

void MessageView::ShowContextMenu(int index, BPoint where)
{
	const Item& item = fItems[index];
	auto* menu = new BPopUpMenu("message", false, false);
	auto add = [&](const char* label, uint32 what, const char* extraName = nullptr, const std::string& extra = {}) {
		auto* message = new BMessage(what);
		message->AddString("channel", fChannel.c_str());
		message->AddString("ts", item.message.ts.c_str());
		if (extraName)
			message->AddString(extraName, extra.c_str());
		auto* menuItem = new BMenuItem(label, message);
		menu->AddItem(menuItem);
		return menuItem;
	};
	if (!fThreadMode) {
		auto* message = new BMessage(kOpenThread);
		message->AddString("channel", fChannel.c_str());
		message->AddString("thread", (item.message.threadTs.empty() ? item.message.ts : item.message.threadTs).c_str());
		menu->AddItem(new BMenuItem("Reply in thread", message));
	}
	auto* reactions = new BMenu("Add reaction");
	for (auto& [name, label] : kQuickReactions) {
		auto* message = new BMessage(kToggleReaction);
		message->AddString("channel", fChannel.c_str());
		message->AddString("ts", item.message.ts.c_str());
		message->AddString("name", name);
		std::string text = ReactionGlyph(name) + "  " + label;
		reactions->AddItem(new BMenuItem(text.c_str(), message));
	}
	menu->AddItem(reactions);
	menu->AddSeparatorItem();
	add("Copy text", kCopyText, "text", formatMessage(item.message, fSession->formatContext()).plainText());
	const TextLayout::Fragment* fragment = item.layout.FragmentAt(where - BPoint(TextLeft(), item.textTop));
	if (fragment && fragment->kind == RunKind::Link)
		add("Copy link", kCopyText, "text", fragment->target);
	if (item.mine) {
		menu->AddSeparatorItem();
		add("Edit message…", kEditMessage);
		add("Delete message", kDeleteMessage);
	}
	menu->SetTargetForItems(Window());
	reactions->SetTargetForItems(Window());
	menu->SetAsyncAutoDestruct(true);
	menu->Go(ConvertToScreen(where), true, false, true);
}

void MessageView::MessageReceived(BMessage* message)
{
	switch (message->what) {
	case kImageLoaded:
		Invalidate();
		break;
	default:
		BView::MessageReceived(message);
	}
}

}  // namespace natter::ui
