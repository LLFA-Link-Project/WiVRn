// SPDX-License-Identifier: GPL-3.0-or-later
#include "keyboard_ui.h"
#include <linux/input-event-codes.h>
#include <string_view>

namespace wivrn
{
static ImGuiKey key(unsigned code)
{
	switch (code)
	{
		case KEY_SPACE:
			return ImGuiKey_Space;
		case KEY_BACKSPACE:
			return ImGuiKey_Backspace;
		case KEY_DELETE:
			return ImGuiKey_Delete;
		case KEY_ENTER:
		case KEY_KPENTER:
			return ImGuiKey_Enter;
		case KEY_TAB:
			return ImGuiKey_Tab;
		case KEY_ESC:
			return ImGuiKey_Escape;
		case KEY_LEFT:
			return ImGuiKey_LeftArrow;
		case KEY_RIGHT:
			return ImGuiKey_RightArrow;
		case KEY_UP:
			return ImGuiKey_UpArrow;
		case KEY_DOWN:
			return ImGuiKey_DownArrow;
		case KEY_HOME:
			return ImGuiKey_Home;
		case KEY_END:
			return ImGuiKey_End;
		case KEY_LEFTSHIFT:
			return ImGuiKey_LeftShift;
		case KEY_RIGHTSHIFT:
			return ImGuiKey_RightShift;
		case KEY_LEFTCTRL:
			return ImGuiKey_LeftCtrl;
		case KEY_RIGHTCTRL:
			return ImGuiKey_RightCtrl;
		case KEY_LEFTALT:
			return ImGuiKey_LeftAlt;
		case KEY_RIGHTALT:
			return ImGuiKey_RightAlt;
		case KEY_0:
			return ImGuiKey_0;
		case KEY_KP0:
			return ImGuiKey_Keypad0;
		case KEY_1:
			return ImGuiKey_1;
		case KEY_KP1:
			return ImGuiKey_Keypad1;
		case KEY_2:
			return ImGuiKey_2;
		case KEY_KP2:
			return ImGuiKey_Keypad2;
		case KEY_3:
			return ImGuiKey_3;
		case KEY_KP3:
			return ImGuiKey_Keypad3;
		case KEY_4:
			return ImGuiKey_4;
		case KEY_KP4:
			return ImGuiKey_Keypad4;
		case KEY_5:
			return ImGuiKey_5;
		case KEY_KP5:
			return ImGuiKey_Keypad5;
		case KEY_6:
			return ImGuiKey_6;
		case KEY_KP6:
			return ImGuiKey_Keypad6;
		case KEY_7:
			return ImGuiKey_7;
		case KEY_KP7:
			return ImGuiKey_Keypad7;
		case KEY_8:
			return ImGuiKey_8;
		case KEY_KP8:
			return ImGuiKey_Keypad8;
		case KEY_9:
			return ImGuiKey_9;
		case KEY_KP9:
			return ImGuiKey_Keypad9;
		default:
			return ImGuiKey_None;
	}
}

static char character(unsigned code, bool shift)
{
	if (code >= KEY_1 and code <= KEY_0)
		return (shift ? std::string_view("!@#$%^&*()") : std::string_view("1234567890"))[code - KEY_1];
	if (code >= KEY_Q and code <= KEY_P)
		return (shift ? std::string_view("QWERTYUIOP") : std::string_view("qwertyuiop"))[code - KEY_Q];
	if (code >= KEY_A and code <= KEY_L)
		return (shift ? std::string_view("ASDFGHJKL") : std::string_view("asdfghjkl"))[code - KEY_A];
	if (code >= KEY_Z and code <= KEY_M)
		return (shift ? std::string_view("ZXCVBNM") : std::string_view("zxcvbnm"))[code - KEY_Z];
	switch (code)
	{
		case KEY_SPACE:
			return ' ';
		case KEY_MINUS:
			return shift ? '_' : '-';
		case KEY_EQUAL:
			return shift ? '+' : '=';
		case KEY_LEFTBRACE:
			return shift ? '{' : '[';
		case KEY_RIGHTBRACE:
			return shift ? '}' : ']';
		case KEY_BACKSLASH:
			return shift ? '|' : '\\';
		case KEY_SEMICOLON:
			return shift ? ':' : ';';
		case KEY_APOSTROPHE:
			return shift ? '"' : '\'';
		case KEY_GRAVE:
			return shift ? '~' : '`';
		case KEY_COMMA:
			return shift ? '<' : ',';
		case KEY_DOT:
		case KEY_KPDOT:
			return shift ? '>' : '.';
		case KEY_SLASH:
		case KEY_KPSLASH:
			return shift ? '?' : '/';
		case KEY_KPMINUS:
			return '-';
		case KEY_KPPLUS:
			return '+';
		case KEY_KPASTERISK:
			return '*';
		default:
			if (auto mapped = key(code); mapped >= ImGuiKey_0 and mapped <= ImGuiKey_9)
				return '0' + mapped - ImGuiKey_0;
			if (auto mapped = key(code); mapped >= ImGuiKey_Keypad0 and mapped <= ImGuiKey_Keypad9)
				return '0' + mapped - ImGuiKey_Keypad0;
			return 0;
	}
}

void keyboard_ui::event(unsigned code, int value, ImGuiIO & io, bool text_input)
{
	if (code >= held.size() or value < 0 or value > 2)
		return;
	if (value != 2)
	{
		held[code] = value == 1;
		// Space is reserved for gaze clicks outside a text field. Sending it as
		// an ImGui key too could activate a previously focused widget off-panel.
		if (auto mapped = key(code); mapped != ImGuiKey_None and (code != KEY_SPACE or text_input or value == 0))
			io.AddKeyEvent(mapped, value == 1);
		io.AddKeyEvent(ImGuiMod_Shift, held[KEY_LEFTSHIFT] or held[KEY_RIGHTSHIFT]);
		io.AddKeyEvent(ImGuiMod_Ctrl, held[KEY_LEFTCTRL] or held[KEY_RIGHTCTRL]);
		io.AddKeyEvent(ImGuiMod_Alt, held[KEY_LEFTALT] or held[KEY_RIGHTALT]);
	}
	if (value != 0 and text_input and not(held[KEY_LEFTCTRL] or held[KEY_RIGHTCTRL] or held[KEY_LEFTALT] or held[KEY_RIGHTALT]))
		if (char c = character(code, held[KEY_LEFTSHIFT] or held[KEY_RIGHTSHIFT]))
			io.AddInputCharacter(c);
}

std::optional<int> keyboard_ui::pin_key(unsigned code)
{
	if (code == KEY_BACKSPACE)
		return -1;
	const auto mapped = key(code);
	if (mapped >= ImGuiKey_0 and mapped <= ImGuiKey_9)
		return mapped - ImGuiKey_0;
	if (mapped >= ImGuiKey_Keypad0 and mapped <= ImGuiKey_Keypad9)
		return mapped - ImGuiKey_Keypad0;
	return {};
}

void keyboard_ui::reset(ImGuiIO & io)
{
	for (unsigned code = 0; code < held.size(); ++code)
		if (held[code])
			if (auto mapped = key(code); mapped != ImGuiKey_None)
				io.AddKeyEvent(mapped, false);
	held.reset();
	io.AddKeyEvent(ImGuiMod_Shift, false);
	io.AddKeyEvent(ImGuiMod_Ctrl, false);
	io.AddKeyEvent(ImGuiMod_Alt, false);
}
} // namespace wivrn
