// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "xr/pose.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <optional>
#include <utility>

namespace wivrn
{
// Coordinates relative to a panel's center, normalized to [-0.5, 0.5].
inline std::optional<std::pair<glm::vec2, float>> intersect_ui_panel(
        glm::vec3 origin,
        glm::vec3 direction,
        glm::vec3 position,
        glm::quat orientation,
        glm::vec2 size,
        float minimum_distance = 0)
{
	const XrPosef panel_pose{{orientation.x, orientation.y, orientation.z, orientation.w}, {position.x, position.y, position.z}};
	if (not xr::valid_pose(panel_pose) or not xr::finite_pose_component(size.x) or not xr::finite_pose_component(size.y) or
	    size.x <= 0 or size.y <= 0)
		return {};
	for (unsigned i = 0; i < 3; ++i)
		if (not xr::finite_pose_component(origin[i]) or not xr::finite_pose_component(direction[i]))
			return {};
	const auto transform = glm::transpose(glm::mat3_cast(orientation));
	const glm::vec3 ray_start = transform * (origin - position);
	const glm::vec3 ray_direction = transform * direction;
	if (ray_direction.z >= -0.0001f)
		return {};
	const float distance = -ray_start.z / ray_direction.z;
	if (distance < minimum_distance or not xr::finite_pose_component(distance))
		return {};
	const glm::vec2 coord = glm::vec2(ray_start + distance * ray_direction) / size;
	if (not xr::finite_pose_component(coord.x) or not xr::finite_pose_component(coord.y) or
	    std::abs(coord.x) > 0.5f or std::abs(coord.y) > 0.5f)
		return {};
	return std::pair{coord, distance};
}
} // namespace wivrn
