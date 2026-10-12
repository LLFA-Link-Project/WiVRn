// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include <nlohmann/json.hpp>
#include <pcap/pcap.h>
#include <span>
namespace wivrn::lighthouse
{
struct usb_packet
{
	pcap_pkthdr header{};
	std::span<const uint8_t> bytes;
};
// Capture owns packet storage until the next poll. Only its dedicated thread polls.
class usb_capture
{
public:
	virtual ~usb_capture() = default;
	virtual nlohmann::json devices() const = 0;
	virtual nlohmann::json current_devices() const;
	virtual bool poll(usb_packet &) = 0;
	virtual nlohmann::json statistics() = 0;
};
std::unique_ptr<usb_capture> open_usbmon_capture();
nlohmann::json enumerate_lighthouse_usb_devices();
#ifdef WIVRN_LH_RECORDING_TEST
using capture_factory = std::unique_ptr<usb_capture> (*)();
void set_test_capture_factory(capture_factory);
#endif
} // namespace wivrn::lighthouse
