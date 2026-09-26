// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#include "ImageCache.h"

#include "Messages.h"
#include "natter/http.h"
#include "natter/session.h"
#include "natter/util.h"

#include <Bitmap.h>
#include <DataIO.h>
#include <FindDirectory.h>
#include <Message.h>
#include <Path.h>
#include <TranslationUtils.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace natter::ui {

ImageCache& ImageCache::Shared()
{
	static ImageCache cache;
	return cache;
}

ImageCache::ImageCache()
	:
	fTransport(natter::defaultTransport()),
	fWorkers(std::make_unique<WorkQueue>(3))
{
	BPath path;
	if (find_directory(B_USER_CACHE_DIRECTORY, &path) == B_OK) {
		path.Append("Natter/images");
		fDirectory = path.Path();
		std::error_code error;
		std::filesystem::create_directories(fDirectory, error);
	}
}

void ImageCache::SetAuthenticatedFetcher(Fetcher fetcher)
{
	std::lock_guard<std::mutex> guard(fLock);
	fFetcher = std::move(fetcher);
}

std::string ImageCache::CachePath(const std::string& url) const
{
	if (fDirectory.empty())
		return std::string();
	std::string digest = natter::sha1(url);
	static const char* hex = "0123456789abcdef";
	std::string name;
	for (unsigned char byte : digest) {
		name += hex[byte >> 4];
		name += hex[byte & 15];
	}
	return fDirectory + "/" + name;
}

const BBitmap* ImageCache::Get(const std::string& url, BMessenger target)
{
	if (url.empty())
		return nullptr;
	std::lock_guard<std::mutex> guard(fLock);
	auto found = fBitmaps.find(url);
	if (found != fBitmaps.end())
		return found->second.get();
	if (fFailed.count(url))
		return nullptr;
	auto& waiting = fWaiting[url];
	bool start = waiting.empty();
	if (target.IsValid())
		waiting.push_back(target);
	if (start)
		fWorkers->post([this, url] { Load(url); });
	return nullptr;
}

static BBitmap* Decode(const std::string& data)
{
	if (data.empty())
		return nullptr;
	BMemoryIO input(data.data(), data.size());
	return BTranslationUtils::GetBitmap(&input);
}

void ImageCache::Load(const std::string& url)
{
	std::string path = CachePath(url);
	std::string data;
	if (!path.empty()) {
		std::ifstream file(path, std::ios::binary);
		if (file) {
			std::stringstream buffer;
			buffer << file.rdbuf();
			data = buffer.str();
		}
	}
	BBitmap* bitmap = Decode(data);
	if (!bitmap) {
		std::shared_ptr<HttpTransport> transport;
		Fetcher fetcher;
		{
			std::lock_guard<std::mutex> guard(fLock);
			transport = fTransport;
			fetcher = fFetcher;
		}
		bool loaded = false;
		// Avatars and emoji are public; files and their thumbnails need the
		// account's session, on files.slack.com or the workspace's own host.
		if (url.find("files.slack.com") != std::string::npos || url.find("/files-pri/") != std::string::npos
				|| url.find("/files-tmb/") != std::string::npos) {
			loaded = fetcher && fetcher(url, data);
		} else {
			HttpRequest request = HttpRequest::get(url);
			request.followRedirects = true;
			request.timeoutMs = 20000;
			HttpResponse response = transport->perform(request);
			if (response.success()) {
				data = std::move(response.body);
				loaded = true;
			}
		}
		if (loaded) {
			bitmap = Decode(data);
			if (bitmap && !path.empty()) {
				std::ofstream file(path, std::ios::binary);
				file.write(data.data(), static_cast<std::streamsize>(data.size()));
			}
		}
	}
	std::vector<BMessenger> waiting;
	{
		std::lock_guard<std::mutex> guard(fLock);
		if (bitmap)
			fBitmaps[url].reset(bitmap);
		else
			fFailed.insert(url);
		waiting = std::move(fWaiting[url]);
		fWaiting.erase(url);
	}
	if (!bitmap)
		return;
	for (BMessenger& target : waiting) {
		BMessage message(kImageLoaded);
		message.AddString("url", url.c_str());
		target.SendMessage(&message, static_cast<BHandler*>(nullptr), 100000);
	}
}

}  // namespace natter::ui
