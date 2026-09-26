// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#include "App.h"

#include "MainWindow.h"
#include "Messages.h"
#include "SignInWindow.h"
#include "natter/credentials.h"

#include <Alert.h>
#include <FindDirectory.h>
#include <Path.h>
#include <Window.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace natter::ui {

App::App()
	:
	BApplication("application/x-vnd.KunanyiOS-Natter")
{
	BPath path;
	find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	path.Append("Natter");
	fDirectory = path.Path();
	std::error_code error;
	std::filesystem::create_directories(fDirectory + "/accounts", error);
	std::filesystem::permissions(fDirectory + "/accounts", std::filesystem::perms::owner_all,
		std::filesystem::perm_options::replace, error);
	LoadSettings();
}

void App::LoadSettings()
{
	std::ifstream file(fDirectory + "/settings.json");
	std::stringstream text;
	text << file.rdbuf();
	fSettings = json::parse(text.str(), nullptr, false);
	if (!fSettings.is_object())
		fSettings = json::object();
}

void App::SaveSettings()
{
	std::string path = fDirectory + "/settings.json";
	std::ofstream file(path + ".new");
	file << fSettings.dump(2);
	file.close();
	std::error_code error;
	std::filesystem::rename(path + ".new", path, error);
}

void App::ReadyToRun()
{
	std::vector<std::string> teams;
	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator(fDirectory + "/accounts", error)) {
		if (entry.path().extension() == ".json")
			teams.push_back(entry.path().stem().string());
	}
	if (teams.empty()) {
		ShowSignIn();
		return;
	}
	std::string last = jStr(fSettings, "lastTeam");
	if (std::find(teams.begin(), teams.end(), last) == teams.end())
		last = teams.front();
	OpenWorkspace(last);
}

void App::ShowSignIn()
{
	if (fSignIn.IsValid()) {
		BMessage activate(B_WINDOW_ACTIVATED);
		fSignIn.SendMessage(&activate);
		return;
	}
	auto* window = new SignInWindow();
	fSignIn = BMessenger(window);
	window->Show();
}

void App::OpenWorkspace(const std::string& teamId)
{
	auto open = fWindows.find(teamId);
	if (open != fWindows.end() && open->second.IsValid()) {
		BWindow* window = nullptr;
		open->second.Target(reinterpret_cast<BLooper**>(&window));
		if (window && window->Lock()) {
			window->Activate();
			window->Unlock();
		}
		return;
	}
	auto credentials = Credentials::load(fDirectory + "/accounts/" + teamId + ".json");
	if (!credentials) {
		ShowSignIn();
		return;
	}
	if (credentials->teamId.empty())
		credentials->teamId = teamId;
	auto* window = new MainWindow(credentials.value(), fDirectory);
	fWindows[teamId] = BMessenger(window);
	fSettings["lastTeam"] = teamId;
	SaveSettings();
	window->Show();
}

void App::QuitIfIdle()
{
	for (auto it = fWindows.begin(); it != fWindows.end();) {
		if (!it->second.IsValid())
			it = fWindows.erase(it);
		else
			++it;
	}
	if (fWindows.empty() && !fSignIn.IsValid())
		PostMessage(B_QUIT_REQUESTED);
}

void App::MessageReceived(BMessage* message)
{
	switch (message->what) {
	case kSignedIn: {
		Credentials credentials = Credentials::fromJson(json::parse(message->GetString("credentials", "{}"), nullptr, false));
		std::string team = credentials.teamId.empty() ? "default" : credentials.teamId;
		Status saved = credentials.save(fDirectory + "/accounts/" + team + ".json");
		if (!saved) {
			(new BAlert("Natter", ("Could not save the sign-in: " + saved.error().message).c_str(), "OK"))->Go(nullptr);
			break;
		}
		fSignIn = BMessenger();
		OpenWorkspace(team);
		break;
	}
	case kAddWorkspace:
		ShowSignIn();
		break;
	case kSignOut: {
		std::string team = message->GetString("team", "");
		if (team.empty())
			break;
		std::error_code error;
		std::filesystem::remove(fDirectory + "/accounts/" + team + ".json", error);
		std::filesystem::remove_all(fDirectory + "/cache/" + team, error);
		break;
	}
	case kChannelSelected: {
		std::string team = message->GetString("team", "");
		if (!team.empty()) {
			fSettings["lastChannel"][team] = message->GetString("channel", "");
			SaveSettings();
		}
		break;
	}
	case kOpenChannel: {
		// A workspace window asks for the conversation it showed last.
		std::string team = message->GetString("team", "");
		BMessage reply(kOpenChannel);
		reply.AddString("channel", jStr(jObj(fSettings, "lastChannel"), team.c_str()).c_str());
		message->SendReply(&reply);
		break;
	}
	case 'nwcl': {
		fWindows.erase(message->GetString("team", ""));
		QuitIfIdle();
		break;
	}
	case 'nsic':
		fSignIn = BMessenger();
		// Closing the sign-in window with nothing else open quits.
		if (fWindows.empty())
			PostMessage(B_QUIT_REQUESTED);
		break;
	case kAbout:
		AboutRequested();
		break;
	default:
		BApplication::MessageReceived(message);
	}
}

void App::AboutRequested()
{
	(new BAlert("About Natter",
		"Natter\n\nA native Slack client for Haiku.\n\n"
		"Natter talks to Slack the way its web client does. It is not made or endorsed by Slack.",
		"OK"))->Go(nullptr);
}

bool App::QuitRequested()
{
	return BApplication::QuitRequested();
}

}  // namespace natter::ui
