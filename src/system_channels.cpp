#include "phone_status.h"
#include "sensor_channels.h"

#include <coreobjects/property_object_protected_ptr.h>

#include <cmath>

namespace daq::modules::android_device
{

namespace
{

/// After a start, the app has this long to report before the channel says it
/// does not.
constexpr int64_t kFirstReport = 5'000'000;

DataDescriptorPtr valueDescriptor(const char* symbol, const char* name, const char* quantity)
{
    auto builder = DataDescriptorBuilder().setSampleType(SampleType::Float32);
    if (*symbol)
        builder.setUnit(Unit(symbol, -1, name, quantity));
    return builder.build();
}

std::string joinLines(const std::vector<std::string>& lines)
{
    std::string out;
    for (const auto& line : lines)
        out += (out.empty() ? "" : "\n") + line;
    return out;
}

}  // namespace

// ---------------------------------------------------------------- StatusChannel

StatusChannel::StatusChannel(const ContextPtr& ctx,
                             const ComponentPtr& parent,
                             const StringPtr& localId,
                             std::shared_ptr<PhoneClock> clock,
                             const FunctionBlockTypePtr& type)
    : PhoneChannel(ctx, parent, localId, type, std::move(clock), {1, 2, 5, 10}, 1, true)
{
}

void StatusChannel::acquire(int rate)
{
    const int64_t period = 1'000'000 / rate;
    time.setDescriptor(timeDescriptor(1'000'000, period));

    const int64_t started = clock->nowUnixMicroseconds();
    int64_t next = (started / period + 1) * period;
    bool ok = false;
    std::vector<float> current;
    while (keepGoing(rate))
    {
        PhoneStatus::want(rate);
        const int64_t now = clock->nowUnixMicroseconds();
        if (next > now)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(std::min<int64_t>(next - now, 100'000)));
            continue;
        }
        if (!sample(current))
        {
            if (now - started > kFirstReport)
                throw std::runtime_error("The app does not report the phone's state. It reports while the app runs.");
            next += period;
            continue;
        }
        if (!ok)
        {
            reportOk();
            ok = true;
        }
        const auto domainPacket = DataPacket(time.getDescriptor(), 1, next);
        time.sendPacket(domainPacket);
        for (size_t i = 0; i < values.size() && i < current.size(); i++)
        {
            const auto packet = DataPacketWithDomain(domainPacket, values[i].getDescriptor(), 1);
            *static_cast<float*>(packet.getRawData()) = current[i];
            values[i].sendPacket(packet);
        }
        next += period;
        // After a stall, the next value is due a period from now.
        if (next < now - period)
            next = (now / period + 1) * period;
    }
}

// ---------------------------------------------------------------- SystemChannel

SystemChannel::SystemChannel(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId, std::shared_ptr<PhoneClock> clock)
    : StatusChannel(ctx, parent, localId, std::move(clock), FunctionBlockType("PhoneSystem", "System", "The phone's CPU, memory, network, battery and storage"))
{
    for (const auto& field : kSystemFields)
    {
        values.push_back(addValueSignal(field.id, valueDescriptor(field.unitSymbol, field.unitName, field.quantity)));
        values.back().setDescription(field.description);
    }
    startAcquisition();
}

SystemChannel::~SystemChannel()
{
    stopAcquisition();
}

bool SystemChannel::sample(std::vector<float>& out)
{
    return PhoneStatus::values(out);
}

// ---------------------------------------------------------------- UsbChannel

UsbChannel::UsbChannel(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId, std::shared_ptr<PhoneClock> clock)
    : StatusChannel(ctx, parent, localId, std::move(clock), FunctionBlockType("PhoneUsb", "USB", "What is connected to the phone's USB port"))
{
    values.push_back(addValueSignal("ChargingPower", valueDescriptor("W", "watts", "power")));
    values.back().setDescription("The power a cable charges the battery with: the charging speed. 0 while the phone does not charge.");
    values.push_back(addValueSignal("ChargingCurrent", valueDescriptor("mA", "milliamperes", "current")));
    values.back().setDescription("The current a cable charges the battery with. 0 while the phone does not charge.");
    values.push_back(addValueSignal("Devices", valueDescriptor("", "", "")));
    values.back().setDescription("The number of USB devices connected to the phone.");

    objPtr.addProperty(StringPropertyBuilder("PowerSource", "").setReadOnly(true).setDescription("Where the phone's power comes from.").build());
    objPtr.addProperty(StringPropertyBuilder("Charger", "")
                           .setReadOnly(true)
                           .setDescription("The most the charger offers, where Android tells it.")
                           .build());
    objPtr.addProperty(StringPropertyBuilder("ConnectedDevices", "")
                           .setReadOnly(true)
                           .setDescription("The USB devices connected to the phone. Cameras and microphones among them get channels of "
                                           "their own where Android supports them; the others are listed here.")
                           .build());
    startAcquisition();
}

UsbChannel::~UsbChannel()
{
    stopAcquisition();
}

bool UsbChannel::sample(std::vector<float>& out)
{
    std::vector<float> system;
    const bool fresh = PhoneStatus::values(system);
    const auto usb = PhoneStatus::usb();
    if (usb.version != shownVersion)
    {
        shownVersion = usb.version;
        const auto object = objPtr.asPtr<IPropertyObjectProtected>();
        object.setProtectedPropertyValue("PowerSource", String(usb.powerSource));
        object.setProtectedPropertyValue("Charger", String(usb.charger));
        object.setProtectedPropertyValue("ConnectedDevices", String(usb.devices.empty() ? "None" : joinLines(usb.devices)));
    }
    const bool charging = system[kCharging] > 0.5f;
    const auto positive = [](float value) { return std::isnan(value) ? value : std::max(0.0f, value); };
    out = {charging ? positive(system[kBatteryPower]) : 0.0f,
           charging ? positive(system[kBatteryCurrent]) : 0.0f,
           static_cast<float>(usb.devices.size())};
    return fresh;
}

}  // namespace daq::modules::android_device
