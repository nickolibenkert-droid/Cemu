#include "AndroidUsbWiimote.h"
#include "util/helpers/helpers.h"

#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>

#include <deque>

namespace
{
	constexpr unsigned kReadTimeoutMs = 100;
	constexpr unsigned kWriteTimeoutMs = 1000;
	// probes of empty slots run under the adapter lock, keep them short
	constexpr unsigned kProbeTimeoutMs = 50;
	constexpr size_t kMaxPacketSize = 64;
	constexpr size_t kMaxQueuedPackets = 64;

	constexpr uint8 kHidRequestTypeOut = 0x21; // host to device, class, interface
	constexpr uint8 kHidSetReport = 0x09;
	constexpr uint16 kHidReportTypeOutput = 0x0200;

	constexpr uint8 kStatusRequestReport = 0x15;

	struct Slot
	{
		uint8 interfaceNumber = 0;
		uint8 inEndpoint = 0;

		std::mutex mutex;
		std::deque<std::vector<uint8>> packets;
		// a slot only counts as a Wii Remote once something answered on it,
		// empty DolphinBar slots never send anything
		std::atomic_bool hasRemote = false;
		std::atomic_bool failed = false;
		uint16 lastButtons = 0;

		std::thread reader;
	};

	std::shared_mutex s_mutex;
	int s_fd = -1;
	uint32 s_generation = 0;
	std::atomic_bool s_running = false;
	std::vector<std::unique_ptr<Slot>> s_slots;

	bool IsCompleteInputReport(const uint8* data, size_t size)
	{
		if (size == 0)
			return false;
		// Sizes include the report ID. The shared parser assumes complete HID reports.
		switch (data[0])
		{
		case 0x20: return size >= 7;  // status
		case 0x21: return size >= 22; // memory read
		case 0x22: return size >= 5;  // acknowledgement
		case 0x30: return size >= 3;  // buttons
		case 0x31: return size >= 6;  // buttons + acceleration
		case 0x32: return size >= 11; // buttons + extension
		case 0x33: return size >= 18; // buttons + acceleration + IR
		case 0x34:
		case 0x35:
		case 0x36:
		case 0x37:
		case 0x3d:
		case 0x3e:
		case 0x3f: return size >= 22;
		default: return false;
		}
	}

