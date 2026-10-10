// SPDX-License-Identifier: GPL-3.0-or-later
#include "local_keyboard.h"
#include <array>
#include <bitset>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <linux/input.h>
#include <spdlog/spdlog.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace wivrn
{
struct local_keyboard::device
{
	int fd;
	std::string path;
	std::bitset<KEY_CNT> pressed;
	std::bitset<KEY_CNT> suppressed;
	bool syncing = false;
	~device()
	{
		close(fd);
	}
};

static std::bitset<KEY_CNT> held_keys(int fd)
{
	std::array<unsigned char, (KEY_CNT + 7) / 8> bits{};
	// Treat a failed snapshot conservatively: require release before any new press.
	if (ioctl(fd, EVIOCGKEY(bits.size()), bits.data()) < 0)
		return std::bitset<KEY_CNT>{}.set();
	std::bitset<KEY_CNT> result;
	for (unsigned key = 0; key < KEY_CNT; ++key)
		result[key] = bits[key / 8] & (1u << (key % 8));
	return result;
}

local_keyboard::local_keyboard()
{
	if (const char * path = std::getenv("WIVRN_UI_KEYBOARD"))
		selected_path = path;
}
local_keyboard::~local_keyboard() = default;

bool local_keyboard::key_down(unsigned code) const
{
	if (code >= KEY_CNT)
		return false;
	for (const auto & input: devices)
		if (input->pressed[code])
			return true;
	return false;
}

void local_keyboard::scan()
{
	std::vector<std::string> paths;
	if (not selected_path.empty())
		paths.push_back(selected_path);
	else
	{
		std::error_code error;
		std::filesystem::directory_iterator it("/dev/input", error), end;
		while (not error and it != end)
		{
			if (it->path().filename().string().starts_with("event"))
				paths.push_back(it->path().string());
			it.increment(error);
		}
	}
	for (const auto & path: paths)
	{
		bool known = false;
		for (const auto & input: devices)
			known |= input->path == path;
		if (known)
			continue;
		int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
		{
			if ((errno == EACCES or errno == EPERM) and not permission_reported)
			{
				spdlog::warn("Local UI keyboard cannot read {}: grant the client user read access to the keyboard evdev node", path);
				permission_reported = true;
			}
			continue;
		}
		std::array<unsigned char, (KEY_CNT + 7) / 8> bits{};
		auto has = [&](unsigned code) { return bits[code / 8] & (1u << (code % 8)); };
		if (ioctl(fd, EVIOCGBIT(EV_KEY, bits.size()), bits.data()) < 0 or not has(KEY_SPACE) or not has(KEY_ENTER))
		{
			close(fd);
			continue;
		}
		auto input = std::make_unique<device>();
		input->fd = fd;
		input->path = path;
		// Starting/resuming with a key held must not click or type unexpectedly.
		input->suppressed = held_keys(fd);
		devices.push_back(std::move(input));
		spdlog::info("Local UI keyboard: {} (nonexclusive evdev input)", path);
	}
}

void local_keyboard::release(device & input, std::vector<key_event> & events)
{
	const auto pressed = input.pressed;
	input.pressed.reset();
	for (unsigned code = 0; code < KEY_CNT; ++code)
		if (pressed[code] and not key_down(code))
			events.push_back({code, 0});
}

std::vector<local_keyboard::key_event> local_keyboard::poll()
{
	std::vector<key_event> result;
	const auto now = std::chrono::steady_clock::now();
	if (now >= next_scan)
	{
		scan();
		next_scan = now + std::chrono::seconds(2);
	}
	for (auto it = devices.begin(); it != devices.end();)
	{
		auto & input = **it;
		bool disconnected = false;
		for (unsigned batch = 0; batch < 128; ++batch)
		{
			std::array<input_event, 32> events;
			ssize_t size = read(input.fd, events.data(), sizeof(events));
			if (size < 0 and errno == EINTR)
				continue;
			if (size < 0 and (errno == EAGAIN or errno == EWOULDBLOCK))
				break;
			if (size <= 0 or size % sizeof(input_event) != 0)
			{
				disconnected = true;
				break;
			}
			for (size_t index = 0; index < size / sizeof(input_event); ++index)
			{
				const auto & event = events[index];
				if (event.type == EV_SYN and event.code == SYN_DROPPED)
				{
					result.push_back({input_lost, 0});
					release(input, result);
					input.syncing = true;
					continue;
				}
				if (input.syncing)
				{
					if (event.type == EV_SYN and event.code == SYN_REPORT)
					{
						input.suppressed = held_keys(input.fd);
						input.syncing = false;
					}
					continue;
				}
				if (event.type != EV_KEY or event.code >= KEY_CNT or event.value < 0 or event.value > 2)
					continue;
				const auto code = event.code;
				if (input.suppressed[code])
				{
					if (event.value == 0)
						input.suppressed[code] = false;
					continue;
				}
				if (event.value == 2)
				{
					if (input.pressed[code])
						result.push_back({code, 2});
					continue;
				}
				const bool before = key_down(code);
				input.pressed[code] = event.value == 1;
				if (before != key_down(code))
					result.push_back({code, event.value});
			}
		}
		if (disconnected)
		{
			result.push_back({input_lost, 0});
			release(input, result);
			it = devices.erase(it);
		}
		else
			++it;
	}
	return result;
}
} // namespace wivrn
