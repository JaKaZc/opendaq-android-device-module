// AndroidDeviceModule: the sensors of the phone the app runs on, as one openDAQ
// device, android://, with a channel per sensor:
//
//   Microphone      one signal (AAudio)
//   Accelerometer   X, Y and Z in m/s² (NDK sensors)
//   Gyroscope       X, Y and Z in rad/s (NDK sensors)
//   Proximity       near (1) or not (0) (NDK sensors)
//   Light           the ambient light, in lux (NDK sensors)
//   InfraredLight   Samsung's infrared light stream, raw fields (NDK sensors)
//   Camera          one JPEG picture per sample (NDK camera)
//
// A channel the phone lacks is left out. The device's configuration, also
// given in the connection string (android://?Microphone=False&Camera=True), says
// whether the Microphone and Camera channels start active.

#include "phone_device.h"

#include <opendaq/module_exports.h>
#include <opendaq/module_factory.h>
#include <opendaq/module_impl.h>

namespace daq::modules::android_device
{

class AndroidDeviceModule final : public Module
{
public:
    explicit AndroidDeviceModule(const ContextPtr& context)
        : Module("AndroidDeviceModule", VersionInfo(2, 0, 0), context, "AndroidDeviceModule")
    {
    }

    ListPtr<IDeviceInfo> onGetAvailableDevices() override
    {
        // Discovery lists the phone until its device exists.
        auto devices = List<IDeviceInfo>();
        std::lock_guard guard(sync);
        const auto existing = created.assigned() ? created.getRef() : nullptr;
        if (!existing.assigned() || existing.isRemoved())
            devices.pushBack(PhoneDevice::createInfo());
        return devices;
    }

    DictPtr<IString, IDeviceType> onGetAvailableDeviceTypes() override
    {
        auto types = Dict<IString, IDeviceType>();
        const auto type = PhoneDevice::createType();
        types.set(type.getId(), type);
        return types;
    }

    DevicePtr onCreateDevice(const StringPtr& connectionString, const ComponentPtr& parent, const PropertyObjectPtr& config) override
    {
        if (connectionString.toStdString() != "android://")
            DAQ_THROW_EXCEPTION(NotFoundException, "The phone's sensors are the device android://, not {}.", connectionString);

        // One device reads the sensors; a second would compete with it for
        // the microphone and the camera.
        std::lock_guard guard(sync);
        if (const auto existing = created.assigned() ? created.getRef() : nullptr; existing.assigned() && !existing.isRemoved())
            DAQ_THROW_EXCEPTION(AlreadyExistsException, "The phone's sensors are already added.");
        const auto device = createWithImplementation<IDevice, PhoneDevice>(context, parent, config);
        created = device;
        return device;
    }

private:
    std::mutex sync;
    WeakRefPtr<IDevice> created;
};

}  // namespace daq::modules::android_device

DECLARE_MODULE_EXPORTS(AndroidDeviceModule)

using daq::modules::android_device::AndroidDeviceModule;
DEFINE_MODULE_EXPORTS(AndroidDeviceModule)
