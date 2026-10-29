#include "phone_clock.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <jni.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <ctime>
#include <random>

namespace daq::modules::android_device
{

namespace
{

constexpr int64_t kMillisecond = 1'000'000;
constexpr int64_t kSecond = 1'000'000'000;

/// Above this difference, the clock steps to the source's time rather than
/// moving towards it gradually.
constexpr int64_t kStepAbove = 10 * kMillisecond;

/// How fast the clock moves towards the source's time: 1 ms per second.
constexpr int64_t kSlewPerTick = 100'000;
constexpr auto kTick = std::chrono::milliseconds(100);

/// The clock asks every 16 seconds, and every 2 seconds for its first eight
/// answers, so it locks quickly.
constexpr int64_t kNtpInterval = 16 * kSecond;
constexpr int64_t kNtpStartInterval = 2 * kSecond;
constexpr size_t kNtpStartAnswers = 8;
constexpr int64_t kNtpRetry = 8 * kSecond;
/// The answers of the last half hour, at most 64, make the estimate.
constexpr int64_t kNtpWindow = 30 * 60 * kSecond;
constexpr size_t kNtpSamples = 64;
/// An NTP answer older than this no longer counts as locked.
constexpr int64_t kNtpFresh = 15 * 60 * kSecond;
/// The most the boot clock's rate is believed to differ from the server's.
constexpr double kMaxRate = 500e-6;

/// A GNSS report older than this no longer counts as locked.
constexpr int64_t kGnssFresh = 30 * kSecond;

int64_t clockNow(clockid_t clock)
{
    timespec now{};
    clock_gettime(clock, &now);
    return static_cast<int64_t>(now.tv_sec) * kSecond + now.tv_nsec;
}

/// Android's Unix time minus the boot clock.
int64_t phoneOffset()
{
    const int64_t before = clockNow(CLOCK_BOOTTIME);
    const int64_t realtime = clockNow(CLOCK_REALTIME);
    const int64_t after = clockNow(CLOCK_BOOTTIME);
    return realtime - (before + (after - before) / 2);
}

std::string formatDuration(int64_t nanoseconds)
{
    const int64_t seconds = nanoseconds / kSecond;
    if (seconds < 120)
        return std::to_string(seconds) + " s";
    return std::to_string(seconds / 60) + " min";
}

uint64_t readBigEndian64(const uint8_t* data)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; i++)
        value = value << 8 | data[i];
    return value;
}

void writeBigEndian64(uint8_t* data, uint64_t value)
{
    for (int i = 7; i >= 0; i--, value >>= 8)
        data[i] = static_cast<uint8_t>(value);
}

