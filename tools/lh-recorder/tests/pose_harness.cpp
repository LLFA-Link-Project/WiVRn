// SPDX-License-Identifier: BSL-1.0
#include "reference_recorder.hpp"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <locale>
#include <sys/resource.h>
#include <thread>
class CommaLocale : public std::numpunct<char>
{
	char do_decimal_point() const override
	{
		return ',';
	}
	char do_thousands_sep() const override
	{
		return '.';
	}
	std::string do_grouping() const override
	{
		return "\3";
	}
};

int main(int argc, char ** argv)
{
	if (std::getenv("LH_TEST_COMMA_LOCALE"))
		std::locale::global(std::locale(std::locale::classic(), new CommaLocale));
	std::cout.imbue(std::locale::classic());
	using namespace lighthouse_reference;
	initialize();
	device("tracker-A", vr::TrackedDeviceClass_GenericTracker);
	device("controller-B", vr::TrackedDeviceClass_Controller);
	vr::DriverPose_t p{};
	p.qRotation.w = p.qWorldFromDriverRotation.w = p.qDriverFromHeadRotation.w = 1;
	p.poseIsValid = p.deviceIsConnected = true;
	p.result = vr::TrackingResult_Running_OK;
	p.poseTimeOffset = -0.004;
	p.vecPosition[0] = 1.25;
	int count = argc > 1 ? std::atoi(argv[1]) : 30;
	auto begin = std::chrono::steady_clock::now();
	for (int i = 0; i < count; ++i)
	{
		if (i == 5)
		{
			p.poseIsValid = false;
			p.result = vr::TrackingResult_Running_OutOfRange;
		}
		if (i == 10)
		{
			p.poseIsValid = true;
			p.result = vr::TrackingResult_Running_OK;
		}
		if (i == 15)
			p.deviceIsConnected = false;
		if (i == 20)
			p.deviceIsConnected = true;
		if (i == 25)
		{
			p.poseIsValid = false;
			p.vecPosition[1] = NAN;
			p.vecAngularVelocity[0] = INFINITY;
		}
		if (i == 26)
		{
			p.poseIsValid = true;
			p.vecPosition[1] = 0;
			p.vecAngularVelocity[0] = 0;
		}
		pose(i % 2 ? "tracker-A" : "controller-B", i % 2 + 1, p, capture_time());
		if (count < 100)
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
	rusage usage{};
	getrusage(RUSAGE_SELF, &usage);
	std::cout << "{\"samples\":" << count << ",\"callback_total_ns\":" << elapsed << ",\"peak_rss_kib\":" << usage.ru_maxrss << "}\n";
	// The destructor joins the writer and flushes all accepted updates.
}
