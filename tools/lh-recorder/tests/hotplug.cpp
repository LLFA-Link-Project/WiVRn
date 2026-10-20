// SPDX-License-Identifier: BSL-1.0
#include "b_hand_tracker.h"
#include "device.hpp"
#include "interfaces/context.hpp"
#include "wivrn_ipc.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_space.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sys/socket.h>
#include <thread>

static void require(bool condition, const char * message)
{
	if (!condition)
	{
		std::fprintf(stderr, "FAIL: %s\n", message);
		std::abort();
	}
}

struct Driver : vr::ITrackedDeviceServerDriver
{
	Context * context;
	bool tracker;
	std::atomic<unsigned> activations{0};
	std::atomic<unsigned> standby_calls{0};
	std::thread::id activation_thread;
	unsigned index = 0;
	Driver(Context * context, bool tracker = false) : context(context), tracker(tracker) {}
	vr::EVRInitError Activate(uint32_t new_index) override
	{
		index = new_index;
		activation_thread = std::this_thread::get_id();
		char controller_profile[] = "index_controller_profile.json";
		char tracker_profile[] = "vive_tracker_profile.json";
		auto role = tracker ? vr::TrackedControllerRole_OptOut : vr::TrackedControllerRole_LeftHand;
		vr::PropertyWrite_t props[2]{};
		props[0].prop = vr::Prop_InputProfilePath_String;
		props[0].writeType = vr::PropertyWrite_Set;
		props[0].pvBuffer = tracker ? tracker_profile : controller_profile;
		props[0].unBufferSize = std::strlen(static_cast<char *>(props[0].pvBuffer)) + 1;
		props[0].unTag = vr::k_unStringPropertyTag;
		props[1].prop = vr::Prop_ControllerRoleHint_Int32;
		props[1].writeType = vr::PropertyWrite_Set;
		props[1].pvBuffer = &role;
		props[1].unBufferSize = sizeof(role);
		props[1].unTag = vr::k_unInt32PropertyTag;
		require(context->WritePropertyBatch(index + 1, props, 2) == vr::TrackedProp_Success, "properties");
		vr::VRInputComponentHandle_t haptic;
		require(context->CreateHapticComponent(index + 1, "/output/haptic", &haptic) == vr::VRInputError_None, "haptics");
		++activations;
		return vr::VRInitError_None;
	}
	void Deactivate() override {}
	void EnterStandby() override
	{
		require(std::this_thread::get_id() == activation_thread, "standby runs on activation/frame thread");
		// This callback takes devices_mut: EnterStandby must run without that lock.
		require(context->TrackedDeviceToPropertyContainer(index) != vr::k_ulInvalidPropertyContainer, "standby callback can reach device");
		++standby_calls;
	}
	void * GetComponent(const char *) override
	{
		return nullptr;
	}
	void DebugRequest(const char *, char *, uint32_t) override {}
	vr::DriverPose_t GetPose() override
	{
		return {};
	}
};

static void wait_for(auto condition)
{
	for (int i = 0; i < 500; ++i)
	{
		if (condition())
			return;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	require(false, "timed out waiting for frame-thread publication");
}

static void test_standby_control_packet()
{
	int sockets[2];
	require(socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) == 0, "create internal control socket");
	wivrn::typed_socket<wivrn::UnixDatagram, from_monado::packets, to_monado::packets> main(sockets[0]);
	wivrn::typed_socket<wivrn::UnixDatagram, to_monado::packets, from_monado::packets> monado(sockets[1]);
	main.send(to_monado::standby_lighthouse_trackers{});
	auto packet = monado.receive();
	require(packet && std::holds_alternative<to_monado::standby_lighthouse_trackers>(*packet), "standby request crosses existing internal control socket");
}

