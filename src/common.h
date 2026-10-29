// Shared pieces of the phone device and its channels.
#pragma once

#include <opendaq/opendaq.h>
#include <opendaq/channel_impl.h>
#include <opendaq/device_impl.h>
#include <opendaq/device_domain_factory.h>
#include <opendaq/device_type_factory.h>
#include <opendaq/search_filter_factory.h>

#include <android/log.h>
#include <sys/system_properties.h>

#include <cstdint>
#include <string>
#include <vector>

namespace daq::modules::android_device
{

constexpr const char* kTag = "AndroidDevice";

/// Every signal counts time from the Unix epoch, as the SDK's reference
/// device does, so phone and network signals share one time axis.
constexpr const char* kUnixEpoch = "1970-01-01T00:00:00Z";

inline UnitPtr seconds()
{
    return Unit("s", -1, "seconds", "time");
}

/// Unix time in nanoseconds as ticks of 1/`rate` seconds. The seconds are
/// split off first, because nanoseconds times 48 000 overflow 64 bits.
inline int64_t unixTicks(int64_t nanoseconds, int64_t rate)
{
    return nanoseconds / 1'000'000'000 * rate + nanoseconds % 1'000'000'000 * rate / 1'000'000'000;
}

inline std::string systemProperty(const char* name)
{
    char value[PROP_VALUE_MAX] = {};
    __system_property_get(name, value);
    return value;
}

}  // namespace daq::modules::android_device
