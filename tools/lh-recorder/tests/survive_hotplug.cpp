// SPDX-License-Identifier: BSL-1.0
// Exercise the real Monado adapter with a synthetic libsurvive event source.
#include "math/m_api.h"
#include "math/m_relation_history.h"
#include "math/m_space.h"
#include "os/os_time.h"
#include "survive_api.h"
#include "survive_interface.h"
#include "survive_internal.h"
#include "survive_recording.h"
#include "vive/vive_poses.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_system.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

struct SurviveSimpleContext
{};
struct SurviveSimpleObject
{
	const char * config;
	SurviveSimpleObject_type type = SurviveSimpleObject_OBJECT;
	SurvivePose head_in_imu = [] { SurvivePose p{}; p.Rot[0] = 1; return p; }();
};
static SurviveSimpleContext context;
static std::mutex events_mutex;
static std::deque<SurviveSimpleEvent> events;
static unsigned closed = 0;

extern "C"
{
	SurviveSimpleContext * survive_simple_init(int argc, char * const * argv)
	{
		bool imu_poses = false;
		for (int i = 1; i + 1 < argc; ++i)
			if (std::strcmp(argv[i], "--report-in-imu") == 0 && std::strcmp(argv[i + 1], "1") == 0)
				imu_poses = true;
		if (!imu_poses)
		{
			std::fprintf(stderr, "FAIL: request IMU poses to match IMU velocities\n");
			std::abort();
		}
		if (survive_recording_enabled())
		{
			bool native = false, timing = false, state = false;
			for (int i = 1; i + 1 < argc; ++i)
			{
				if (!std::strcmp(argv[i], "--record"))
					native = std::strstr(argv[i + 1], "native.rec.gz") != nullptr;
				if (!std::strcmp(argv[i], "--record-pose-timing"))
					timing = !std::strcmp(argv[i + 1], "1");
				if (!std::strcmp(argv[i], "--record-tracking-state"))
					state = !std::strcmp(argv[i + 1], "1");
			}
			if (!native || !timing || !state)
				std::abort();
		}
		return &context;
	}
	void survive_simple_object_get_transform_to_imu(const SurviveSimpleObject * object, SurvivePose * local)
	{
		*local = object->head_in_imu;
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
	SurviveSimpleObject right{R"({"model_number":"Knuckles Right","device_serial_number":"LHR-RIGHT"})"};
	SurviveSimpleObject tracker{R"({"model_number":"VIVE Tracker 3.0 MV","device_serial_number":"LHR-TRACKER"})"};
	// Model libsurvive's device-origin transform, which its default pose output
	// applies even though the reported linear velocity remains at the IMU.
	for (auto * object: {&left, &right, &tracker})
	{
		object->head_in_imu.Pos[0] = 0.08;
		object->head_in_imu.Pos[1] = -0.04;
		object->head_in_imu.Pos[2] = 0.03;
		object->head_in_imu.Rot[0] = std::cos(0.3);
		object->head_in_imu.Rot[3] = std::sin(0.3);
	}
	config(left);
	config(right);
	config(tracker);
	xrt_system_devices system{};
	auto * survive = survive_create_devices(&system);
	require(survive != nullptr && system.static_xdev_count == 3, "prediction fixtures created");
	Publication publication{&system};
	survive_start(survive, added, &publication);
	double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
	for (int sample = 0; sample < 2; ++sample)
		for (auto * object: {&left, &right, &tracker})
		{
			SurviveSimpleEvent event{};
			event.event_type = SurviveSimpleEventType_PoseUpdateEvent;
			auto & p = event.d.__private_pose_event;
			p.object = object;
			p.time = now - 0.02 + sample * 0.01;
			p.pose.Rot[0] = 1;
			p.pose.Pos[0] = 0.123456789012; // Preserve native double precision before Monado float conversion.
			p.velocity.Pos[0] = 1;          // 1 m/s along X: distance equals prediction seconds.
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
		int64_t resolved = 0, horizon = 0;
		xrt_space_relation diagnostic{};
		m_relation_history_get_clamped_with_prediction(adapter->relation_hist, latest_ns + 1'000'000'000, expected_seconds == 1 ? INT64_MAX : int64_t(expected_seconds * 1e9), &resolved, &horizon, &diagnostic);
		require(std::abs(horizon - int64_t(expected_seconds * 1e9)) <= 1, "exact diagnostic horizon under history lock");
		require(resolved == latest_ns + horizon, "resolved timestamp follows source sample, including clamp zero");
		m_relation_history_get_clamped_with_prediction(adapter->relation_hist, latest_ns - 1'000'000'000, 0, &resolved, &horizon, &diagnostic);
		require(horizon == 0, "zero clamp disables reverse extrapolation too");
		xrt_device_get_tracked_pose(device, input, latest_ns, &base);
		xrt_device_get_tracked_pose(device, input, latest_ns + 1'000'000'000, &predicted);
		require(std::abs(predicted.pose.position.x - base.pose.position.x - expected_seconds) < 1e-5,
		        "controller and tracker prediction use the shared limit");
		if (device->supported.hand_tracking)
		{
			xrt_hand_joint_set base_hand{}, predicted_hand{};
			int64_t timestamp;
			auto hand_input = device->device_type == XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER ? XRT_INPUT_HT_CONFORMING_RIGHT : XRT_INPUT_HT_CONFORMING_LEFT;
			xrt_device_get_hand_tracking(device, hand_input, latest_ns, &base_hand, &timestamp);
			xrt_device_get_hand_tracking(device, hand_input, latest_ns + 1'000'000'000, &predicted_hand, &timestamp);
			require(base_hand.is_active && predicted_hand.is_active, "prediction hand fixtures active");
			require(std::abs(predicted_hand.hand_pose.pose.position.x - base_hand.hand_pose.pose.position.x - expected_seconds) < 1e-5,
			        "emulated hand prediction uses the same limit");
		}
	}
	// Regression for steamvr_lh's raw-reference-first issue: a rotating offset
	// must follow an arc, rather than extending its initial tangent linearly.
	// Feed events through the real adapter, including its coordinate conversion.
	for (auto * object: {&left, &right, &tracker})
	{
		SurviveSimpleEvent event{};
		event.event_type = SurviveSimpleEventType_PoseUpdateEvent;
		auto & p = event.d.__private_pose_event;
		p.object = object;
		p.time = now;
		p.pose.Rot[0] = 1;
		p.velocity.Pos[0] = 1;
		p.velocity.AxisAngleRot[2] = 20; // Monado world +Y, 20 rad/s.
		enqueue(event);
	}
	for (size_t i = 0; i < system.static_xdev_count; ++i)
	{
		auto * device = system.static_xdevs[i];
		auto * adapter = reinterpret_cast<survive_device *>(device);
		wait_for([&] { return m_relation_history_get_size(adapter->relation_hist) == 3; });
		int64_t latest_ns;
		xrt_space_relation raw{};
		require(m_relation_history_get_latest(adapter->relation_hist, &latest_ns, &raw), "rotating raw sample available");
		auto check = [&](xrt_input_name input, const xrt_space_relation & result, const xrt_pose * extra = nullptr) {
			xrt_pose offset = XRT_POSE_IDENTITY;
			vive_poses_get_pose_offset(device->name, device->device_type, input, &offset);
			if (extra)
				math_pose_transform(&offset, extra, &offset);
			xrt_pose head{{0, 0, float(std::sin(0.3)), float(std::cos(0.3))}, {0.08f, -0.04f, 0.03f}};
			math_pose_transform(&head, &offset, &offset);
			xrt_vec3 r0{};
			math_quat_rotate_vec3(&raw.pose.orientation, &offset.position, &r0);
			const double angle = 20 * expected_seconds;
			xrt_quat turn{0, float(std::sin(angle / 2)), 0, float(std::cos(angle / 2))};
			xrt_quat predicted_raw{}, expected_orientation{};
			math_quat_rotate(&turn, &raw.pose.orientation, &predicted_raw);
			math_quat_rotate(&predicted_raw, &offset.orientation, &expected_orientation);
			math_quat_normalize(&expected_orientation);
			const auto & q = result.pose.orientation;
			const double dot = q.x * expected_orientation.x + q.y * expected_orientation.y +
			                   q.z * expected_orientation.z + q.w * expected_orientation.w;
			require(std::abs(std::abs(dot) - 1) < 1e-5, "offset orientation follows the predicted raw reference");
			const double rx = std::cos(angle) * r0.x + std::sin(angle) * r0.z;
			const double rz = -std::sin(angle) * r0.x + std::cos(angle) * r0.z;
			require(std::abs(result.pose.position.x - (raw.pose.position.x + expected_seconds + rx)) < 1e-5 &&
			                std::abs(result.pose.position.y - (raw.pose.position.y + r0.y)) < 1e-5 &&
			                std::abs(result.pose.position.z - (raw.pose.position.z + rz)) < 1e-5,
			        "predict raw reference before composing rotating offset");
			require(std::abs(result.linear_velocity.x - (1 + 20 * rz)) < 1e-4 &&
			                std::abs(result.linear_velocity.y) < 1e-4 &&
			                std::abs(result.linear_velocity.z + 20 * rx) < 1e-4,
			        "offset velocity uses the predicted orientation");
		};
		const int64_t requested_ns = latest_ns + 1'000'000'000;
		if (device->supported.hand_tracking)
		{
			for (auto input: {XRT_INPUT_INDEX_GRIP_POSE, XRT_INPUT_INDEX_AIM_POSE})
			{
				xrt_space_relation result{};
				xrt_device_get_tracked_pose(device, input, requested_ns, &result);
				check(input, result);
			}
			auto input = device->device_type == XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER ? XRT_INPUT_HT_CONFORMING_RIGHT : XRT_INPUT_HT_CONFORMING_LEFT;
			xrt_hand_joint_set result{};
			int64_t timestamp;
			const float side = device->device_type == XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER ? 1.f : -1.f;
			xrt_pose canonical_palm{{-0.46f, side * 0.02f, side * 0.01f, 0.89f}, {side * 0.015f, 0, 0.001f}};
			math_quat_normalize(&canonical_palm.orientation);
			xrt_pose open_wrist{};
			xrt_vec3 open_tip{};
			for (float curl: {0.f, 0.5f, 1.f})
			{
				for (auto & finger: adapter->ctrl.curl)
					finger = curl;
				xrt_device_get_hand_tracking(device, input, requested_ns, &result, &timestamp);
				require(result.is_active && timestamp == requested_ns, "rotating hand active at requested time");
				const auto & joints = result.values.hand_joint_set_default;
				// Undo the requested mirrored wrist roll to check that the
				// preceding canonical placement and wrist position are retained.
				xrt_space_relation unrolled = result.hand_pose;
				xrt_quat undo_roll{0, 0, side * float(std::sqrt(0.5)), float(std::sqrt(0.5))};
				math_quat_rotate(&unrolled.pose.orientation, &undo_roll, &unrolled.pose.orientation);
				const xrt_vec3 forward{0, 0, -1};
				xrt_vec3 before{}, after{};
				math_quat_rotate_vec3(&unrolled.pose.orientation, &forward, &before);
				math_quat_rotate_vec3(&result.hand_pose.pose.orientation, &forward, &after);
				require(std::abs(before.x - after.x) < 1e-5 && std::abs(before.y - after.y) < 1e-5 &&
				                std::abs(before.z - after.z) < 1e-5,
				        "mirrored wrist roll preserves the finger direction");
				xrt_relation_chain chain{};
				m_relation_chain_push_pose(&chain, &joints[XRT_HAND_JOINT_PALM].relation.pose);
				m_relation_chain_push_relation(&chain, &unrolled);
				xrt_space_relation world_palm{};
				m_relation_chain_resolve(&chain, &world_palm);
				check(XRT_INPUT_INDEX_GRIP_POSE, world_palm, &canonical_palm);
				const auto & wrist = result.hand_pose.pose;
				if (curl == 0)
				{
					open_wrist = wrist;
					open_tip = joints[XRT_HAND_JOINT_MIDDLE_TIP].relation.pose.position;
				}
				else
				{
					require(std::abs(wrist.position.x - open_wrist.position.x) < 1e-5 &&
					                std::abs(wrist.position.y - open_wrist.position.y) < 1e-5 &&
					                std::abs(wrist.position.z - open_wrist.position.z) < 1e-5,
					        "finger curls do not move the wrist anchor");
					require(std::abs(joints[XRT_HAND_JOINT_MIDDLE_TIP].relation.pose.position.z - open_tip.z) > 0.001,
					        "finger curls still animate local joints");
				}
			}
		}
		else
		{
			xrt_space_relation result{};
			xrt_device_get_tracked_pose(device, XRT_INPUT_GENERIC_TRACKER_POSE, requested_ns, &result);
			check(XRT_INPUT_GENERIC_TRACKER_POSE, result);
		}
	}
	survive_stop(survive);
	for (auto & device: system.static_xdevs)
		xrt_device_destroy(&device);
	survive_destroy(survive);
	std::puts("libsurvive prediction, shared wrist placement, mirrored wrist roll and prediction limit: passed");
	return 0;
}

static int test_main(int argc, char ** argv)

{
	if (argc == 2)
		return test_prediction_limit(std::strtod(argv[1], nullptr));
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
	return test_prediction_limit(0.05);
}

int main(int argc, char ** argv)
{
	const char * directory = std::getenv("WIVRN_TEST_SURVIVE_RECORDER");
	if (directory)
		require(survive_recording_initialize(directory), "recording initializes");
	int result = 0;
	if (argc == 2 && !std::strcmp(argv[1], "--recorder-stress"))
	{
		survive_recording_sample sample{};
		std::strcpy(sample.serial, "synthetic-stress");
		sample.role = XRT_DEVICE_TYPE_GENERIC_TRACKER;
		sample.stage = "history_input";
		sample.callback_ns = os_monotonic_get_ns();
		sample.relation.pose.orientation.w = 1;
		for (int i = 0; i < 100000; ++i)
			survive_recording_submit(&sample);
	}
	else
		result = test_main(argc, argv);
	survive_recording_finish();
	return result;
}
