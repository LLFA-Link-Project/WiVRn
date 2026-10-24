// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace wivrn
{
enum class lighthouse_backend
{
	none,
	steamvr,
	survive,
};

// A legacy config or environment can request both. Prefer libsurvive so that
// only one driver owns the receivers; ignore backends absent from this build.
constexpr lighthouse_backend select_lighthouse_backend(bool steamvr_requested, bool survive_requested, bool steamvr_supported, bool survive_supported)
{
	if (survive_requested && survive_supported)
		return lighthouse_backend::survive;
	if (steamvr_requested && steamvr_supported)
		return lighthouse_backend::steamvr;
	return lighthouse_backend::none;
}
} // namespace wivrn
