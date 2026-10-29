#include "sensor_channels.h"

#include <aaudio/AAudio.h>

#include <cmath>

namespace daq::modules::android_device
{

namespace
{

/// Closes the stream however acquire() leaves.
struct StreamGuard
{
    AAudioStream* stream = nullptr;
    ~StreamGuard()
    {
        if (stream)
        {
            AAudioStream_requestStop(stream);
            AAudioStream_close(stream);
        }
    }
};

std::string audioError(const char* what, aaudio_result_t result)
{
    return std::string(what) + ": " + AAudio_convertResultToText(result);
}

}  // namespace

MicrophoneChannel::MicrophoneChannel(const ContextPtr& ctx,
                                     const ComponentPtr& parent,
                                     const StringPtr& localId,
                                     std::shared_ptr<PhoneClock> clock,
                                     bool startActive,
                                     int32_t audioDevice)
    : PhoneChannel(ctx,
                   parent,
                   localId,
                   audioDevice == 0 ? FunctionBlockType("PhoneMicrophone", "Microphone", "The phone's microphone")
                                    : FunctionBlockType("PhoneUsbMicrophone", "USB microphone", "A microphone connected to the phone's USB port"),
                   std::move(clock),
                   {8000, 16000, 44100, 48000},
                   3,
                   startActive)
    , audioDevice(audioDevice)
{
    addSetting("Sensitivity", 0.0, Unit("Pa", -1, "pascals", "sound pressure"),
               "The sound pressure at full scale, which Calibrate finds. With 0, the microphone is not calibrated, and Audio is in "
               "full scale from -1 to 1; otherwise Audio is in pascals.",
               sensitivity);
    addSetting("CalibrationLevel", 94.0, Unit("dB", -1, "decibels", "sound pressure level"),
               "The level of the sound calibrator that Calibrate measures, 94 dB for most.", calibrationLevel);
    addProcedure("Calibrate",
                 "Hold a sound calibrator to the microphone and run this: it measures two seconds of the calibrator's tone and sets "
                 "Sensitivity, so Audio is in pascals and the levels are dB SPL.",
                 [this] { calibrating = true; });
    addProcedure("ResetCalibration", "Sets Sensitivity to 0, so Audio is in full scale again.", [this] { objPtr.setPropertyValue("Sensitivity", 0.0); });
    audio = addValueSignal("Audio",
                           DataDescriptorBuilder().setSampleType(SampleType::Float32).setValueRange(Range(-1.0, 1.0)).setName("Audio").build());
    startAcquisition();
}

MicrophoneChannel::~MicrophoneChannel()
{
    stopAcquisition();
}

void MicrophoneChannel::acquire(int rate)
{
    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK)
        throw std::runtime_error(audioError("AAudio is not available", result));
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setSampleRate(builder, rate);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    if (audioDevice != 0)
        AAudioStreamBuilder_setDeviceId(builder, audioDevice);
    // Voice recognition is the preset with the least processing that every
    // phone supports: no automatic gain and no noise suppression.
    if (__builtin_available(android 28, *))
        AAudioStreamBuilder_setInputPreset(builder, AAUDIO_INPUT_PRESET_VOICE_RECOGNITION);

    StreamGuard guard;
    result = AAudioStreamBuilder_openStream(builder, &guard.stream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK)
    {
        guard.stream = nullptr;
        throw std::runtime_error(audioError("The microphone could not be opened. Check that the app may record audio", result));
    }

    // The phone may run the stream at another rate than asked for.
    const int actual = AAudioStream_getSampleRate(guard.stream);
    time.setDescriptor(timeDescriptor(actual, 1));

    result = AAudioStream_requestStart(guard.stream);
    if (result != AAUDIO_OK)
        throw std::runtime_error(audioError("The microphone could not be started", result));
    reportOk();

