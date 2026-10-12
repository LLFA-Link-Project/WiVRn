// SPDX-License-Identifier: GPL-3.0-or-later
#include "lighthouse_recording.h"
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char ** environ;
namespace wivrn
{
void start_lighthouse_recording(const configuration & snapshot) noexcept
{
#if WIVRN_FEATURE_STEAMVR_LIGHTHOUSE
	// Explicit CLI-managed sessions already carry their immutable output path.
	if (const char * enabled = std::getenv("LH_RECORDER_ENABLE"); enabled && std::string_view(enabled) == "1")
		return;
	if (!snapshot.lighthouse_recording_enabled)
		return;
	try
	{
		if (snapshot.lighthouse_recording_output.empty() || !snapshot.lighthouse_recording_output.is_absolute())
			throw std::runtime_error("Lighthouse output directory must be absolute");
		auto root = snapshot.lighthouse_recording_output;
		std::filesystem::create_directories(root);
		auto stamp = std::chrono::system_clock::now().time_since_epoch();
		std::time_t wall = std::time(nullptr);
		std::tm local{};
		localtime_r(&wall, &local);
		char date[32]{};
		std::strftime(date, sizeof(date), "%Y-%m-%d_%H%M%S", &local);
		auto name = std::string(date) + "_" + std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(stamp).count() % 1000000000) + "_" + std::to_string(getpid());
		auto session = root / name;
		if (!std::filesystem::create_directory(session))
			throw std::runtime_error("Lighthouse session directory collision");
		std::filesystem::permissions(session, std::filesystem::perms::owner_all);
		std::string output = session.string(), owner = std::to_string(getpid());
		std::string executable = WIVRN_LH_RECORDER_EXECUTABLE;
		setenv("LH_RECORDER_MONADO_REV", WIVRN_LH_MONADO_REV, true);
		setenv("LH_RECORDER_WIVRN_BUILD", WIVRN_LH_WIVRN_REV, true);
		char * argv[] = {executable.data(), const_cast<char *>("run"), const_cast<char *>("--output"), output.data(), const_cast<char *>("--owner-pid"), owner.data(), nullptr};
		pid_t pid;
		int result = posix_spawn(&pid, executable.c_str(), nullptr, nullptr, argv, environ);
		if (result)
			throw std::system_error(result, std::generic_category(), "Cannot start lh-recorder");
		// Startup handshake only. No runtime IPC, config watcher, or status polling.
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(7);
		while (!std::filesystem::exists(session / "ready.json"))
		{
			int status;
			if (waitpid(pid, &status, WNOHANG) == pid)
				throw std::runtime_error("Lighthouse recorder initialization failed; see session logs");
			if (std::chrono::steady_clock::now() >= deadline)
			{
				kill(pid, SIGTERM);
				throw std::runtime_error("Lighthouse recorder initialization timed out");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		setenv("LH_RECORDER_OUTPUT", output.c_str(), true);
		setenv("LH_RECORDER_ENABLE", "1", true);
		std::cerr << "Lighthouse recording session: " << output << '\n';
	}
	catch (const std::exception & error)
	{
		std::cerr << "Lighthouse recording unavailable: " << error.what() << '\n';
	}
#endif
}
} // namespace wivrn
