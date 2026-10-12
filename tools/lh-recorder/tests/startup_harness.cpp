// SPDX-License-Identifier: GPL-3.0-or-later
#include "lighthouse_recording.h"
#include "os/os_time.h"
#include "reference_recorder.hpp"
#include <stdexcept>

// Verify the steady_clock domain used by Monado's private chrono_timestamp_ns().
#include <chrono>
#include <iostream>
#include <thread>
int main(int argc, char ** argv)
{
	for (int i = 0; i < 1000; ++i)
	{
		auto before = os_monotonic_get_ns();
		auto driver = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		auto after = os_monotonic_get_ns();
		if (driver < before || driver > after)
			throw std::runtime_error("steamvr_lh chrono_timestamp_ns differs from CLOCK_MONOTONIC");
	}
	if (argc != 2)
		return 2;
	wivrn::configuration::set_config_file(argv[1]);
	const wivrn::configuration snapshot;
	wivrn::start_lighthouse_recording(snapshot);
	lighthouse_reference::device("synthetic-tracker", vr::TrackedDeviceClass_GenericTracker);
	vr::DriverPose_t p{};
	p.qRotation.w = p.qWorldFromDriverRotation.w = p.qDriverFromHeadRotation.w = 1;
	p.poseIsValid = p.deviceIsConnected = true;
	p.result = vr::TrackingResult_Running_OK;
	std::cout << "ready\n"
	          << std::flush;
	for (int i = 0; i < 120; i++)
	{
		lighthouse_reference::pose("synthetic-tracker", 1, p, lighthouse_reference::capture_time());
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	std::cout << (snapshot.lighthouse_recording_enabled ? "enabled" : "disabled") << " " << snapshot.lighthouse_recording_output << '\n';
}
