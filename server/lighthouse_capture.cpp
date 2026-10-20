// SPDX-License-Identifier: GPL-3.0-or-later
#include "lighthouse_capture.h"
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

namespace wivrn::lighthouse
{
namespace
{
// Reuse the pinned libsurvive driver_usbmon.c VID/PID/codename mapping and .usbdevs format.
struct supported_device
{
	unsigned vid, pid;
	const char * codename;
};
constexpr supported_device supported[] = {
        {0x28de, 0x2000, "HMD"},
        {0x0bb4, 0x2c87, "BD"},
        {0x28de, 0x2101, "WM"},
        {0x28de, 0x2022, "TR"},
        {0x28de, 0x2300, "T2"},
        {0x28de, 0x2012, "WW"},
        {0x28de, 0x2102, "KN"}};
std::string read_text(const std::filesystem::path & path)
{
	std::ifstream file(path);
	std::string result;
	std::getline(file, result);
	return result;
}
class linux_usbmon final : public usb_capture
{
	pcap_t * handle = nullptr;
	nlohmann::json initial_devices;

public:
	linux_usbmon()
	{
		initial_devices = enumerate_lighthouse_usb_devices();
		char error[PCAP_ERRBUF_SIZE]{};
		handle = pcap_create("usbmon0", error);
		if (!handle)
			throw std::runtime_error(std::string("usbmon: ") + error);
		try
		{
			if (pcap_set_snaplen(handle, 65535) || pcap_set_promisc(handle, 0) || pcap_set_timeout(handle, 100))
				throw std::runtime_error("usbmon capture configuration failed");
			int activated = pcap_activate(handle);
			if (activated < 0)
				throw std::runtime_error(std::string("usbmon capture failed: ") + pcap_geterr(handle) + "; load usbmon and grant read access to /dev/usbmon0");
			if (pcap_datalink(handle) != DLT_USB_LINUX_MMAPPED)
				throw std::runtime_error("usbmon requires DLT_USB_LINUX_MMAPPED (220)");
			if (pcap_setnonblock(handle, 1, error))
				throw std::runtime_error(std::string("usbmon nonblocking mode failed: ") + error);
		}
		catch (...)
		{
			pcap_close(handle);
			handle = nullptr;
			throw;
		}
	}
	~linux_usbmon() override
	{
		if (handle)
			pcap_close(handle);
	}
	nlohmann::json devices() const override
	{
		return initial_devices;
	}
	bool poll(usb_packet & result) override
	{
		pcap_pkthdr * header = nullptr;
		const u_char * bytes = nullptr;
		int rc = pcap_next_ex(handle, &header, &bytes);
		if (rc == 0)
			return false;
		if (rc != 1)
			throw std::runtime_error(std::string("usbmon read failed: ") + pcap_geterr(handle));
		result.header = *header;
		result.bytes = {bytes, header->caplen};
		return true;
	}
	nlohmann::json statistics() override
	{
		pcap_stat stats{};
		bool available = pcap_stats(handle, &stats) == 0;
		return {{"schema_version", 1}, {"stats_available", available}, {"received", stats.ps_recv}, {"dropped", stats.ps_drop}, {"interface_dropped", stats.ps_ifdrop}};
	}
};
} // namespace

nlohmann::json enumerate_lighthouse_usb_devices()
{
	auto result = nlohmann::json::array();
	// sysfs enumeration performs no USB open, claim, detach, or control request.
	for (const auto & entry: std::filesystem::directory_iterator("/sys/bus/usb/devices"))
	{
		try
		{
			auto path = entry.path();
			unsigned vid = std::stoul(read_text(path / "idVendor"), nullptr, 16);
			unsigned pid = std::stoul(read_text(path / "idProduct"), nullptr, 16);
			if (vid != 0x28de && vid != 0x0bb4)
				continue;
			nlohmann::json codename = nullptr;
			for (auto device: supported)
				if (device.vid == vid && device.pid == pid)
					codename = device.codename;
			auto serial = read_text(path / "serial");
			result.push_back({{"sysfs_path", path.filename().string()}, {"vid", vid}, {"pid", pid}, {"bus", std::stoul(read_text(path / "busnum"))}, {"address", std::stoul(read_text(path / "devnum"))}, {"serial", serial.empty() ? nlohmann::json(nullptr) : nlohmann::json(serial)}, {"stable_id", serial.empty() ? nlohmann::json(nullptr) : nlohmann::json(serial)}, {"codename", codename}, {"supported_by_pinned_libsurvive", !codename.is_null()}});
		}
		catch (const std::invalid_argument &)
		{}
		catch (const std::out_of_range &)
		{}
	}
	return result;
}
nlohmann::json usb_capture::current_devices() const
{
	return enumerate_lighthouse_usb_devices();
}
std::unique_ptr<usb_capture> open_usbmon_capture()
{
	return std::make_unique<linux_usbmon>();
}
} // namespace wivrn::lighthouse
