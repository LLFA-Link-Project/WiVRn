// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only OpenVR provider: an HMD with tracking but no display component.
#include "openvr_driver.h"
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace
{
std::atomic<unsigned> activated{}, deactivated{}, display_requests{}, cleaned{};
std::atomic<bool> active{}, tracking{true};
vr::IVRServerDriverHost * host{};
vr::IVRProperties * properties{};
class hmd_driver : public vr::ITrackedDeviceServerDriver
{
public:
	vr::EVRInitError Activate(uint32_t id) override
	{
		++activated;
		active = true;
		if (not std::getenv("LH_TRACKING_ONLY_HMD_SERIAL"))
			return vr::VRInitError_None;
		float frequency = 72;
		vr::PropertyWrite_t prop{};
		prop.prop = vr::Prop_DisplayFrequency_Float;
		prop.writeType = vr::PropertyWrite_Set;
		prop.unTag = vr::k_unFloatPropertyTag;
		prop.pvBuffer = &frequency;
		prop.unBufferSize = sizeof(frequency);
		properties->WritePropertyBatch(properties->TrackedDeviceToPropertyContainer(id), &prop, 1);
		return vr::VRInitError_None;
	}
	void Deactivate() override
	{
		active = false;
		++deactivated;
	}
	void EnterStandby() override {}
	void * GetComponent(const char *) override
	{
		++display_requests;
		return nullptr;
	}
	void DebugRequest(const char *, char *, uint32_t) override {}
	vr::DriverPose_t GetPose() override
	{
		return {};
	}
} hmd;
class provider : public vr::IServerTrackedDeviceProvider
{
public:
	vr::EVRInitError Init(vr::IVRDriverContext * ctx) override
	{
		vr::EVRInitError err{};
		host = static_cast<vr::IVRServerDriverHost *>(ctx->GetGenericInterface(vr::IVRServerDriverHost_Version, &err));
		properties = static_cast<vr::IVRProperties *>(ctx->GetGenericInterface(vr::IVRProperties_Version, &err));
		if (!host || !properties)
			return vr::VRInitError_Driver_Failed;
		host->TrackedDeviceAdded("TEST-LIGHTHOUSE-HMD", vr::TrackedDeviceClass_HMD, &hmd);
		return vr::VRInitError_None;
	}
	void Cleanup() override
	{
		++cleaned;
	}
	const char * const * GetInterfaceVersions() override
	{
		return vr::k_InterfaceVersions;
	}
	void RunFrame() override
	{
		if (!active)
			return;
		vr::DriverPose_t pose{};
		pose.poseIsValid = true;
		pose.deviceIsConnected = true;
		pose.result = tracking ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
		pose.poseTimeOffset = -0.025;
		pose.qRotation.w = pose.qWorldFromDriverRotation.w = pose.qDriverFromHeadRotation.w = 1;
		pose.vecPosition[0] = 1;
		pose.vecPosition[1] = 2;
		pose.vecPosition[2] = 3;
		pose.vecDriverFromHeadTranslation[0] = 0.1;
		host->TrackedDevicePoseUpdated(0, pose, sizeof(pose));
	}
	bool ShouldBlockStandbyMode() override
	{
		return false;
	}
	void EnterStandby() override {}
	void LeaveStandby() override {}
} instance;
} // namespace
extern "C" void * HmdDriverFactory(const char * version, int * error)
{
	if (std::strcmp(version, vr::IServerTrackedDeviceProvider_Version) == 0)
	{
		*error = vr::VRInitError_None;
		return &instance;
	}
	*error = vr::VRInitError_Init_InterfaceNotFound;
	return nullptr;
}
extern "C" unsigned llfa_mock_state()
{
	return activated | (deactivated << 4) | (display_requests << 8) | (cleaned << 12);
}
extern "C" void llfa_mock_tracking(bool enabled)
{
	tracking = enabled;
}
