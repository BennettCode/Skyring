#include "bridge/Gamepad.h"

#include <Windows.h>
#include <SetupAPI.h>
#include <hidsdi.h>
#include <hidpi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// DualSense (USB or Bluetooth) read through the Windows HID API. The device is opened shared, so other readers (Elden Ring's
// libScePad, Steam) still can. One thread: find → read reports (blocking overlapped reads) → on error close and search again.
namespace sxer::pad
{
	namespace
	{
		constexpr USHORT kSonyVid = 0x054C;
		constexpr USHORT kPids[] = { 0x0CE6, 0x0DF2 };  // DualSense, DualSense Edge

		std::mutex g_lock;
		std::optional<DsState> g_state;
		std::atomic<std::uint32_t> g_packet{ 0 };

		struct Device
		{
			HANDLE handle = INVALID_HANDLE_VALUE;
			std::size_t reportLength = 0;
			bool bluetooth = false;
			USHORT pid = 0;
		};

		// Enumerates present HID interfaces and opens the first DualSense.
		Device Find()
		{
			Device found;
			GUID hid{};
			HidD_GetHidGuid(&hid);
			const HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
			if (set == INVALID_HANDLE_VALUE) {
				return found;
			}
			SP_DEVICE_INTERFACE_DATA iface{ .cbSize = sizeof(SP_DEVICE_INTERFACE_DATA) };
			for (DWORD i = 0; found.handle == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &iface); ++i) {
				DWORD size = 0;
				SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &size, nullptr);
				if (size == 0) {
					continue;
				}
				std::vector<std::uint8_t> buffer(size);
				auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
				detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
				if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, size, nullptr, nullptr)) {
					continue;
				}
				// No access rights needed to read the attributes; keeps us from opening (and waking) unrelated devices.
				const HANDLE probe = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
				if (probe == INVALID_HANDLE_VALUE) {
					continue;
				}
				HIDD_ATTRIBUTES attr{ .Size = sizeof(HIDD_ATTRIBUTES) };
				const bool sony = HidD_GetAttributes(probe, &attr) && attr.VendorID == kSonyVid &&
				                  std::find(std::begin(kPids), std::end(kPids), attr.ProductID) != std::end(kPids);
				CloseHandle(probe);
				if (!sony) {
					continue;
				}
				HANDLE handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
					OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
				if (handle == INVALID_HANDLE_VALUE) {
					handle = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
						FILE_FLAG_OVERLAPPED, nullptr);
				}
				if (handle == INVALID_HANDLE_VALUE) {
					SKSE::log::warn("[pad] DualSense found (pid {:04X}) but can't open it (error {})", attr.ProductID, GetLastError());
					continue;
				}
				PHIDP_PREPARSED_DATA pre = nullptr;
				HIDP_CAPS caps{};
				if (HidD_GetPreparsedData(handle, &pre)) {
					HidP_GetCaps(pre, &caps);
					HidD_FreePreparsedData(pre);
				}
				found = { handle, caps.InputReportByteLength, caps.InputReportByteLength != 64, attr.ProductID };
			}
			SetupDiDestroyDeviceInfoList(set);
			return found;
		}

		// Bluetooth pads send a short report until feature report 0x05 (calibration) is read; reading it switches on report 0x31.
		void EnableExtendedReports(const Device& a_dev)
		{
			std::uint8_t feature[64]{ 0x05 };
			const bool ok = HidD_GetFeature(a_dev.handle, feature, sizeof(feature));
			SKSE::log::info("[pad] Bluetooth: extended reports {}", ok ? "requested" : "request failed (simple reports still work)");
		}

		void Publish(const std::optional<DsState>& a_state)
		{
			std::lock_guard lock(g_lock);
			if (g_state != a_state) {
				g_state = a_state;
				g_packet.fetch_add(1, std::memory_order_relaxed);
			}
		}

		void LogButtons(std::uint32_t a_old, std::uint32_t a_new)
		{
			if (a_old != a_new) {
				SKSE::log::info("[pad] buttons {:05X} -> {:05X}", a_old, a_new);
			}
		}

		void Run()
		{
			auto lastScan = std::chrono::steady_clock::now() - std::chrono::seconds(10);
			bool reportedMissing = false;
			for (;;) {
				if (std::chrono::steady_clock::now() - lastScan < std::chrono::seconds(2)) {
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					continue;
				}
				lastScan = std::chrono::steady_clock::now();
				Device dev = Find();
				if (dev.handle == INVALID_HANDLE_VALUE) {
					if (!reportedMissing) {
						SKSE::log::info("[pad] no DualSense found; looking again every 2 s");
						reportedMissing = true;
					}
					continue;
				}
				reportedMissing = false;
				SKSE::log::info("[pad] DualSense opened (pid {:04X}, {}, report {} bytes)", dev.pid, dev.bluetooth ? "Bluetooth" : "USB", dev.reportLength);
				if (dev.bluetooth) {
					EnableExtendedReports(dev);
				}
				std::vector<std::uint8_t> report(dev.reportLength > 0 ? dev.reportLength : 128);
				OVERLAPPED ov{};
				ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
				std::uint32_t lastButtons = 0;
				std::uint64_t reports = 0, unparsed = 0;
				DWORD error = 0;
				for (;;) {
					ResetEvent(ov.hEvent);
					DWORD read = 0;
					if (!ReadFile(dev.handle, report.data(), static_cast<DWORD>(report.size()), &read, &ov)) {
						if (GetLastError() != ERROR_IO_PENDING) {
							error = GetLastError();
							break;
						}
						// Blocks until a report arrives; unplugging completes the read with an error.
						if (!GetOverlappedResult(dev.handle, &ov, &read, TRUE)) {
							error = GetLastError();
							break;
						}
					}
					const auto state = ParseReport(report.data(), read, dev.bluetooth);
					if (!state) {
						if (unparsed++ == 0) {
							SKSE::log::info("[pad] ignoring report id {:02X} ({} bytes)", report[0], read);
						}
						continue;
					}
					if (reports++ == 0) {
						SKSE::log::info("[pad] first report: id {:02X}, left stick {} {}", report[0], state->lx, state->ly);
					}
					LogButtons(lastButtons, state->buttons);
					lastButtons = state->buttons;
					Publish(state);
				}
				CancelIo(dev.handle);
				CloseHandle(ov.hEvent);
				CloseHandle(dev.handle);
				Publish(std::nullopt);
				SKSE::log::info("[pad] DualSense lost (error {}, {} reports); looking again", error, reports);
			}
		}
	}

	void Start()
	{
		static std::once_flag once;
		std::call_once(once, [] { std::thread(Run).detach(); });
	}

	std::optional<DsState> Latest()
	{
		std::lock_guard lock(g_lock);
		return g_state;
	}

	std::uint32_t Packet() { return g_packet.load(std::memory_order_relaxed); }
}
