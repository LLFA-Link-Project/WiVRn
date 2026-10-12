// SPDX-License-Identifier: GPL-3.0-or-later
#include "lighthouse_capture.h"
#include <array>
#include <chrono>
#include <cstring>
#include <pcap/usb.h>
#include <stdexcept>
#include <sys/time.h>
#include <thread>

namespace
{
unsigned live_captures = 0;
class synthetic_capture final : public wivrn::lighthouse::usb_capture
{
	std::array<uint8_t, 68> bytes{};
	uint64_t count = 0;

public:
	synthetic_capture()
	{
		++live_captures;
	}
	~synthetic_capture() override
	{
		--live_captures;
	}
	nlohmann::json devices() const override
	{
		return nlohmann::json::array({{{"sysfs_path", "synthetic"}, {"vid", 0x28de}, {"pid", 0x2101}, {"bus", 1}, {"address", 7}, {"serial", "synthetic-radio"}, {"stable_id", "synthetic-radio"}, {"codename", "WM"}, {"supported_by_pinned_libsurvive", true}}});
	}
	nlohmann::json current_devices() const override
	{
		return devices();
	}
	bool poll(wivrn::lighthouse::usb_packet & packet) override
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		timeval time{};
		gettimeofday(&time, nullptr);
		pcap_usb_header_mmapped usb{};
		auto id = ++count;
		std::memcpy(&usb.id, &id, sizeof(id));
		int64_t sec = time.tv_sec;
		std::memcpy(&usb.ts_sec, &sec, sizeof(sec));
		usb.ts_usec = time.tv_usec;
		usb.event_type = 'C';
		usb.transfer_type = 1;
		usb.endpoint_number = 0x81;
		usb.device_address = 7;
		usb.bus_id = 1;
		usb.setup_flag = '-';
		usb.data_flag = 0;
		usb.urb_len = usb.data_len = 4;
		std::memcpy(bytes.data(), &usb, sizeof(usb));
		bytes[64] = 0xff; // synthetic, unrecognized sensor report; never a pose-solving fixture
		packet.header = {time, bytes.size(), bytes.size()};
		packet.bytes = bytes;
		return true;
	}
	nlohmann::json statistics() override
	{
		return {{"schema_version", 1}, {"stats_available", true}, {"received", count}, {"dropped", 0}, {"interface_dropped", 0}, {"synthetic", true}};
	}
};
} // namespace
std::unique_ptr<wivrn::lighthouse::usb_capture> make_synthetic_capture()
{
	return std::make_unique<synthetic_capture>();
}
std::unique_ptr<wivrn::lighthouse::usb_capture> make_denied_capture()
{
	throw std::runtime_error("SYNTHETIC usbmon: permission denied");
}

unsigned synthetic_capture_instances()
{
	return live_captures;
}
