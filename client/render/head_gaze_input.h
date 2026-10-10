// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace wivrn
{
// One physical press may start at most one drag. Cancellation requires release
// before reacquisition, even if tracking/the panel returns while Space is held.
class head_gaze_click
{
	bool held = false;
	bool dragging = false;

public:
	bool key(bool down, bool may_start)
	{
		if (down and not held)
			dragging = may_start;
		if (not down)
			dragging = false;
		held = down;
		return dragging;
	}
	void cancel()
	{
		dragging = false;
	}
	bool pressed() const
	{
		return dragging;
	}
};
} // namespace wivrn