// State notifications must survive the stale measurement timestamps seen on
// real tracker disconnects, while stale coordinates must never enter history.
static void test_tracker_pose_status(xrt_device * xdev)
{
	auto * device = static_cast<Device *>(xdev);
	constexpr int64_t second = 1000000000;
	int64_t arrival = 200 * second;
	vr::DriverPose_t pose{};
	pose.qRotation.w = pose.qWorldFromDriverRotation.w = pose.qDriverFromHeadRotation.w = 1;
	pose.deviceIsConnected = pose.poseIsValid = true;
	pose.result = vr::TrackingResult_Running_OK;
	pose.vecPosition[0] = 1;
	auto send = [&] {
		device->update_pose(pose, 0, arrival);
		arrival += second;
	};
	auto locate = [&] {
		xrt_space_relation relation{};
		require(xrt_device_get_tracked_pose(xdev, XRT_INPUT_GENERIC_TRACKER_POSE, arrival, &relation) == XRT_SUCCESS, "locate tracker pose");
		return relation;
	};
	constexpr auto valid = XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_VALID_BIT;
	constexpr auto tracked = XRT_SPACE_RELATION_POSITION_TRACKED_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT;
	send();
	require((locate().relation_flags & (valid | tracked)) == (valid | tracked), "normal tracker is valid and tracked");

	// Reject these coordinates, but propagate the current tracking state.
	pose.poseTimeOffset = -72.870883224;
	pose.vecPosition[0] = 99;
	pose.result = vr::TrackingResult_Running_OutOfRange;
	send();
	auto held = locate();
	require((held.relation_flags & valid) == valid, "tracking loss retains a valid last pose");
	require(!(held.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT), "stale tracking-loss notification clears position tracked");
	require(held.relation_flags & XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT, "rotation-only tracking retains orientation tracked");
	require(held.pose.position.x == 1, "stale tracking-loss coordinates are rejected");

	// The actual power-off recordings first reported result 101 with a valid pose.
	pose.result = vr::TrackingResult_Calibrating_OutOfRange;
	pose.poseTimeOffset = std::numeric_limits<double>::quiet_NaN();
	send();
	held = locate();
	require((held.relation_flags & valid) == valid && !(held.relation_flags & tracked), "nonfinite tracking-loss time retains validity but clears tracking");
	pose.poseTimeOffset = -72.870883224;
	pose.deviceIsConnected = false; // Even if the driver leaves poseIsValid set.
	send();
	require(locate().relation_flags == 0, "stale disconnect clears all location flags");

	pose.deviceIsConnected = true;
	pose.poseIsValid = false;
	pose.poseTimeOffset = std::numeric_limits<double>::quiet_NaN();
	send();
	require(locate().relation_flags == 0, "invalid timestamp cannot resurrect a disconnected pose");

	pose.poseIsValid = true;
	send();
	require(locate().relation_flags == 0, "reconnect without a usable measurement cannot revive the old pose");
	pose.result = vr::TrackingResult_Running_OK;
	pose.poseTimeOffset = 0;
	pose.vecPosition[0] = 2;
	send();
	require((locate().relation_flags & (valid | tracked)) == (valid | tracked), "tracker recovers after reconnect");
	require(locate().pose.position.x == 2, "reconnected tracker uses the new pose");

	pose.poseIsValid = false;
	pose.poseTimeOffset = -143.696311;
	send();
	require(locate().relation_flags == 0, "stale invalid pose clears location flags while still connected");

	pose.poseIsValid = true;
	pose.poseTimeOffset = 0;
	send();
	require((locate().relation_flags & tracked) == tracked, "tracking recovers after an invalid pose");
	pose.poseIsValid = false;
	pose.poseTimeOffset = std::numeric_limits<double>::infinity();
	send();
	require(locate().relation_flags == 0, "nonfinite timestamp does not discard invalidation");
}

