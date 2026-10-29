// The NDK sensor API, as the motion and on-change channels use it.
#pragma once

#include "common.h"
#include "phone_clock.h"

#include <android/looper.h>
#include <android/sensor.h>

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>

namespace daq::modules::android_device
{

/// The app's package name, which the sensor manager asks for.
inline std::string packageName()
{
    std::ifstream cmdline("/proc/self/cmdline");
    std::string name;
    std::getline(cmdline, name, '\0');
    return name;
}

inline ASensorManager* sensorManager()
{
    static const std::string package = packageName();
    return ASensorManager_getInstanceForPackage(package.c_str());
}

/// The phone's default sensor of a type, or null when it has none.
inline const ASensor* defaultSensor(int type)
{
    ASensorManager* manager = sensorManager();
    return manager ? ASensorManager_getDefaultSensor(manager, type) : nullptr;
}

/// A sensor by Android's type, or by its string type for a vendor's own
/// sensor such as "com.samsung.sensor.light_ir". Null when the phone has none
/// or does not let apps use it.
inline const ASensor* findSensor(int type, const char* stringType)
{
    if (!stringType)
        return defaultSensor(type);
    ASensorManager* manager = sensorManager();
    if (!manager)
        return nullptr;
    ASensorList list = nullptr;
    const int count = ASensorManager_getSensorList(manager, &list);
    for (int i = 0; i < count; i++)
        if (std::string(ASensor_getStringType(list[i])) == stringType)
            return list[i];
    return nullptr;
}

/// Adds the sensor's name and vendor as read-only properties, so the user can
/// see which of the phone's sensors a channel reads.
inline void addSensorInfo(const PropertyObjectPtr& object, const ASensor* sensor)
{
    if (!sensor)
        return;
    object.addProperty(StringPropertyBuilder("SensorName", ASensor_getName(sensor))
                           .setReadOnly(true)
                           .setDescription("The phone's name for the sensor this device reads.")
                           .build());
    object.addProperty(StringPropertyBuilder("SensorVendor", ASensor_getVendor(sensor))
                           .setReadOnly(true)
                           .setDescription("The maker of the sensor.")
                           .build());
}

/// One sensor's event queue on the calling thread's looper. The sensor stops
/// and the queue is freed however the acquisition leaves.
class SensorQueue
{
public:
    SensorQueue(const ASensor* sensor, int64_t periodMicroseconds, const std::string& what)
        : manager(sensorManager())
        , sensor(sensor)
    {
        if (!manager || !sensor)
            throw std::runtime_error("The phone has no " + what);
        ALooper* looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
        queue = ASensorManager_createEventQueue(manager, looper, 0, nullptr, nullptr);
        if (!queue)
            throw std::runtime_error("The " + what + "'s event queue could not be created");
        const int status = ASensorEventQueue_registerSensor(queue, sensor, static_cast<int32_t>(periodMicroseconds), 0);
        if (status < 0)
        {
            ASensorManager_destroyEventQueue(manager, queue);
            throw std::runtime_error("The " + what + " could not be started (error " + std::to_string(status) + ")");
        }
    }

    ~SensorQueue()
    {
        ASensorEventQueue_disableSensor(queue, sensor);
        ASensorManager_destroyEventQueue(manager, queue);
    }

    SensorQueue(const SensorQueue&) = delete;
    SensorQueue& operator=(const SensorQueue&) = delete;

    /// Waits up to `timeoutMilliseconds` for events, then hands each one to
    /// `handle`.
    template <typename Handler>
    void poll(int timeoutMilliseconds, Handler&& handle)
    {
        ALooper_pollOnce(timeoutMilliseconds, nullptr, nullptr, nullptr);
        ASensorEvent events[64];
        ssize_t count;
        while ((count = ASensorEventQueue_getEvents(queue, events, 64)) > 0)
            for (ssize_t i = 0; i < count; i++)
                handle(events[i]);
    }

private:
    ASensorManager* manager;
    const ASensor* sensor;
    ASensorEventQueue* queue = nullptr;
};

}  // namespace daq::modules::android_device
