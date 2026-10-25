// SPDX-License-Identifier: GPL-3.0-or-later
#include "lighthouse_recording.h"
#include "lighthouse_native_validation.h"
#if WIVRN_FEATURE_STEAMVR_LIGHTHOUSE || WIVRN_FEATURE_SURVIVE
#include <nlohmann/json.hpp>
#if WIVRN_FEATURE_STEAMVR_LIGHTHOUSE
#include "lighthouse_capture.h"
#include "reference_recorder.hpp"
#include <pcap/usb.h>
#endif
#if WIVRN_FEATURE_SURVIVE
#include "lighthouse_build_provenance.h"
#include "survive_recording.h"
#endif
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <openssl/evp.h>
#include <optional>
#include <set>
#include <sstream>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>
#include <zlib.h>

namespace wivrn
{
namespace
{
using json = nlohmann::json;
#if WIVRN_FEATURE_STEAMVR_LIGHTHOUSE
using namespace lighthouse;
#endif
int64_t timestamp(clockid_t id)
{
	timespec t{};
	if (clock_gettime(id, &t))
		throw std::system_error(errno, std::generic_category(), "clock_gettime");
	return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}
json clock_sample(const char * phase)
{
	auto before = timestamp(CLOCK_MONOTONIC);
	auto real = timestamp(CLOCK_REALTIME);
	auto after = timestamp(CLOCK_MONOTONIC);
	auto raw_before = timestamp(CLOCK_MONOTONIC);
	auto raw = timestamp(CLOCK_MONOTONIC_RAW);
	auto raw_after = timestamp(CLOCK_MONOTONIC);
	return {{"schema_version", 1}, {"phase", phase}, {"monotonic_before_ns", before}, {"realtime_ns", real}, {"monotonic_after_ns", after}, {"offset_realtime_minus_monotonic_ns", real - (before + after) / 2}, {"uncertainty_ns", (after - before + 1) / 2}, {"raw_ns", raw}, {"raw_monotonic_before_ns", raw_before}, {"raw_monotonic_after_ns", raw_after}, {"clocks", {"CLOCK_REALTIME", "CLOCK_MONOTONIC", "CLOCK_MONOTONIC_RAW"}}};
}
std::string encoded(const json & value)
{
	return value.dump(-1, ' ', false, json::error_handler_t::replace);
}
void append(std::ofstream & file, const json & record)
{
	file << encoded(record) << '\n';
	file.flush();
	if (!file)
		throw std::runtime_error("Lighthouse metadata write failed");
}
void save(const std::filesystem::path & file, const json & value)
{
	auto temp = file;
	temp += ".tmp";
	std::ofstream out(temp);
	append(out, value);
	out.close();
	if (!out)
		throw std::runtime_error("Lighthouse metadata close failed");
	std::filesystem::rename(temp, file);
}
json read_json(const std::filesystem::path & file)
{
	std::ifstream input(file);
	if (!input)
		return nullptr;
	return json::parse(input, nullptr, false);
}
std::string digest(const std::filesystem::path & path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file)
		throw std::runtime_error("Cannot hash " + path.string());
	std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
	if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
		throw std::runtime_error("SHA-256 initialization failed");
	std::array<char, 65536> buffer;
	while (file)
	{
		file.read(buffer.data(), buffer.size());
		if (EVP_DigestUpdate(context.get(), buffer.data(), file.gcount()) != 1)
			throw std::runtime_error("SHA-256 update failed");
	}
	if (!file.eof())
		throw std::runtime_error("SHA-256 file read failed");
	std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
	unsigned size = 0;
	if (EVP_DigestFinal_ex(context.get(), bytes.data(), &size) != 1)
		throw std::runtime_error("SHA-256 finish failed");
	std::ostringstream result;
	result.imbue(std::locale::classic());
	for (unsigned i = 0; i < size; ++i)
		result << std::hex << std::setw(2) << std::setfill('0') << unsigned(bytes[i]);
	return result.str();
}
// Standard gzip JSONL; used only by the sampler thread, never pose callbacks.
class gzip_jsonl
{
	gzFile file = nullptr;

public:
	~gzip_jsonl()
	{
		if (file)
			gzclose(file);
	}
	void open(const std::filesystem::path & path)
	{
		file = gzopen(path.c_str(), "wb6");
		if (!file)
			throw std::runtime_error("Cannot open " + path.string());
	}
	void write(const json & record)
	{
		auto line = encoded(record) + '\n';
		if (!file || gzwrite(file, line.data(), line.size()) != int(line.size()) ||
		    gzflush(file, Z_SYNC_FLUSH) != Z_OK)
			throw std::runtime_error("Lighthouse gzip metadata write failed");
	}
	void close()
	{
		if (!file)
			return;
		auto f = file;
		file = nullptr;
		if (gzclose(f) != Z_OK)
			throw std::runtime_error("Lighthouse gzip metadata close failed");
	}
};
void append(gzip_jsonl & file, const json & record)
{
	file.write(record);
}
// Read one bounded JSONL record at a time when sealing the session.
class gzip_input
{
	gzFile file;

public:
	explicit gzip_input(const std::filesystem::path & path) : file(gzopen(path.c_str(), "rb"))
	{
		if (!file)
			throw std::runtime_error("Cannot read " + path.string());
	}
	~gzip_input()
	{
		gzclose(file);
	}
	bool getline(std::string & line)
	{
		line.clear();
		std::array<char, 4096> buffer;
		for (;;)
		{
			auto before = gztell(file);
			if (!gzgets(file, buffer.data(), buffer.size()))
				break;
			auto after = gztell(file);
			if (before < 0 || after < before || after - before >= z_off_t(buffer.size()))
				throw std::runtime_error("Invalid gzip read position");
			line.append(buffer.data(), size_t(after - before));
			if (line.size() > 1024 * 1024)
				throw std::runtime_error("Oversized event record");
			if (!line.empty() && line.back() == '\n')
				return true;
		}
		int code;
		const char * message = gzerror(file, &code);
		if (code != Z_OK && code != Z_STREAM_END)
			throw std::runtime_error(message);
		return !line.empty();
	}
};
#if WIVRN_FEATURE_SURVIVE
class survive_session
{
	std::filesystem::path directory;
	json manifest;
	gzip_jsonl clocks;
	std::mutex mutex;
	std::condition_variable wake;
	std::thread sampler;
	bool stopping = false, finalized = false;
	std::string clock_error;

public:
	survive_session(std::filesystem::path path, const configuration & snapshot) : directory(std::move(path))
	{
		int64_t effective_clamp = snapshot.lh_max_extrapolation.value_or(50);
		if (!snapshot.lh_max_extrapolation && getenv("LH_MAX_EXTRAPOLATION_MS"))
			effective_clamp = strtoll(getenv("LH_MAX_EXTRAPOLATION_MS"), nullptr, 10);
		manifest = {{"schema_version", 2}, {"source", "libsurvive"}, {"pose_backend", "libsurvive"}, {"session_id", directory.filename().string()}, {"logger_version", "3.0.0"}, {"status", "recording"}, {"start_realtime_ns", timestamp(CLOCK_REALTIME)}, {"end_realtime_ns", nullptr}, {"actual_wivrn_build", WIVRN_LH_WIVRN_REV}, {"monado_revision", WIVRN_LH_MONADO_REV}, {"usb_capture_engine", nullptr}, {"raw_input", "native.rec.gz"}, {"clock_domains", {{"poses", "CLOCK_MONOTONIC"}, {"native_receive", "libsurvive recording elapsed clock"}, {"native_sensor", "device-local ticks; native POSE_TIMING specifies Hz"}}}, {"configuration", {{"lighthouse-recording", true}, {"use-survive", true}, {"lh-max-extrapolation", json(effective_clamp)}, {"inherited_clamp_env_at_start", getenv("LH_MAX_EXTRAPOLATION_MS") ? json(getenv("LH_MAX_EXTRAPOLATION_MS")) : json(nullptr)}}}, {"native_recorder", {{"synchronous", true}, {"dropped", nullptr}, {"write_error_counter", nullptr}, {"close_result", "not exposed by native recorder"}, {"logging_overhead", "not measured; do not equate recorded runs with normal uninstrumented behavior"}}}, {"files_sha256", json::object()}, {"errors", json::array()}};
		save(directory / "manifest.json", manifest);
		save(directory / "wivrn-settings.json", manifest["configuration"]);
		save(directory / "build-provenance.json", json::parse(lighthouse_build_provenance));
		manifest["libsurvive_revision"] = json::parse(lighthouse_build_provenance)["libsurvive"]["revision"];
		manifest["executable"] = {{"path", std::filesystem::read_symlink("/proc/self/exe").string()},
		                          {"sha256", digest("/proc/self/exe")}};
		std::ofstream(directory / "scene-markers.jsonl");
		clocks.open(directory / "clock_sync.jsonl.gz");
		append(clocks, clock_sample("start"));
		if (!survive_recording_initialize(directory.c_str()))
			throw std::runtime_error("libsurvive pose recorder initialization failed");
		try
		{
			sampler = std::thread([this] {
				try
				{
					std::unique_lock lock(mutex);
					while (!wake.wait_for(lock, std::chrono::seconds(1), [this] { return stopping; }))
					{
						lock.unlock();
						append(clocks, clock_sample("periodic"));
						lock.lock();
					}
				}
				catch (const std::exception & e)
				{
					clock_error = e.what();
				}
			});
		}
		catch (...)
		{
			survive_recording_finish();
			throw;
		}
		std::cerr << "Lighthouse recording session (libsurvive): " << directory << '\n';
	}
	~survive_session()
	{
		finish(1);
	}
	void finish(int exit_status) noexcept
	{
		if (finalized)
			return;
		finalized = true;
		{
			std::lock_guard lock(mutex);
			stopping = true;
		}
		wake.notify_all();
		if (sampler.joinable())
			sampler.join();
		survive_recording_finish();
		try
		{
			auto & errors = manifest["errors"];
			if (exit_status)
				errors.push_back("Monado exited with status " + std::to_string(exit_status));
			if (!clock_error.empty())
				errors.push_back(clock_error);
			append(clocks, clock_sample("end"));
			clocks.close();
			auto status = read_json(directory / "pose_status.json");
			manifest["loss"]["pose"] = status;
			if (!status.is_object() || !status.value("stopped", false) || !status.value("context_initialized", false) ||
			    !status.value("error", std::string()).empty() || status.value("written", uint64_t(0)) == 0)
				errors.push_back("libsurvive pose writer incomplete, empty or failed");
			if (status.is_object() && status.value("dropped", uint64_t(0)))
				errors.push_back("libsurvive pose records dropped");
			json tags = json::object(), devices = json::object(), options = json::object();
			json integrity = {{"scope", "NUL/header/CONFIG/fixed numeric records; unknown diagnostic bodies not validated"}, {"lines", 0}, {"malformed_lines", 0}, {"examples", json::array()}};
			uint64_t native_lines = 0, bad_lines = 0;
			try
			{
				gzip_input native(directory / "native.rec.gz");
				for (std::string line; native.getline(line);)
				{
					++native_lines;
					auto problem = lighthouse::native_line_error(line);
					if (!problem.empty())
					{
						++bad_lines;
						if (integrity["examples"].size() < 8)
							integrity["examples"].push_back({{"line", native_lines}, {"reason", problem}});
						continue;
					}
					std::istringstream in(line);
					double elapsed;
					std::string device, tag;
					if (!(in >> elapsed >> device >> tag))
						continue;
					if (device == "OPTION")
					{
						tags["OPTION"] = tags.value("OPTION", uint64_t(0)) + 1;
						std::string type, value;
						in >> type;
						std::getline(in, value);
						options[tag] = {{"type", type}, {"value", value}};
					}
					else
						tags[tag] = tags.value(tag, uint64_t(0)) + 1;
					if (tag == "CONFIG")
					{
						auto text = line.substr(line.find(" CONFIG ") + 8);
						auto suffix = text.find("\\r\\n");
						if (suffix != std::string::npos)
							text.resize(suffix);
						auto config = json::parse(text, nullptr, false);
						if (config.is_object())
							devices[device] = config;
					}
				}
				manifest["native_recorder"]["gzip_crc_and_read"] = "passed";
			}
			catch (const std::exception & e)
			{
				errors.push_back(e.what());
				manifest["native_recorder"]["gzip_crc_and_read"] = "failed";
			}
			integrity["lines"] = native_lines;
			integrity["malformed_lines"] = bad_lines;
			manifest["native_record_integrity"] = integrity;
			if (bad_lines)
				errors.push_back("Malformed native records: " + std::to_string(bad_lines));
			manifest["native_tag_counts"] = tags;
			for (const char * required: {"i", "I", "B", "POSE", "POSE_TIMING", "POSE_STATE", "POSE_BIAS"})
				if (!tags.value(required, uint64_t(0)))
					errors.push_back(std::string("Missing native diagnostic: ") + required);
			save(directory / "native-devices.json", devices);
			save(directory / "native-options.json", options);
			auto settings = read_json(directory / "survive-settings.json");
			manifest["survive_settings"] = settings;
			if (settings.is_object() && settings.value("expected_trial_options", json(nullptr)).is_object())
			{
				json actual = json::object();
				for (const auto & [key, value]: settings["expected_trial_options"].items())
				{
					double effective = NAN;
					try
					{
						effective = std::stod(options.at(key).at("value").get<std::string>());
					}
					catch (...)
					{}
					actual[key] = std::isfinite(effective) ? json(effective) : json(nullptr);
					if (!std::isfinite(effective) || effective != value.get<double>())
						errors.push_back("Trial option mismatch: " + key);
				}
				manifest["trial_effective_options"] = actual;
				if (manifest["configuration"]["lh-max-extrapolation"] != 0)
					errors.push_back("Tracking comparison requires prediction clamp 0 ms");
			}
			if (!settings.is_object())
				errors.push_back("libsurvive settings snapshot missing");
			if (settings.is_object() && settings.contains("configfile"))
			{
				auto path = settings["configfile"].get<std::string>();
				if (std::filesystem::is_regular_file(path))
					std::filesystem::copy_file(path, directory / "calibration.final.json");
				else
					errors.push_back("libsurvive calibration file unavailable after close");
			}
			auto loaded = read_json(directory / "loaded-survive-objects.json");
			if (!loaded.is_array() || loaded.empty())
				errors.push_back("No loaded libsurvive objects observed");
			else
				for (auto & object: loaded)
				{
					try
					{
						object["sha256"] = digest(object["path"].get<std::string>());
					}
					catch (const std::exception & e)
					{
						object["sha256"] = nullptr;
						errors.push_back(e.what());
					}
				}
			manifest["loaded_survive_objects"] = loaded;
			manifest["native_coverage"] = "tag presence and gzip integrity only; native input loss/drop accounting unavailable";
			manifest["owner_exit_status"] = exit_status;
			manifest["end_realtime_ns"] = timestamp(CLOCK_REALTIME);
			manifest["status"] = errors.empty() ? "complete" : "abnormal";
			for (const auto & entry: std::filesystem::directory_iterator(directory))
				if (entry.is_regular_file() && entry.path().filename() != "manifest.json")
					manifest["files_sha256"][entry.path().filename().string()] = digest(entry.path());
			save(directory / "manifest.json", manifest);
		}
		catch (const std::exception & e)
		{
			try
			{
				manifest["status"] = "abnormal";
				manifest["errors"].push_back(e.what());
				manifest["end_realtime_ns"] = timestamp(CLOCK_REALTIME);
				save(directory / "manifest.json", manifest);
			}
			catch (...)
			{}
		}
	}
};
std::unique_ptr<survive_session> survive_recording_session;
#endif
#if WIVRN_FEATURE_STEAMVR_LIGHTHOUSE
struct pcap_file_header
{
	uint32_t magic = 0xa1b2c3d4;
	uint16_t major = 2, minor = 4;
	int32_t zone = 0;
	uint32_t accuracy = 0, snaplen = 65535, linktype = DLT_USB_LINUX_MMAPPED;
};
struct pcap_packet_header
{
	uint32_t seconds, microseconds, captured, original;
};
static_assert(sizeof(pcap_file_header) == 24 && sizeof(pcap_packet_header) == 16);
static_assert(sizeof(pcap_usb_header_mmapped) == 64);

#ifdef WIVRN_LH_RECORDING_TEST
capture_factory test_capture_factory = nullptr;
#endif
class native_session
{
	std::filesystem::path directory;
	json manifest, initial_devices, previous_clock;
	json jumps = json::array(), unobserved = json::array();
	std::set<std::pair<unsigned, unsigned>> targets, observed, missing;
	std::atomic<bool> stopping{false};
	std::thread capture_thread, clock_thread;
	std::mutex wait_mutex, error_mutex;
	std::condition_variable wake;
	std::vector<std::string> errors;
	std::unique_ptr<usb_capture> capture;
	gzFile output = nullptr;
	gzip_jsonl clocks, device_changes;
	uint64_t packet_count = 0, incomplete_payloads = 0, truncated_packets = 0, short_headers = 0;
	uint64_t sync_count = 0, jump_count = 0;
	int64_t max_read_uncertainty = 0, max_alignment_estimate = 0;
	bool finalized = false, write_error = false, missing_report_truncated = false;
	void error(const std::string & message) noexcept
	{
		try
		{
			std::lock_guard lock(error_mutex);
			if (errors.size() < 16)
				errors.push_back(message);
			std::cerr << "Lighthouse recording: " << message << '\n';
		}
		catch (...)
		{}
	}
	void sample(const char * phase)
	{
		auto current = clock_sample(phase);
		append(clocks, current);
		++sync_count;
		auto uncertainty = current["uncertainty_ns"].get<int64_t>();
		max_read_uncertainty = std::max(max_read_uncertainty, uncertainty);
		if (!previous_clock.is_null())
		{
			auto elapsed = current["monotonic_before_ns"].get<int64_t>() - previous_clock["monotonic_after_ns"].get<int64_t>();
			auto change = std::abs(current["offset_realtime_minus_monotonic_ns"].get<int64_t>() - previous_clock["offset_realtime_minus_monotonic_ns"].get<int64_t>());
			auto prior_uncertainty = previous_clock["uncertainty_ns"].get<int64_t>();
			if (elapsed <= 0 || current["realtime_ns"].get<int64_t>() <= previous_clock["realtime_ns"].get<int64_t>() ||
			    change > 1000000 + std::max<int64_t>(elapsed, 0) / 2000 + uncertainty + prior_uncertainty)
			{
				++jump_count;
				if (jumps.size() < 1000)
					jumps.push_back({{"monotonic_start_ns", previous_clock["monotonic_before_ns"]}, {"monotonic_end_ns", current["monotonic_after_ns"]}, {"offset_change_ns", change}});
			}
			else
				max_alignment_estimate = std::max(max_alignment_estimate, std::max(uncertainty, prior_uncertainty) + change + 1000);
		}
		previous_clock = std::move(current);
	}
	void gzip_write(const void * data, unsigned size)
	{
		if (gzwrite(output, data, size) != int(size))
		{
			write_error = true;
			throw std::runtime_error("USB gzip write failed");
		}
	}
	void capture_loop() noexcept
	{
		try
		{
			while (!stopping)
			{
				usb_packet packet;
				if (!capture->poll(packet))
				{
					std::unique_lock lock(wait_mutex);
					wake.wait_for(lock, std::chrono::milliseconds(1), [this] { return stopping.load(); });
					continue;
				}
				if (packet.bytes.size() < sizeof(pcap_usb_header_mmapped))
				{
					++short_headers;
					continue;
				}
				pcap_usb_header_mmapped usb{};
				std::memcpy(&usb, packet.bytes.data(), sizeof(usb));
				auto key = std::make_pair(unsigned(usb.bus_id), unsigned(usb.device_address));
				if (!targets.contains(key))
					continue;
				if (packet.header.caplen != packet.bytes.size() || packet.header.caplen > 65535 || packet.header.caplen > packet.header.len)
					throw std::runtime_error("Invalid usbmon capture length");
				pcap_packet_header header{uint32_t(packet.header.ts.tv_sec), uint32_t(packet.header.ts.tv_usec), packet.header.caplen, packet.header.len};
				gzip_write(&header, sizeof(header));
				gzip_write(packet.bytes.data(), packet.bytes.size());
				++packet_count;
				observed.insert(key);
				truncated_packets += packet.header.caplen < packet.header.len;
				incomplete_payloads += usb.data_flag == 0 && (usb.data_len > packet.bytes.size() - sizeof(usb) || usb.urb_len > packet.bytes.size() - sizeof(usb));
			}
		}
		catch (const std::exception & e)
		{
			error(e.what());
		}
	}
	void clock_loop() noexcept
	{
		try
		{
			auto prior_devices = initial_devices;
			while (!stopping)
			{
				std::unique_lock lock(wait_mutex);
				wake.wait_for(lock, std::chrono::seconds(1), [this] { return stopping.load(); });
				if (stopping)
					break;
				lock.unlock();
				sample("periodic");
				auto current_devices = capture ? capture->current_devices() : enumerate_lighthouse_usb_devices();
				if (current_devices != prior_devices)
				{
					append(device_changes, {{"capture_time_ns", timestamp(CLOCK_MONOTONIC)}, {"capture_clock", "CLOCK_MONOTONIC"}, {"devices", current_devices}});
					for (auto device: current_devices)
					{
						auto key = std::make_pair(device["bus"].get<unsigned>(), device["address"].get<unsigned>());
						if (!targets.contains(key) && !missing.contains(key))
						{
							if (missing.size() < 4096)
							{
								missing.insert(key);
								unobserved.push_back(device);
							}
							else
								missing_report_truncated = true;
						}
					}
					prior_devices = std::move(current_devices);
				}
			}
		}
		catch (const std::exception & e)
		{
			error(e.what());
		}
	}

public:
	explicit native_session(std::filesystem::path path) : directory(std::move(path))
	{
		utsname kernel{};
		uname(&kernel);
		manifest = {{"schema_version", 1}, {"source", "steamvr_lh"}, {"pose_backend", "steamvr_lh"}, {"session_id", directory.filename().string()}, {"logger_version", "2.1.0"}, {"start_realtime_ns", timestamp(CLOCK_REALTIME)}, {"end_realtime_ns", nullptr}, {"kernel", kernel.release}, {"actual_wivrn_build", WIVRN_LH_WIVRN_REV}, {"monado_revision", WIVRN_LH_MONADO_REV}, {"usb_capture_engine", "embedded_linux_usbmon_libpcap"}, {"libsurvive_revision", nullptr}, {"libsurvive_format_reference_revision", "23f03665070549f0c13a6e768dfaf7764f75c417"}, {"clock_domains", {{"poses", "CLOCK_MONOTONIC"}, {"usb", "CLOCK_REALTIME"}, {"sync", {"CLOCK_REALTIME", "CLOCK_MONOTONIC", "CLOCK_MONOTONIC_RAW"}}}}, {"configuration", {{"output_directory", directory.string()}, {"usb_and_pose", true}, {"sync_interval_s", 1}}}, {"calibration_files", nullptr}, {"calibration_metadata_available", false}, {"status", "recording"}, {"files_sha256", json::object()}, {"errors", json::array()}};

		const char * reference = getenv("LH_STABILIZE_REFERENCE_SERIAL");
		manifest["configuration"]["lighthouse-reference-serial"] = reference ? reference : "";
		manifest["configuration"]["lighthouse-reference-convention"] = "base_station_origin_xyzw_v1";

#ifdef WIVRN_LH_RECORDING_TEST
		manifest["synthetic"] = true;
		manifest["test_harness"] = true;
#endif
		manifest["stream_compression"] = {{"format", "gzip"}, {"level", 6}, {"jsonl_streams", {"poses.jsonl.gz", "events.jsonl.gz", "clock_sync.jsonl.gz", "usb_devices.jsonl.gz"}}};
		save(directory / "manifest.json", manifest);
		clocks.open(directory / "clock_sync.jsonl.gz");
		device_changes.open(directory / "usb_devices.jsonl.gz");
		sample("start");
		try
		{
#ifdef WIVRN_LH_RECORDING_TEST
			capture = test_capture_factory ? test_capture_factory() : open_usbmon_capture();
#else
			capture = open_usbmon_capture();
#endif
			initial_devices = capture->devices();
		}
		catch (const std::exception & e)
		{
			error(e.what());
			try
			{
				initial_devices = enumerate_lighthouse_usb_devices();
			}
			catch (const std::exception & enumeration_error)
			{
				error(enumeration_error.what());
				initial_devices = json::array();
			}
		}
		save(directory / "devices.json", initial_devices);
		append(device_changes, {{"capture_time_ns", timestamp(CLOCK_MONOTONIC)}, {"capture_clock", "CLOCK_MONOTONIC"}, {"devices", initial_devices}});
		std::ofstream listing(directory / "usb.pcap.gz.usbdevs");
		listing.imbue(std::locale::classic());
		for (const auto & device: initial_devices)
		{
			if (!device.value("supported_by_pinned_libsurvive", false))
				continue;
			targets.emplace(device["bus"].get<unsigned>(), device["address"].get<unsigned>());
			listing << device["vid"].get<unsigned>() << ' ' << device["pid"].get<unsigned>() << ' ' << device["bus"].get<unsigned>() << ' ' << device["address"].get<unsigned>() << ' ' << device["codename"].get<std::string>() << '\n';
		}
		listing.close();
		if (!listing)
			error("Cannot write USB device correspondence");
		output = gzopen((directory / "usb.pcap.gz").c_str(), "wb");
		if (!output)
			error("Cannot open USB capture output");
		else
		{
			pcap_file_header header;
			try
			{
				gzip_write(&header, sizeof(header));
			}
			catch (const std::exception & e)
			{
				error(e.what());
			}
		}
		if (targets.empty())
			error("No supported Lighthouse USB devices observed at startup");
		try
		{
			clock_thread = std::thread([this] { clock_loop(); });
			if (capture && output && !targets.empty() && !write_error)
				capture_thread = std::thread([this] { capture_loop(); });
		}
		catch (const std::exception & e)
		{
			error(e.what());
		}
		std::cerr << "Lighthouse recording session: " << directory << '\n';
	}
	~native_session()
	{
		finish();
	}
	void finish(std::optional<int> exit_status = std::nullopt) noexcept
	{
		if (finalized)
			return;
		finalized = true;
		stopping = true;
		wake.notify_all();
		if (capture_thread.joinable())
			capture_thread.join();
		if (clock_thread.joinable())
			clock_thread.join();
		try
		{
			manifest["owner_exit_status"] = exit_status ? json(*exit_status) : json(nullptr);
			if (exit_status && *exit_status != 0)
				error("Monado exited with status " + std::to_string(*exit_status));
			sample("end");
			clocks.close();
			device_changes.close();
			if (output)
			{
				if (gzclose(output) != Z_OK)
				{
					write_error = true;
					error("USB gzip close failed");
				}
				output = nullptr;
			}
			auto usb_stats = capture ? capture->statistics() : json{{"schema_version", 1}, {"stats_available", false}};
			capture.reset(); // Close the kernel observer before checksums and final metadata.
			usb_stats["write_error"] = write_error;
			save(directory / "usb_stats.json", usb_stats);
			auto poses = read_json(directory / "pose_status.json");
			if (!poses.is_object() || !poses.value("stopped", false))
				error("Pose writer not finalized");
			else if (!poses.value("error", std::string()).empty())
				error("Pose writer error: " + poses["error"].get<std::string>());
			if (!poses.is_object() || poses.value("written", uint64_t(0)) == 0)
				error("No driver reference poses recorded");
			if (!packet_count)
				error("No Lighthouse USB packets recorded");
			for (const auto & device: initial_devices)
				if (!observed.contains({device["bus"].get<unsigned>(), device["address"].get<unsigned>()}))
					unobserved.push_back(device);
			json announcements = json::object();
			gzip_input events(directory / "events.jsonl.gz");
			for (std::string line; events.getline(line);)
			{
				auto event = json::parse(line, nullptr, false);
				if (event.is_discarded())
				{
					error("Invalid event record");
					break;
				}
				if (event.value("event", std::string()) == "device_announced" && announcements.size() < 1024)
					announcements[event["device_id"].get<std::string>()] = {{"device_id", event["device_id"]}, {"device_class", event["device_class"]}};
			}
			manifest["valve_devices_observed"] = json::array();
			manifest["base_stations_observed"] = json::array();
			for (const auto & device: announcements)
			{
				manifest["valve_devices_observed"].push_back(device);
				if (device["device_class"] == 4)
					manifest["base_stations_observed"].push_back(device);
			}
			manifest["base_station_metadata_basis"] = "delivered driver announcements only";
			manifest["usb_driver_pose_mapping"] = "Not inferred: use captured device configuration; receiver and tracked-object serials may differ";
			manifest["loss"] = {{"pose", poses}, {"usb", usb_stats}, {"usb_incomplete_payloads", incomplete_payloads}, {"usb_truncated_packets", truncated_packets}, {"usb_short_headers", short_headers}};
			manifest["usb_packet_count"] = packet_count;
			manifest["usb_unobserved_devices"] = unobserved;
			manifest["usb_unobserved_report_truncated"] = missing_report_truncated;
			manifest["clock_sync"] = {{"sample_count", sync_count}, {"max_read_uncertainty_ns", max_read_uncertainty}, {"max_estimated_interval_alignment_error_ns", max_alignment_estimate}, {"clock_jump_count", jump_count}, {"clock_jump_intervals", jumps}, {"clock_jump_report_truncated", jump_count > jumps.size()}, {"error_model", "read brackets + observed interval offset variation + 1000ns USB quantization; not a guaranteed bound"}, {"unsampled_steps_detectable", false}};
			for (const auto & entry: std::filesystem::directory_iterator(directory))
			{
				auto name = entry.path().filename().string();
				if (!entry.is_regular_file() || name == "manifest.json" || name == "validation.json" || name.ends_with(".tmp"))
					continue;
				manifest["files_sha256"][name] = digest(entry.path());
			}
			try
			{
				manifest["owner_executable_sha256"] = digest("/proc/self/exe");
			}
			catch (const std::exception & e)
			{
				error(e.what());
			}
			manifest["end_realtime_ns"] = timestamp(CLOCK_REALTIME);
			manifest["errors"] = errors;
			manifest["status"] = errors.empty() ? "complete" : "abnormal";
			save(directory / "manifest.json", manifest);
			auto warnings = json::array();
			if (poses.is_object() && poses.value("dropped", uint64_t(0)))
				warnings.push_back("pose_records_dropped");
			if (!usb_stats.value("stats_available", false))
				warnings.push_back("usb_capture_loss_unknown");
			else if (usb_stats.value("dropped", uint64_t(0)) || usb_stats.value("interface_dropped", uint64_t(0)))
				warnings.push_back("usb_capture_records_dropped");
			if (short_headers)
				warnings.push_back("usb_short_headers_skipped");
			if (incomplete_payloads || truncated_packets)
				warnings.push_back("usb_payload_incomplete");
			if (jump_count)
				warnings.push_back("clock_jump_intervals");
			if (!unobserved.empty())
				warnings.push_back("usb_target_devices_not_observed");
			save(directory / "validation.json", {{"schema_version", 1}, {"valid", errors.empty()}, {"issues", errors}, {"warnings", warnings}, {"validation_scope", "capture counters, packet lengths and writer finalization; full content checks via offline validate"}, {"full_content_validation", false}, {"pose_count", poses.is_object() ? poses.value("written", uint64_t(0)) : 0}, {"usb_packet_count", packet_count}, {"clock_sync", manifest["clock_sync"]}});
		}
		catch (const std::exception & e)
		{
			error(e.what());
			if (output)
			{
				gzclose(output);
				output = nullptr;
			}
			try
			{
				manifest["status"] = "abnormal";
				manifest["errors"] = errors;
				save(directory / "manifest.json", manifest);
			}
			catch (...)
			{}
		}
	}
};
std::unique_ptr<native_session> session;
#endif
bool started = false;
} // namespace
#if defined(WIVRN_LH_RECORDING_TEST) && WIVRN_FEATURE_STEAMVR_LIGHTHOUSE
namespace lighthouse
{
void set_test_capture_factory(capture_factory factory)
{
	test_capture_factory = factory;
}
} // namespace lighthouse
#endif
void start_lighthouse_recording(const configuration & snapshot) noexcept
{
	// Dashboard settings are the sole public trigger, read once for this Monado lifetime.
	if (started)
		return;
	started = true;
	setenv("LH_RECORDER_ENABLE", "0", true);
	unsetenv("LH_RECORDER_OUTPUT");
	if (!snapshot.lighthouse_recording_enabled)
		return;
	try
	{
		auto root = expand_lighthouse_recording_directory(snapshot.lighthouse_recording_output);
		if (root.empty() || !root.is_absolute())
			throw std::runtime_error("Lighthouse output directory must be absolute or begin with ~/");
		std::filesystem::create_directories(root);
		auto now = std::chrono::system_clock::now().time_since_epoch();
		std::time_t wall = std::time(nullptr);
		std::tm local{};
		localtime_r(&wall, &local);
		char date[32]{};
		std::strftime(date, sizeof(date), "%Y-%m-%d_%H%M%S", &local);
		auto directory = root / (std::string(date) + (snapshot.selected_lighthouse_backend() == lighthouse_backend::survive ? "_libsurvive_" : "_") + std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() % 1000000000) + "_" + std::to_string(getpid()));
		if (!std::filesystem::create_directory(directory))
			throw std::runtime_error("Lighthouse session directory collision");
		std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
		// Internal handoff to the Monado component. No external environment opt-in or helper program.
		setenv("LH_RECORDER_OUTPUT", directory.c_str(), true);
		setenv("LH_RECORDER_ENABLE", "1", true);
		if (snapshot.selected_lighthouse_backend() == lighthouse_backend::survive)
		{
#if WIVRN_FEATURE_SURVIVE
			survive_recording_session = std::make_unique<survive_session>(directory, snapshot);
#endif
		}
		else
		{
#if WIVRN_FEATURE_STEAMVR_LIGHTHOUSE
			lighthouse_reference::initialize();
			session = std::make_unique<native_session>(directory);
#endif
		}
	}
	catch (const std::exception & e)
	{
		std::cerr << "Lighthouse recording unavailable: " << e.what() << '\n';
	}
}
void stop_lighthouse_recording(int exit_status) noexcept
{
#if WIVRN_FEATURE_SURVIVE
	if (survive_recording_session)
		survive_recording_session->finish(exit_status);
	survive_recording_session.reset();
#endif
#if WIVRN_FEATURE_STEAMVR_LIGHTHOUSE
	lighthouse_reference::finish();
	if (session)
		session->finish(exit_status);
	session.reset();
#endif
}
} // namespace wivrn
#else
namespace wivrn
{
void start_lighthouse_recording(const configuration &) noexcept {}
void stop_lighthouse_recording(int) noexcept {}
} // namespace wivrn
#endif
