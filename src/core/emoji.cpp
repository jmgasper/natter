// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT

#include "natter/emoji.h"
#include "natter/util.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace natter::emoji {

namespace {

struct EmojiSequence {
	const char* utf8;
	int flags;          // 1: takes a skin tone
};

struct EmojiName {
	const char* name;
	int index;
};

struct EmojiToneOverride {
	int index;
	const char* toned[5];
};

#include "emoji_table.inc"


const EmojiSequence*
find(std::string_view name, int* indexOut = nullptr)
{
	auto it = std::lower_bound(std::begin(kEmojiNames), std::end(kEmojiNames), name,
		[](const EmojiName& entry, std::string_view key) {
			return std::string_view(entry.name) < key;
		});
	if (it == std::end(kEmojiNames) || std::string_view(it->name) != name)
		return nullptr;
	if (indexOut != nullptr)
		*indexOut = it->index;
	return &kEmojiSequences[it->index];
}


// Insert the modifier after the first code point, dropping a variation
// selector that followed it (the rule every single-person emoji follows).
std::string
applyTone(const char* utf8, int tone)
{
	static const uint32_t kModifiers[] = {0x1F3FB, 0x1F3FC, 0x1F3FD, 0x1F3FE, 0x1F3FF};
	std::string base(utf8);
	size_t first = 1;
	unsigned char lead = static_cast<unsigned char>(base[0]);
	if (lead >= 0xF0)
		first = 4;
	else if (lead >= 0xE0)
		first = 3;
	else if (lead >= 0xC0)
		first = 2;
	std::string out = base.substr(0, first);
	appendUtf8(out, kModifiers[tone - 2]);
	std::string rest = base.substr(first);
	if (startsWith(rest, "\xEF\xB8\x8F"))   // U+FE0F
		rest.erase(0, 3);
	return out + rest;
}

}  // namespace


int
skinToneFromName(std::string_view name)
{
	if (!startsWith(name, "skin-tone-") || name.size() != 11)
		return 0;
	char digit = name[10];
	if (digit < '2' || digit > '6')
		return 0;
	return digit - '0';
}


void
splitSkinTone(std::string_view full, std::string& name, int& tone)
{
	tone = 0;
	size_t marker = full.find(":skin-tone-");
	if (marker == std::string_view::npos) {
		name = std::string(full);
		return;
	}
	std::string_view base = full.substr(0, marker);
	if (endsWith(base, ":"))
		base.remove_suffix(1);
	std::string_view toneName = full.substr(marker + 1);
	if (endsWith(toneName, ":"))
		toneName.remove_suffix(1);
	name = std::string(base);
	tone = skinToneFromName(toneName);
}


std::optional<std::string>
lookup(std::string_view full)
{
	std::string name;
	int tone = 0;
	splitSkinTone(full, name, tone);
	return lookup(name, tone);
}


std::optional<std::string>
lookup(std::string_view name, int skinTone)
{
	int index = -1;
	const EmojiSequence* sequence = find(name, &index);
	if (sequence == nullptr)
		return std::nullopt;
	if (skinTone < 2 || skinTone > 6 || (sequence->flags & 1) == 0)
		return std::string(sequence->utf8);
	for (const EmojiToneOverride& entry : kToneOverrides) {
		if (entry.index == index)
			return std::string(entry.toned[skinTone - 2]);
	}
	return applyTone(sequence->utf8, skinTone);
}


bool
takesSkinTone(std::string_view name)
{
	const EmojiSequence* sequence = find(name);
	return sequence != nullptr && (sequence->flags & 1) != 0;
}


size_t
count()
{
	return std::size(kEmojiNames);
}

}  // namespace natter::emoji
