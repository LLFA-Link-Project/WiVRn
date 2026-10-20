// SPDX-License-Identifier: BSL-1.0
#include "b_hand_tracker.h"
#include "interfaces/context.hpp"
#include "xrt/xrt_device.h"
#include "xrt/xrt_space.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
	unsigned index = 0;
	Driver(Context * context, bool tracker = false) : context(context), tracker(tracker) {}
	vr::EVRInitError Activate(uint32_t new_index) override
	{
		index = new_index;
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
	void EnterStandby() override {}
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
	require(ctx->get_roles(&devices, &roles) == XRT_SUCCESS && roles.left == 1, "tracker preserves hand roles");
	{
		std::lock_guard lock(ctx->devices_mut);
		ctx->stopping = true;
		ctx->xsysd = nullptr;
	}
	ctx->stop_frame_thread();
	require(!ctx->TrackedDeviceAdded("after-stop", vr::TrackedDeviceClass_Controller, &first), "shutdown rejects additions");
	for (auto & device: devices.static_xdevs)
		xrt_device_destroy(&device);
	ctx.reset();
	std::puts("Lighthouse hotplug tests passed");
}
