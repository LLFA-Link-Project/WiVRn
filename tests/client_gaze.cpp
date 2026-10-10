// SPDX-License-Identifier: GPL-3.0-or-later
#include "linux/keyboard_ui.h"
#include "render/head_gaze_input.h"
#include "render/pointer_intersection.h"
#include <cfloat>
#include <cstdlib>
#include <imgui.h>
#include <imgui_internal.h>
#include <iostream>
#include <linux/input-event-codes.h>
#include <string_view>

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
	using wivrn::intersect_ui_panel;
	const glm::quat identity{1, 0, 0, 0};
	const auto center = intersect_ui_panel({0, 0, 0}, {0, 0, -1}, {0, 0, -1}, identity, {0.6, 0.4});
	check(center and glm::length(center->first) < 0.00001f, "Head gaze must hit the center of a forward panel");
	const auto moved = intersect_ui_panel({0, 0, 0}, {0.1, 0.05, -1}, {0, 0, -1}, identity, {0.6, 0.4});
	check(moved and moved->first.x > 0 and moved->first.y > 0, "Head movement must move the pointer");
	check(not intersect_ui_panel({0, 0, 0}, {1, 0, -1}, {0, 0, -1}, identity, {0.6, 0.4}), "Off-panel gaze must miss");
	check(not intersect_ui_panel({0, 0, 0}, {0, 0, -1}, {0, 0, 1}, identity, {0.6, 0.4}), "Panel behind the head must miss");
	check(not intersect_ui_panel({0, 0, 0}, {1, 0, 0}, {0, 0, -1}, identity, {0.6, 0.4}), "Parallel ray must miss");
	check(not intersect_ui_panel({0, 0, 0}, {0, 0, -1}, {0, 0, -1}, glm::quat{0, 0, 0, 0}, {0.6, 0.4}), "Invalid UI orientation must miss");
	check(intersect_ui_panel({0, 0, -1.001f}, {0, 0, -1}, {0, 0, -1}, identity, {0.6, 0.4}, -0.01f).has_value(), "Existing hand touch tolerance must remain valid");

	wivrn::head_gaze_click click;
	check(click.key(true, true), "Space down over UI must start a drag");
	check(click.key(true, true), "Repeated down must keep a single drag");
	click.cancel();
	check(not click.key(true, true), "Tracking/panel recovery while held must not click again");
	check(not click.key(false, true), "Space release must clear the drag");
	check(not click.key(true, false), "Space down off-panel or while typing must not click");
	check(not click.key(true, true), "Moving onto UI while holding Space must not start a drag");
	click.key(false, false);
	check(click.key(true, true), "Fresh press after recovery must work");
	click.key(false, true);

	ImGui::CreateContext();
	auto & io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.DisplaySize = {600, 400};
	io.DeltaTime = 1.0f / 60;
	io.Fonts->AddFontDefault();
	unsigned char * pixels;
	int width, height;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	wivrn::keyboard_ui keyboard;
	keyboard.event(KEY_SPACE, 1, io, false);
	keyboard.event(KEY_SPACE, 2, io, false);
	check(ImGui::GetCurrentContext()->InputEventsQueue.empty(), "Space outside text must not activate ImGui keyboard navigation");
	keyboard.event(KEY_SPACE, 0, io, false);
	io.ClearEventsQueue();
	keyboard.event(KEY_SPACE, 1, io, true);
	bool space_text = false, space_key = false;
	for (const auto & event: ImGui::GetCurrentContext()->InputEventsQueue)
	{
		space_text |= event.Type == ImGuiInputEventType_Text and event.Text.Char == ' ';
		space_key |= event.Type == ImGuiInputEventType_Key and event.Key.Key == ImGuiKey_Space and event.Key.Down;
	}
	check(space_text and space_key, "Space while editing must be a space character/key");
	keyboard.reset(io);
	io.ClearEventsQueue();
	keyboard.event(KEY_LEFTSHIFT, 1, io, true);
	keyboard.event(KEY_A, 1, io, true);
	bool uppercase = false;
	for (const auto & event: ImGui::GetCurrentContext()->InputEventsQueue)
		uppercase |= event.Type == ImGuiInputEventType_Text and event.Text.Char == 'A';
	check(uppercase, "Physical keyboard must support Shift ASCII input");
	keyboard.reset(io);
	io.ClearEventsQueue();
	check(wivrn::keyboard_ui::pin_key(KEY_7) == 7 and wivrn::keyboard_ui::pin_key(KEY_KP3) == 3 and
	              wivrn::keyboard_ui::pin_key(KEY_BACKSPACE) == -1,
	      "PIN keys must cover digits, keypad and backspace");

	// Exercise actual Dear ImGui widgets with the production Space drag gate.
	bool checkbox = false;
	float slider = 0;
	int buttons = 0;
	int selected_tab = 0;
	char address[64] = {};
	ImVec2 button_position, checkbox_position, slider_position, tab_position, text_position;
	auto frame = [&](ImVec2 position, bool down) {
		io.AddMousePosEvent(position.x, position.y);
		io.AddMouseButtonEvent(0, down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos({0, 0});
		ImGui::SetNextWindowSize({600, 400});
		ImGui::Begin("Lobby", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
		if (ImGui::Button("Connect to server", {200, 40}))
			++buttons;
		button_position = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
		ImGui::Checkbox("Setting", &checkbox);
		checkbox_position = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
		ImGui::SetNextItemWidth(200);
		ImGui::SliderFloat("Slider", &slider, 0, 1);
		slider_position = ImGui::GetItemRectMin() + ImVec2{180, 10};
		if (ImGui::BeginTabBar("Tabs"))
		{
			if (ImGui::BeginTabItem("Servers"))
			{
				selected_tab = 0;
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Settings"))
			{
				selected_tab = 1;
				ImGui::EndTabItem();
			}
			tab_position = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
			ImGui::EndTabBar();
		}
		ImGui::InputText("Address", address, sizeof(address));
		text_position = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
		ImGui::End();
		ImGui::Render();
	};
	frame({-FLT_MAX, -FLT_MAX}, false);
	frame(button_position, false);
	frame(button_position, click.key(true, true));
	for (int i = 0; i < 60; ++i)
		frame(button_position, click.pressed());
	frame(button_position, click.key(false, true));
	check(buttons == 1, "Space hold must click a server button once");
	frame(checkbox_position, false);
	frame(checkbox_position, click.key(true, true));
	frame(checkbox_position, click.key(false, true));
	check(checkbox, "Gaze and Space must toggle a checkbox");
	frame(slider_position, click.key(true, true));
	frame(slider_position, click.pressed());
	frame(slider_position, click.key(false, true));
	check(slider > 0.5f, "Space drag must operate a slider");
	frame(button_position, click.key(true, true));
	click.cancel();
	frame({-FLT_MAX, -FLT_MAX}, false);
	check(buttons == 1, "Off-panel release must not activate a button");
	click.key(false, false);
	frame(tab_position, false);
	frame(tab_position, click.key(true, true));
	frame(tab_position, click.key(false, true));
	frame(tab_position, false);
	check(selected_tab == 1, "Gaze and Space must select a settings tab");
	frame(text_position, false);
	frame(text_position, click.key(true, true));
	frame(text_position, click.key(false, true));
	frame(text_position, false);
	check(io.WantTextInput, "InputText must acquire physical keyboard focus");
	keyboard.event(KEY_A, 1, io, io.WantTextInput);
	keyboard.event(KEY_A, 0, io, io.WantTextInput);
	keyboard.event(KEY_SPACE, 1, io, io.WantTextInput);
	keyboard.event(KEY_SPACE, 0, io, io.WantTextInput);
	for (int i = 0; i < 8; ++i)
		frame(text_position, false);
	check(std::string_view(address) == "a ", "Physical text input must insert letters and a literal space");
	check(buttons == 1, "Typing Space must not click a server button");
	keyboard.event(KEY_BACKSPACE, 1, io, true);
	keyboard.event(KEY_BACKSPACE, 0, io, true);
	for (int i = 0; i < 4; ++i)
		frame(text_position, false);
	check(std::string_view(address) == "a", "Physical Backspace must edit the field");
	keyboard.event(KEY_ENTER, 1, io, true);
	keyboard.event(KEY_ENTER, 0, io, true);
	for (int i = 0; i < 4; ++i)
		frame(text_position, false);
	check(not io.WantTextInput, "Enter must finish text input so gaze clicks can resume");
	ImGui::DestroyContext();
	std::cout << "Head gaze, keyboard and ImGui interaction checks passed\n";
}
