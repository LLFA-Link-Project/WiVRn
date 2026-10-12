// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "driver/configuration.h"
namespace wivrn
{
// Called once in the Monado child before ipc_server_main_common. Never reloads settings.
void start_lighthouse_recording(const configuration & snapshot) noexcept;
} // namespace wivrn
