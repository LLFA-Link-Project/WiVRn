/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "wivrn_ipc.h"
#include "driver/wivrn_connection.h"
#include "wivrn_config.h"

#if WIVRN_FEATURE_STEAMVR_LIGHTHOUSE
#include "steamvr_lh_interface.h"
#endif

std::unique_ptr<wivrn::wivrn_connection> connection;

std::optional<to_monado::packets> receive_from_main()
{
	return wivrn_ipc_socket_monado->receive();
}

void standby_lighthouse_trackers()
{
#if WIVRN_FEATURE_STEAMVR_LIGHTHOUSE
	if (steamvr_lh_standby_trackers(nullptr))
		return;
#endif
	send_to_main(from_monado::server_error{
	        .where = "Turn off Lighthouse trackers",
	        .message = "SteamVR tracked devices support is not active in this session.",
	});
}
