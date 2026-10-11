// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "math/m_space.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_tracking.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <optional>
#include <string_view>

namespace wivrn
{
// A non-owning tracking source. The session retains ownership of the Lighthouse device.
class external_head
{
public:
	using latest_pose_fn = bool (*)(xrt_device *, int64_t *, xrt_space_relation *);
	static constexpr int64_t stale_ns = 100'000'000;
	static constexpr int64_t prediction_ns = 20'000'000;

private:
	xrt_device * source;
	xrt_input_name input;
	latest_pose_fn latest;
	xrt_pose source_from_head;

public:
	static bool finite(float value)
	{
		return (std::bit_cast<uint32_t>(value) & 0x7f800000u) != 0x7f800000u;
	}

	static bool valid_pose(const xrt_pose & pose)
	{
		const auto & q = pose.orientation;
		if (not finite(q.x) or not finite(q.y) or not finite(q.z) or not finite(q.w))
			return false;
		float norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
		return finite(norm) and norm >= 0.9801f and norm <= 1.0201f and
		       finite(pose.position.x) and finite(pose.position.y) and finite(pose.position.z);
	}

	static std::optional<xrt_input_name> pose_input(const xrt_device & device)
	{
		// Prefer the driver's head reference: OpenVR already applies DriverFromHead.
		for (auto name: {XRT_INPUT_GENERIC_HEAD_POSE, XRT_INPUT_GENERIC_TRACKER_POSE})
			for (uint32_t i = 0; i < device.input_count; ++i)
				if (device.inputs[i].name == name)
					return name;
		return std::nullopt;
	}

	external_head(xrt_device & device, xrt_input_name name, latest_pose_fn query, xrt_pose mount = XRT_POSE_IDENTITY) :
	        source(&device), input(name), latest(query), source_from_head(mount)
	{
		math_quat_normalize(&source_from_head.orientation);
	}

	xrt_tracking_origin * origin() const
	{
		return source->tracking_origin;
	}

	xrt_space_relation get_pose(int64_t target_ns, int64_t now_ns) const
	{
		xrt_space_relation observation{}, predicted{};
		int64_t observed_ns = 0;
		constexpr int required = XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
		                         XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT;
		// Do not turn a stale prediction, or loss of Lighthouse tracking, into an absolute reference.
		if (not latest(source, &observed_ns, &observation) or observed_ns <= 0 or
		    observed_ns > now_ns + prediction_ns or now_ns - observed_ns > stale_ns or
		    (observation.relation_flags & required) != required or not valid_pose(observation.pose))
			return XRT_SPACE_RELATION_ZERO;
		const auto bounded_target = std::min(target_ns, observed_ns + prediction_ns);
		if (bounded_target < observed_ns - stale_ns or
		    xrt_device_get_tracked_pose(source, input, bounded_target, &predicted) != XRT_SUCCESS or
		    (predicted.relation_flags & required) != required or not valid_pose(predicted.pose))
			return XRT_SPACE_RELATION_ZERO;

		math_quat_normalize(&predicted.pose.orientation);
		if (not finite(predicted.linear_velocity.x) or not finite(predicted.linear_velocity.y) or not finite(predicted.linear_velocity.z))
		{
			predicted.relation_flags = xrt_space_relation_flags(predicted.relation_flags & ~XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT);
			predicted.linear_velocity = {};
		}
		if (not finite(predicted.angular_velocity.x) or not finite(predicted.angular_velocity.y) or not finite(predicted.angular_velocity.z))
		{
			predicted.relation_flags = xrt_space_relation_flags(predicted.relation_flags & ~XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);
			predicted.angular_velocity = {};
		}

		xrt_relation_chain chain{};
		m_relation_chain_push_pose(&chain, &source_from_head);
		m_relation_chain_push_relation(&chain, &predicted);
		xrt_space_relation result{};
		m_relation_chain_resolve(&chain, &result);
		return result;
	}
};
} // namespace wivrn
