/* WiVRn VR streaming
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <gst/gst.h>
#include <string>

namespace wivrn::gstreamer
{
// Keep the emitting element and GError domain/code: an appsrc flow error alone
// does not identify the decoder/converter which rejected the buffer.
inline std::string describe_bus_message(GstMessage * message)
{
	GError * error = nullptr;
	gchar * debug = nullptr;
	if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
		gst_message_parse_error(message, &error, &debug);
	else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING)
		gst_message_parse_warning(message, &error, &debug);
	auto path = GST_MESSAGE_SRC(message) ? gst_object_get_path_string(GST_MESSAGE_SRC(message)) : nullptr;
	std::string result = path ? path : "unknown element";
	g_free(path);
	if (error)
	{
		result += ": ";
		result += error->message;
		result += " [";
		result += g_quark_to_string(error->domain);
		result += ":" + std::to_string(error->code) + "]";
	}
	if (debug)
		result += std::string(" (") + debug + ")";
	g_clear_error(&error);
	g_free(debug);
	return result;
}
} // namespace wivrn::gstreamer
