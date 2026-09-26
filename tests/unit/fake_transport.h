// Natter - an HttpTransport that answers from canned responses.
// SPDX-License-Identifier: MIT
#pragma once

#include "natter/http.h"
#include "natter/util.h"
#include "testing.h"

#include <deque>
#include <map>
#include <mutex>

class FakeTransport : public natter::HttpTransport {
public:
	// Queue a response for a method ("users.list") or a full URL. Responses
	// are used in order; the last one repeats.
	void add(const std::string& key, natter::HttpResponse response)
	{
		std::lock_guard<std::mutex> guard(fLock);
		fRoutes[key].push_back(std::move(response));
	}

	void addJson(const std::string& key, const std::string& body, int status = 200,
		natter::Headers headers = {})
	{
		natter::HttpResponse response;
		response.status = status;
		response.body = body;
		response.headers = std::move(headers);
		response.headers.emplace_back("Content-Type", "application/json; charset=utf-8");
		add(key, std::move(response));
	}

	void addFixture(const std::string& key, const std::string& file)
	{
		addJson(key, testing::fixture(file));
	}

	void addError(const std::string& key, const std::string& code, int status = 200)
	{
		addJson(key, "{\"ok\":false,\"error\":\"" + code + "\"}", status);
	}

	natter::HttpResponse perform(const natter::HttpRequest& request) override
	{
		std::lock_guard<std::mutex> guard(fLock);
		requests.push_back(request);
		for (auto& [key, queue] : fRoutes) {
			if (request.url == key || natter::endsWith(request.url, "/" + key)) {
				natter::HttpResponse response = queue.front();
				if (queue.size() > 1)
					queue.pop_front();
				if (request.sink && !response.body.empty()) {
					request.sink(response.body.data(), response.body.size());
					response.body.clear();
				}
				return response;
			}
		}
		natter::HttpResponse missing;
		missing.status = 200;
		missing.body = "{\"ok\":false,\"error\":\"unknown_method\"}";
		return missing;
	}

	// Requests whose URL ends with "/<method>" (or equals it).
	std::vector<natter::HttpRequest> calls(const std::string& method)
	{
		std::lock_guard<std::mutex> guard(fLock);
		std::vector<natter::HttpRequest> out;
		for (const natter::HttpRequest& request : requests) {
			if (request.url == method || natter::endsWith(request.url, "/" + method))
				out.push_back(request);
		}
		return out;
	}

	static std::string field(const natter::HttpRequest& request, const std::string& name)
	{
		for (const auto& [key, value] : request.form) {
			if (key == name)
				return value;
		}
		return {};
	}

	static bool hasField(const natter::HttpRequest& request, const std::string& name)
	{
		for (const auto& [key, value] : request.form) {
			if (key == name)
				return true;
		}
		return false;
	}

	std::vector<natter::HttpRequest> requests;

private:
	std::mutex fLock;
	std::map<std::string, std::deque<natter::HttpResponse>> fRoutes;
};
