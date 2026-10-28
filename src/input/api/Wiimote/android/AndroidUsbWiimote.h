#pragma once

#include <input/api/Wiimote/WiimoteDevice.h>

// One Wii Remote slot of a Mayflash DolphinBar in mode 4 (USB 057e:0306).
// The bar has one HID interface per slot. Input reports arrive on the interface's
// interrupt IN endpoint, output reports are sent as HID SET_REPORT control transfers.
// Android only lets apps open USB devices through UsbManager, so the Java side opens the bar,
// claims its interfaces and hands the file descriptor to SetAdapter()
class AndroidUsbWiimote : public WiimoteDevice
{
  public:
	AndroidUsbWiimote(size_t slot, uint32 generation);
	~AndroidUsbWiimote() override = default;

	bool write_data(const std::vector<uint8>& data) override;
	std::optional<std::vector<uint8>> read_data() override;
	bool operator==(const WiimoteDevice& o) const override;

	static std::vector<WiimoteDevicePtr> get_devices();

	struct SlotInfo
	{
		uint8 interfaceNumber;
		uint8 inEndpoint;
	};
	// fd stays owned by the Java UsbDeviceConnection. ClearAdapter() must be called before it is closed
	static void SetAdapter(int fd, const std::vector<SlotInfo>& slots);
	static void ClearAdapter();

  private:
	size_t m_slot;
	uint32 m_generation;
};
