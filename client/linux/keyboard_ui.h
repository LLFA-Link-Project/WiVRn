// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <bitset>
#include <imgui.h>
#include <optional>

namespace wivrn
{
// Dependency-free US ASCII mapping for addresses/PINs. No IME or layout daemon.
class keyboard_ui
{
	std::bitset<256> held;

public:
	void event(unsigned code, int value, ImGuiIO & io, bool text_input);
	void reset(ImGuiIO & io);
	static std::optional<int> pin_key(unsigned code);
};
} // namespace wivrn
