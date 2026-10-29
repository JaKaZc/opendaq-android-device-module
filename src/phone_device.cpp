#include "phone_device.h"

#include "android_sensors.h"
#include "phone_status.h"
#include "usb_host.h"
#include "sensor_channels.h"

#include <coreobjects/property_object_protected_ptr.h>
#include <opendaq/sync_component_private_ptr.h>

#include <algorithm>
#include <cctype>
#include <random>
#include <cmath>

namespace daq::modules::android_device
{

namespace
{

/// The time sources, in the order of TimeSource and the TimeSource property,
/// as the names of the Synchronization component's interfaces.
const char* const kInterfaces[] = {"PhoneClock", "NetworkTime", "GnssTime"};

/// The sync interface modes, as SyncInterfaceBase lists them.
constexpr Int kModeInput = 0;
constexpr Int kModeOutput = 1;
constexpr Int kModeOff = 3;

/// Set while the reporter writes the modes, so its writes are not taken for
/// the user's choice.
thread_local bool reportingModes = false;

/// The interface states, as InterfaceClockSync lists them.
constexpr Int kStateOk = 0;
constexpr Int kStateWarning = 2;

}  // namespace

namespace
{

/// A text from the device's configuration, or the fallback when it is empty.
std::string configText(const PropertyObjectPtr& config, const char* name, const char* fallback)
{
    if (!config.assigned() || !config.hasProperty(name))
        return fallback;
    const std::string value = config.getPropertyValue(name).asPtr<IString>().toStdString();
    return value.empty() ? fallback : value;
}

}  // namespace

PhoneDevice::PhoneDevice(const ContextPtr& ctx, const ComponentPtr& parent, const PropertyObjectPtr& config)
    : Device(ctx, parent, configText(config, "LocalId", "Sensors"), nullptr, configText(config, "Name", "Android device"))
    , clock(std::make_shared<PhoneClock>())
{
    this->deviceInfo = createInfo(configText(config, "Name", "Android device"), configText(config, "SerialNumber", ""));
    initComponentStatus();
    setDeviceDomain(DeviceDomain(Ratio(1, 1'000'000), kUnixEpoch, seconds()));

    const auto option = [&](const char* name, bool fallback)
    {
        return config.assigned() && config.hasProperty(name) ? static_cast<bool>(config.getPropertyValue(name)) : fallback;
    };
    addTimeProperties();
    initSync();
    addChannels(option("Microphone", true), option("Camera", false));
    dashboard = option("Dashboard", false);
    reporter = std::thread([this] { reportTime(); });
}

PhoneDevice::~PhoneDevice()
{
    {
        std::lock_guard guard(lock);
        stopping = true;
    }
    wake.notify_all();
    if (reporter.joinable())
        reporter.join();
}

DeviceTypePtr PhoneDevice::createType()
{
    auto config = PropertyObject();
    config.addProperty(BoolPropertyBuilder("Microphone", True)
                           .setDescription("Whether the Microphone channel starts active. The app needs the permission to record audio.")
                           .build());
    // As the root device of an app's instance, the phone has the app's name
    // for it, a local ID and a serial number, which other openDAQ clients see
    // and mDNS announces.
    config.addProperty(StringProperty("Name", ""));
    config.addProperty(StringProperty("LocalId", ""));
    config.addProperty(StringProperty("SerialNumber", ""));
    config.addProperty(BoolPropertyBuilder("Dashboard", False)
                           .setDescription("Whether the device adds a Dashboard, a web page of its main signals on port 8090, as the "
                                           "Device Server does.")
                           .build());
    config.addProperty(BoolPropertyBuilder("Camera", False)
                           .setDescription("Whether the Camera channel starts active. A running camera keeps Android's privacy "
                                           "indicator on and costs battery.")
                           .build());
    return DeviceType("AndroidDevice", "Android device", "The sensors and state of the Android phone or tablet the app runs on, as the channels of one device", "android", config);
}

DeviceInfoPtr PhoneDevice::createInfo(const std::string& name, const std::string& serialNumber)
{
    auto info = DeviceInfo("android://", name);
    info.setDeviceType(createType());
    if (!serialNumber.empty())
        info.setSerialNumber(serialNumber);
    info.setManufacturer(systemProperty("ro.product.manufacturer"));
    info.setModel(systemProperty("ro.product.model"));
    return info;
}

uint64_t PhoneDevice::onGetTicksSinceOrigin()
{
    return static_cast<uint64_t>(clock->nowUnixMicroseconds());
}

std::set<OperationModeType> PhoneDevice::onGetAvailableOperationModes()
{
    return {OperationModeType::Idle, OperationModeType::Operation, OperationModeType::SafeOperation};
}

OperationModeType PhoneDevice::onGetDefaultOperationMode()
{
    return OperationModeType::Operation;
}

void PhoneDevice::onOperationModeChanged(OperationModeType modeType)
{
    // As the SDK's reference device: Idle makes the device inactive, and with
    // it every channel, whose sensor then stops.
    setActive(modeType != OperationModeType::Idle);
}

bool PhoneDevice::allowAddDevicesFromModules()
{
    return true;
}

bool PhoneDevice::allowAddFunctionBlocksFromModules()
{
    return true;
}

void PhoneDevice::removed()
{
    {
        std::lock_guard guard(lock);
        stopping = true;
    }
    wake.notify_all();
    Device::removed();
}

void PhoneDevice::addChannels(bool microphone, bool camera)
{
    createAndAddChannel<MicrophoneChannel>(ioFolder, "Microphone", clock, microphone);
    for (const auto* kind : {&kAccelerometer, &kGyroscope, &kMagnetometer, &kLinearAcceleration, &kOrientation})
        if (MotionChannel::available(*kind))
            createAndAddChannel<MotionChannel>(ioFolder, kind->localId, clock, *kind);
    if (OnChangeChannel::available(kProximity))
        createAndAddChannel<ProximityChannel>(ioFolder, kProximity.localId, clock);
    if (OnChangeChannel::available(kLight))
        createAndAddChannel<LightChannel>(ioFolder, kLight.localId, clock);
    if (OnChangeChannel::available(kInfraredLight))
        createAndAddChannel<InfraredLightChannel>(ioFolder, kInfraredLight.localId, clock);
    if (OnChangeChannel::available(kPressure))
        createAndAddChannel<ValueChannel>(ioFolder, kPressure.localId, clock, kPressure, Unit("hPa", -1, "hectopascals", "pressure"));
    if (OnChangeChannel::available(kHumidity))
        createAndAddChannel<ValueChannel>(ioFolder, kHumidity.localId, clock, kHumidity, Unit("%", -1, "percent", "relative humidity"));
    if (OnChangeChannel::available(kTemperature))
        createAndAddChannel<ValueChannel>(ioFolder, kTemperature.localId, clock, kTemperature, Unit("°C", -1, "degrees Celsius", "temperature"));
    if (CameraChannel::available())
        createAndAddChannel<CameraChannel>(ioFolder, "Camera", clock, camera);
    createAndAddChannel<SystemChannel>(ioFolder, "System", clock);
    createAndAddChannel<UsbChannel>(ioFolder, "Usb", clock).setName("USB");
}

void PhoneDevice::addTimeProperties()
{
    objPtr.addProperty(SelectionPropertyBuilder("TimeSource", List<IString>("Phone clock", "Network time", "GNSS time"), 0)
                           .setDescription("Where the time of every channel's samples comes from: the phone's own clock, an NTP server, "
                                           "or the phone's satellite receiver, which needs the location permission.")
                           .build());
    objPtr.addProperty(StringPropertyBuilder("NtpServer", "time.android.com")
                           .setVisible(EvalValue("$TimeSource == 1"))
                           .setDescription("The NTP server that network time comes from.")
                           .build());
    objPtr.addProperty(StringPropertyBuilder("TimeStatus", "The time is the phone's own clock.")
                           .setReadOnly(true)
                           .setDescription("Whether the time follows its source, and why not.")
                           .build());
    objPtr.addProperty(FloatPropertyBuilder("ClockOffset", 0.0)
                           .setReadOnly(true)
                           .setUnit(Unit("ms", -1, "milliseconds", "time"))
                           .setDescription("The time source's time minus the phone's own clock.")
                           .build());
    objPtr.addProperty(FloatPropertyBuilder("ClockUncertainty", 0.0)
                           .setReadOnly(true)
                           .setUnit(Unit("ms", -1, "milliseconds", "time"))
                           .setDescription("How far the time source's time may be off, by its own account.")
                           .build());

    objPtr.addProperty(BoolPropertyBuilder("ServeTime", False)
                           .setDescription("Serves this phone's time to other devices on the network, over NTP on UDP port 12300, so another "
                                           "phone can follow it as its network time. The time served is the phone's time as it follows its "
                                           "own source, such as GNSS.")
                           .build());
    objPtr.addProperty(StringPropertyBuilder("TimeServer", "")
                           .setReadOnly(true)
                           .setVisible(EvalValue("$ServeTime"))
                           .setDescription("Where other devices reach this phone's time, as an NTP server, or why it cannot be served.")
                           .build());

    objPtr.getOnPropertyValueWrite("TimeSource") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
    {
        selectSource(static_cast<Int>(args.getValue()));
        modesDirty = true;
    };
    objPtr.getOnPropertyValueWrite("ServeTime") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
    {
        serveTime(static_cast<bool>(args.getValue()));
    };
    objPtr.getOnPropertyValueWrite("NtpServer") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
    {
        clock->setNtpServer(static_cast<std::string>(args.getValue().asPtr<IString>()));
    };
}

void PhoneDevice::initSync()
{
    // One sync interface per time source, so the Synchronization component's
    // Source selection names them. Each is an InterfaceClockSync, which the
    // SDK registers, under a name of its own.
    const auto typeManager = context.getTypeManager();
    for (const char* name : kInterfaces)
    {
        if (!typeManager.hasType(name))
            typeManager.addType(PropertyObjectClassBuilder(typeManager, name).setParentName("InterfaceClockSync").build());
    }

    getSyncComponent(&sync);
    const auto syncPrivate = sync.asPtr<ISyncComponentPrivate>(true);
    for (const char* name : kInterfaces)
        syncPrivate.addInterface(PropertyObject(typeManager, name));
    sync.setSelectedSource(0);
    syncPrivate.setSyncLocked(true);

    // Each interface offers the modes it has: PhoneClock is the time source
    // (Input) or serves this phone's time to others (Output); network and
    // GNSS time can only be a source. Choosing a mode acts on it.
    const PropertyObjectPtr interfaces = sync.getPropertyValue("Interfaces");
    for (int i = 0; i < 3; i++)
    {
        const PropertyObjectPtr item = interfaces.getPropertyValue(kInterfaces[i]);
        try
        {
            item.setPropertyValue("ModeOptions",
                                  i == 0 ? Dict<IInteger, IString>({{kModeInput, "Input"}, {kModeOutput, "Output"}, {kModeOff, "Off"}})
                                         : Dict<IInteger, IString>({{kModeInput, "Input"}, {kModeOff, "Off"}}));
        }
        catch (const std::exception& error)
        {
            __android_log_print(ANDROID_LOG_WARN, kTag, "The modes of %s could not be set: %s", kInterfaces[i], error.what());
        }
        item.getOnPropertyValueWrite("Mode") += [this, i](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
        {
            if (!reportingModes)
                chooseMode(i, static_cast<Int>(args.getValue()));
        };
    }

    // Choosing the Source on the Synchronization component chooses the
    // TimeSource too.
    sync.getOnPropertyValueWrite("Source") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
    {
        const Int source = args.getValue();
        if (selecting || source < 0 || source > 2)
            return;
        selecting = true;
        try
        {
            objPtr.setPropertyValue("TimeSource", source);
        }
        catch (...)
        {
        }
        selecting = false;
    };
}

void PhoneDevice::chooseMode(int index, Int mode)
{
    // Whatever was chosen, the reporter writes back the modes as they then
    // are, such as Input again for a source that cannot be switched off.
    modesDirty = true;
    try
    {
        const Int source = objPtr.getPropertyValue("TimeSource");
        if (index == 0)
        {
            objPtr.setPropertyValue("ServeTime", mode == kModeOutput);
            if (mode == kModeInput)
                objPtr.setPropertyValue("TimeSource", 0);
        }
        else if (mode == kModeInput)
            objPtr.setPropertyValue("TimeSource", index);
        else if (mode == kModeOff && source == index)
            objPtr.setPropertyValue("TimeSource", 0);
    }
    catch (const std::exception& error)
    {
        __android_log_print(ANDROID_LOG_WARN, kTag, "The mode could not be applied: %s", error.what());
    }
    wake.notify_all();
}

void PhoneDevice::serveTime(bool on)
{
    std::string text;
    {
        std::lock_guard guard(lock);
        if (!on)
        {
            timeServer.reset();
            timeServerError.clear();
        }
        else if (!timeServer || !timeServer->running())
        {
            auto server = std::make_unique<TimeServer>(clock);
            timeServerError.clear();
            if (server->start(TimeServer::kPort, timeServerError))
                timeServer = std::move(server);
            else
                timeServer.reset();
        }
        text = timeServerError;
    }
    if (!on || !text.empty())
        objPtr.asPtr<IPropertyObjectProtected>().setProtectedPropertyValue("TimeServer", String(text.empty() ? "" : text + "."));
    modesDirty = true;
    wake.notify_all();
}

void PhoneDevice::selectSource(Int source)
{
    clock->setSource(static_cast<TimeSource>(std::clamp<Int>(source, 0, 2)));
    if (selecting)
        return;
    selecting = true;
    try
    {
        sync.setSelectedSource(source);
    }
    catch (...)
    {
    }
    selecting = false;
}

void PhoneDevice::reportTime()
{
    const auto protectedObject = objPtr.asPtr<IPropertyObjectProtected>();
    bool first = true;
    bool shownServing = false;
    std::string shownServer;
    PhoneClock::Status shown;
    int ticks = 0;
    std::unique_lock guard(lock);
    while (!stopping)
    {
        guard.unlock();
        const auto now = clock->status();
        try
        {
            if (first || now.message != shown.message)
                protectedObject.setProtectedPropertyValue("TimeStatus", String(now.message));
            // Offsets are shown to a hundredth of a millisecond, and changed
            // only when that changes.
            const auto rounded = [](double value) { return std::round(value * 100) / 100; };
            if (first || rounded(now.offsetMilliseconds) != rounded(shown.offsetMilliseconds))
                protectedObject.setProtectedPropertyValue("ClockOffset", Float(rounded(now.offsetMilliseconds)));
            if (first || rounded(now.uncertaintyMilliseconds) != rounded(shown.uncertaintyMilliseconds))
                protectedObject.setProtectedPropertyValue("ClockUncertainty", Float(rounded(now.uncertaintyMilliseconds)));

            bool serving = false;
            {
                std::lock_guard serverGuard(lock);
                serving = timeServer && timeServer->running();
            }
            if (modesDirty.exchange(false) || first || now.locked != shown.locked || now.source != shown.source || serving != shownServing)
            {
                sync.asPtr<ISyncComponentPrivate>(true).setSyncLocked(now.locked);
                const PropertyObjectPtr interfaces = sync.getPropertyValue("Interfaces");
                for (int i = 0; i < 3; i++)
                {
                    // PhoneClock is Output while this phone serves its time,
                    // whichever source the time itself follows.
                    const bool selected = static_cast<int>(now.source) == i;
                    const PropertyObjectPtr syncInterface = interfaces.getPropertyValue(kInterfaces[i]);
                    reportingModes = true;
                    try
                    {
                        syncInterface.setPropertyValue("Mode", i == 0 && serving ? kModeOutput : selected ? kModeInput : kModeOff);
                    }
                    catch (...)
                    {
                        reportingModes = false;
                        throw;
                    }
                    reportingModes = false;
                    const PropertyObjectPtr status = syncInterface.getPropertyValue("Status");
                    status.setPropertyValue("State", selected && !now.locked ? kStateWarning : kStateOk);
                }
                shownServing = serving;
            }

            // Where others reach this phone's time; the addresses change
            // with the network, so they are read every few seconds.
            if (serving && (ticks % 10 == 0 || shownServer.empty()))
            {
                std::string text;
                for (const auto& address : TimeServer::addresses())
                    text += (text.empty() ? "" : ", ") + address + ":" + std::to_string(TimeServer::kPort);
                text = text.empty() ? "Serving this phone's time, but the phone is on no network." : "Serving this phone's time at " + text + ".";
                if (text != shownServer)
                {
                    protectedObject.setProtectedPropertyValue("TimeServer", String(text));
                    shownServer = text;
                }
            }
            else if (!serving)
                shownServer.clear();
            if (first || now.locked != shown.locked || (!now.locked && now.message != shown.message))
            {
                if (now.locked)
                    setComponentStatus(ComponentStatus::Ok);
                else
                    setComponentStatusWithMessage(ComponentStatus::Warning, now.message);
            }
        }
        catch (const std::exception& error)
        {
            __android_log_print(ANDROID_LOG_WARN, kTag, "The time status could not be shown: %s", error.what());
        }
        shown = now;
        first = false;
        if (ticks == 0)
        {
            // Once, after the device is in place: function blocks come from
            // another module, through the module manager.
            try
            {
                addDerivedBlocks();
            }
            catch (const std::exception& error)
            {
                __android_log_print(ANDROID_LOG_WARN, kTag, "The derived signals could not be added: %s", error.what());
            }
        }
        if (++ticks % 4 == 1)
        {
            try
            {
                updateUsbChannels();
            }
            catch (const std::exception& error)
            {
                __android_log_print(ANDROID_LOG_WARN, kTag, "The USB channels could not be updated: %s", error.what());
            }
        }
        guard.lock();
        wake.wait_for(guard, std::chrono::milliseconds(500), [this] { return stopping; });
    }
}

void PhoneDevice::updateUsbChannels()
{
    {
        std::lock_guard guard(lock);
        if (stopping)
            return;
    }

    // Cameras, through Android's external camera support.
    const auto cameras = CameraChannel::externalCameras();
    for (auto it = usbCameras.begin(); it != usbCameras.end();)
    {
        if (std::find(cameras.begin(), cameras.end(), it->first) == cameras.end())
        {
            removeChannel(ioFolder, it->second);
            it = usbCameras.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (const auto& id : cameras)
    {
        if (usbCameras.count(id))
            continue;
        std::string localId = "UsbCamera";
        for (const char c : id)
            if (std::isalnum(static_cast<unsigned char>(c)))
                localId += c;
        const auto channel = createAndAddChannel<CameraChannel>(ioFolder, localId, clock, true, id);
        channel.setName(usbCameras.empty() ? "USB camera" : "USB camera " + std::to_string(usbCameras.size() + 1));
        usbCameras[id] = channel;
    }

    // Cameras, serial devices and input devices the app opened for the module.
    const auto connections = UsbHost::connections();
    for (auto it = usbDevices.begin(); it != usbDevices.end();)
    {
        const bool present = std::any_of(connections.begin(), connections.end(), [&](const auto& c) { return c->key == it->first; });
        if (!present)
        {
            removeChannel(ioFolder, it->second);
            it = usbDevices.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (const auto& connection : connections)
    {
        if (usbDevices.count(connection->key))
            continue;
        std::string suffix;
        for (const char c : connection->key)
            if (std::isdigit(static_cast<unsigned char>(c)))
                suffix += c;
        ChannelPtr channel;
        if (connection->kind == "camera")
            channel = createAndAddChannel<UvcCameraChannel>(ioFolder, "UsbCamera" + suffix, clock, connection);
        else if (connection->kind == "serial")
            channel = createAndAddChannel<UsbSerialChannel>(ioFolder, "UsbSerial" + suffix, clock, connection);
        else if (connection->kind == "input")
            channel = createAndAddChannel<UsbInputChannel>(ioFolder, "UsbInput" + suffix, clock, connection);
        else
            continue;
        if (!connection->name.empty())
            channel.setName(connection->name);
        usbDevices[connection->key] = channel;
    }

    // Microphones, as the app reports them.
    const auto inputs = PhoneStatus::usb().audioInputs;
    const auto has = [&](int id) {
        return std::any_of(inputs.begin(), inputs.end(), [&](const auto& input) { return input.first == id; });
    };
    for (auto it = usbMicrophones.begin(); it != usbMicrophones.end();)
    {
        if (!has(it->first))
        {
            removeChannel(ioFolder, it->second);
            it = usbMicrophones.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (const auto& [id, name] : inputs)
    {
        if (usbMicrophones.count(id))
            continue;
        const auto channel = createAndAddChannel<MicrophoneChannel>(ioFolder, "UsbMicrophone" + std::to_string(id), clock, true, id);
        channel.setName(name.empty() ? "USB microphone" : name);
        usbMicrophones[id] = channel;
    }
}

void PhoneDevice::addDerivedBlocks()
{
    const auto find = [&](const std::string& channel, const std::string& signal) -> SignalPtr
    {
        for (const auto& item : objPtr.asPtr<IDevice>().getChannels())
            if (item.getLocalId() == channel)
                for (const auto& candidate : item.getSignals())
                    if (candidate.getLocalId() == signal)
                        return candidate;
        return nullptr;
    };
    const auto output = [](const FunctionBlockPtr& block, const std::string& signal) -> SignalPtr
    {
        if (!block.assigned())
            return nullptr;
        for (const auto& candidate : block.getSignals())
            if (candidate.getLocalId() == signal)
                return candidate;
        return nullptr;
    };
    const auto add = [&](const char* type, const char* name, const SignalPtr& input, double window) -> FunctionBlockPtr
    {
        if (!input.assigned())
            return nullptr;
        try
        {
            const auto block = objPtr.asPtr<IDevice>().addFunctionBlock(type);
            block.setName(name);
            if (window > 0)
                block.setPropertyValue("Window", window);
            block.getInputPorts()[0].connect(input);
            return block;
        }
        catch (const std::exception& error)
        {
            __android_log_print(ANDROID_LOG_WARN, kTag, "%s could not be added: %s", name, error.what());
            return nullptr;
        }
    };

    const auto audio = find("Microphone", "Audio");
    const auto soundLevel = add("UniversalSoundLevelMeter", "Sound level", audio, 0);
    const auto soundSpectrum = add("UniversalSpectrum", "Sound spectrum", audio, 0);
    // Vibration as ISO 10816 measures it, from the accelerometer's three
    // axes; its filter takes off gravity.
    FunctionBlockPtr vibration;
    if (const auto x = find("Accelerometer", "X"); x.assigned())
    {
        vibration = add("UniversalVibration", "Vibration", x, 0);
        if (vibration.assigned())
        {
            const auto ports = vibration.getInputPorts();
            if (ports.getCount() >= 3)
            {
                ports[1].connect(find("Accelerometer", "Y"));
                ports[2].connect(find("Accelerometer", "Z"));
            }
        }
    }

    if (!dashboard)
        return;
    // The Dashboard's input ports grow as they are connected: each connection
    // adds the next free one.
    const auto board = objPtr.asPtr<IDevice>().addFunctionBlock("UniversalDashboard");
    board.setName("Dashboard");
    board.setPropertyValue("Title", objPtr.asPtr<IComponent>().getName());
    // A new six-digit PIN at every start, which the app shows: the page
    // changes settings only with it.
    std::random_device random;
    board.setPropertyValue("Pin", String(std::to_string(100000 + random() % 900000)));
    for (const auto& signal : {output(soundLevel, "Level"),
                               output(soundLevel, "Bands"),
                               output(soundSpectrum, "Spectrum"),
                               output(vibration, "VelocityRms"),
                               output(vibration, "DominantFrequency"),
                               find("Orientation", "Pitch"),
                               find("System", "BatteryLevel"),
                               find("Pressure", "Pressure")})
    {
        if (!signal.assigned())
            continue;
        for (const auto& port : board.getInputPorts())
        {
            if (!port.getSignal().assigned())
            {
                port.connect(signal);
                break;
            }
        }
    }
}

}  // namespace daq::modules::android_device

