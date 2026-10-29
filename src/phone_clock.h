// The phone device's clock: one time base for every channel, from a time
// source the user chooses.
//
// Android stamps sensor events, audio frames and camera pictures with the boot
// clock (CLOCK_BOOTTIME), which counts steadily from the phone's start. The
// clock turns those stamps into Unix time by adding an offset, which comes
// from the chosen source:
//
//   Phone clock    Android's own time (CLOCK_REALTIME), which Android sets from
//                  the network when automatic date and time is on.
//   Network time   An NTP server, asked by the clock itself every 64 seconds.
//   GNSS time      The phone's satellite receiver, reported by the app, which
//                  needs the location permission for it (GnssTime.kt).
//
// A small change of the offset is applied gradually, 1 ms per second, so time
// stays continuous. A change above 10 ms is a step, which the channels see in
// steps() and start their time axis again.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace daq::modules::android_device
{

enum class TimeSource : int
{
    Phone = 0,
    Network = 1,
    Gnss = 2,
};

class PhoneClock
{
public:
    PhoneClock();
    ~PhoneClock();

    PhoneClock(const PhoneClock&) = delete;
    PhoneClock& operator=(const PhoneClock&) = delete;

    /// A boot clock stamp, in nanoseconds, as Unix time in nanoseconds.
    int64_t toUnixNanoseconds(int64_t bootNanoseconds) const
    {
        return bootNanoseconds + offset.load();
    }

    int64_t nowUnixNanoseconds() const;
    int64_t nowUnixMicroseconds() const
    {
        return nowUnixNanoseconds() / 1000;
    }

    /// Counts the steps of the clock. A channel that sees it change starts its
    /// time axis again.
    uint64_t steps() const
    {
        return stepCount.load();
    }

    void setSource(TimeSource source);
    void setNtpServer(const std::string& server);

    struct Status
    {
        TimeSource source = TimeSource::Phone;
        /// Whether the clock follows the chosen source.
        bool locked = false;
        /// The source's time minus the phone's own, and how far it may be off.
        double offsetMilliseconds = 0;
        double uncertaintyMilliseconds = 0;
        /// A sentence on the state, such as why the source is not available.
        std::string message;
    };

    Status status();

    /// The boot clock now, in nanoseconds.
    static int64_t bootNanoseconds();

private:
    void run();
    /// Asks the NTP server: its time minus the boot clock, and the round trip of
    /// the fastest of four questions. Returns false, with the reason, when it
    /// did not answer.
    bool queryNtp(const std::string& server, int64_t& offset, int64_t& delay, std::string& error);

    // The offset from the boot clock to Unix time, as applied.
    std::atomic<int64_t> offset{0};
    std::atomic<uint64_t> stepCount{0};

    std::mutex lock;
    std::condition_variable wake;
    bool stopping = false;
    TimeSource source = TimeSource::Phone;
    std::string ntpServer = "time.android.com";
    bool ntpChanged = true;
    Status current;
    std::thread thread;
};

/// Whether any phone clock wants GNSS time, for the app's GnssTime.
bool gnssTimeWanted();

/// An NTP server (RFC 5905, server mode) that gives the phone clock's time to
/// other devices on the network, on its own thread, so another phone can
/// follow this one as its network time.
class TimeServer
{
public:
    static constexpr uint16_t kPort = 12300;

    explicit TimeServer(std::shared_ptr<PhoneClock> clock);
    ~TimeServer();

    TimeServer(const TimeServer&) = delete;
    TimeServer& operator=(const TimeServer&) = delete;

    /// Starts serving on the port; false, with the reason, when it cannot.
    bool start(uint16_t port, std::string& error);
    void stop();
    bool running() const
    {
        return socket >= 0;
    }
    uint16_t port() const
    {
        return boundPort;
    }

    /// The phone's IPv4 addresses on its networks, without the loopback.
    static std::vector<std::string> addresses();

private:
    void run();

    std::shared_ptr<PhoneClock> clock;
    std::atomic<int> socket{-1};
    uint16_t boundPort = 0;
    std::atomic<bool> stopping{false};
    std::thread thread;
};

}  // namespace daq::modules::android_device
