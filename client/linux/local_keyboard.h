// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace wivrn
{
// evdev is read only while the local lobby has OpenXR input focus. No EVIOCGRAB.
class local_keyboard
{
public:
	static constexpr unsigned input_lost = ~0u;
	struct key_event
	{
		unsigned code;
		int value; // 0: release, 1: press, 2: repeat (never a new mouse press)
	};
	local_keyboard();
	~local_keyboard();
	std::vector<key_event> poll();
	bool key_down(unsigned code) const;

private:
	struct device;
	std::vector<std::unique_ptr<device>> devices;
	std::chrono::steady_clock::time_point next_scan{};
	std::string selected_path;
	bool permission_reported = false;
	void scan();
	void release(device & input, std::vector<key_event> & events);
};
} // namespace wivrn
