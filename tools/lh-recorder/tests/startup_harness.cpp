// SPDX-License-Identifier: GPL-3.0-or-later
#include "lighthouse_capture.h"
#include "lighthouse_recording.h"
std::unique_ptr<wivrn::lighthouse::usb_capture> make_synthetic_capture();
std::unique_ptr<wivrn::lighthouse::usb_capture> make_denied_capture();
unsigned synthetic_capture_instances();
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
	if (argc < 2 || argc > 3)
		return 2;
	wivrn::configuration::set_config_file(argv[1]);
	const wivrn::configuration snapshot;
	wivrn::lighthouse::set_test_capture_factory(argc == 3 && std::string_view(argv[2]) == "real" ? nullptr : argc == 3 && std::string_view(argv[2]) == "deny" ? make_denied_capture
	                                                                                                                                                          : make_synthetic_capture);
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
		if (i == 60)
			wivrn::start_lighthouse_recording(wivrn::configuration()); // ignored for this lifetime
		lighthouse_reference::pose("synthetic-tracker", 1, p, lighthouse_reference::capture_time());
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	// Explicit shutdown mirrors the server's normal and exception exit paths.
	wivrn::stop_lighthouse_recording(argc == 3 && std::string_view(argv[2]) == "abnormal" ? 1 : 0);
	if (synthetic_capture_instances() != 0)
		throw std::runtime_error("USB capture remains open after shutdown");
	std::cout << (snapshot.lighthouse_recording_enabled ? "enabled" : "disabled") << " " << snapshot.lighthouse_recording_output << '\n';
}
