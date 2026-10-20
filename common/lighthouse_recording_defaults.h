// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdlib>
#include <filesystem>
namespace wivrn
{
inline std::filesystem::path expand_lighthouse_recording_directory(const std::filesystem::path & input)
{
	auto text = input.string();
	if (text == "~" || text.starts_with("~/"))
	{
		if (const char * home = std::getenv("HOME"); home && std::filesystem::path(home).is_absolute())
			return text == "~" ? std::filesystem::path(home) : std::filesystem::path(home) / text.substr(2);
		return {};
	}
	return input;
}
inline std::filesystem::path default_lighthouse_recording_directory()
{
	if (const char * data = std::getenv("XDG_DATA_HOME"); data && std::filesystem::path(data).is_absolute())
		return std::filesystem::path(data) / "wivrn" / "lighthouse-recordings";
	if (const char * home = std::getenv("HOME"))
		return std::filesystem::path(home) / ".local/share/wivrn/lighthouse-recordings";
	return {}; // No guessed home directory.
}
} // namespace wivrn
