// Natter - a native Slack client core.
// SPDX-License-Identifier: MIT

#include "natter/models.h"

namespace natter {

namespace {

void
putIfSet(json& j, const char* key, const std::string& value)
{
	if (!value.empty())
		j[key] = value;
}


void
putIfSet(json& j, const char* key, int64_t value)
{
	if (value != 0)
		j[key] = value;
}


void
putIfTrue(json& j, const char* key, bool value)
{
	if (value)
		j[key] = true;
}


// Collect "<prefix><size>" keys (image_48, thumb_360, ...) into a size map.
std::map<int, std::string>
sizedUrls(const json& j, const std::string& prefix)
{
	std::map<int, std::string> out;
	if (!j.is_object())
		return out;
	for (auto it = j.begin(); it != j.end(); ++it) {
		const std::string& key = it.key();
		if (!startsWith(key, prefix) || !it->is_string())
			continue;
		std::string rest = key.substr(prefix.size());
		if (rest.empty() || rest.find_first_not_of("0123456789") != std::string::npos)
			continue;
		out[std::stoi(rest)] = it->get<std::string>();
	}
	return out;
}


std::string
pickSized(const std::map<int, std::string>& urls, int minSize)
{
	if (urls.empty())
		return {};
	auto it = urls.lower_bound(minSize);
	if (it == urls.end())
		return urls.rbegin()->second;
	return it->second;
}


std::vector<std::string>
stringList(const json& array)
{
	std::vector<std::string> out;
	if (!array.is_array())
		return out;
	for (const json& item : array) {
		if (item.is_string())
			out.push_back(item.get<std::string>());
	}
	return out;
}


Topic
topicFromJson(const json& j)
{
	Topic topic;
	if (j.is_string()) {
		topic.value = j.get<std::string>();
		return topic;
	}
	topic.value = jStr(j, "value");
	topic.creator = jStr(j, "creator");
	topic.lastSet = jInt(j, "last_set");
	return topic;
}


json
topicToJson(const Topic& topic)
{
	return json{{"value", topic.value}, {"creator", topic.creator},
		{"last_set", topic.lastSet}};
}

}  // namespace

// ---- Team ------------------------------------------------------------------

Team
Team::fromJson(const json& j)
{
	Team team;
	team.id = jStr(j, "id");
	team.name = jStr(j, "name");
	team.domain = jStr(j, "domain");
	team.url = jStr(j, "url");
	team.enterpriseId = jStr(j, "enterprise_id");
	const json& icon = jObj(j, "icon");
	for (const char* key : {"image_132", "image_102", "image_88", "image_68",
			"image_44", "image_34"}) {
		std::string url = jStr(icon, key);
		if (!url.empty()) {
			team.iconUrl = url;
			break;
		}
	}
	if (team.iconUrl.empty())
		team.iconUrl = jStr(j, "icon_url");
	return team;
}


json
Team::toJson() const
{
	json j = {{"id", id}, {"name", name}};
	putIfSet(j, "domain", domain);
	putIfSet(j, "url", url);
	putIfSet(j, "enterprise_id", enterpriseId);
	if (!iconUrl.empty())
		j["icon"] = json{{"image_132", iconUrl}};
	return j;
}

// ---- User ------------------------------------------------------------------

std::string
User::bestName() const
{
	if (!displayName.empty())
		return displayName;
	if (!realName.empty())
		return realName;
	if (!name.empty())
		return name;
	return id;
}


std::string
User::avatarUrl(int minSize) const
{
	return pickSized(avatars, minSize);
}


User
User::fromJson(const json& j)
{
	User user;
	const json& profile = jObj(j, "profile");
	user.id = jStr(j, "id");
	user.teamId = jStr(j, "team_id", jStr(profile, "team"));
	user.name = jStr(j, "name");
	user.realName = jStr(profile, "real_name", jStr(j, "real_name"));
	if (user.realName.empty())
		user.realName = jStr(j, "real_name");
	user.displayName = jStr(profile, "display_name");
	user.title = jStr(profile, "title");
	user.color = jStr(j, "color");
	user.tz = jStr(j, "tz");
	user.tzOffset = jInt(j, "tz_offset");
	user.deleted = jBool(j, "deleted");
	user.isBot = jBool(j, "is_bot");
	user.isAppUser = jBool(j, "is_app_user");
	user.botId = jStr(profile, "bot_id");
	user.statusText = jStr(profile, "status_text");
	user.statusEmoji = jStr(profile, "status_emoji");
	user.presence = jStr(j, "presence");
	user.avatars = sizedUrls(profile, "image_");
	std::string original = jStr(profile, "image_original");
	if (!original.empty() && user.avatars.empty())
		user.avatars[1024] = original;
	return user;
}


json
User::toJson() const
{
	json profile = json::object();
	putIfSet(profile, "real_name", realName);
	putIfSet(profile, "display_name", displayName);
	putIfSet(profile, "title", title);
	putIfSet(profile, "status_text", statusText);
	putIfSet(profile, "status_emoji", statusEmoji);
	putIfSet(profile, "bot_id", botId);
	for (const auto& [size, url] : avatars)
		profile["image_" + std::to_string(size)] = url;

	json j = {{"id", id}, {"name", name}, {"profile", profile}};
	putIfSet(j, "team_id", teamId);
	putIfSet(j, "real_name", realName);
	putIfSet(j, "color", color);
	putIfSet(j, "tz", tz);
	putIfSet(j, "tz_offset", tzOffset);
	putIfTrue(j, "deleted", deleted);
	putIfTrue(j, "is_bot", isBot);
	putIfTrue(j, "is_app_user", isAppUser);
	putIfSet(j, "presence", presence);
	return j;
}

// ---- Reaction and File -------------------------------------------------------

Reaction
Reaction::fromJson(const json& j)
{
	Reaction reaction;
	reaction.name = jStr(j, "name");
	reaction.users = stringList(jArr(j, "users"));
	reaction.count = static_cast<int>(jInt(j, "count",
		static_cast<int64_t>(reaction.users.size())));
	return reaction;
}


json
Reaction::toJson() const
{
	return json{{"name", name}, {"count", count}, {"users", users}};
}


std::string
File::thumbUrl(int minSize) const
{
	return pickSized(thumbs, minSize);
}


File
File::fromJson(const json& j)
{
	File file;
	file.id = jStr(j, "id");
	file.name = jStr(j, "name");
	file.title = jStr(j, "title");
	file.mimetype = jStr(j, "mimetype");
	file.filetype = jStr(j, "filetype");
	file.prettyType = jStr(j, "pretty_type");
	file.size = jInt(j, "size");
	file.urlPrivate = jStr(j, "url_private");
	file.urlPrivateDownload = jStr(j, "url_private_download");
	file.permalink = jStr(j, "permalink");
	file.mode = jStr(j, "mode");
	file.originalWidth = static_cast<int>(jInt(j, "original_w"));
	file.originalHeight = static_cast<int>(jInt(j, "original_h"));
	file.thumbs = sizedUrls(j, "thumb_");
	return file;
}


json
File::toJson() const
{
	json j = {{"id", id}};
	putIfSet(j, "name", name);
	putIfSet(j, "title", title);
	putIfSet(j, "mimetype", mimetype);
	putIfSet(j, "filetype", filetype);
	putIfSet(j, "pretty_type", prettyType);
	putIfSet(j, "size", size);
	putIfSet(j, "url_private", urlPrivate);
	putIfSet(j, "url_private_download", urlPrivateDownload);
	putIfSet(j, "permalink", permalink);
	putIfSet(j, "mode", mode);
	putIfSet(j, "original_w", static_cast<int64_t>(originalWidth));
	putIfSet(j, "original_h", static_cast<int64_t>(originalHeight));
	for (const auto& [size, url] : thumbs)
		j["thumb_" + std::to_string(size)] = url;
	return j;
}

// ---- Message -----------------------------------------------------------------

Message
Message::fromJson(const json& j, const std::string& channel)
{
	Message message;
	message.channel = jStr(j, "channel", channel);
	if (message.channel.empty())
		message.channel = channel;
	message.ts = jStr(j, "ts");
	message.user = jStr(j, "user");
	message.botId = jStr(j, "bot_id");
	message.username = jStr(j, "username");
	message.text = jStr(j, "text");
	message.subtype = jStr(j, "subtype");
	message.threadTs = jStr(j, "thread_ts");
	message.parentUserId = jStr(j, "parent_user_id");
	message.replyCount = static_cast<int>(jInt(j, "reply_count"));
	message.replyUsersCount = static_cast<int>(jInt(j, "reply_users_count"));
	message.replyUsers = stringList(jArr(j, "reply_users"));
	message.latestReply = jStr(j, "latest_reply");
	message.hidden = jBool(j, "hidden");
	message.isStarred = jBool(j, "is_starred");
	message.permalink = jStr(j, "permalink");

	const json& edited = jObj(j, "edited");
	if (edited.is_object())
		message.edited = Edited{jStr(edited, "user"), jStr(edited, "ts")};

	for (const json& item : jArr(j, "reactions"))
		message.reactions.push_back(Reaction::fromJson(item));
	for (const json& item : jArr(j, "files"))
		message.files.push_back(File::fromJson(item));
	// Some events carry a single legacy "file" object.
	const json& single = jObj(j, "file");
	if (single.is_object() && message.files.empty())
		message.files.push_back(File::fromJson(single));

	if (jHas(j, "attachments"))
		message.attachments = j["attachments"];
	if (jHas(j, "blocks"))
		message.blocks = j["blocks"];
	if (jHas(j, "bot_profile"))
		message.botProfile = j["bot_profile"];
	return message;
}


json
Message::toJson() const
{
	json j = {{"type", "message"}, {"ts", ts}};
	putIfSet(j, "channel", channel);
	putIfSet(j, "user", user);
	putIfSet(j, "bot_id", botId);
	putIfSet(j, "username", username);
	j["text"] = text;
	putIfSet(j, "subtype", subtype);
	putIfSet(j, "thread_ts", threadTs);
	putIfSet(j, "parent_user_id", parentUserId);
	putIfSet(j, "reply_count", static_cast<int64_t>(replyCount));
	putIfSet(j, "reply_users_count", static_cast<int64_t>(replyUsersCount));
	if (!replyUsers.empty())
		j["reply_users"] = replyUsers;
	putIfSet(j, "latest_reply", latestReply);
	if (edited)
		j["edited"] = json{{"user", edited->user}, {"ts", edited->ts}};
	if (!reactions.empty()) {
		json list = json::array();
		for (const Reaction& reaction : reactions)
			list.push_back(reaction.toJson());
		j["reactions"] = list;
	}
	if (!files.empty()) {
		json list = json::array();
		for (const File& file : files)
			list.push_back(file.toJson());
		j["files"] = list;
	}
	if (!attachments.is_null())
		j["attachments"] = attachments;
	if (!blocks.is_null())
		j["blocks"] = blocks;
	if (!botProfile.is_null())
		j["bot_profile"] = botProfile;
	putIfTrue(j, "hidden", hidden);
	putIfTrue(j, "is_starred", isStarred);
	putIfSet(j, "permalink", permalink);
	return j;
}

// ---- Channel -----------------------------------------------------------------

ChannelKind
Channel::kind() const
{
	if (isIm)
		return ChannelKind::Im;
	if (isMpim)
		return ChannelKind::Mpim;
	if (isPrivate || isGroup)
		return ChannelKind::Private;
	return ChannelKind::Public;
}


Channel
Channel::fromJson(const json& j)
{
	Channel channel;
	channel.id = jStr(j, "id");
	channel.name = jStr(j, "name");
	channel.nameNormalized = jStr(j, "name_normalized");
	channel.isChannel = jBool(j, "is_channel");
	channel.isGroup = jBool(j, "is_group");
	channel.isIm = jBool(j, "is_im");
	channel.isMpim = jBool(j, "is_mpim");
	channel.isPrivate = jBool(j, "is_private");
	channel.isArchived = jBool(j, "is_archived");
	channel.isGeneral = jBool(j, "is_general");
	channel.isExtShared = jBool(j, "is_ext_shared");
	// Direct messages have no is_member; being listed means belonging.
	channel.isMember = jBool(j, "is_member", channel.isIm || channel.isMpim);
	channel.imUser = jStr(j, "user");
	channel.topic = topicFromJson(j.contains("topic") ? j["topic"] : json());
	channel.purpose = topicFromJson(j.contains("purpose") ? j["purpose"] : json());
	channel.created = jInt(j, "created");
	channel.numMembers = static_cast<int>(jInt(j, "num_members"));
	channel.lastRead = jStr(j, "last_read");
	channel.priority = static_cast<int64_t>(jDouble(j, "priority") * 1000);

	const json& latest = j.contains("latest") ? j["latest"] : json();
	if (latest.is_object())
		channel.latestTs = jStr(latest, "ts");
	else if (latest.is_string())
		channel.latestTs = latest.get<std::string>();

	channel.unreadCount = static_cast<int>(jInt(j, "unread_count_display",
		jInt(j, "unread_count")));
	channel.mentionCount = static_cast<int>(jInt(j, "mention_count_display",
		jInt(j, "mention_count")));
	channel.hasUnreads = jBool(j, "has_unreads", channel.unreadCount > 0);
	// A private channel is reported as is_group by some methods.
	if (channel.isGroup && !channel.isMpim)
		channel.isPrivate = true;
	return channel;
}


json
Channel::toJson() const
{
	json j = {{"id", id}};
	putIfSet(j, "name", name);
	putIfSet(j, "name_normalized", nameNormalized);
	j["is_channel"] = isChannel;
	j["is_group"] = isGroup;
	j["is_im"] = isIm;
	j["is_mpim"] = isMpim;
	j["is_private"] = isPrivate;
	j["is_member"] = isMember;
	putIfTrue(j, "is_archived", isArchived);
	putIfTrue(j, "is_general", isGeneral);
	putIfTrue(j, "is_ext_shared", isExtShared);
	putIfSet(j, "user", imUser);
	if (!topic.value.empty())
		j["topic"] = topicToJson(topic);
	if (!purpose.value.empty())
		j["purpose"] = topicToJson(purpose);
	putIfSet(j, "created", created);
	putIfSet(j, "num_members", static_cast<int64_t>(numMembers));
	putIfSet(j, "last_read", lastRead);
	putIfSet(j, "latest", latestTs);
	j["unread_count_display"] = unreadCount;
	j["mention_count_display"] = mentionCount;
	putIfTrue(j, "has_unreads", hasUnreads);
	if (priority != 0)
		j["priority"] = static_cast<double>(priority) / 1000;
	return j;
}

// ---- small responses -----------------------------------------------------------

AuthInfo
AuthInfo::fromJson(const json& j)
{
	AuthInfo info;
	info.url = jStr(j, "url");
	info.team = jStr(j, "team");
	info.user = jStr(j, "user");
	info.teamId = jStr(j, "team_id");
	info.userId = jStr(j, "user_id");
	info.botId = jStr(j, "bot_id");
	info.enterpriseId = jStr(j, "enterprise_id");
	info.isEnterpriseInstall = jBool(j, "is_enterprise_install");
	return info;
}


Presence
Presence::fromJson(const json& j)
{
	Presence presence;
	presence.presence = jStr(j, "presence");
	presence.online = jBool(j, "online", presence.presence == "active");
	presence.autoAway = jBool(j, "auto_away");
	presence.manualAway = jBool(j, "manual_away");
	presence.lastActivity = jInt(j, "last_activity");
	presence.connectionCount = static_cast<int>(jInt(j, "connection_count"));
	return presence;
}

}  // namespace natter
