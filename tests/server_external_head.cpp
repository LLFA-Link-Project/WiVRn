// SPDX-License-Identifier: GPL-3.0-or-later
#include "driver/external_head.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace
{
void check(bool value, const char * message)
{
	if (not value)
	{
		std::cerr << message << '\n';
		std::exit(1);
	}
}
struct fake_source : xrt_device
{
	xrt_tracking_origin origin{};
	xrt_input head_input{.active = true, .name = XRT_INPUT_GENERIC_HEAD_POSE};
	xrt_space_relation observation = XRT_SPACE_RELATION_ZERO;
	xrt_space_relation predicted = XRT_SPACE_RELATION_ZERO;
	int64_t observation_ns = 1'000'000'000;
	int64_t requested_ns = 0;
	unsigned queries = 0;
	bool available = true;
	xrt_result_t result = XRT_SUCCESS;
	fake_source()
	{
		tracking_origin = &origin;
		inputs = &head_input;
		input_count = 1;
		get_tracked_pose = [](xrt_device * dev, xrt_input_name input, int64_t target, xrt_space_relation * out) {
			auto & self = *static_cast<fake_source *>(dev);
			check(input == self.head_input.name, "wrong source input");
			++self.queries;
			self.requested_ns = target;
			*out = self.predicted;
			return self.result;
		};
		observation.relation_flags = xrt_space_relation_flags(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
		                                                      XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
		                                                      XRT_SPACE_RELATION_POSITION_TRACKED_BIT | XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT |
		                                                      XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);
		observation.pose = XRT_POSE_IDENTITY;
		observation.pose.position = {1, 2, 3};
		predicted = observation;
	}
	static bool latest(xrt_device * dev, int64_t * time, xrt_space_relation * out)
	{
		auto & self = *static_cast<fake_source *>(dev);
		*time = self.observation_ns;
		*out = self.observation;
		return self.available;
	}
};
} // namespace
int main()
{
	using wivrn::external_head;
	fake_source source;
	external_head head(source, XRT_INPUT_GENERIC_HEAD_POSE, fake_source::latest);
	check(head.origin() == source.tracking_origin, "tracking origin must be shared");
	check(external_head::pose_input(source) == XRT_INPUT_GENERIC_HEAD_POSE, "head pose selection");
	auto pose = head.get_pose(1'080'000'000, 1'005'000'000);
	check(pose.pose.position.x == 1 && pose.pose.position.y == 2 && pose.pose.position.z == 3, "external position");
	check(source.requested_ns == 1'020'000'000, "prediction must be bounded by observation time");
	check((pose.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT) != 0, "external 6DoF flags");
	check(head.get_pose(1'100'000'000, 1'100'000'001).relation_flags == 0, "stale source must be invalid");
	check(head.get_pose(1'000'000'000, 900'000'000).relation_flags == 0, "future source clock mismatch");
	check(head.get_pose(800'000'000, 1'000'000'000).relation_flags == 0, "old target must be invalid");
	source.available = false;
	check(head.get_pose(1'000'000'000, 1'000'000'000).relation_flags == 0, "empty history");
	source.available = true;
	const auto valid = source.observation;
	for (int bit: {XRT_SPACE_RELATION_POSITION_TRACKED_BIT, XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT, XRT_SPACE_RELATION_POSITION_VALID_BIT, XRT_SPACE_RELATION_ORIENTATION_VALID_BIT})
	{
		source.observation.relation_flags = xrt_space_relation_flags(valid.relation_flags & ~bit);
		check(head.get_pose(1'000'000'000, 1'000'000'000).relation_flags == 0, "tracking loss must not masquerade as 6DoF");
	}
	source.observation = valid;
	source.predicted.pose.orientation.w = std::numeric_limits<float>::quiet_NaN();
	check(head.get_pose(1'000'000'000, 1'000'000'000).relation_flags == 0, "invalid predicted pose");
	source.predicted = valid;
	source.predicted.linear_velocity.x = std::numeric_limits<float>::quiet_NaN();
	auto sanitized = head.get_pose(1'000'000'000, 1'000'000'000);
	check(not(sanitized.relation_flags & XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT), "nonfinite velocity must not be valid");
	source.predicted = valid;
	source.result = XRT_ERROR_INPUT_UNSUPPORTED;
	check(head.get_pose(1'000'000'000, 1'000'000'000).relation_flags == 0, "driver error");
	source.result = XRT_SUCCESS;
	source.head_input.name = XRT_INPUT_GENERIC_TRACKER_POSE;
	check(external_head::pose_input(source) == XRT_INPUT_GENERIC_TRACKER_POSE, "generic tracker selection");
	xrt_pose offset = XRT_POSE_IDENTITY;
	offset.position.x = 0.1f;
	source.predicted.pose.orientation = {0, 0, std::sqrt(0.5f), std::sqrt(0.5f)};
	source.predicted.angular_velocity.z = 2;
	external_head mounted(source, XRT_INPUT_GENERIC_TRACKER_POSE, fake_source::latest, offset);
	pose = mounted.get_pose(1'000'000'000, 1'000'000'000);
	check(std::abs(pose.pose.position.x - 1) < 1e-5f && std::abs(pose.pose.position.y - 2.1f) < 1e-5f, "mount offset must rotate with tracker");
	check(std::abs(pose.linear_velocity.x + 0.2f) < 1e-5f, "mount lever arm velocity");
	check(pose.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT, "mount keeps tracking validity");
	offset.orientation.w = 0;
	check(not external_head::valid_pose(offset), "zero quaternion rejected");
	offset = XRT_POSE_IDENTITY;
	offset.position.x = std::numeric_limits<float>::infinity();
	check(not external_head::valid_pose(offset), "nonfinite mount rejected with fast math");
	source.input_count = 0;
	check(not external_head::pose_input(source), "no unsupported pose selection");
	std::cout << "External Lighthouse head tests passed\n";
}
