// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/imgui_impl.h"
#include "xr/pose.h"
#include <array>
#include <bit>
#include <cstdlib>
#include <iostream>

static void check(bool condition, const char * message)
{
	if (not condition)
	{
		std::cerr << message << '\n';
		std::exit(1);
	}
}

int main()
{
	// Exercise the same aggregate initialization as the lobby's main viewport.
	const imgui_context::viewport viewport{
	        .space = xr::spaces::world,
	        .size = {0.6, 0.4},
	        .vp_origin = {0, 0},
	        .vp_size = {1500, 1000},
	};
	XrPosef pose{
	        .orientation = {viewport.orientation.x, viewport.orientation.y, viewport.orientation.z, viewport.orientation.w},
	        .position = {viewport.position.x, viewport.position.y, viewport.position.z},
	};
	check(xr::valid_pose(pose), "Initial UI viewport must have a valid pose before recentering");
	pose.orientation = {0, 0, 0, 0};
	check(not xr::valid_pose(pose), "Zero quaternion must be rejected");
	pose.orientation = {0, 0.6f, 0, 0.8f};
	check(xr::valid_pose(pose), "Tracked rotation must remain usable");
	pose.orientation.w = 2;
	check(not xr::valid_pose(pose), "Non-unit quaternion must be rejected");
	for (uint32_t bits: {0x7fc00000u, 0x7f800000u, 0xff800000u, 0x7f7fffffu})
	{
		pose = {{0, 0, 0, 1}, {0, 0, 0}};
		pose.orientation.x = std::bit_cast<float>(bits);
		check(not xr::valid_pose(pose), "Nonfinite or overflowing orientation must be rejected with fast-math");
	}
	for (uint32_t bits: {0x7fc00000u, 0x7f800000u, 0xff800000u})
	{
		pose = {{0, 0, 0, 1}, {0, 0, 0}};
		pose.position.y = std::bit_cast<float>(bits);
		check(not xr::valid_pose(pose), "Nonfinite position must be rejected with fast-math");
	}
	std::array<XrView, 2> views{XrView{.pose = {{0, 0, 0, 1}, {-0.0325f, 0, 0}}},
	                            XrView{.pose = {{0, 0, 0, 1}, {0.0325f, 0, 0}}}};
	const auto valid = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
	check(xr::valid_views(valid, views), "Valid untracked 3DoF views must render");
	check(not xr::valid_views(XR_VIEW_STATE_ORIENTATION_VALID_BIT, views), "Invalid positions must not be read");
	check(not xr::valid_views(XR_VIEW_STATE_POSITION_VALID_BIT, views), "Invalid orientations must not be read");
	check(not xr::valid_views(valid, {}), "Empty views must not render");
	views[1].pose.orientation = {};
	check(not xr::valid_views(valid, views), "Invalid second-eye pose must suppress the frame");
	std::cout << "Client pose regression checks passed\n";
}
