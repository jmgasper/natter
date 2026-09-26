// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#include "Composer.h"

#include "Messages.h"
#include "Theme.h"

#include <Button.h>
#include <LayoutBuilder.h>
#include <ScrollView.h>
#include <Window.h>

namespace natter::ui {

class ComposerText : public BTextView {
public:
	explicit ComposerText(Composer* owner)
		:
		BTextView("composer text"),
		fOwner(owner)
	{
		SetWordWrap(true);
		SetStylable(false);
		SetInsets(6, 5, 6, 5);
	}

	void SetPlaceholder(const std::string& placeholder)
	{
		fPlaceholder = placeholder;
		Invalidate();
	}

	void KeyDown(const char* bytes, int32 count) override
	{
		if (count == 1 && bytes[0] == B_ENTER) {
			int32 modifiersNow = modifiers();
			if (!(modifiersNow & (B_SHIFT_KEY | B_OPTION_KEY))) {
				fOwner->Send();
				return;
			}
		}
		BTextView::KeyDown(bytes, count);
		if (count >= 1 && bytes[0] >= ' ' && Window()) {
			BMessage typing(kTyping);
			Window()->PostMessage(&typing);
		}
	}

	void Draw(BRect updateRect) override
	{
		BTextView::Draw(updateRect);
		if (TextLength() == 0 && !fPlaceholder.empty()) {
			const Theme& theme = Theme::Current();
			SetHighColor(theme.muted);
			font_height metrics;
			GetFontHeight(&metrics);
			DrawString(fPlaceholder.c_str(), BPoint(8, 5 + metrics.ascent));
		}
	}

	void InsertText(const char* text, int32 length, int32 offset, const text_run_array* runs) override
	{
		BTextView::InsertText(text, length, offset, runs);
		Invalidate();
	}

	void DeleteText(int32 start, int32 finish) override
	{
		BTextView::DeleteText(start, finish);
		Invalidate();
	}

private:
	Composer* fOwner;
	std::string fPlaceholder;
};

Composer::Composer(const char* name)
	:
	BView(name, 0)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	fText = new ComposerText(this);
	auto* scroll = new BScrollView("composer scroll", fText, 0, false, true);
	font_height metrics;
	be_plain_font->GetHeight(&metrics);
	float line = metrics.ascent + metrics.descent + metrics.leading;
	scroll->SetExplicitMinSize(BSize(B_SIZE_UNSET, line * 2 + 14));
	scroll->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, line * 2 + 14));
	fAttach = new BButton("attach", "+", new BMessage(kUploadFile));
	fAttach->SetToolTip("Upload a file");
	fAttach->SetExplicitMaxSize(BSize(32, B_SIZE_UNSET));
	BLayoutBuilder::Group<>(this, B_HORIZONTAL, 6)
		.SetInsets(8, 4, 8, 8)
		.Add(fAttach)
		.Add(scroll);
}

void Composer::AttachedToWindow()
{
	BView::AttachedToWindow();
	fAttach->SetTarget(this);
}

void Composer::SetConversation(const std::string& channel, const std::string& thread, const std::string& placeholder)
{
	fChannel = channel;
	fThread = thread;
	fText->SetPlaceholder(placeholder);
}

void Composer::Focus()
{
	fText->MakeFocus(true);
}

void Composer::Send()
{
	std::string text = fText->Text();
	while (!text.empty() && (text.back() == '\n' || text.back() == ' '))
		text.pop_back();
	if (text.empty() || fChannel.empty())
		return;
	BMessage message(kSendMessage);
	message.AddString("channel", fChannel.c_str());
	if (!fThread.empty())
		message.AddString("thread", fThread.c_str());
	message.AddString("text", text.c_str());
	Window()->PostMessage(&message);
	fText->SetText("");
}

void Composer::MessageReceived(BMessage* message)
{
	if (message->what == kUploadFile) {
		BMessage upload(kUploadFile);
		upload.AddString("channel", fChannel.c_str());
		if (!fThread.empty())
			upload.AddString("thread", fThread.c_str());
		Window()->PostMessage(&upload);
		return;
	}
	BView::MessageReceived(message);
}

}  // namespace natter::ui
