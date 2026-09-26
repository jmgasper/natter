// Natter - a native Slack client for Haiku.
// SPDX-License-Identifier: MIT
#include "App.h"

#include <cstdio>
#include <cstdlib>

int main()
{
	natter::ui::App app;
	app.Run();
#if NATTER_WEBKIT
	// WebKit's worker threads may still run their thread-local destructors;
	// leave without destroying libbe's globals under them, as Summit does.
	std::fflush(nullptr);
	std::_Exit(0);
#endif
	return 0;
}