/// An NTP timestamp (seconds since 1900 and a binary fraction) as Unix time in
/// nanoseconds.
int64_t ntpToUnixNanoseconds(const uint8_t* data)
{
    const uint64_t value = readBigEndian64(data);
    int64_t seconds = static_cast<int64_t>(value >> 32);
    // The seconds wrap in 2036; a small value is from the next era.
    if (seconds < 2'208'988'800LL)
        seconds += 1LL << 32;
    const auto fraction = static_cast<uint64_t>(value & 0xFFFFFFFFu);
    return (seconds - 2'208'988'800LL) * kSecond + static_cast<int64_t>((fraction * kSecond) >> 32);
}

/// The newest time the app's GnssTime reported, shared by every phone clock.
struct GnssReport
{
    bool valid = false;
    int64_t offset = 0;
    int64_t uncertainty = 0;
    int64_t at = 0;  // boot clock
    std::string problem;
};

std::mutex gnssLock;
GnssReport gnss;
std::atomic<int> gnssClocks{0};

}  // namespace

bool gnssTimeWanted()
{
    return gnssClocks > 0;
}

PhoneClock::PhoneClock()
{
    offset = phoneOffset();
    thread = std::thread([this] { run(); });
}

PhoneClock::~PhoneClock()
{
    {
        std::lock_guard guard(lock);
        stopping = true;
        if (source == TimeSource::Gnss)
            gnssClocks--;
    }
    wake.notify_all();
    thread.join();
}

int64_t PhoneClock::bootNanoseconds()
{
    return clockNow(CLOCK_BOOTTIME);
}

int64_t PhoneClock::nowUnixNanoseconds() const
{
    return toUnixNanoseconds(bootNanoseconds());
}

void PhoneClock::setSource(TimeSource newSource)
{
    {
        std::lock_guard guard(lock);
        if (newSource == source)
            return;
        if (source == TimeSource::Gnss)
            gnssClocks--;
        if (newSource == TimeSource::Gnss)
            gnssClocks++;
        source = newSource;
        ntpChanged = true;
    }
    wake.notify_all();
}

void PhoneClock::setNtpServer(const std::string& server)
{
    {
        std::lock_guard guard(lock);
        ntpServer = server;
        ntpChanged = true;
    }
    wake.notify_all();
}

PhoneClock::Status PhoneClock::status()
{
    std::lock_guard guard(lock);
    return current;
}

bool PhoneClock::queryNtp(const std::string& server, int64_t& bestOffset, int64_t& delay, std::string& error)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* found = nullptr;
    // "host" or "host:port"; Studio serves time on a port of its own.
    std::string host = server;
    std::string port = "123";
    const auto colon = server.rfind(':');
    if (colon != std::string::npos && server.find(':') == colon && colon + 1 < server.size())
    {
        host = server.substr(0, colon);
        port = server.substr(colon + 1);
    }
    if (host.empty() || getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0 || !found)
    {
        error = "its name could not be resolved";
        return false;
    }

    // Four questions; the answer with the shortest round trip is the most
    // exact, because the network delays it least.
    static std::mt19937_64 random(std::random_device{}());
    int64_t bestDelay = INT64_MAX;
    error = "it did not answer within a second";
    for (int attempt = 0; attempt < 4; attempt++)
    {
        const int socket = ::socket(found->ai_family, SOCK_DGRAM, 0);
        if (socket < 0)
        {
            error = "no network socket could be opened";
            break;
        }
        timeval timeout{1, 0};
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        // Version 4, client mode. The transmit timestamp is a random number,
        // which the server returns as the originate timestamp, so a stray
        // answer is not taken for this one.
        uint8_t request[48] = {};
        request[0] = 0x23;
        const uint64_t cookie = random();
        writeBigEndian64(request + 40, cookie);

        uint8_t reply[48] = {};
        const int64_t sent = bootNanoseconds();
        ssize_t received = -1;
        if (sendto(socket, request, sizeof(request), 0, found->ai_addr, found->ai_addrlen) == sizeof(request))
            received = recv(socket, reply, sizeof(reply), 0);
        const int64_t answered = bootNanoseconds();
        close(socket);

        if (received < 48 || (reply[0] & 7) != 4 || readBigEndian64(reply + 24) != cookie)
            continue;
        if (reply[1] == 0)
        {
            error = "it asked not to be asked";
            continue;
        }
        const int64_t serverReceived = ntpToUnixNanoseconds(reply + 32);
        const int64_t serverSent = ntpToUnixNanoseconds(reply + 40);
        const int64_t roundTrip = std::max<int64_t>(0, (answered - sent) - (serverSent - serverReceived));
        if (roundTrip < bestDelay)
        {
            bestDelay = roundTrip;
            bestOffset = ((serverReceived - sent) + (serverSent - answered)) / 2;
        }
    }
    freeaddrinfo(found);
    if (bestDelay == INT64_MAX)
        return false;
    delay = bestDelay;
    return true;
}

