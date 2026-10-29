// The phone's sensors as the channels of one openDAQ device.
#pragma once

#include "common.h"
#include "phone_clock.h"
#include "usb_host.h"

#include <android/sensor.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace daq::modules::android_device
{

/// A channel with one acquisition thread and a SampleRate selection.
///
/// The thread reads the sensor while the channel is active, which needs the
/// channel and its device active; the device is inactive in the Idle operation
/// mode. A sample rate change is only recorded by the property handler; the
/// thread sees it and reconfigures itself. The handler runs under the property
/// object's lock, which the thread must never wait for.
class PhoneChannel : public Channel
{
public:
    PhoneChannel(const ContextPtr& ctx,
                 const ComponentPtr& parent,
                 const StringPtr& localId,
                 const FunctionBlockTypePtr& type,
                 std::shared_ptr<PhoneClock> clock,
                 std::vector<int> rates,
                 size_t defaultRate,
                 bool startActive);

    ~PhoneChannel() override;

protected:
    /// Reads the sensor at `rate` until keepGoing(rate) turns false or the
    /// sensor fails.
    virtual void acquire(int rate) = 0;

    /// Every subclass calls startAcquisition() last in its constructor and
    /// stopAcquisition() in its destructor, so the thread runs only while the
    /// subclass's members exist.
    void startAcquisition();
    void stopAcquisition();

    bool keepGoing(int rate) const
    {
        return !stopping && running && requestedRate == rate;
    }

    void reportOk();
    void reportError(const std::string& message);

    void activeChanged() override;
    void notifyActiveChanged(bool onUpdate) override;
    void removed() override;

    /// Adds a value signal on the channel's time signal.
    SignalConfigPtr addValueSignal(const std::string& localId, const DataDescriptorPtr& descriptor);

    /// Adds a button, a procedure without arguments, such as Tare.
    void addProcedure(const std::string& name, const std::string& description, std::function<void()> action);
    /// Adds a number setting whose writes land in `target`, for the
    /// acquisition thread to read.
    void addSetting(const std::string& name, double initial, const UnitPtr& unit, const std::string& description, std::atomic<double>& target);

    /// The time signal's descriptor: ticks of 1/`ticksPerSecond` s since the
    /// Unix epoch, `delta` ticks apart.
    static DataDescriptorPtr timeDescriptor(int64_t ticksPerSecond, int64_t delta);

    std::shared_ptr<PhoneClock> clock;
    SignalConfigPtr time;
    std::atomic<int> requestedRate{0};

private:
    void run();

    std::vector<int> rates;
    std::atomic<bool> stopping{false};
    std::atomic<bool> running{true};
    std::thread thread;
};

/// The microphone, through AAudio: one Float32 signal from -1 to 1. Each
/// block's time comes from AAudio's timestamp of when its frames were
/// recorded.
class MicrophoneChannel final : public PhoneChannel
{
public:
    /// A USB microphone has Android's audio device ID in `audioDevice`; the
    /// phone's own microphone leaves it 0.
    MicrophoneChannel(const ContextPtr& ctx,
                      const ComponentPtr& parent,
                      const StringPtr& localId,
                      std::shared_ptr<PhoneClock> clock,
                      bool startActive,
                      int32_t audioDevice = 0);
    ~MicrophoneChannel() override;

protected:
    void acquire(int rate) override;

private:
    SignalConfigPtr audio;
    int32_t audioDevice;

    // Calibration: the sound pressure at full scale, 0 when not calibrated,
    // and the level of the calibrator a Calibrate measures.
    std::atomic<double> sensitivity{0};
    std::atomic<double> calibrationLevel{94};
    std::atomic<bool> calibrating{false};
};

/// What tells the accelerometer and the gyroscope apart. Both are three-axis
/// motion sensors and share MotionChannel.
struct MotionKind
{
    int sensorType;           // ASENSOR_TYPE_...
    const char* localId;      // the channel's local ID and name
    const char* typeId;       // the channel's function block type
    const char* unitSymbol;
    const char* unitName;
    const char* quantity;
    const char* names[3];     // the three signals, such as X, Y and Z
    bool magnitude;           // a fourth signal, the length of the vector
    bool angles;              // values in degrees that wrap at ±180
    /// Turns an event into the three values; null takes the event's vector.
    void (*convert)(const ASensorEvent& event, float* values);
};

extern const MotionKind kAccelerometer;
extern const MotionKind kGyroscope;
extern const MotionKind kMagnetometer;
extern const MotionKind kLinearAcceleration;
extern const MotionKind kOrientation;

/// A three-axis motion sensor through the NDK sensor API: X, Y and Z on a
/// fixed time grid, interpolated from the sensor's events by their stamps.
class MotionChannel final : public PhoneChannel
{
public:
    MotionChannel(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId, std::shared_ptr<PhoneClock> clock, const MotionKind& kind);
    ~MotionChannel() override;

    /// Whether the phone has this sensor at all.
    static bool available(const MotionKind& kind);

protected:
    void acquire(int rate) override;

private:
    const MotionKind& kind;
    std::vector<SignalConfigPtr> axes;

    // Calibration: value = (raw - offset) * scale, per axis; a Tare sets the
    // offsets to the mean of the next second.
    std::array<std::atomic<double>, 3> offsets{};
    std::atomic<double> scale{1};
    std::atomic<bool> taring{false};
};

/// What tells the on-change sensors apart: the proximity sensor, the light
/// sensor and Samsung's infrared light stream. Android reports the first two
/// only when their value changes, and the stream at an uneven, low rate, so
/// they share OnChangeChannel.
struct OnChangeKind
{
    int sensorType;            // ASENSOR_TYPE_..., for a standard sensor
    const char* stringType;    // a vendor sensor's string type, or null
    const char* localId;       // the channel's local ID and name
    const char* typeId;        // the channel's function block type
    const char* what;          // the sensor in a sentence, such as "light sensor"
    std::vector<const char*> signals;
    const char* signalDescription;
};

extern const OnChangeKind kProximity;
extern const OnChangeKind kLight;
extern const OnChangeKind kInfraredLight;

/// A sensor whose values the channel holds and repeats at the chosen rate, so
/// its signals are step curves on a fixed time grid. Each value holds from
/// the stamp of the event that brought it.
class OnChangeChannel : public PhoneChannel
{
public:
    OnChangeChannel(const ContextPtr& ctx,
                    const ComponentPtr& parent,
                    const StringPtr& localId,
                    std::shared_ptr<PhoneClock> clock,
                    const OnChangeKind& kind,
                    const DataDescriptorPtr& descriptor);

    static const ASensor* sensorFor(const OnChangeKind& kind);
    static bool available(const OnChangeKind& kind);

protected:
    /// The signals' values for one sensor event, one per signal of the kind,
    /// on the acquisition thread.
    virtual void read(const ASensorEvent& event, float* values) = 0;

    void acquire(int rate) override;

    /// Adds Offset and Scale, for calibrated(): raw * scale + offset.
    void addCalibration(const UnitPtr& unit);
    float calibrated(float raw) const
    {
        return static_cast<float>(raw * calibrationScale.load() + calibrationOffset.load());
    }

    const OnChangeKind& kind;

private:
    std::vector<SignalConfigPtr> values;
    std::atomic<double> calibrationOffset{0};
    std::atomic<double> calibrationScale{1};
};

/// The proximity sensor, as the signal Near: 1 when an object is near, 0 when
/// not. Most phones report only near (0 cm) or far (the sensor's range), and
/// no distance in between, so the channel does not pretend to measure one. The
/// distance Android last reported is a read-only property.
class ProximityChannel final : public OnChangeChannel
{
public:
    ProximityChannel(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId, std::shared_ptr<PhoneClock> clock);
    ~ProximityChannel() override;

protected:
    void read(const ASensorEvent& event, float* values) override;

private:
    float farthest = 0;
    float reported = -1;
};

extern const OnChangeKind kPressure;
extern const OnChangeKind kHumidity;
extern const OnChangeKind kTemperature;

/// A sensor with one value in its event's first field, such as the
/// barometer, the humidity sensor or the ambient temperature sensor.
class ValueChannel final : public OnChangeChannel
{
public:
    ValueChannel(const ContextPtr& ctx,
                 const ComponentPtr& parent,
                 const StringPtr& localId,
                 std::shared_ptr<PhoneClock> clock,
                 const OnChangeKind& kind,
                 const UnitPtr& unit);
    ~ValueChannel() override;

protected:
    void read(const ASensorEvent& event, float* values) override;
};

/// The ambient light sensor, in lux.
class LightChannel final : public OnChangeChannel
{
public:
    LightChannel(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId, std::shared_ptr<PhoneClock> clock);
    ~LightChannel() override;

protected:
    void read(const ASensorEvent& event, float* values) override;
};

/// Samsung's infrared ambient light stream (com.samsung.sensor.light_ir),
/// which apps may read without a permission. Samsung does not document its
/// event fields, so the channel shows the ones that change as raw values.
class InfraredLightChannel final : public OnChangeChannel
{
public:
    InfraredLightChannel(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId, std::shared_ptr<PhoneClock> clock);
    ~InfraredLightChannel() override;

protected:
    void read(const ASensorEvent& event, float* values) override;
};

/// A camera of the phone, or one connected to its USB port, through the NDK
/// camera API: one Binary signal,
/// Image, with one JPEG picture per sample, as openDAQ's video convention
/// has it (the unit's symbol names the format, as the SDK's Video Player
/// expects). The camera hardware encodes the JPEGs; the channel takes the
/// newest one at the chosen frame rate, with the time its exposure started.
class CameraChannel final : public PhoneChannel
{
public:
    /// A USB camera has its camera ID in `externalId`; the phone's own camera
    /// channel leaves it empty and offers the back and front cameras.
    CameraChannel(const ContextPtr& ctx,
                  const ComponentPtr& parent,
                  const StringPtr& localId,
                  std::shared_ptr<PhoneClock> clock,
                  bool startActive,
                  const std::string& externalId = "");
    ~CameraChannel() override;

    /// Whether the phone has a camera that apps may open.
    static bool available();

    /// The IDs of the cameras connected to the USB port, where Android
    /// supports USB cameras (the external camera HAL, standard since
    /// Android 14).
    static std::vector<std::string> externalCameras();

protected:
    void acquire(int rate) override;

private:
    SignalConfigPtr image;
    std::string externalId;

    // The camera settings as their handlers last saw them. A change bumps
    // `version`, and the acquisition reopens the camera.
    std::atomic<int> facing{0};
    std::atomic<int> resolution{1};
    std::atomic<int> quality{70};
    std::atomic<int> version{0};
};

/// A channel of values the app reports (PhoneStatus.kt), held and repeated
/// at the chosen rate as step curves, like the on-change sensors.
class StatusChannel : public PhoneChannel
{
public:
    StatusChannel(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId, std::shared_ptr<PhoneClock> clock, const FunctionBlockTypePtr& type);

protected:
    /// The values now, one per signal. False when the app has not reported
    /// for a while.
    virtual bool sample(std::vector<float>& values) = 0;

    void acquire(int rate) override;

    std::vector<SignalConfigPtr> values;
};

/// The phone's state: CPU, memory, Wi-Fi, network traffic, battery,
/// temperature and storage (kSystemFields).
class SystemChannel final : public StatusChannel
{
public:
    SystemChannel(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId, std::shared_ptr<PhoneClock> clock);
    ~SystemChannel() override;

protected:
    bool sample(std::vector<float>& values) override;
};

/// The USB port: the charging power and current while a cable charges the
/// phone, and the number of USB devices connected. Its read-only properties
/// say where the power comes from, the charger's rating and which devices are
/// connected. Cameras and microphones on the port get channels of their own.
class UsbChannel final : public StatusChannel
{
public:
    UsbChannel(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId, std::shared_ptr<PhoneClock> clock);
    ~UsbChannel() override;

protected:
    bool sample(std::vector<float>& values) override;

private:
    uint64_t shownVersion = UINT64_MAX;
};


/// A channel of a device on the USB port that the module reads itself
/// (usb_host.h), whose values it holds and repeats at the chosen rate.
class UsbDeviceChannel : public PhoneChannel
{
public:
    UsbDeviceChannel(const ContextPtr& ctx,
                     const ComponentPtr& parent,
                     const StringPtr& localId,
                     const FunctionBlockTypePtr& type,
                     std::shared_ptr<PhoneClock> clock,
                     std::shared_ptr<UsbConnection> connection,
                     std::vector<int> rates,
                     size_t defaultRate);

protected:
    /// Sends every grid point up to now with the values held.
    void emitHeld(int64_t& next, int64_t period, const std::vector<float>& held);

    std::shared_ptr<UsbConnection> connection;
    std::vector<SignalConfigPtr> values;
};

/// A USB camera read with the UVC protocol: its MJPEG pictures as the Image
/// signal, for phones whose Android does not offer USB cameras to apps.
class UvcCameraChannel final : public UsbDeviceChannel
{
public:
    UvcCameraChannel(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId, std::shared_ptr<PhoneClock> clock, std::shared_ptr<UsbConnection> connection);
    ~UvcCameraChannel() override;

protected:
    void acquire(int rate) override;

private:
    struct FrameSize
    {
        PictureLayout layout;
        uint8_t format;
        uint8_t index;
        int width;
        int height;
        uint32_t interval;
    };
    std::vector<FrameSize> sizes;
    uint16_t uvcVersion = 0x0100;
    int streamingInterface = -1;
    SignalConfigPtr image;
    std::atomic<int> resolution{0};
    std::atomic<int> version{0};
};

/// A USB serial device (CDC ACM), such as an Arduino: each line it sends is
/// read as numbers, one signal each. "name:value" pairs name the signals.
class UsbSerialChannel final : public UsbDeviceChannel
{
public:
    UsbSerialChannel(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId, std::shared_ptr<PhoneClock> clock, std::shared_ptr<UsbConnection> connection);
    ~UsbSerialChannel() override;

protected:
    void acquire(int rate) override;

private:
    std::atomic<int> baud{115200};
    std::atomic<int> version{0};
    std::vector<std::string> names;
};

/// Another USB input device (HID), such as a game controller: the bytes of
/// its reports as signals.
class UsbInputChannel final : public UsbDeviceChannel
{
public:
    UsbInputChannel(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId, std::shared_ptr<PhoneClock> clock, std::shared_ptr<UsbConnection> connection);
    ~UsbInputChannel() override;

protected:
    void acquire(int rate) override;

private:
    uint8_t endpoint = 0;
    size_t reportBytes = 0;
};

}  // namespace daq::modules::android_device
