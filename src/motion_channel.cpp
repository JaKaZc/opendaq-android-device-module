#include "android_sensors.h"
#include "sensor_channels.h"

#include <algorithm>
#include <cmath>

namespace daq::modules::android_device
{

namespace
{

/// Android's rotation vector, a quaternion, as azimuth, pitch and roll in
/// degrees, as SensorManager.getOrientation computes them.
void rotationToOrientation(const ASensorEvent& event, float* values)
{
    const double x = event.data[0];
    const double y = event.data[1];
    const double z = event.data[2];
    const double rest = 1 - x * x - y * y - z * z;
    const double w = event.data[3] != 0 ? event.data[3] : (rest > 0 ? std::sqrt(rest) : 0);
    const double r1 = 2 * x * y - 2 * z * w;
    const double r4 = 1 - 2 * x * x - 2 * z * z;
    const double r6 = 2 * x * z - 2 * y * w;
    const double r7 = 2 * y * z + 2 * x * w;
    const double r8 = 1 - 2 * x * x - 2 * y * y;
    constexpr double kDegrees = 180.0 / 3.14159265358979323846;
    values[0] = static_cast<float>(std::atan2(r1, r4) * kDegrees);
    values[1] = static_cast<float>(std::asin(std::clamp(-r7, -1.0, 1.0)) * kDegrees);
    values[2] = static_cast<float>(std::atan2(-r6, r8) * kDegrees);
}

}  // namespace

const MotionKind kAccelerometer{
    ASENSOR_TYPE_ACCELEROMETER,
    "Accelerometer",
    "PhoneAccelerometer",
    "m/s²",
    "meters per second squared",
    "acceleration",
    {"X", "Y", "Z"},
    true,
    false,
    nullptr,
};

const MotionKind kGyroscope{
    ASENSOR_TYPE_GYROSCOPE,
    "Gyroscope",
    "PhoneGyroscope",
    "rad/s",
    "radians per second",
    "angular velocity",
    {"X", "Y", "Z"},
    true,
    false,
    nullptr,
};

const MotionKind kMagnetometer{
    ASENSOR_TYPE_MAGNETIC_FIELD,
    "Magnetometer",
    "PhoneMagnetometer",
    "µT",
    "microteslas",
    "magnetic flux density",
    {"X", "Y", "Z"},
    true,
    false,
    nullptr,
};

/// The acceleration without gravity, which Android derives from the
/// accelerometer and the gyroscope: what moves or shakes the phone.
const MotionKind kLinearAcceleration{
    ASENSOR_TYPE_LINEAR_ACCELERATION,
    "LinearAcceleration",
    "PhoneLinearAcceleration",
    "m/s²",
    "meters per second squared",
    "acceleration",
    {"X", "Y", "Z"},
    true,
    false,
    nullptr,
};

/// Which way the phone points, from Android's fusion of the accelerometer,
/// the gyroscope and the magnetometer: azimuth from north, pitch and roll.
const MotionKind kOrientation{
    ASENSOR_TYPE_ROTATION_VECTOR,
    "Orientation",
    "PhoneOrientation",
    "°",
    "degrees",
    "angle",
    {"Azimuth", "Pitch", "Roll"},
    false,
    true,
    rotationToOrientation,
};

namespace
{

/// Above this gap between two events, the grid starts again at the next event
/// instead of drawing a straight line across the gap.
constexpr int64_t kMaxGapMicroseconds = 500'000;

std::string lowercase(std::string text)
{
    for (auto& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

}  // namespace

MotionChannel::MotionChannel(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId, std::shared_ptr<PhoneClock> clock, const MotionKind& kind)
    : PhoneChannel(ctx,
                   parent,
                   localId,
                   FunctionBlockType(kind.typeId, kind.localId, std::string("The phone's ") + lowercase(kind.localId)),
                   std::move(clock),
                   {25, 50, 100, 200},
                   2,
                   true)
    , kind(kind)
{
    const auto descriptor = DataDescriptorBuilder()
                                .setSampleType(SampleType::Float32)
                                .setUnit(Unit(kind.unitSymbol, -1, kind.unitName, kind.quantity))
                                .build();
    for (const char* name : kind.names)
        axes.push_back(addValueSignal(name, descriptor));
    if (kind.magnitude)
    {
        axes.push_back(addValueSignal("Magnitude", descriptor));
        axes.back().setDescription("The length of the vector of X, Y and Z.");
    }
    addSensorInfo(objPtr, defaultSensor(kind.sensorType));

    const auto unit = Unit(kind.unitSymbol, -1, kind.unitName, kind.quantity);
    for (int axis = 0; axis < 3; axis++)
        addSetting(std::string("Offset") + kind.names[axis], 0.0, unit,
                   std::string("Taken off ") + kind.names[axis] + " before it is scaled: the reading at rest, which Tare measures.",
                   offsets[axis]);
    addSetting("Scale", 1.0, nullptr, "Multiplies every value after the offset is taken off, to correct the sensor's gain.", scale);
    addProcedure("Tare",
                 "Makes the current reading zero: measures the mean of the next second as the offsets. The phone should lie still. "
                 "On the accelerometer this also takes gravity off.",
                 [this] { taring = true; });
    addProcedure("ResetCalibration", "Sets the offsets to 0 and the scale to 1.", [this]
                 {
                     for (int axis = 0; axis < 3; axis++)
                         objPtr.setPropertyValue(std::string("Offset") + this->kind.names[axis], 0.0);
                     objPtr.setPropertyValue("Scale", 1.0);
                 });
    startAcquisition();
}

MotionChannel::~MotionChannel()
{
    stopAcquisition();
}

bool MotionChannel::available(const MotionKind& kind)
{
    return defaultSensor(kind.sensorType) != nullptr;
}

void MotionChannel::acquire(int rate)
{
    // Microseconds between grid points. The rates on offer all divide a
    // second evenly.
    const int64_t period = 1'000'000 / rate;
    SensorQueue queue(defaultSensor(kind.sensorType), period, lowercase(kind.localId));

    time.setDescriptor(timeDescriptor(1'000'000, period));
    reportOk();

    uint64_t steps = clock->steps();
    // A Tare collects the raw values of one second.
    int64_t tareUntil = -1;
    double tareSum[3] = {};
    size_t tareCount = 0;
    bool havePrevious = false;
    int64_t previousTime = 0;
    float previous[3] = {};
    int64_t next = 0;

    // Samples on the grid, waiting to be sent: they start at `first` and are
    // `period` apart.
    int64_t first = 0;
    const size_t signals = axes.size();
    std::vector<std::vector<float>> pending(signals);

    auto flush = [&]
    {
        const auto count = pending[0].size();
        if (count == 0)
            return;
        const auto domainPacket = DataPacket(time.getDescriptor(), count, first);
        time.sendPacket(domainPacket);
        for (size_t axis = 0; axis < signals; axis++)
        {
            const auto packet = DataPacketWithDomain(domainPacket, axes[axis].getDescriptor(), count);
            std::memcpy(packet.getRawData(), pending[axis].data(), count * sizeof(float));
            axes[axis].sendPacket(packet);
            pending[axis].clear();
        }
    };

    while (keepGoing(rate))
    {
        queue.poll(50, [&](const ASensorEvent& event)
        {
            if (event.type != kind.sensorType)
                return;
            // Events carry boot clock stamps of when the sensor measured.
            const int64_t at = clock->toUnixNanoseconds(event.timestamp) / 1000;
            // The motion sensors share the event layout; the rotation
            // vector is converted to angles.
            float value[3] = {event.vector.x, event.vector.y, event.vector.z};
            if (kind.convert)
                kind.convert(event, value);

            if (taring.exchange(false))
            {
                tareUntil = at + 1'000'000;
                tareSum[0] = tareSum[1] = tareSum[2] = 0;
                tareCount = 0;
            }
            if (tareUntil >= 0)
            {
                for (int axis = 0; axis < 3; axis++)
                    tareSum[axis] += value[axis];
                tareCount++;
                if (at >= tareUntil)
                {
                    tareUntil = -1;
                    for (int axis = 0; axis < 3; axis++)
                        objPtr.setPropertyValue(std::string("Offset") + kind.names[axis], tareSum[axis] / static_cast<double>(tareCount));
                }
            }
            // Calibrated: the offset off, then scaled; an angle wraps back
            // into ±180°.
            for (int axis = 0; axis < 3; axis++)
            {
                value[axis] = static_cast<float>((value[axis] - offsets[axis].load()) * scale.load());
                if (kind.angles)
                    value[axis] = std::remainder(value[axis], 360.0f);
            }

            if (clock->steps() != steps)
            {
                // The clock stepped; the grid starts again on the new time.
                steps = clock->steps();
                havePrevious = false;
            }
            if (havePrevious && at <= previousTime)
                return;
            if (!havePrevious || at - previousTime > kMaxGapMicroseconds)
            {
                // Start the grid at the first point at or after this event.
                flush();
                next = (at + period - 1) / period * period;
            }
            else
            {
                // Every grid point up to this event lies between the previous
                // event and this one.
                for (; next <= at; next += period)
                {
                    const float f = static_cast<float>(next - previousTime) / static_cast<float>(at - previousTime);
                    if (pending[0].empty())
                        first = next;
                    float point[3];
                    for (int axis = 0; axis < 3; axis++)
                    {
                        float change = value[axis] - previous[axis];
                        // An angle that wraps, such as an azimuth from 179°
                        // to -179°, turns the short way.
                        if (kind.angles && change > 180)
                            change -= 360;
                        else if (kind.angles && change < -180)
                            change += 360;
                        point[axis] = previous[axis] + f * change;
                        if (kind.angles && point[axis] > 180)
                            point[axis] -= 360;
                        else if (kind.angles && point[axis] < -180)
                            point[axis] += 360;
                        pending[axis].push_back(point[axis]);
                    }
                    if (kind.magnitude)
                        pending[3].push_back(std::sqrt(point[0] * point[0] + point[1] * point[1] + point[2] * point[2]));
                }
            }
            havePrevious = true;
            previousTime = at;
            std::copy(value, value + 3, previous);
        });
        flush();
    }
}

}  // namespace daq::modules::android_device
