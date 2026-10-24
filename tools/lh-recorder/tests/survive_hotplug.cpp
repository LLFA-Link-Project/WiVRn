// SPDX-License-Identifier: BSL-1.0
// Exercise the real Monado adapter with a synthetic libsurvive event source.
#include "math/m_relation_history.h"
#include "os/os_time.h"
#include "survive_api.h"
#include "survive_interface.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_system.h"
#include "survive_internal.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

struct SurviveSimpleContext
{};
struct SurviveSimpleObject
{
	const char * config;
	SurviveSimpleObject_type type = SurviveSimpleObject_OBJECT;
};
static SurviveSimpleContext context;
static std::mutex events_mutex;
static std::deque<SurviveSimpleEvent> events;
static unsigned closed = 0;

extern "C"
{
	SurviveSimpleContext * survive_simple_init(int, char * const *)
	{
		return &context;
	}
	void survive_simple_start_thread(SurviveSimpleContext *) {}
	void survive_simple_close(SurviveSimpleContext *)
	{
		++closed;
	}
	const SurviveSimpleObject * survive_simple_get_first_object(SurviveSimpleContext *)
	{
		return nullptr;
	}
	const SurviveSimpleObject * survive_simple_get_next_object(SurviveSimpleContext *, const SurviveSimpleObject *)
	{
		return nullptr;
	}
	SurviveSimpleObject_type survive_simple_object_get_type(const SurviveSimpleObject * object)
	{
		return object->type;
	}
	const char * survive_simple_object_name(const SurviveSimpleObject *)
	{
		return "synthetic";
	}
	const char * survive_simple_json_config(const SurviveSimpleObject * object)
	{
		return object->config;
	}
	SurviveSimpleEventType survive_simple_next_event(SurviveSimpleContext *, SurviveSimpleEvent * event)
	{
		std::lock_guard lock(events_mutex);
		if (events.empty())
			return event->event_type = SurviveSimpleEventType_None;
		*event = events.front();
		events.pop_front();
		return event->event_type;
	}
}

