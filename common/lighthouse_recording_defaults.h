// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdlib>
#include <filesystem>
namespace wivrn
{
inline std::filesystem::path default_lighthouse_recording_directory()
{
	if (const char * data = std::getenv("XDG_DATA_HOME"); data && std::filesystem::path(data).is_absolute())
		return std::filesystem::path(data) / "wivrn" / "lighthouse-recordings";
	if (const char * home = std::getenv("HOME"))
		return std::filesystem::path(home) / ".local/share/wivrn/lighthouse-recordings";
	return {}; // No guessed home directory.
}
} // namespace wivrn