	void ReaderThread(int fd, Slot* slot)
	{
		SetThreadName("Wiimote-usb");
		std::array<uint8, kMaxPacketSize> buffer{};
		while (s_running.load(std::memory_order_relaxed))
		{
			usbdevfs_bulktransfer transfer{};
			transfer.ep = slot->inEndpoint;
			transfer.len = buffer.size();
			transfer.timeout = kReadTimeoutMs;
			transfer.data = buffer.data();
			const int result = ioctl(fd, USBDEVFS_BULK, &transfer);
			if (result > 0)
			{
				if (!IsCompleteInputReport(buffer.data(), static_cast<size_t>(result)))
					continue;
				// test aid: log button changes so input can be checked without a game
				if (buffer[0] == 0x20 || buffer[0] == 0x21 || buffer[0] == 0x22 || (buffer[0] >= 0x30 && buffer[0] <= 0x37))
				{
					const uint16 buttons = ((buffer[1] << 8) | buffer[2]) & 0x1F9F;
					if (buttons != slot->lastButtons)
					{
						slot->lastButtons = buttons;
						cemuLog_log(LogType::Force, "DolphinBar slot {}: buttons {:#06x} (report {:#x})", slot->interfaceNumber, buttons, buffer[0]);
					}
				}
				std::scoped_lock lock(slot->mutex);
				if (slot->packets.size() >= kMaxQueuedPackets)
					slot->packets.pop_front();
				slot->packets.emplace_back(buffer.cbegin(), buffer.cbegin() + result);
				slot->hasRemote = true;
			}
			else if (result < 0 && errno != ETIMEDOUT)
			{
				if (errno == ENODEV || errno == ESHUTDOWN)
				{
					cemuLog_log(LogType::Force, "DolphinBar slot {}: device gone", slot->interfaceNumber);
					slot->failed = true;
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}
	}

	bool WriteReport(int fd, uint8 interfaceNumber, const std::vector<uint8>& data, unsigned timeoutMs = kWriteTimeoutMs)
	{
		if (data.empty())
			return false;
		// same layout hidapi uses on Linux: report id in wValue and as the first data byte
		auto buffer = data;
		usbdevfs_ctrltransfer transfer{};
		transfer.bRequestType = kHidRequestTypeOut;
		transfer.bRequest = kHidSetReport;
		transfer.wValue = kHidReportTypeOutput | data[0];
		transfer.wIndex = interfaceNumber;
		transfer.wLength = static_cast<uint16>(buffer.size());
		transfer.timeout = timeoutMs;
		transfer.data = buffer.data();
		const int result = ioctl(fd, USBDEVFS_CONTROL, &transfer);
		if (result == static_cast<int>(buffer.size()))
			return true;
		// Empty-slot probes commonly time out; only log failed writes to discovered remotes.
		if (timeoutMs != kProbeTimeoutMs)
			cemuLog_log(LogType::Force, "DolphinBar slot {}: report {:#x} write failed ({} of {} bytes, errno {})",
				interfaceNumber, data[0], result, buffer.size(), result < 0 ? errno : 0);
		return false;
	}
} // namespace

AndroidUsbWiimote::AndroidUsbWiimote(size_t slot, uint32 generation)
	: m_slot(slot), m_generation(generation)
{
}

bool AndroidUsbWiimote::write_data(const std::vector<uint8>& data)
{
	std::shared_lock lock(s_mutex);
	if (s_fd < 0 || m_generation != s_generation || m_slot >= s_slots.size())
		return false;
	const auto& slot = s_slots[m_slot];
	if (slot->failed)
		return false;
	return WriteReport(s_fd, slot->interfaceNumber, data);
}

std::optional<std::vector<uint8>> AndroidUsbWiimote::read_data()
{
	std::shared_lock lock(s_mutex);
	if (s_fd < 0 || m_generation != s_generation || m_slot >= s_slots.size())
		return {};
	auto& slot = s_slots[m_slot];
	if (slot->failed)
		return {};

	std::scoped_lock slotLock(slot->mutex);
	if (slot->packets.empty())
		return std::vector<uint8>{};
	auto packet = std::move(slot->packets.front());
	slot->packets.pop_front();
	return packet;
}

bool AndroidUsbWiimote::operator==(const WiimoteDevice& o) const
{
	auto other = dynamic_cast<const AndroidUsbWiimote*>(&o);
	if (!other)
		return false;
	return m_slot == other->m_slot && m_generation == other->m_generation;
}

std::vector<WiimoteDevicePtr> AndroidUsbWiimote::get_devices()
{
	std::vector<WiimoteDevicePtr> devices;
	std::shared_lock lock(s_mutex);
	if (s_fd < 0)
		return devices;

	for (size_t i = 0; i < s_slots.size(); ++i)
	{
		const auto& slot = s_slots[i];
		if (slot->failed)
			continue;
		if (slot->hasRemote)
		{
			devices.push_back(std::make_shared<AndroidUsbWiimote>(i, s_generation));
			continue;
		}
		// ask the slot for a status report, a synced remote answers and shows up on the next scan
		WriteReport(s_fd, slot->interfaceNumber, {kStatusRequestReport, 0x00}, kProbeTimeoutMs);
	}
	return devices;
}

void AndroidUsbWiimote::SetAdapter(int fd, const std::vector<SlotInfo>& slots)
{
	ClearAdapter();

	std::scoped_lock lock(s_mutex);
	s_fd = fd;
	++s_generation;
	s_running = true;
	for (const auto& info : slots)
	{
		auto slot = std::make_unique<Slot>();
		slot->interfaceNumber = info.interfaceNumber;
		slot->inEndpoint = info.inEndpoint;
		slot->reader = std::thread(ReaderThread, fd, slot.get());
		s_slots.push_back(std::move(slot));
	}
	cemuLog_log(LogType::Force, "DolphinBar opened with {} Wii Remote slots", s_slots.size());
}

void AndroidUsbWiimote::ClearAdapter()
{
	std::scoped_lock lock(s_mutex);
	if (s_fd < 0)
		return;
	s_running = false;
	for (auto& slot : s_slots)
	{
		if (slot->reader.joinable())
			slot->reader.join();
	}
	s_slots.clear();
	s_fd = -1;
	cemuLog_log(LogType::Force, "DolphinBar closed");
}
