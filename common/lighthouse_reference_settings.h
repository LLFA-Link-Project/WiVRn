// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace wivrn
{
inline bool valid_lighthouse_serial(const std::string & serial)
{
	return !serial.empty() && serial.size() < 256 &&
	       std::all_of(serial.begin(), serial.end(), [](unsigned char c) { return c > 32 && c < 127; });
}
inline std::vector<std::string> lighthouse_reference_candidates(const nlohmann::json & config)
{
	std::vector<std::string> result;
	auto append = [&](const nlohmann::json & value) {
		if (!value.is_string())
			return;
		auto serial = value.get<std::string>();
		if (valid_lighthouse_serial(serial) && result.size() < 64 &&
		    std::find(result.begin(), result.end(), serial) == result.end())
			result.push_back(serial);
	};
	if (!config.is_object())
		return result;
	if (auto it = config.find("lighthouse-reference-serial"); it != config.end())
		append(*it);
	if (auto it = config.find("lighthouse-reference-candidates"); it != config.end() && it->is_array())
		for (const auto & item: *it)
			append(item);
	return result;
}
inline bool remember_lighthouse_reference(nlohmann::json & config, const std::string & serial)
{
	if (!config.is_object() || !valid_lighthouse_serial(serial))
		return false;
	auto candidates = lighthouse_reference_candidates(config);
	if (std::find(candidates.begin(), candidates.end(), serial) != candidates.end() &&
	    config.contains("lighthouse-reference-candidates") && config["lighthouse-reference-candidates"] == candidates)
		return false;
	if (std::find(candidates.begin(), candidates.end(), serial) == candidates.end())
	{
		if (candidates.size() == 64)
			return false;
		candidates.push_back(serial);
	}
	config["lighthouse-reference-candidates"] = candidates;
	return true;
}
} // namespace wivrn
