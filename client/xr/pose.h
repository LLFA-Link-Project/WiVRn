// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <span>
#include <openxr/openxr.h>

namespace xr
{
// WiVRn is compiled with -ffast-math, which can optimize away std::isfinite.
inline bool finite_pose_component(float value)
{
	return (std::bit_cast<uint32_t>(value) & 0x7f800000u) != 0x7f800000u;
}

// OpenXR permits at most 1% deviation from unit quaternion length.
inline bool valid_pose(const XrPosef & pose)
{
	const auto & q = pose.orientation;
	if (not finite_pose_component(q.x) or not finite_pose_component(q.y) or
	    not finite_pose_component(q.z) or not finite_pose_component(q.w))
		return false;
	const float norm_squared = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
	return finite_pose_component(norm_squared) and norm_squared >= 0.99f * 0.99f and norm_squared <= 1.01f * 1.01f and
	       finite_pose_component(pose.position.x) and finite_pose_component(pose.position.y) and finite_pose_component(pose.position.z);
}

inline bool valid_views(XrViewStateFlags flags, std::span<const XrView> views)
{
	const auto required = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
	return (flags & required) == required and not views.empty() and
	       std::ranges::all_of(views, [](const XrView & view) { return valid_pose(view.pose); });
}
} // namespace xr