void PhoneClock::run()
{
    // Network time: the recent answers, each the server's time minus the
    // boot clock and the round trip it took, and when to ask next.
    struct NtpSample
    {
        int64_t at;
        int64_t offset;
        int64_t delay;
    };
    std::deque<NtpSample> samples;
    size_t answers = 0;
    int64_t nextNtp = 0;
    std::string ntpError;
    std::string server;

    // The best estimate of the server's time minus the boot clock at `now`:
    // the answers with the shortest round trips, which the network delayed
    // least, fitted with a straight line, whose slope is how much faster the
    // boot clock runs than the server's.
    const auto estimate = [&](int64_t now, int64_t& offset, int64_t& uncertainty)
    {
        int64_t shortest = INT64_MAX;
        for (const auto& sample : samples)
            shortest = std::min(shortest, sample.delay);
        std::vector<const NtpSample*> good;
        for (const auto& sample : samples)
            if (sample.delay <= shortest + shortest / 2 + 2 * kMillisecond)
                good.push_back(&sample);
        double meanAt = 0;
        double meanOffset = 0;
        for (const auto* sample : good)
        {
            meanAt += static_cast<double>(sample->at);
            meanOffset += static_cast<double>(sample->offset);
        }
        meanAt /= static_cast<double>(good.size());
        meanOffset /= static_cast<double>(good.size());
        double slope = 0;
        if (good.size() >= 3 && good.back()->at - good.front()->at >= 60 * kSecond)
        {
            double covariance = 0;
            double variance = 0;
            for (const auto* sample : good)
            {
                const double dt = static_cast<double>(sample->at) - meanAt;
                covariance += dt * (static_cast<double>(sample->offset) - meanOffset);
                variance += dt * dt;
            }
            if (variance > 0)
                slope = std::clamp(covariance / variance, -kMaxRate, kMaxRate);
        }
        double residual = 0;
        for (const auto* sample : good)
        {
            const double fitted = meanOffset + slope * (static_cast<double>(sample->at) - meanAt);
            residual += (static_cast<double>(sample->offset) - fitted) * (static_cast<double>(sample->offset) - fitted);
        }
        residual = std::sqrt(residual / static_cast<double>(good.size()));
        offset = static_cast<int64_t>(meanOffset + slope * (static_cast<double>(now) - meanAt));
        uncertainty = shortest / 2 + static_cast<int64_t>(residual);
    };

    bool applied = false;
    std::unique_lock guard(lock);
    while (!stopping)
    {
        const TimeSource chosen = source;
        if (ntpChanged)
        {
            ntpChanged = false;
            if (server != ntpServer)
            {
                server = ntpServer;
                samples.clear();
                answers = 0;
                ntpError.clear();
            }
            nextNtp = 0;
        }
        guard.unlock();

        const int64_t now = bootNanoseconds();
        const int64_t phone = phoneOffset();
        int64_t target = phone;
        Status next;
        next.source = chosen;

        if (chosen == TimeSource::Phone)
        {
            next.locked = true;
            next.message = "The time is the phone's own clock.";
        }
        else if (chosen == TimeSource::Network)
        {
            if (now >= nextNtp)
            {
                int64_t measured = 0;
                int64_t delay = 0;
                if (queryNtp(server, measured, delay, ntpError))
                {
                    const int64_t answered = bootNanoseconds();
                    samples.push_back({answered, measured, delay});
                    while (samples.size() > kNtpSamples || answered - samples.front().at > kNtpWindow)
                        samples.pop_front();
                    answers++;
                    ntpError.clear();
                    nextNtp = answered + (answers < kNtpStartAnswers ? kNtpStartInterval : kNtpInterval);
                }
                else
                {
                    nextNtp = bootNanoseconds() + kNtpRetry;
                }
            }
            if (!samples.empty())
            {
                const int64_t age = now - samples.back().at;
                int64_t uncertainty = 0;
                estimate(now, target, uncertainty);
                next.uncertaintyMilliseconds = static_cast<double>(uncertainty) / kMillisecond;
                next.locked = age < kNtpFresh;
                if (next.locked)
                    next.message = "The time follows the NTP server " + server + ".";
                else
                    next.message = "The NTP server " + server + " has not answered for " + formatDuration(age) +
                                   ". The time continues from its last answer.";
            }
            else if (!ntpError.empty())
            {
                next.message = "The NTP server " + server + " could not be used, because " + ntpError +
                               ". The time is the phone's own clock until it answers.";
            }
            else
            {
                next.message = "Asking the NTP server " + server + ".";
            }
        }
        else
        {
            GnssReport report;
            {
                std::lock_guard gnssGuard(gnssLock);
                report = gnss;
            }
            if (report.valid)
            {
                const int64_t age = now - report.at;
                target = report.offset;
                next.uncertaintyMilliseconds = static_cast<double>(report.uncertainty) / kMillisecond;
                next.locked = age < kGnssFresh;
                if (next.locked)
                    next.message = "The time follows the phone's satellite receiver.";
                else
                    next.message = "The satellite receiver has not reported time for " + formatDuration(age) +
                                   ". The time continues from its last report.";
            }
            else if (!report.problem.empty())
            {
                next.message = report.problem + " The time is the phone's own clock until then.";
            }
            else
            {
                next.message = "Waiting for time from the phone's satellite receiver, which needs a view of the sky. The time is "
                               "the phone's own clock until then.";
            }
        }
        next.offsetMilliseconds = static_cast<double>(target - phone) / kMillisecond;

        // Move to the target: at once at the start or for a large difference,
        // gradually otherwise.
        const int64_t difference = target - offset.load();
        if (!applied || std::llabs(difference) > kStepAbove)
        {
            offset = target;
            if (applied && difference != 0)
                stepCount++;
            applied = true;
        }
        else
        {
            offset += std::clamp(difference, -kSlewPerTick, kSlewPerTick);
        }

        guard.lock();
        current = next;
        wake.wait_for(guard, kTick, [&] { return stopping || ntpChanged; });
    }
}

