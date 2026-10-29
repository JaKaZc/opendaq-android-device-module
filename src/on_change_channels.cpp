#include "android_sensors.h"
#include "sensor_channels.h"

#include <coreobjects/property_object_protected_ptr.h>

#include <algorithm>
#include <deque>

namespace daq::modules::android_device
{

const OnChangeKind kProximity{
    ASENSOR_TYPE_PROXIMITY,
    nullptr,
    "Proximity",
    "PhoneProximity",
    "proximity sensor",
    {"Near"},
    "1 when an object is near the proximity sensor, 0 when not. Most phones report only near or far, not a distance.",
};

const OnChangeKind kLight{
    ASENSOR_TYPE_LIGHT,
    nullptr,
    "Light",
    "PhoneLight",
    "light sensor",
    {"Illuminance"},
    "The ambient light at the phone's light sensor.",
};

const OnChangeKind kInfraredLight{
    0,
    "com.samsung.sensor.light_ir",
    "InfraredLight",
    "PhoneInfraredLight",
    "infrared light stream",
    {"Field0", "Field4", "Field5", "Field6"},
    "A raw field of Samsung's infrared light stream. Samsung does not document what the fields mean; covering the top of "
    "the phone shows which ones follow an approaching object.",
};

const OnChangeKind kPressure{
    ASENSOR_TYPE_PRESSURE,
    nullptr,
    "Pressure",
    "PhonePressure",
    "barometer",
    {"Pressure"},
    "The air pressure at the phone's barometer.",
};

const OnChangeKind kHumidity{
    ASENSOR_TYPE_RELATIVE_HUMIDITY,
    nullptr,
    "Humidity",
    "PhoneHumidity",
    "humidity sensor",
    {"Humidity"},
    "The relative humidity of the air around the phone.",
};

const OnChangeKind kTemperature{
    ASENSOR_TYPE_AMBIENT_TEMPERATURE,
    nullptr,
    "Temperature",
    "PhoneTemperature",
    "ambient temperature sensor",
    {"Temperature"},
    "The temperature of the air around the phone.",
};

namespace
{

/// Android's own rule for "near" (DisplayPowerController): closer than the
/// sensor's range and closer than 5 cm.
constexpr float kNearThreshold = 5.0f;

/// The event fields of the infrared light stream that change, as seen on a
/// Galaxy Z Fold7. The others stay 0. Their order matches kInfraredLight's
/// signals.
constexpr int kInfraredFields[] = {0, 4, 5, 6};

/// The values on the grid lag this far behind the clock, so an event that
/// arrives late still changes them from its own stamp on.
constexpr int64_t kLatencyMicroseconds = 100'000;

/// A stamp further than this from the time of arrival is not believed; some
/// phones stamp on-change events with the time of a previous change.
constexpr int64_t kStampTolerance = 1'000'000;

}  // namespace

// ---------------------------------------------------------------- OnChangeChannel

OnChangeChannel::OnChangeChannel(const ContextPtr& ctx,
                                 const ComponentPtr& parent,
                                 const StringPtr& localId,
                                 std::shared_ptr<PhoneClock> clock,
                                 const OnChangeKind& kind,
                                 const DataDescriptorPtr& descriptor)
    : PhoneChannel(ctx,
                   parent,
                   localId,
                   FunctionBlockType(kind.typeId, kind.localId, std::string("The phone's ") + kind.what),
                   std::move(clock),
                   {5, 10, 20, 50},
                   1,
                   true)
    , kind(kind)
{
    for (const char* id : kind.signals)
    {
        values.push_back(addValueSignal(id, descriptor));
        values.back().setDescription(kind.signalDescription);
    }
    addSensorInfo(objPtr, sensorFor(kind));
}

const ASensor* OnChangeChannel::sensorFor(const OnChangeKind& kind)
{
    return findSensor(kind.sensorType, kind.stringType);
}

bool OnChangeChannel::available(const OnChangeKind& kind)
{
    return sensorFor(kind) != nullptr;
}

void OnChangeChannel::acquire(int rate)
{
    const int64_t period = 1'000'000 / rate;
    const ASensor* sensor = sensorFor(kind);
    // The sampling period does not apply to an on-change sensor, but the
    // queue needs one.
    SensorQueue queue(sensor, period, kind.what);
    const int eventType = sensor ? ASensor_getType(sensor) : 0;

    time.setDescriptor(timeDescriptor(1'000'000, period));
    reportOk();

    // Android sends the current value when the sensor is registered, and
    // after that only changes. Until the first value, there is nothing to
    // repeat.
    struct Change
    {
        int64_t at;
        std::vector<float> values;
    };
    const size_t count = values.size();
    bool haveValue = false;
    std::vector<float> held(count);
    std::deque<Change> changes;
    int64_t next = 0;
    uint64_t steps = clock->steps();
    std::vector<std::vector<float>> pending(count);

    while (keepGoing(rate))
    {
        // Wake at least once per grid point, sooner when the value changes.
        const int64_t untilNext = haveValue ? std::max<int64_t>(0, next + kLatencyMicroseconds - clock->nowUnixMicroseconds()) : period;
        queue.poll(static_cast<int>(std::min<int64_t>(untilNext / 1000 + 1, 100)), [&](const ASensorEvent& event)
        {
            if (event.type != eventType)
                return;
            const int64_t now = clock->nowUnixMicroseconds();
            int64_t at = clock->toUnixNanoseconds(event.timestamp) / 1000;
            if (at > now || now - at > kStampTolerance)
                at = now;
            Change change{at, std::vector<float>(count)};
            read(event, change.values.data());
            if (!haveValue)
            {
                next = (at + period - 1) / period * period;
                held = change.values;
                haveValue = true;
                return;
            }
            // Changes are kept in order of their stamps.
            auto place = changes.end();
            while (place != changes.begin() && std::prev(place)->at > at)
                --place;
            changes.insert(place, std::move(change));
        });
        if (!haveValue)
            continue;
        if (clock->steps() != steps)
        {
            // The clock stepped; the grid continues from the new time.
            steps = clock->steps();
            next = (clock->nowUnixMicroseconds() - kLatencyMicroseconds + period - 1) / period * period;
        }

        // Every grid point that is old enough carries the values held at its
        // time.
        const int64_t until = clock->nowUnixMicroseconds() - kLatencyMicroseconds;
        const int64_t first = next;
        for (; next <= until; next += period)
        {
            while (!changes.empty() && changes.front().at <= next)
            {
                held = std::move(changes.front().values);
                changes.pop_front();
            }
            for (size_t i = 0; i < count; i++)
                pending[i].push_back(held[i]);
        }
        const size_t samples = pending[0].size();
        if (samples == 0)
            continue;
        const auto domainPacket = DataPacket(time.getDescriptor(), samples, first);
        time.sendPacket(domainPacket);
        for (size_t i = 0; i < count; i++)
        {
            const auto packet = DataPacketWithDomain(domainPacket, values[i].getDescriptor(), samples);
            std::memcpy(packet.getRawData(), pending[i].data(), samples * sizeof(float));
            values[i].sendPacket(packet);
            pending[i].clear();
        }
    }
}

// ---------------------------------------------------------------- ProximityChannel

ProximityChannel::ProximityChannel(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId, std::shared_ptr<PhoneClock> clock)
    : OnChangeChannel(ctx, parent, localId, std::move(clock), kProximity, DataDescriptorBuilder().setSampleType(SampleType::Float32).setValueRange(Range(0, 1)).build())
{
    objPtr.addProperty(FloatPropertyBuilder("ReportedDistance", 0.0)
                           .setReadOnly(true)
                           .setUnit(Unit("cm", -1, "centimeters", "distance"))
                           .setDescription("The distance Android last reported. On most phones it is 0 for near and the sensor's range for "
                                           "far, with nothing in between.")
                           .build());
    startAcquisition();
}

ProximityChannel::~ProximityChannel()
{
    stopAcquisition();
}

void ProximityChannel::read(const ASensorEvent& event, float* values)
{
    const float distance = event.distance;
    if (distance != reported)
    {
        reported = distance;
        objPtr.asPtr<IPropertyObjectProtected>().setProtectedPropertyValue("ReportedDistance", static_cast<Float>(distance));
    }

    // The NDK does not tell the sensor's range, so the farthest value seen
    // stands in for it. A sensor that has only reported near so far is
    // judged by the 5 cm threshold alone.
    farthest = std::max(farthest, distance);
    const float threshold = farthest > 0 ? std::min(farthest, kNearThreshold) : kNearThreshold;
    values[0] = distance >= 0 && distance < threshold ? 1.0f : 0.0f;
}

void OnChangeChannel::addCalibration(const UnitPtr& unit)
{
    addSetting("Offset", 0.0, unit, "Added to every value after it is scaled.", calibrationOffset);
    addSetting("Scale", 1.0, nullptr, "Multiplies every value before the offset is added, to correct the sensor's gain.", calibrationScale);
}

// ---------------------------------------------------------------- ValueChannel

ValueChannel::ValueChannel(const ContextPtr& ctx,
                           const ComponentPtr& parent,
                           const StringPtr& localId,
                           std::shared_ptr<PhoneClock> clock,
                           const OnChangeKind& kind,
                           const UnitPtr& unit)
    : OnChangeChannel(ctx, parent, localId, std::move(clock), kind, DataDescriptorBuilder().setSampleType(SampleType::Float32).setUnit(unit).build())
{
    addCalibration(unit);
    startAcquisition();
}

ValueChannel::~ValueChannel()
{
    stopAcquisition();
}

void ValueChannel::read(const ASensorEvent& event, float* values)
{
    values[0] = calibrated(event.data[0]);
}

// ---------------------------------------------------------------- LightChannel

LightChannel::LightChannel(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId, std::shared_ptr<PhoneClock> clock)
    : OnChangeChannel(ctx, parent, localId, std::move(clock), kLight, DataDescriptorBuilder().setSampleType(SampleType::Float32).setUnit(Unit("lx", -1, "lux", "illuminance")).build())
{
    addCalibration(Unit("lx", -1, "lux", "illuminance"));
    startAcquisition();
}

LightChannel::~LightChannel()
{
    stopAcquisition();
}

void LightChannel::read(const ASensorEvent& event, float* values)
{
    values[0] = calibrated(event.light);
}

// ---------------------------------------------------------------- InfraredLightChannel

InfraredLightChannel::InfraredLightChannel(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId, std::shared_ptr<PhoneClock> clock)
    : OnChangeChannel(ctx, parent, localId, std::move(clock), kInfraredLight, DataDescriptorBuilder().setSampleType(SampleType::Float32).build())
{
    startAcquisition();
}

InfraredLightChannel::~InfraredLightChannel()
{
    stopAcquisition();
}

void InfraredLightChannel::read(const ASensorEvent& event, float* values)
{
    for (size_t i = 0; i < std::size(kInfraredFields); i++)
        values[i] = event.data[kInfraredFields[i]];
}

}  // namespace daq::modules::android_device
