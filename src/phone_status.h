// The phone's state that only Android's Java API tells: CPU, memory, Wi-Fi,
// network, battery, temperature, storage and what is on the USB port. The
// app's PhoneStatus.kt reports it here, and the System and Usb channels read
// it.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace daq::modules::android_device
{

/// One value of the System channel, in the order PhoneStatus.kt reports them.
struct StatusField
{
    const char* id;
    const char* unitSymbol;  // empty for none
    const char* unitName;
    const char* quantity;
    const char* description;
};

enum SystemField
{
    kCpu,
    kCpuFrequency,
    kMemoryUsed,
    kMemoryAvailable,
    kWifiSignal,
    kWifiLinkSpeed,
    kNetworkReceive,
    kNetworkSend,
    kBatteryLevel,
    kBatteryVoltage,
    kBatteryCurrent,
    kBatteryPower,
    kBatteryTemperature,
    kCharging,
    kThermalHeadroom,
    kStorageFree,
    kSystemFieldCount
};

extern const StatusField kSystemFields[kSystemFieldCount];

/// What is on the USB port, as the app last reported it.
struct UsbState
{
    /// Where the phone's power comes from, such as "USB" or "Wireless", or
    /// "Battery".
    std::string powerSource;
    /// The charger's rating where Android tells it, such as "9 V, 2 A".
    std::string charger;
    /// The USB devices connected to the phone, one line each.
    std::vector<std::string> devices;
    /// The USB microphones: Android's audio device ID and the product name.
    std::vector<std::pair<int, std::string>> audioInputs;
    /// Counts the reports, so readers see a change.
    uint64_t version = 0;
};

class PhoneStatus
{
public:
    /// The newest values of the System channel's fields, NaN where Android
    /// does not tell one. False when the app has not reported for five
    /// seconds.
    static bool values(std::vector<float>& out);
    static UsbState usb();

    /// A channel reading the values at this rate asks the app to report at
    /// least as often.
    static void want(int rate);
    static int wantedIntervalMilliseconds();

    static void report(const float* values, int count);
    static void reportUsb(UsbState state);
};

}  // namespace daq::modules::android_device