// ---------------------------------------------------------------- TimeServer

namespace
{

constexpr int64_t kNtpEpochSeconds = 2'208'988'800;  // from 1900 to 1970

void putTimestamp(uint8_t* out, int64_t unixNanoseconds)
{
    const int64_t seconds = unixNanoseconds / 1'000'000'000 + kNtpEpochSeconds;
    const uint64_t fraction = (static_cast<uint64_t>(unixNanoseconds % 1'000'000'000) << 32) / 1'000'000'000;
    for (int i = 0; i < 4; i++)
    {
        out[i] = static_cast<uint8_t>(static_cast<uint32_t>(seconds) >> (24 - 8 * i));
        out[4 + i] = static_cast<uint8_t>(static_cast<uint32_t>(fraction) >> (24 - 8 * i));
    }
}

}  // namespace

TimeServer::TimeServer(std::shared_ptr<PhoneClock> clock)
    : clock(std::move(clock))
{
}

TimeServer::~TimeServer()
{
    stop();
}

bool TimeServer::start(uint16_t port, std::string& error)
{
    if (running())
        return true;
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        error = std::string("No socket: ") + std::strerror(errno);
        return false;
    }
    const int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    {
        error = "Port " + std::to_string(port) + " could not be opened: " + std::strerror(errno);
        close(fd);
        return false;
    }
    boundPort = port;
    stopping = false;
    socket = fd;
    thread = std::thread([this] { run(); });
    return true;
}

void TimeServer::stop()
{
    stopping = true;
    if (thread.joinable())
        thread.join();
    const int fd = socket.exchange(-1);
    if (fd >= 0)
        close(fd);
}

std::vector<std::string> TimeServer::addresses()
{
    std::vector<std::string> out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0)
        return out;
    for (const ifaddrs* item = list; item; item = item->ifa_next)
    {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET || (item->ifa_flags & IFF_LOOPBACK) || !(item->ifa_flags & IFF_UP))
            continue;
        char text[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(item->ifa_addr)->sin_addr, text, sizeof(text)))
            out.emplace_back(text);
    }
    freeifaddrs(list);
    return out;
}

