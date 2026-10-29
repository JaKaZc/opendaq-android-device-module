// The phone as one openDAQ device, with a channel for each of its sensors.
#pragma once

#include "common.h"
#include "phone_clock.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace daq::modules::android_device
{

/// The phone's sensors as channels of one device (android://), which the apps
/// make the root device of their instance: Microphone,
/// Accelerometer, Gyroscope, Proximity, Light, InfraredLight and Camera, the
/// ones the phone has. Every channel has its own sample rate and settings and
/// can be switched off with its Active attribute.
///
/// The device's operation modes are Idle, which stops every sensor, and
/// Operation and SafeOperation, which run the active channels. All channels
/// take their time from the device's PhoneClock, whose source is the
/// TimeSource property and which the Synchronization component reports.
class PhoneDevice final : public Device
{
public:
    PhoneDevice(const ContextPtr& ctx, const ComponentPtr& parent, const PropertyObjectPtr& config);
    ~PhoneDevice() override;

    static DeviceTypePtr createType();
    static DeviceInfoPtr createInfo(const std::string& name = "Android device", const std::string& serialNumber = "");

protected:
    uint64_t onGetTicksSinceOrigin() override;
    std::set<OperationModeType> onGetAvailableOperationModes() override;
    OperationModeType onGetDefaultOperationMode() override;
    void onOperationModeChanged(OperationModeType modeType) override;
    void removed() override;
    /// As the root device of an app's instance, the phone takes the devices
    /// and function blocks the app adds, such as a device on the network or
    /// the Universal renderer.
    bool allowAddDevicesFromModules() override;
    bool allowAddFunctionBlocksFromModules() override;

private:
    void addChannels(bool microphone, bool camera);
    void addTimeProperties();
    void initSync();
    void selectSource(Int source);
    /// A mode chosen on one of the Synchronization component's interfaces:
    /// Input makes it the time source, Output on PhoneClock serves this
    /// phone's time, and Off leaves it.
    void chooseMode(int index, Int mode);
    /// Starts or stops serving this phone's time to other devices.
    void serveTime(bool on);
    /// Shows the clock's state in the properties, the Synchronization
    /// component and the device's status, every half second, and every two
    /// seconds updates the USB channels.
    void reportTime();
    /// Adds a channel for each camera and microphone connected to the USB
    /// port, and removes the channels of those disconnected.
    void updateUsbChannels();
    /// Adds the derived signals as function blocks of the Universal renderer
    /// module, connected to the sensors: a sound level meter, a sound
    /// spectrum, and vibration as ISO 10816 has it; and, when the
    /// configuration asks, a Dashboard of them.
    void addDerivedBlocks();

    std::shared_ptr<PhoneClock> clock;
    SyncComponentPtr sync;
    bool dashboard = false;
    bool selecting = false;
    std::unique_ptr<TimeServer> timeServer;
    std::string timeServerError;
    // The reporter writes the interfaces' modes again: a mode was chosen,
    // or serving started or stopped.
    std::atomic<bool> modesDirty{true};

    std::mutex lock;
    std::condition_variable wake;
    bool stopping = false;
    std::thread reporter;

    // The channels of USB cameras by camera ID, and of USB microphones by
    // Android's audio device ID.
    std::map<std::string, ChannelPtr> usbCameras;
    std::map<int, ChannelPtr> usbMicrophones;
    // The channels of the USB devices the module reads itself, by Android's
    // device name.
    std::map<std::string, ChannelPtr> usbDevices;
};

}  // namespace daq::modules::android_device
