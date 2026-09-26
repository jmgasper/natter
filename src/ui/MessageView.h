// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#pragma once

#include "TextLayout.h"
#include "natter/models.h"

#include <View.h>

#include <string>
#include <utility>
#include <vector>

namespace natter {
class Session;
}

namespace natter::ui {

// A conversation, or one thread of it, drawn from the session's store.
// Messages are laid out for the current width; only the visible ones are
// drawn. The view is the target of a vertical BScrollBar it manages itself.
// Clicks on links, mentions, reactions, files and reply counts, and the
// context menu, become messages to the window.
class MessageView : public BView {
public:
	MessageView(Session* session, bool thread);

	void SetConversation(const std::string& channel, const std::string& threadTs = {});
	const std::string& Channel() const { return fChannel; }
	const std::string& ThreadTs() const { return fThread; }
	// Reads the conversation from the store again.
	void Reload();
	void ScrollToEnd();
	void SetLoadingOlder(bool loading, bool more);

	void AttachedToWindow() override;
	void Draw(BRect updateRect) override;
	void FrameResized(float width, float height) override;
	void MouseDown(BPoint where) override;
	void MouseMoved(BPoint where, uint32 transit, const BMessage* dragged) override;
	void MessageReceived(BMessage* message) override;
	void ScrollTo(BPoint where) override;
	using BView::ScrollTo;

private:
	struct Item {
		Message message;
		TextLayout layout;
		std::string author;
		std::string avatarUrl;
		std::string time;
		std::string dayLabel;          // a day separator above this message
		bool compact = false;          // same author, shortly after the last
		bool mine = false;
		float top = 0;
		float height = 0;
		float textTop = 0;
		std::vector<std::pair<BRect, std::string>> reactions;   // chip, name
		std::vector<std::pair<BRect, File>> files;
		std::vector<std::pair<BRect, std::string>> images;       // thumbnail, url
		std::vector<std::pair<BRect, TextLayout>> attachments;
		BRect replies;
	};

	void Rebuild(bool keepPosition);
	void LayoutItems();
	void UpdateScrollBar();
	void DrawItem(Item& item, BRect updateRect);
	int ItemAt(BPoint where) const;
	float TextLeft() const;
	float TextWidth() const;
	void ShowContextMenu(int index, BPoint where);
	void SendToWindow(BMessage& message);

	Session* fSession;
	bool fThreadMode;
	std::string fChannel;
	std::string fThread;
	std::vector<Item> fItems;
	float fContentHeight = 0;
	int fHover = -1;
	float fLaidOutWidth = 0;
	bool fLoadingOlder = false;
	bool fMoreOlder = true;
	bool fAtEnd = true;
};

}  // namespace natter::ui