void TimeServer::run()
{
    // The stratum and reference say what the phone's time follows: the
    // satellites, a server, or only itself. They are read once a second.
    uint8_t stratum = 4;
    char reference[4] = {'L', 'O', 'C', 'L'};
    auto checked = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    while (!stopping)
    {
        const int fd = socket;
        pollfd waiting{fd, POLLIN, 0};
        if (poll(&waiting, 1, 500) <= 0)
            continue;
        uint8_t request[512];
        sockaddr_storage from{};
        socklen_t fromLength = sizeof(from);
        const ssize_t length = recvfrom(fd, request, sizeof(request), 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
        const int64_t received = clock->nowUnixNanoseconds();
        if (length < 48 || (request[0] & 7) != 3)
            continue;

        if (std::chrono::steady_clock::now() - checked > std::chrono::seconds(1))
        {
            checked = std::chrono::steady_clock::now();
            const auto status = clock->status();
            const bool gnss = status.locked && status.source == TimeSource::Gnss;
            const bool network = status.locked && status.source == TimeSource::Network;
            stratum = gnss ? 1 : network ? 3 : 4;
            std::memcpy(reference, gnss ? "GPS\0" : network ? "NTP\0" : "LOCL", 4);
        }

        const int version = (request[0] >> 3) & 7 ? (request[0] >> 3) & 7 : 4;
        uint8_t reply[48] = {};
        reply[0] = static_cast<uint8_t>(version << 3 | 4);  // no leap warning, server mode
        reply[1] = stratum;
        reply[2] = request[2];  // the poll interval, as asked
        reply[3] = 0xEC;        // precision, about a microsecond
        std::memcpy(reply + 12, reference, 4);
        putTimestamp(reply + 16, received);              // reference: the clock is set
        std::memcpy(reply + 24, request + 40, 8);        // originate: the client's transmit time
        putTimestamp(reply + 32, received);              // receive
        putTimestamp(reply + 40, clock->nowUnixNanoseconds());  // transmit
        sendto(fd, reply, sizeof(reply), 0, reinterpret_cast<sockaddr*>(&from), fromLength);
    }
}

}  // namespace daq::modules::android_device

using daq::modules::android_device::gnss;
using daq::modules::android_device::gnssLock;

// The app's GnssTime (io.opendaq.androiddevice.GnssTime) reports the satellite
// receiver's time through these, while a phone clock wants it.

extern "C" JNIEXPORT jboolean JNICALL Java_io_opendaq_androiddevice_GnssTime_wanted(JNIEnv*, jclass)
{
    return daq::modules::android_device::gnssTimeWanted() ? JNI_TRUE : JNI_FALSE;
}

/// The receiver's Unix time at a boot clock stamp, and how far it may be off,
/// all in nanoseconds. A report is taken unless it is less exact than a recent
/// one.
extern "C" JNIEXPORT void JNICALL Java_io_opendaq_androiddevice_GnssTime_report(JNIEnv*, jclass, jlong unixNanoseconds, jlong bootNanoseconds, jlong uncertaintyNanoseconds)
{
    std::lock_guard guard(gnssLock);
    const int64_t now = daq::modules::android_device::PhoneClock::bootNanoseconds();
    const bool recent = gnss.valid && now - gnss.at < 5'000'000'000LL;
    if (recent && uncertaintyNanoseconds > gnss.uncertainty * 2)
        return;
    gnss.valid = true;
    gnss.offset = unixNanoseconds - bootNanoseconds;
    gnss.uncertainty = uncertaintyNanoseconds;
    gnss.at = bootNanoseconds;
    gnss.problem.clear();
}

/// Why the app cannot report GNSS time, such as a missing permission, or null
/// when it can.
extern "C" JNIEXPORT void JNICALL Java_io_opendaq_androiddevice_GnssTime_problem(JNIEnv* env, jclass, jstring message)
{
    std::string text;
    if (message)
    {
        const char* chars = env->GetStringUTFChars(message, nullptr);
        text = chars ? chars : "";
        env->ReleaseStringUTFChars(message, chars);
    }
    std::lock_guard guard(gnssLock);
    gnss.problem = text;
}
