// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#pragma once

#include <TextView.h>
#include <View.h>

#include <string>

class BButton;

namespace natter::ui {

// Where a message is written. Enter sends (kSendMessage with "channel",
// "thread" and "text"), Shift-Enter starts a new line.
class Composer : public BView {
public:
	explicit Composer(const char* name);

	void SetConversation(const std::string& channel, const std::string& thread, const std::string& placeholder);
	void Focus();
	void AttachedToWindow() override;
	void MessageReceived(BMessage* message) override;

private:
	friend class ComposerText;
	void Send();

	class ComposerText* fText;
	BButton* fAttach;
	std::string fChannel;
	std::string fThread;
};

}  // namespace natter::ui
