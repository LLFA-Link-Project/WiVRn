// SPDX-License-Identifier: GPL-3.0-or-later
#include "driver/configuration.h"
#include "lighthouse_backend.h"
#include "wivrn_config.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void require(bool condition)
{
	if (!condition)
		throw std::runtime_error("Lighthouse backend selection assertion failed");
}
int main()
{
	using enum wivrn::lighthouse_backend;
	using wivrn::select_lighthouse_backend;
	require(select_lighthouse_backend(false, false, true, true) == none);
	require(select_lighthouse_backend(true, false, true, true) == steamvr);
	require(select_lighthouse_backend(false, true, true, true) == survive);
	require(select_lighthouse_backend(true, true, true, true) == survive);
	require(select_lighthouse_backend(true, true, true, false) == steamvr);
	require(select_lighthouse_backend(true, true, false, true) == survive);
	require(select_lighthouse_backend(true, true, false, false) == none);
	unsetenv("WIVRN_USE_STEAMVR_LH");
	unsetenv("WIVRN_USE_SURVIVE");
	char directory[] = "/tmp/wivrn-backend-test-XXXXXX";
	require(mkdtemp(directory) != nullptr);
	auto path = std::filesystem::path(directory) / "config.json";
	wivrn::configuration::set_config_file(path);
	auto load = [&](const char * json) {
		std::ofstream(path) << json;
		return wivrn::configuration().selected_lighthouse_backend();
	};
	const auto built_steamvr = WIVRN_FEATURE_STEAMVR_LIGHTHOUSE ? steamvr : none;
	const auto built_survive = WIVRN_FEATURE_SURVIVE ? survive : none;
	require(load("{}") == none);
	require(load(R"({"use-steamvr-lh":true})") == built_steamvr);
	require(load(R"({"use-survive":true})") == built_survive);
	require(load(R"({"use-steamvr-lh":true,"use-survive":true})") ==
	        select_lighthouse_backend(true, true, WIVRN_FEATURE_STEAMVR_LIGHTHOUSE, WIVRN_FEATURE_SURVIVE));
	// Re-read settings just as a new internal Monado process does on restart.
	require(load(R"({"use-steamvr-lh":true,"use-survive":false})") == built_steamvr);
	require(load(R"({"use-steamvr-lh":false,"use-survive":true})") == built_survive);
	require(load(R"({"use-steamvr-lh":false,"use-survive":false})") == none);
	setenv("WIVRN_USE_STEAMVR_LH", "1", true);
	require(load("{}") == built_steamvr);
	setenv("WIVRN_USE_SURVIVE", "1", true);
	require(load("{}") == select_lighthouse_backend(true, true, WIVRN_FEATURE_STEAMVR_LIGHTHOUSE, WIVRN_FEATURE_SURVIVE));
	std::filesystem::remove(path);
	std::filesystem::remove(directory);
	std::cout << "Lighthouse backend config reload, exclusivity, build availability and environment: passed\n";
}