static void test_hand_source_switch()
{
	struct Hand : xrt_device
	{
		xrt_input input{};
		int calls = 0;
		Hand() : xrt_device{}
		{
			input.name = XRT_INPUT_HT_CONFORMING_LEFT;
			inputs = &input;
			input_count = 1;
			supported.hand_tracking = true;
			get_hand_tracking = [](xrt_device * device, xrt_input_name, int64_t, xrt_hand_joint_set * joints, int64_t * timestamp) {
				++static_cast<Hand *>(device)->calls;
				*joints = {}; // Inactive: no real space overseer needed.
				*timestamp = 0;
				return XRT_SUCCESS;
			};
		}
	} first, second;
	xrt_device * selected = &first;
	xrt_system_devices devices{};
	xrt_hand_tracker_create_info info{};
	info.hand = XRT_HAND_LEFT;
	info.requested_sources[0] = XRT_INPUT_HT_CONFORMING_LEFT;
	info.requested_source_count = 1;
	xrt_hand_tracker * tracker = nullptr;
	auto callback = [](xrt_input_name, xrt_system_devices *, void * data, uint32_t capacity, xrt_device ** out, uint32_t * count) {
		*count = 0;
		if (capacity && *static_cast<xrt_device **>(data))
		{
			out[0] = *static_cast<xrt_device **>(data);
			*count = 1;
		}
		return XRT_SUCCESS;
	};
	require(b_hand_tracker_create_with_cb(&devices, &info, callback, &selected, &tracker) == XRT_SUCCESS, "create dynamic hand tracker");
	xrt_space_overseer overseer{};
	xrt_space base{};
	xrt_hand_tracker_location location{};
	require(xrt_hand_tracker_locate(tracker, &overseer, &base, nullptr, 0, &location) == XRT_SUCCESS, "locate initial hand source");
	require(first.calls == 1 && second.calls == 0, "initial hand source used");
	selected = &second;
	require(xrt_hand_tracker_locate(tracker, &overseer, &base, nullptr, 0, &location) == XRT_SUCCESS, "locate after hand source switch");
	require(first.calls == 1 && second.calls == 1, "existing hand tracker follows new controller");
	selected = nullptr;
	require(xrt_hand_tracker_locate(tracker, &overseer, &base, nullptr, 0, &location) == XRT_SUCCESS && !location.is_active, "missing hand source becomes inactive");
	xrt_hand_tracker_destroy(&tracker);
}