    // Blocks of 20 ms, read on this thread rather than in a real-time callback.
    std::vector<float> buffer(std::max(64, actual / 50));
    // The signal's unit follows the calibration: pascals once calibrated,
    // full scale before.
    double described = -1;
    const auto describe = [&](double pascals)
    {
        described = pascals;
        auto builder = DataDescriptorBuilder().setSampleType(SampleType::Float32).setName("Audio");
        if (pascals > 0)
            builder.setUnit(Unit("Pa", -1, "pascals", "sound pressure")).setValueRange(Range(-pascals, pascals));
        else
            builder.setValueRange(Range(-1.0, 1.0));
        audio.setDescriptor(builder.build());
    };
    // A Calibrate collects the mean square of two seconds of the tone.
    double calibrationSum = 0;
    int64_t calibrationCount = -1;
    int64_t ticks = -1;
    int64_t framesRead = 0;
    uint64_t steps = clock->steps();
    // The microphone's clock and the phone's run at slightly different rates.
    // Their difference is averaged, and once it exceeds a millisecond the time
    // axis catches up, so it never drifts from the phone's clock.
    double drift = 0;
    const double tolerance = std::max(2.0, actual / 1000.0);
    while (keepGoing(rate))
    {
        const auto frames = AAudioStream_read(guard.stream, buffer.data(), static_cast<int32_t>(buffer.size()), 200'000'000);
        if (frames < 0)
            throw std::runtime_error(audioError("The microphone stopped", frames));
        if (frames == 0)
            continue;

        // When the block's first frame was recorded: by AAudio's stamp of a
        // recent frame, or else by when the block arrived.
        int64_t position = 0;
        int64_t stamp = 0;
        int64_t expected;
        if (AAudioStream_getTimestamp(guard.stream, CLOCK_BOOTTIME, &position, &stamp) == AAUDIO_OK && position > 0)
            expected = unixTicks(clock->toUnixNanoseconds(stamp), actual) + (framesRead - position);
        else
            expected = unixTicks(clock->nowUnixNanoseconds(), actual) - frames;

        if (ticks < 0 || clock->steps() != steps)
        {
            ticks = expected;
            steps = clock->steps();
            drift = 0;
        }
        else
        {
            drift += 0.02 * (static_cast<double>(expected - ticks) - drift);
            if (std::abs(drift) > tolerance)
            {
                ticks = expected;
                drift = 0;
            }
        }

        if (calibrating.exchange(false))
        {
            calibrationSum = 0;
            calibrationCount = 0;
        }
        if (calibrationCount >= 0)
        {
            for (int32_t i = 0; i < frames; i++)
                calibrationSum += static_cast<double>(buffer[static_cast<size_t>(i)]) * buffer[static_cast<size_t>(i)];
            calibrationCount += frames;
            if (calibrationCount >= 2 * actual)
            {
                const double rms = std::sqrt(calibrationSum / static_cast<double>(calibrationCount));
                calibrationCount = -1;
                if (rms > 1e-6)
                    objPtr.setPropertyValue("Sensitivity", 20e-6 * std::pow(10.0, calibrationLevel.load() / 20.0) / rms);
                else
                    reportError("Calibrate heard silence. Hold the calibrator to the microphone and try again.");
            }
        }
        const double pascals = sensitivity.load();
        if (pascals != described)
            describe(pascals);

        const auto domainPacket = DataPacket(time.getDescriptor(), frames, ticks);
        const auto packet = DataPacketWithDomain(domainPacket, audio.getDescriptor(), frames);
        auto* out = static_cast<float*>(packet.getRawData());
        const float factor = pascals > 0 ? static_cast<float>(pascals) : 1.0f;
        for (int32_t i = 0; i < frames; i++)
            out[i] = buffer[static_cast<size_t>(i)] * factor;
        time.sendPacket(domainPacket);
        audio.sendPacket(packet);
        ticks += frames;
        framesRead += frames;
    }
}

}  // namespace daq::modules::android_device
