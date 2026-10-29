#include "phone_status.h"

#include "phone_clock.h"

#include <jni.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace daq::modules::android_device
{

const StatusField kSystemFields[kSystemFieldCount] = {
    {"Cpu", "%", "percent", "", "The processor time this app uses, in percent of all cores."},
    {"CpuFrequency", "MHz", "megahertz", "frequency", "The average clock frequency of the processor's cores, where Android lets apps read it."},
    {"MemoryUsed", "%", "percent", "", "The share of the phone's memory in use."},
    {"MemoryAvailable", "MB", "megabytes", "", "The memory available to apps."},
    {"WifiSignal", "dBm", "decibel-milliwatts", "power", "The strength of the Wi-Fi signal, while the phone is on Wi-Fi."},
    {"WifiLinkSpeed", "Mbit/s", "megabits per second", "", "The Wi-Fi link speed, while the phone is on Wi-Fi."},
    {"NetworkReceive", "kB/s", "kilobytes per second", "", "The data the phone receives over all networks."},
    {"NetworkSend", "kB/s", "kilobytes per second", "", "The data the phone sends over all networks."},
    {"BatteryLevel", "%", "percent", "", "The battery's charge."},
    {"BatteryVoltage", "V", "volts", "voltage", "The battery's voltage."},
    {"BatteryCurrent", "mA", "milliamperes", "current", "The current into the battery: positive while it charges, negative while it discharges."},
    {"BatteryPower", "W", "watts", "power", "The power into the battery, the charging speed: positive while it charges, negative while it discharges."},
    {"BatteryTemperature", "°C", "degrees Celsius", "temperature", "The battery's temperature."},
    {"Charging", "", "", "", "1 while the phone charges, 0 when not."},
    {"ThermalHeadroom", "", "", "", "How close the phone is to slowing down because of heat: 1 means it slows down. Android 11 and later."},
    {"StorageFree", "GB", "gigabytes", "", "The free space of the phone's storage."},
};

namespace
{

constexpr int64_t kStale = 5'000'000'000LL;

std::mutex lock;
std::vector<float> latest(kSystemFieldCount, NAN);
int64_t reportedAt = INT64_MIN;
UsbState usbState;
int wantedRate = 0;
int64_t wantedAt = INT64_MIN;

}  // namespace

bool PhoneStatus::values(std::vector<float>& out)
{
    std::lock_guard guard(lock);
    out = latest;
    return reportedAt != INT64_MIN && PhoneClock::bootNanoseconds() - reportedAt < kStale;
}

UsbState PhoneStatus::usb()
{
    std::lock_guard guard(lock);
    return usbState;
}

void PhoneStatus::want(int rate)
{
    std::lock_guard guard(lock);
    const int64_t now = PhoneClock::bootNanoseconds();
    // The fastest reader of the last two seconds sets the pace.
    if (now - wantedAt > 2'000'000'000LL || rate > wantedRate)
        wantedRate = rate;
    wantedAt = now;
}

int PhoneStatus::wantedIntervalMilliseconds()
{
    std::lock_guard guard(lock);
    if (wantedRate <= 0 || PhoneClock::bootNanoseconds() - wantedAt > 2'000'000'000LL)
        return 0;
    return std::max(50, 1000 / wantedRate);
}

void PhoneStatus::report(const float* values, int count)
{
    std::lock_guard guard(lock);
    for (int i = 0; i < kSystemFieldCount; i++)
        latest[i] = i < count ? values[i] : NAN;
    reportedAt = PhoneClock::bootNanoseconds();
}

void PhoneStatus::reportUsb(UsbState state)
{
    std::lock_guard guard(lock);
    const bool changed = state.powerSource != usbState.powerSource || state.charger != usbState.charger ||
                         state.devices != usbState.devices || state.audioInputs != usbState.audioInputs;
    state.version = usbState.version + (changed ? 1 : 0);
    usbState = std::move(state);
}

}  // namespace daq::modules::android_device

using daq::modules::android_device::PhoneStatus;
using daq::modules::android_device::UsbState;

namespace
{

std::string text(JNIEnv* env, jstring value)
{
    if (!value)
        return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string out = chars ? chars : "";
    env->ReleaseStringUTFChars(value, chars);
    return out;
}

}  // namespace

// The app's PhoneStatus (io.opendaq.androiddevice.PhoneStatus) reports through
// these.

/// How often the app should report, in milliseconds, or 0 when no channel
/// reads the values and every two seconds is enough.
extern "C" JNIEXPORT jint JNICALL Java_io_opendaq_androiddevice_PhoneStatus_wantedInterval(JNIEnv*, jclass)
{
    return PhoneStatus::wantedIntervalMilliseconds();
}

/// The System channel's values, in the order of kSystemFields.
extern "C" JNIEXPORT void JNICALL Java_io_opendaq_androiddevice_PhoneStatus_report(JNIEnv* env, jclass, jfloatArray values)
{
    const jsize count = env->GetArrayLength(values);
    std::vector<float> copy(static_cast<size_t>(count));
    env->GetFloatArrayRegion(values, 0, count, copy.data());
    PhoneStatus::report(copy.data(), count);
}

extern "C" JNIEXPORT void JNICALL Java_io_opendaq_androiddevice_PhoneStatus_reportUsb(JNIEnv* env,
                                                                                     jclass,
                                                                                     jstring powerSource,
                                                                                     jstring charger,
                                                                                     jobjectArray devices,
                                                                                     jintArray audioIds,
                                                                                     jobjectArray audioNames)
{
    UsbState state;
    state.powerSource = text(env, powerSource);
    state.charger = text(env, charger);
    for (jsize i = 0; i < env->GetArrayLength(devices); i++)
    {
        auto item = static_cast<jstring>(env->GetObjectArrayElement(devices, i));
        state.devices.push_back(text(env, item));
        env->DeleteLocalRef(item);
    }
    const jsize inputs = std::min(env->GetArrayLength(audioIds), env->GetArrayLength(audioNames));
    std::vector<jint> ids(static_cast<size_t>(inputs));
    env->GetIntArrayRegion(audioIds, 0, inputs, ids.data());
    for (jsize i = 0; i < inputs; i++)
    {
        auto item = static_cast<jstring>(env->GetObjectArrayElement(audioNames, i));
        state.audioInputs.emplace_back(ids[static_cast<size_t>(i)], text(env, item));
        env->DeleteLocalRef(item);
    }
    PhoneStatus::reportUsb(std::move(state));
}
