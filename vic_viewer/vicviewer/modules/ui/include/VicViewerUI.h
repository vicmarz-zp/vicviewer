#pragma once

#include "AppContext.h"

#include <Windows.h>
#include <string>

namespace vic::ui {

struct LaunchOptions {
	enum class Mode {
		Manual,
		Host,
		Viewer
	};

	Mode mode = Mode::Manual;
	std::wstring sessionCode;
	bool minimizeOnStart = false;
};

int run(HINSTANCE instance, int showCommand, vic::AppContext& context, const LaunchOptions& options = {});

}
