// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>
namespace wivrn::lighthouse
{
// Partial grammar: headers, CONFIG JSON and fixed numeric sensor/pose records.
// Unknown diagnostic tags are retained; this is not a complete playback parser.
inline std::string native_line_error(std::string line)
{
	if (line.find('\0') != std::string::npos)
		return "nul_byte";
	while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
		line.pop_back();
	if (line.ends_with("\\r\\n"))
		line.resize(line.size() - 4);
	std::istringstream input(line);
	std::vector<std::string> fields;
	for (std::string v; input >> v;)
		fields.push_back(std::move(v));
	if (fields.size() < 3)
		return "invalid_header";
	auto number = [](const std::string & v) {
		char * end = nullptr;
		std::strtod(v.c_str(), &end);
		return end != v.c_str() && !*end;
	};
	if (!number(fields[0]) || !std::isfinite(std::strtod(fields[0].c_str(), nullptr)))
		return "invalid_receive_time";
	if (fields[1] == "INFO")
		return {};
	if (fields[1] == "OPTION")
	{
		if (fields.size() < 4)
			return "invalid_option";
		if (fields[3] != "s" && (fields.size() != 5 || !number(fields[4])))
			return "invalid_option_value";
		return {};
	}
	if (fields[2] == "CONFIG")
	{
		auto start = line.find(" CONFIG ");
		if (start == std::string::npos || !nlohmann::json::parse(line.substr(start + 8), nullptr, false).is_object())
			return "invalid_config_json";
		return {};
	}
	static const std::map<std::string, size_t> numeric = {
	        {"i", 12},
	        {"I", 12},
	        {"B", 5},
	        {"W", 4},
	        {"Y", 4},
	        {"C", 3},
	        {"A", 6},
	        {"POSE", 7},
	        {"VELOCITY", 6},
	        {"POSE_TIMING", 4},
	        {"POSE_STATE", 12},
	        {"POSE_BIAS", 15},
	        {"POSE_BIAS_VAR", 7},
	        {"LH_POSE", 8},
	        {"INPUT_TIME", 2},
	        {"IMU_SCALES", 2},
	        {"BUTTON", 2},
	        {"DISCONNECT", 0},
	};
	auto it = numeric.find(fields[2]);
	if (it == numeric.end())
		return {};
	if (fields.size() != 3 + it->second)
		return "wrong_numeric_field_count";
	for (size_t i = 3; i < fields.size(); ++i)
		if (!number(fields[i]))
			return "invalid_numeric_field";
	return {};
}
} // namespace wivrn::lighthouse