static void require(bool condition, const char * message)
{
	if (!condition)
	{
		std::fprintf(stderr, "FAIL: %s\n", message);
		std::abort();
	}
}
static void enqueue(SurviveSimpleEvent event)
{
	std::lock_guard lock(events_mutex);
	events.push_back(event);
}
static void config(SurviveSimpleObject & object)
{
	SurviveSimpleEvent event{};
	event.event_type = SurviveSimpleEventType_ConfigEvent;
	event.d.__private_config_event.object = &object;
	enqueue(event);
}
static void pose(SurviveSimpleObject & object)
{
	SurviveSimpleEvent event{};
	event.event_type = SurviveSimpleEventType_PoseUpdateEvent;
	event.d.__private_pose_event.object = &object;
	event.d.__private_pose_event.time = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
	event.d.__private_pose_event.pose.Rot[0] = 1;
	enqueue(event);
}
static void wait_for(auto condition)
{
	for (int i = 0; i < 1000; ++i)
	{
		if (condition())
			return;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	require(false, "timed out waiting for event processing");
}
struct Publication
{
	xrt_system_devices * system;
	std::atomic<unsigned> count{0};
};
static void added(xrt_device * device, void * userdata)
{
	auto & publication = *static_cast<Publication *>(userdata);
	auto & system = *publication.system;
	for (size_t i = 0; i < __atomic_load_n(&system.static_xdev_count, __ATOMIC_ACQUIRE); ++i)
		require(system.static_xdevs[i] != device, "callback precedes publication");
	++publication.count;
}
static int test_prediction_limit(double expected_seconds)
{
	SurviveSimpleObject left{R"({"model_number":"Knuckles Left","device_serial_number":"LHR-LEFT"})"};
	SurviveSimpleObject tracker{R"({"model_number":"VIVE Tracker 3.0 MV","device_serial_number":"LHR-TRACKER"})"};
	config(left);
	config(tracker);
	xrt_system_devices system{};
	auto * survive = survive_create_devices(&system);
	require(survive != nullptr && system.static_xdev_count == 2, "prediction fixtures created");
	Publication publication{&system};
	survive_start(survive, added, &publication);
	double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
	for (int sample = 0; sample < 2; ++sample)
		for (auto * object: {&left, &tracker})
		{
			SurviveSimpleEvent event{};
			event.event_type = SurviveSimpleEventType_PoseUpdateEvent;
			auto & p = event.d.__private_pose_event;
			p.object = object;
			p.time = now - 0.02 + sample * 0.01;
			p.pose.Rot[0] = 1;
			p.velocity.Pos[0] = 1; // 1 m/s along X: distance equals prediction seconds.
			enqueue(event);
		}
	for (size_t i = 0; i < system.static_xdev_count; ++i)
	{
		auto * device = system.static_xdevs[i];
		auto * adapter = reinterpret_cast<survive_device *>(device);
		wait_for([&] { return m_relation_history_get_size(adapter->relation_hist) == 2; });
		int64_t latest_ns;
		xrt_space_relation latest{};
		require(m_relation_history_get_latest(adapter->relation_hist, &latest_ns, &latest), "latest sample available");
		auto input = device->device_type == XRT_DEVICE_TYPE_GENERIC_TRACKER ? XRT_INPUT_GENERIC_TRACKER_POSE : XRT_INPUT_INDEX_GRIP_POSE;
		xrt_space_relation base{}, predicted{};
		xrt_device_get_tracked_pose(device, input, latest_ns, &base);
		xrt_device_get_tracked_pose(device, input, latest_ns + 1'000'000'000, &predicted);
		require(std::abs(predicted.pose.position.x - base.pose.position.x - expected_seconds) < 1e-5,
		        "controller and tracker prediction use the shared limit");
		if (device->supported.hand_tracking)
		{
			xrt_hand_joint_set base_hand{}, predicted_hand{};
			int64_t timestamp;
			xrt_device_get_hand_tracking(device, XRT_INPUT_HT_CONFORMING_LEFT, latest_ns, &base_hand, &timestamp);
			xrt_device_get_hand_tracking(device, XRT_INPUT_HT_CONFORMING_LEFT, latest_ns + 1'000'000'000, &predicted_hand, &timestamp);
			require(base_hand.is_active && predicted_hand.is_active, "prediction hand fixtures active");
			require(std::abs(predicted_hand.hand_pose.pose.position.x - base_hand.hand_pose.pose.position.x - expected_seconds) < 1e-5,
			        "emulated hand prediction uses the same limit");
		}
	}
	survive_stop(survive);
	for (auto & device: system.static_xdevs)
		xrt_device_destroy(&device);
	survive_destroy(survive);
	std::puts("libsurvive controller, tracker and emulated hand prediction limit: passed");
	return 0;
}

int main(int argc, char ** argv)

{
	xrt_system_devices system{};
	auto * survive = survive_create_devices(&system);
	require(survive != nullptr && system.static_xdev_count == 0, "empty startup keeps discovery alive");
	Publication publication{&system};
	survive_start(survive, added, &publication);
	auto count = [&] { return __atomic_load_n(&system.static_xdev_count, __ATOMIC_ACQUIRE); };
	SurviveSimpleObject left{R"({"model_number":"Knuckles Left","device_serial_number":"LHR-LEFT"})"};
	SurviveSimpleObject right{R"({"model_number":"Knuckles Right","device_serial_number":"LHR-RIGHT"})"};
	SurviveSimpleObject tracker{R"({"model_number":"VIVE Tracker 3.0 MV","device_serial_number":"LHR-TRACKER"})"};
	config(left);
	config(right);
	config(tracker);
	wait_for([&] { return count() == 3; });
	auto * left_device = system.static_xdevs[0];
	pose(left);
	pose(right);
	xrt_system_roles roles{};
	wait_for([&] {
		survive_get_roles(survive, &roles);
		return roles.left == 0 && roles.right == 1;
	});
	require(roles.left_profile == XRT_DEVICE_INDEX_CONTROLLER, "controller interaction profile");
	xrt_device_update_inputs(left_device);
	require(left_device->inputs[0].active, "live inputs active");
	xrt_space_relation relation{};
	xrt_device_get_tracked_pose(left_device, XRT_INPUT_INDEX_GRIP_POSE, os_monotonic_get_ns(), &relation);
	require(relation.relation_flags != 0, "live pose available");
	std::this_thread::sleep_for(std::chrono::milliseconds(2100));
	survive_get_roles(survive, &roles);
	require(roles.left < 0 && roles.right < 0, "silent controllers release roles to headset");
	xrt_device_update_inputs(left_device);
	require(!left_device->inputs[0].active, "silent inputs inactive");
	xrt_device_get_tracked_pose(left_device, XRT_INPUT_INDEX_GRIP_POSE, os_monotonic_get_ns(), &relation);
	require(relation.relation_flags == 0, "silent pose invalidated");
	xrt_hand_joint_set hand{};
	int64_t timestamp;
	xrt_device_get_hand_tracking(left_device, XRT_INPUT_HT_CONFORMING_LEFT, os_monotonic_get_ns(), &hand, &timestamp);
	require(!hand.is_active, "silent hand tracking inactive");
	SurviveSimpleObject reconnected = left;
	config(reconnected);
	config(reconnected);
	pose(reconnected);
	wait_for([&] {
		survive_get_roles(survive, &roles);
		return roles.left == 0;
	});
	require(count() == 3 && publication.count == 3, "repeated config and replug reuse identity");
	require(system.static_xdevs[0] == left_device, "OpenXR device pointer remains stable");
	xrt_device_update_inputs(left_device);
	require(left_device->inputs[0].active, "reconnected inputs active");
	survive_stop(survive);
	auto start = std::chrono::steady_clock::now();
	survive_stop(survive);
	for (size_t i = 0; i < count(); ++i)
		xrt_device_destroy(&system.static_xdevs[i]);
	survive_destroy(survive);
	require(closed == 1, "context closes once after all devices");
	require(std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "shutdown with no queued events is bounded");
	// Startup devices are exposed before publication starts. Shutdown must also
	// work when the owner never starts event processing (session setup failure).
	xrt_system_devices startup_system{};
	config(tracker);
	auto * startup = survive_create_devices(&startup_system);
	require(startup != nullptr && startup_system.static_xdev_count == 1, "startup device published");
	xrt_device_destroy(&startup_system.static_xdevs[0]);
	survive_destroy(startup);
	require(closed == 2, "unstarted system closes after device destruction");
	std::puts("libsurvive empty startup, live publication, roles, inactivity, reconnect and shutdown: passed");
}