int main()
{
	test_standby_control_packet();
	test_hand_source_switch();
	// Exercise the real frame thread and driver callbacks without SteamVR or hardware.
	auto ctx = Context::create("/tmp/wivrn-hotplug-no-steam", "/tmp/wivrn-hotplug-no-steamvr", {});
	require(bool(ctx), "empty context survives");
	xrt_system_devices devices{};
	std::atomic<unsigned> callbacks{0};
	{
		std::lock_guard lock(ctx->devices_mut);
		ctx->in_setup = false;
		ctx->xsysd = &devices;
		ctx->device_added_ud = &callbacks;
		ctx->device_added_cb = [](xrt_device *, void * p) { ++*static_cast<std::atomic<unsigned> *>(p); };
	}
	Driver first(ctx.get()), replacement(ctx.get()), tracker(ctx.get(), true);
	require(ctx->TrackedDeviceAdded("controller-A", vr::TrackedDeviceClass_Controller, &first), "late controller accepted");
	wait_for([&] { return __atomic_load_n(&devices.static_xdev_count, __ATOMIC_ACQUIRE) == 1; });
	require(callbacks == 1, "space registered before publishing");
	xrt_device * original = devices.static_xdevs[0];
	xrt_system_roles roles{};
	require(ctx->get_roles(&devices, &roles) == XRT_SUCCESS && roles.left == 0, "hotplug controller gets left role");
	auto generation = roles.generation_id;
	auto input_count = original->input_count;
	auto output_count = original->output_count;

	require(ctx->TrackedDeviceAdded("controller-A", vr::TrackedDeviceClass_Controller, &replacement), "reconnect accepted");
	wait_for([&] { return replacement.activations.load() == 1; });
	require(devices.static_xdevs[0] == original && devices.static_xdev_count == 1, "reconnect reuses device and IPC index");
	require(original->input_count == input_count && original->output_count == output_count, "reconnect preserves input/output layout");
	require(callbacks == 1, "reconnect does not duplicate space registration");

	vr::DriverPose_t off{};
	ctx->TrackedDevicePoseUpdated(replacement.index, off, sizeof(off));
	Driver second(ctx.get());
	require(ctx->TrackedDeviceAdded("controller-B", vr::TrackedDeviceClass_Controller, &second), "second controller accepted");
	wait_for([&] { return __atomic_load_n(&devices.static_xdev_count, __ATOMIC_ACQUIRE) == 2; });
	require(ctx->get_roles(&devices, &roles) == XRT_SUCCESS && roles.left == 1, "connected controller takes role from powered-off controller");
	require(roles.generation_id > generation, "role generation changes");

	require(ctx->TrackedDeviceAdded("tracker-A", vr::TrackedDeviceClass_GenericTracker, &tracker), "late tracker accepted");
	wait_for([&] { return __atomic_load_n(&devices.static_xdev_count, __ATOMIC_ACQUIRE) == 3; });
	require(devices.static_xdevs[2]->device_type == XRT_DEVICE_TYPE_GENERIC_TRACKER, "tracker stays out of controller roles");
	test_tracker_pose_status(devices.static_xdevs[2]);
	require(ctx->get_roles(&devices, &roles) == XRT_SUCCESS && roles.left == 1, "tracker preserves hand roles");
	// A controller that reports a tracker profile must still be excluded by its
	// original OpenVR class, as must every normal and powered-off controller.
	Driver tracker_profile_controller(ctx.get(), true);
	require(ctx->TrackedDeviceAdded("controller-with-tracker-profile", vr::TrackedDeviceClass_Controller, &tracker_profile_controller), "tracker-profile controller accepted");
	wait_for([&] { return __atomic_load_n(&devices.static_xdev_count, __ATOMIC_ACQUIRE) == 4; });
	xrt_system_devices foreign_system{};
	require(!ctx->request_tracker_standby(&foreign_system), "foreign system cannot request standby");
	require(ctx->request_tracker_standby(&devices), "tracker standby accepted");
	wait_for([&] { return tracker.standby_calls.load() == 1; });
	require(first.standby_calls == 0 && replacement.standby_calls == 0 && second.standby_calls == 0 && tracker_profile_controller.standby_calls == 0, "all controllers excluded from standby");

	Driver later_tracker(ctx.get(), true);
	require(ctx->TrackedDeviceAdded("tracker-B", vr::TrackedDeviceClass_GenericTracker, &later_tracker), "second tracker hotplugged");
	wait_for([&] { return __atomic_load_n(&devices.static_xdev_count, __ATOMIC_ACQUIRE) == 5; });
	require(ctx->request_tracker_standby(nullptr), "active-system standby accepted after hotplug");
	wait_for([&] { return tracker.standby_calls.load() == 2 && later_tracker.standby_calls.load() == 1; });
	require(first.standby_calls == 0 && replacement.standby_calls == 0 && second.standby_calls == 0 && tracker_profile_controller.standby_calls == 0, "controllers remain excluded after tracker hotplug");
	{
		std::lock_guard lock(ctx->devices_mut);
		ctx->stopping = true;
		ctx->xsysd = nullptr;
	}
	ctx->stop_frame_thread();
	require(!ctx->request_tracker_standby(&devices), "standby rejected during shutdown");
	require(!ctx->TrackedDeviceAdded("after-stop", vr::TrackedDeviceClass_Controller, &first), "shutdown rejects additions");
	for (auto & device: devices.static_xdevs)
		xrt_device_destroy(&device);
	ctx.reset();
	std::puts("Lighthouse hotplug tests passed");
}
