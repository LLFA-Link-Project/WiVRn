// SPDX-License-Identifier: GPL-3.0-or-later
#include "driver/external_head.h"
#include "os/os_time.h"
#include "steamvr_lh_interface.h"
#include "xrt/xrt_system.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <dlfcn.h>
#include <iostream>
#include <string>
#include <thread>

void check(bool condition, const char * message)
{
	if (!condition)
	{
		std::cerr << message << '\n';
		std::exit(1);
	}
}
int main(int argc, char ** argv)
{
	check(argc == 4, "usage: test mock-root plugin-path selected|auto|wrong|normal");
	setenv("STEAMVR_PATH", argv[1], true);
	setenv("LH_DISCOVER_WAIT_MS", "30", true);
	setenv("LH_LOAD_SLIMEVR", "0", true);
	const std::string mode = argv[3];
	if (mode == "auto")
		setenv("LH_TRACKING_ONLY_HMD_SERIAL", "*", true);
	else if (mode == "selected")
		setenv("LH_TRACKING_ONLY_HMD_SERIAL", "TEST-LIGHTHOUSE-HMD", true);
	else if (mode == "wrong")
		setenv("LH_TRACKING_ONLY_HMD_SERIAL", "OTHER-HMD", true);
	else
		unsetenv("LH_TRACKING_ONLY_HMD_SERIAL");
	void * library = dlopen(argv[2], RTLD_NOW);
	check(library, "load mock driver");
	auto state = reinterpret_cast<unsigned (*)()>(dlsym(library, "llfa_mock_state"));
	auto tracking = reinterpret_cast<void (*)(bool)>(dlsym(library, "llfa_mock_tracking"));
	check(state && tracking, "mock controls");
	xrt_system_devices devices{};
	check(steamvr_lh_create_devices(nullptr, &devices) == XRT_SUCCESS, "driver discovery");
	if (mode == "wrong" || mode == "normal")
	{
		check(devices.static_xdev_count == 0, "unselected or display-less normal HMD must not be returned");
		check(mode != "wrong" || (state() & 15) == 0, "wrong serial must never activate");
		check(mode != "normal" || ((state() >> 8) & 15) == 1, "default behavior still requires display");
		dlclose(library);
		return 0;
	}
	check(devices.static_xdev_count == 1, "one selected HMD");
	auto * device = devices.static_xdevs[0];
	check(device->device_type == XRT_DEVICE_TYPE_GENERIC_TRACKER && device->hmd == nullptr, "tracking-only source has no fake display");
	check(!device->supported.compositor_info && !device->get_view_poses, "no display callbacks");
	check(((state() >> 8) & 15) == 0, "tracking-only mode must not request display component");
	int64_t observed_ns{};
	xrt_space_relation observed{};
	check(steamvr_lh_get_latest_pose(device, &observed_ns, &observed), "actual pose history");
	const auto age = os_monotonic_get_ns() - observed_ns;
	check(age >= 20'000'000 && age < 100'000'000, "OpenVR negative seconds offset must map to monotonic nanoseconds");
	check(std::abs(observed.pose.position.x - 1.1f) < 1e-5f, "driver head transform applied exactly once");
	wivrn::external_head head(*device, XRT_INPUT_GENERIC_HEAD_POSE, steamvr_lh_get_latest_pose);
	auto now = os_monotonic_get_ns();
	auto pose = head.get_pose(now + 20'000'000, now);
	check(pose.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT, "tracked 6DoF reaches external head");
	check(std::abs(pose.pose.position.x - 1.1f) < 1e-5f, "head reference retained");
	tracking(false);
	for (int i = 0; i < 20; ++i)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		now = os_monotonic_get_ns();
		pose = head.get_pose(now, now);
		if (!pose.relation_flags)
			break;
	}
	check(!pose.relation_flags, "Lighthouse loss must invalidate independent head tracking");
	tracking(true);
	for (int i = 0; i < 20; ++i)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		now = os_monotonic_get_ns();
		pose = head.get_pose(now, now);
		if (pose.relation_flags)
			break;
	}
	check(pose.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT, "Lighthouse tracking reacquisition");
	xrt_device_destroy(&devices.static_xdevs[0]);
	check(((state() >> 4) & 15) == 1 && ((state() >> 12) & 15) == 1, "deactivation and provider cleanup");
	// A new session must construct independent history and clean it up again.
	devices.static_xdev_count = 0;
	check(steamvr_lh_create_devices(nullptr, &devices) == XRT_SUCCESS && devices.static_xdev_count == 1, "rediscovery after teardown");
	check(steamvr_lh_get_latest_pose(devices.static_xdevs[0], &observed_ns, &observed), "new session history");
	xrt_device_destroy(&devices.static_xdevs[0]);
	check((state() & 15) == 2 && ((state() >> 4) & 15) == 2 && ((state() >> 12) & 15) == 2, "repeated session cleanup");
	dlclose(library);
	std::cout << "Lighthouse provider integration test passed\n";
}
