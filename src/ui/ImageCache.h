// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#pragma once

#include <Messenger.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

class BBitmap;

namespace natter {
class HttpTransport;
class WorkQueue;
}

namespace natter::ui {

// Avatars, custom emoji and file thumbnails. Get() answers at once with a
// bitmap already loaded; otherwise it starts a download (kept on disk in
// ~/config/cache/Natter/images) and sends kImageLoaded with "url" to the
// target when it arrives. Bitmaps live as long as the cache.
class ImageCache {
public:
	static ImageCache& Shared();

	// Files on files.slack.com need the account's credentials; the window
	// that owns a session lends its download function for those.
	using Fetcher = std::function<bool(const std::string& url, std::string& data)>;
	void SetAuthenticatedFetcher(Fetcher fetcher);
	const BBitmap* Get(const std::string& url, BMessenger target);

private:
	ImageCache();
	void Load(const std::string& url);
	std::string CachePath(const std::string& url) const;

	std::mutex fLock;
	std::map<std::string, std::unique_ptr<BBitmap>> fBitmaps;
	std::map<std::string, std::vector<BMessenger>> fWaiting;
	std::set<std::string> fFailed;
	std::shared_ptr<HttpTransport> fTransport;
	Fetcher fFetcher;
	std::unique_ptr<WorkQueue> fWorkers;
	std::string fDirectory;
};

}  // namespace natter::ui
