#include "sensor_channels.h"

#include <coreobjects/callable_info_factory.h>
#include <coreobjects/property_object_protected_ptr.h>

namespace daq::modules::android_device
{

PhoneChannel::PhoneChannel(const ContextPtr& ctx,
                           const ComponentPtr& parent,
                           const StringPtr& localId,
                           const FunctionBlockTypePtr& type,
                           std::shared_ptr<PhoneClock> clock,
                           std::vector<int> rates,
                           size_t defaultRate,
                           bool startActive)
    : Channel(type, ctx, parent, localId)
    , clock(std::move(clock))
    , rates(std::move(rates))
{
    initComponentStatus();

    auto names = List<IString>();
    for (const int rate : this->rates)
        names.pushBack(std::to_string(rate));
    objPtr.addProperty(SelectionPropertyBuilder("SampleRate", names, static_cast<Int>(defaultRate))
                           .setUnit(Unit("Hz"))
                           .setDescription("Samples per second of every signal of the channel.")
                           .build());
    requestedRate = this->rates[defaultRate];
    objPtr.getOnPropertyValueWrite("SampleRate") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
    {
        const auto index = static_cast<size_t>(static_cast<Int>(args.getValue()));
        if (index < this->rates.size())
            requestedRate = this->rates[index];
    };

    time = createAndAddSignal("Time", nullptr, false);
    if (!startActive)
        setActive(False);
    running = this->active;
}

PhoneChannel::~PhoneChannel()
{
    stopAcquisition();
}

void PhoneChannel::startAcquisition()
{
    thread = std::thread([this] { run(); });
}

void PhoneChannel::stopAcquisition()
{
    stopping = true;
    if (thread.joinable())
        thread.join();
}

void PhoneChannel::activeChanged()
{
    Channel::activeChanged();
    running = this->active;
}

void PhoneChannel::notifyActiveChanged(bool onUpdate)
{
    Channel::notifyActiveChanged(onUpdate);
    running = this->active;
}

void PhoneChannel::removed()
{
    // The thread is joined in the destructor, outside the component's lock,
    // which the thread may be waiting for.
    stopping = true;
    Channel::removed();
}

void PhoneChannel::reportOk()
{
    setComponentStatus(ComponentStatus::Ok);
}

void PhoneChannel::reportError(const std::string& message)
{
    __android_log_print(ANDROID_LOG_WARN, kTag, "%s: %s", this->localId.toStdString().c_str(), message.c_str());
    setComponentStatusWithMessage(ComponentStatus::Error, message);
}

SignalConfigPtr PhoneChannel::addValueSignal(const std::string& localId, const DataDescriptorPtr& descriptor)
{
    auto signal = createAndAddSignal(localId, descriptor);
    signal.setDomainSignal(time);
    return signal;
}

void PhoneChannel::addProcedure(const std::string& name, const std::string& description, std::function<void()> action)
{
    objPtr.addProperty(FunctionPropertyBuilder(name, ProcedureInfo()).setReadOnly(true).setDescription(description).build());
    objPtr.asPtr<IPropertyObjectProtected>(true).setProtectedPropertyValue(name, Procedure([action] { action(); }));
}

void PhoneChannel::addSetting(const std::string& name, double initial, const UnitPtr& unit, const std::string& description, std::atomic<double>& target)
{
    auto builder = FloatPropertyBuilder(name, initial).setDescription(description);
    if (unit.assigned())
        builder.setUnit(unit);
    objPtr.addProperty(builder.build());
    target = initial;
    objPtr.getOnPropertyValueWrite(name) += [&target](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
    {
        target = static_cast<double>(args.getValue());
    };
}

DataDescriptorPtr PhoneChannel::timeDescriptor(int64_t ticksPerSecond, int64_t delta)
{
    return DataDescriptorBuilder()
        .setSampleType(SampleType::Int64)
        .setTickResolution(Ratio(1, ticksPerSecond))
        .setRule(LinearDataRule(delta, 0))
        .setOrigin(kUnixEpoch)
        .setUnit(seconds())
        .setName("Time")
        .build();
}

void PhoneChannel::run()
{
    bool wasRunning = true;
    while (!stopping)
    {
        if (!running)
        {
            if (wasRunning)
                setComponentStatusWithMessage(ComponentStatus::Ok, "The channel is inactive, so its sensor is off.");
            wasRunning = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        wasRunning = true;

        const int rate = requestedRate;
        try
        {
            acquire(rate);
        }
        catch (const std::exception& error)
        {
            reportError(error.what());
            // After a failure, try again a second later rather than at once.
            for (int i = 0; i < 10 && keepGoing(rate); i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

}  // namespace daq::modules::android_device
