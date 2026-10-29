#include "sensor_channels.h"
#include "usb_host.h"

#include <coreobjects/property_object_protected_ptr.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace daq::modules::android_device
{

namespace
{

uint32_t read32(const uint8_t* d)
{
    return static_cast<uint32_t>(d[0]) | static_cast<uint32_t>(d[1]) << 8 | static_cast<uint32_t>(d[2]) << 16 | static_cast<uint32_t>(d[3]) << 24;
}

void write32(uint8_t* d, uint32_t value)
{
    for (int i = 0; i < 4; i++, value >>= 8)
        d[i] = static_cast<uint8_t>(value);
}

DataDescriptorPtr floatDescriptor()
{
    return DataDescriptorBuilder().setSampleType(SampleType::Float32).build();
}

/// A signal's local ID from a name the device sends, such as "temp".
std::string signalId(const std::string& name)
{
    std::string out;
    for (const char c : name)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            out += c;
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0])))
        out = "Value" + out;
    return out;
}

}  // namespace

// ---------------------------------------------------------------- UsbDeviceChannel

UsbDeviceChannel::UsbDeviceChannel(const ContextPtr& ctx,
                                   const ComponentPtr& parent,
                                   const StringPtr& localId,
                                   const FunctionBlockTypePtr& type,
                                   std::shared_ptr<PhoneClock> clock,
                                   std::shared_ptr<UsbConnection> connection,
                                   std::vector<int> rates,
                                   size_t defaultRate)
    : PhoneChannel(ctx, parent, localId, type, std::move(clock), std::move(rates), defaultRate, true)
    , connection(std::move(connection))
{
}

void UsbDeviceChannel::emitHeld(int64_t& next, int64_t period, const std::vector<float>& held)
{
    const int64_t now = clock->nowUnixMicroseconds();
    // After a stall, the grid continues from now rather than catching up.
    if (next < now - 100 * period)
        next = (now / period) * period;
    const int64_t first = next;
    size_t samples = 0;
    for (; next <= now; next += period)
        samples++;
    if (samples == 0 || values.empty())
        return;
    const auto domainPacket = DataPacket(time.getDescriptor(), samples, first);
    time.sendPacket(domainPacket);
    for (size_t i = 0; i < values.size(); i++)
    {
        const auto packet = DataPacketWithDomain(domainPacket, values[i].getDescriptor(), samples);
        auto* out = static_cast<float*>(packet.getRawData());
        std::fill(out, out + samples, i < held.size() ? held[i] : NAN);
        values[i].sendPacket(packet);
    }
}

// ---------------------------------------------------------------- UvcCameraChannel

UvcCameraChannel::UvcCameraChannel(const ContextPtr& ctx,
                                   const ComponentPtr& parent,
                                   const StringPtr& localId,
                                   std::shared_ptr<PhoneClock> clock,
                                   std::shared_ptr<UsbConnection> connection)
    : UsbDeviceChannel(ctx,
                       parent,
                       localId,
                       FunctionBlockType("PhoneUsbCamera", "USB camera", "A camera connected to the phone's USB port"),
                       std::move(clock),
                       std::move(connection),
                       {1, 2, 5, 10, 15, 30},
                       3)
{
    // The control interface tells the UVC version; the streaming interface
    // lists the formats and frame sizes. MJPEG is read where the camera
    // offers it, because the camera compresses the pictures itself;
    // otherwise uncompressed video is read and compressed here.
    uint8_t format = 0;
    PictureLayout layout = PictureLayout::Mjpeg;
    std::vector<FrameSize> uncompressed;
    std::string offered;
    const auto offer = [&offered](const std::string& name)
    {
        offered += (offered.empty() ? "" : ", ") + name;
    };
    for (const auto& item : this->connection->description.interfaces)
    {
        if (item.cls != 14 || item.alternate != 0)
            continue;
        for (const auto& d : item.classSpecific)
        {
            if (d.size() < 3)
                continue;
            if (item.subclass == 1 && d[2] == 0x01 && d.size() >= 5)
                uvcVersion = static_cast<uint16_t>(d[3] | d[4] << 8);
            if (item.subclass != 2)
                continue;
            streamingInterface = item.number;
            // A format descriptor starts the frame sizes that follow it.
            const uint8_t subtype = d[2];
            if (subtype == 0x06 && d.size() >= 4)
            {
                format = d[3];
                layout = PictureLayout::Mjpeg;
                offer("MJPEG");
            }
            else if (subtype == 0x04 && d.size() >= 22)
            {
                const auto known = uncompressedLayout(d.data() + 5);
                format = known ? d[3] : 0;
                layout = known.value_or(PictureLayout::Mjpeg);
                offer(std::string(reinterpret_cast<const char*>(d.data() + 5), 4));
            }
            else if (subtype == 0x0A || subtype == 0x0C || subtype == 0x10 || subtype == 0x12)
            {
                format = 0;
                offer(subtype == 0x10 && d.size() >= 21 ? std::string(reinterpret_cast<const char*>(d.data() + 5), 4) : "a stream format");
            }
            else if (format != 0 && d.size() >= 26 && ((subtype == 0x07 && layout == PictureLayout::Mjpeg) || (subtype == 0x05 && layout != PictureLayout::Mjpeg)))
            {
                const FrameSize size{layout, format, d[3], d[5] | d[6] << 8, d[7] | d[8] << 8, read32(d.data() + 21)};
                (layout == PictureLayout::Mjpeg ? sizes : uncompressed).push_back(size);
            }
        }
    }
    if (sizes.empty())
        sizes = std::move(uncompressed);

    auto names = List<IString>();
    size_t chosen = 0;
    long best = LONG_MAX;
    for (size_t i = 0; i < sizes.size(); i++)
    {
        names.pushBack(std::to_string(sizes[i].width) + " × " + std::to_string(sizes[i].height));
        const long distance = std::labs(static_cast<long>(sizes[i].width) * sizes[i].height - 640L * 480);
        if (distance < best)
        {
            best = distance;
            chosen = i;
        }
    }
    if (sizes.empty())
        names.pushBack("None");
    resolution = static_cast<int>(chosen);
    objPtr.addProperty(SelectionPropertyBuilder("Resolution", names, static_cast<Int>(chosen))
                           .setDescription("The picture size, from those the camera offers.")
                           .build());
    objPtr.getOnPropertyValueWrite("Resolution") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
    {
        resolution = static_cast<int>(static_cast<Int>(args.getValue()));
        version++;
    };

    objPtr.addProperty(StringPropertyBuilder("Diagnostics", "")
                           .setReadOnly(true)
                           .setDescription("What the stream negotiated and how many packets and pictures arrived, every second.")
                           .build());
    if (sizes.empty())
        objPtr.asPtr<IPropertyObjectProtected>().setProtectedPropertyValue(
            "Diagnostics", String("The camera offers " + (offered.empty() ? std::string("no video format") : offered) + ", none of which this channel reads."));
    image = addValueSignal("Image", DataDescriptorBuilder().setSampleType(SampleType::Binary).setUnit(Unit("jpeg")).build());
    image.setDescription("One JPEG picture per sample, as the camera sends it or compressed from its uncompressed video. The data descriptor's metadata holds its width and height.");
    startAcquisition();
}

UvcCameraChannel::~UvcCameraChannel()
{
    stopAcquisition();
}

void UvcCameraChannel::acquire(int rate)
{
    if (streamingInterface < 0)
        throw std::runtime_error("The device has no video stream.");
    if (sizes.empty())
        throw std::runtime_error("The camera sends its pictures in a form this channel does not read. See Diagnostics.");
    const int startVersion = version;
    const auto& size = sizes[std::min<size_t>(static_cast<size_t>(std::max(0, resolution.load())), sizes.size() - 1)];
    auto& usb = *connection;
    const auto iface = static_cast<uint16_t>(streamingInterface);
    const auto object = objPtr.asPtr<IPropertyObjectProtected>();

    // Negotiates the stream. It starts from the camera's own defaults (some
    // cameras refuse fields left zero), then asks for the format, the frame
    // size and the camera's frame interval; the most the camera sends at
    // once comes back.
    usb.setInterface(iface, 0);
    std::vector<uint8_t> probe;
    bool agreed = false;
    for (const size_t length : {uvcVersion >= 0x0150 ? size_t{48} : uvcVersion >= 0x0110 ? size_t{34} : size_t{26}, size_t{26}, size_t{34}})
    {
        probe.assign(length, 0);
        const auto n = static_cast<uint16_t>(length);
        usb.control(0xA1, 0x87, 0x0100, iface, probe.data(), n);  // GET_DEF; a camera without it keeps zeros
        probe[0] = 1;  // keep the frame interval
        probe[1] = 0;
        probe[2] = size.format;
        probe[3] = size.index;
        write32(probe.data() + 4, size.interval);
        if (usb.control(0x21, 0x01, 0x0100, iface, probe.data(), n) < 0)
            continue;
        if (usb.control(0xA1, 0x81, 0x0100, iface, probe.data(), n) < 0)
            continue;
        if (usb.control(0x21, 0x01, 0x0200, iface, probe.data(), n) < 0)
            continue;
        agreed = true;
        break;
    }
    if (!agreed)
        throw std::runtime_error("The camera did not accept the stream's settings.");
    const size_t maxPayload = read32(probe.data() + 22);
    const uint32_t interval = read32(probe.data() + 4);

    // The endpoints to try: bulk on the plain setting, or the isochronous
    // alternate settings from the smallest that carries a whole payload up
    // to the largest. A camera that names no payload size starts with the
    // largest.
    std::vector<std::pair<int, const UsbEndpoint*>> candidates;
    for (const auto& item : usb.description.interfaces)
    {
        if (item.number != streamingInterface)
            continue;
        for (const auto& e : item.endpoints)
            if (e.in() && ((item.alternate == 0 && e.type() == 2) || (item.alternate != 0 && e.type() == 1)))
                candidates.emplace_back(item.alternate, &e);
    }
    if (candidates.empty())
        throw std::runtime_error("The camera has no video endpoint.");
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.second->packetBytes() < b.second->packetBytes(); });
    size_t first = candidates.size() - 1;
    if (candidates.front().second->type() == 2)
        first = 0;
    else if (maxPayload > 0)
        for (size_t i = 0; i < candidates.size(); i++)
            if (candidates[i].second->packetBytes() >= maxPayload)
            {
                first = i;
                break;
            }

    const int64_t period = 1'000'000 / rate;
    time.setDescriptor(timeDescriptor(1'000'000, period));
    image.setDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Binary)
                            .setUnit(Unit("jpeg"))
                            .setName("Image")
                            .setMetadata(Dict<IString, IString>({{"width", std::to_string(size.width)},
                                                                 {"height", std::to_string(size.height)},
                                                                 {"camera", "usb"}}))
                            .build());

    std::string error;
    for (size_t candidate = first; candidate < candidates.size(); candidate++)
    {
        const auto [alternate, endpoint] = candidates[candidate];
        if (alternate != 0 && !usb.setInterface(iface, static_cast<unsigned>(alternate)))
        {
            error = "the alternate setting " + std::to_string(alternate) + " could not be selected";
            continue;
        }

        // Pictures arrive in payloads, each with a short header: bit 0 of its
        // second byte toggles with every picture and bit 1 ends one. The
        // newest whole picture is sent every period.
        uint64_t packets = 0;
        uint64_t payloads = 0;
        uint64_t pictures = 0;
        uint64_t flagged = 0;
        uint64_t incomplete = 0;
        std::vector<uint8_t> frame;
        const size_t rawBytes = uncompressedBytes(size.layout, size.width, size.height);
        int lastToggle = -1;
        int64_t next = clock->nowUnixMicroseconds();
        const auto started = std::chrono::steady_clock::now();
        auto shown = started - std::chrono::seconds(2);
        bool gaveUp = false;
        const auto describe = [&]
        {
            char text[512];
            std::snprintf(text, sizeof(text),
                          "%s %d \xC3\x97 %d at %.1f pictures a second, UVC %x.%02x. Alternate setting %d, %s, %zu bytes a packet "
                          "(the camera asked for %zu). %llu packets, %llu payloads with data, %llu pictures, %llu incomplete, %llu "
                          "payloads marked with an error.",
                          layoutName(size.layout), size.width, size.height, interval ? 1e7 / interval : 0.0, uvcVersion >> 8, uvcVersion & 0xFF, alternate,
                          endpoint->type() == 2 ? "bulk" : "isochronous", endpoint->packetBytes(), maxPayload,
                          static_cast<unsigned long long>(packets), static_cast<unsigned long long>(payloads),
                          static_cast<unsigned long long>(pictures), static_cast<unsigned long long>(incomplete),
                          static_cast<unsigned long long>(flagged));
            object.setProtectedPropertyValue("Diagnostics", String(text));
        };
        const auto send = [&](const std::vector<uint8_t>& picture, int64_t now)
        {
            const auto domainPacket = DataPacket(time.getDescriptor(), 1, now);
            const auto packet = BinaryDataPacket(domainPacket, image.getDescriptor(), picture.size());
            std::memcpy(packet.getRawData(), picture.data(), picture.size());
            time.sendPacket(domainPacket);
            image.sendPacket(packet);
            if (pictures == 1)
                reportOk();
        };
        const auto finish = [&]
        {
            if (size.layout != PictureLayout::Mjpeg)
            {
                // An uncompressed picture is whole only with all its bytes;
                // a shorter one lost packets on the way. Only the pictures
                // that are sent are compressed.
                if (frame.size() >= rawBytes && rawBytes > 0)
                {
                    pictures++;
                    const int64_t now = clock->nowUnixMicroseconds();
                    if (now >= next)
                    {
                        next = std::max(next + period, now);
                        send(encodeJpeg(size.layout, frame.data(), size.width, size.height, 85), now);
                    }
                }
                else if (!frame.empty())
                    incomplete++;
                frame.clear();
                return;
            }
            // Some cameras pad the start; the picture begins at its first
            // start-of-image marker.
            size_t begin = 0;
            while (begin + 1 < std::min<size_t>(frame.size(), 64) && !(frame[begin] == 0xFF && frame[begin + 1] == 0xD8))
                begin++;
            if (frame.size() > begin + 4 && frame[begin] == 0xFF && frame[begin + 1] == 0xD8)
            {
                pictures++;
                const int64_t now = clock->nowUnixMicroseconds();
                if (now >= next)
                {
                    next = std::max(next + period, now);
                    send(completeMjpeg(frame.data() + begin, frame.size() - begin), now);
                }
            }
            frame.clear();
        };
        const auto payload = [&](const uint8_t* data, size_t length)
        {
            packets++;
            if (length < 2 || data[0] < 2 || data[0] > length)
                return;
            const size_t header = data[0];
            const uint8_t flags = data[1];
            const int toggle = flags & 1;
            if (lastToggle >= 0 && toggle != lastToggle && !frame.empty())
                finish();
            lastToggle = toggle;
            if (flags & 0x40)
                flagged++;
            if (length > header)
            {
                payloads++;
                frame.insert(frame.end(), data + header, data + length);
            }
            if (frame.size() > 16 * 1024 * 1024)
                frame.clear();
            if (flags & 0x02)
                finish();
        };
        const auto running = [&]
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - shown >= std::chrono::seconds(1))
            {
                shown = now;
                describe();
            }
            // No picture in three seconds: this setting does not work, and
            // the next larger one is tried.
            if (pictures == 0 && now - started > std::chrono::seconds(3) && candidate + 1 < candidates.size())
            {
                gaveUp = true;
                return false;
            }
            return keepGoing(rate) && version == startVersion;
        };

        bool streamed = true;
        if (endpoint->type() == 1)
        {
            streamed = usb.readIsochronous(endpoint->address, endpoint->packetBytes(), payload, running, error);
        }
        else
        {
            std::vector<uint8_t> buffer(std::max<size_t>(maxPayload, 16 * 1024));
            while (running())
            {
                const int n = usb.transfer(endpoint->address, buffer.data(), buffer.size(), 500);
                if (n < 0)
                {
                    streamed = false;
                    error = "the camera was disconnected";
                    break;
                }
                if (n > 0)
                    payload(buffer.data(), static_cast<size_t>(n));
            }
        }
        describe();
        usb.setInterface(iface, 0);
        if (!streamed)
            throw std::runtime_error("The camera stopped: " + error + ".");
        if (!gaveUp)
            return;
        __android_log_print(ANDROID_LOG_WARN, kTag, "USB camera: no picture on alternate setting %d, trying the next", alternate);
    }
    throw std::runtime_error("The camera sends no pictures on any of its settings. See Diagnostics.");
}

// ---------------------------------------------------------------- UsbSerialChannel

UsbSerialChannel::UsbSerialChannel(const ContextPtr& ctx,
                                   const ComponentPtr& parent,
                                   const StringPtr& localId,
                                   std::shared_ptr<PhoneClock> clock,
                                   std::shared_ptr<UsbConnection> connection)
    : UsbDeviceChannel(ctx,
                       parent,
                       localId,
                       FunctionBlockType("PhoneUsbSerial", "USB serial device", "A serial device on the phone's USB port, read as numbers"),
                       std::move(clock),
                       std::move(connection),
                       {10, 20, 50, 100, 200},
                       2)
{
    static const int kBauds[] = {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
    auto names = List<IString>();
    for (const int b : kBauds)
        names.pushBack(std::to_string(b));
    objPtr.addProperty(SelectionPropertyBuilder("BaudRate", names, 4)
                           .setUnit(Unit("Bd"))
                           .setDescription("The serial speed the device sends at.")
                           .build());
    objPtr.getOnPropertyValueWrite("BaudRate") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
    {
        const auto index = static_cast<size_t>(static_cast<Int>(args.getValue()));
        baud = kBauds[std::min(index, std::size(kBauds) - 1)];
        version++;
    };
    objPtr.addProperty(StringPropertyBuilder("LastLine", "")
                           .setReadOnly(true)
                           .setDescription("The last line the device sent, as text. Numbers in it become signals; \"name:value\" pairs name them.")
                           .build());
    startAcquisition();
}

UsbSerialChannel::~UsbSerialChannel()
{
    stopAcquisition();
}

void UsbSerialChannel::acquire(int rate)
{
    const int startVersion = version;
    auto& usb = *connection;
    int communication = -1;
    const UsbEndpoint* in = nullptr;
    for (const auto& item : usb.description.interfaces)
    {
        if (item.cls == 2 && communication < 0)
            communication = item.number;
        if (item.cls == 10 || (item.cls == 2 && !in))
            for (const auto& e : item.endpoints)
                if (e.in() && e.type() == 2)
                    in = &e;
    }
    if (!in)
        throw std::runtime_error("The serial device has no data endpoint.");
    if (communication >= 0)
    {
        // 8 data bits, no parity, one stop bit; DTR and RTS on, which boards
        // such as the Arduino wait for.
        uint8_t coding[7] = {};
        write32(coding, static_cast<uint32_t>(baud.load()));
        coding[6] = 8;
        usb.control(0x21, 0x20, 0, static_cast<uint16_t>(communication), coding, 7);
        usb.control(0x21, 0x22, 3, static_cast<uint16_t>(communication), nullptr, 0);
    }

    const int64_t period = 1'000'000 / rate;
    time.setDescriptor(timeDescriptor(1'000'000, period));
    reportOk();

    std::vector<uint8_t> buffer(4096);
    std::string pending;
    std::vector<float> held;
    int64_t next = (clock->nowUnixMicroseconds() / period + 1) * period;
    auto lastShown = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    std::string lastLine;
    while (keepGoing(rate) && version == startVersion)
    {
        const int n = usb.transfer(in->address, buffer.data(), buffer.size(), 20);
        if (n < 0)
            throw std::runtime_error("The serial device was disconnected.");
        pending.append(reinterpret_cast<const char*>(buffer.data()), static_cast<size_t>(n));
        size_t end;
        while ((end = pending.find('\n')) != std::string::npos)
        {
            std::string line = pending.substr(0, end);
            pending.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lastLine = line;

            // Tokens split by commas, semicolons, tabs or spaces: a number,
            // or a name and a number joined by ':' or '='.
            size_t position = 0;
            size_t column = 0;
            while (position < line.size())
            {
                const size_t stop = line.find_first_of(",;\t ", position);
                const std::string token = line.substr(position, stop == std::string::npos ? std::string::npos : stop - position);
                position = stop == std::string::npos ? line.size() : stop + 1;
                if (token.empty())
                    continue;
                const size_t join = token.find_first_of(":=");
                const std::string name = join == std::string::npos ? "" : token.substr(0, join);
                const std::string number = join == std::string::npos ? token : token.substr(join + 1);
                char* parsed = nullptr;
                const float value = std::strtof(number.c_str(), &parsed);
                if (parsed == number.c_str())
                    continue;
                const std::string id = name.empty() ? "Value" + std::to_string(column + 1) : signalId(name);
                column++;
                auto found = std::find(names.begin(), names.end(), id);
                if (found == names.end())
                {
                    if (names.size() >= 16)
                        continue;
                    names.push_back(id);
                    values.push_back(addValueSignal(id, floatDescriptor()));
                    found = names.end() - 1;
                }
                const auto index = static_cast<size_t>(found - names.begin());
                if (held.size() <= index)
                    held.resize(index + 1, NAN);
                held[index] = value;
            }
        }
        if (pending.size() > 65536)
            pending.clear();
        if (!lastLine.empty() && std::chrono::steady_clock::now() - lastShown > std::chrono::milliseconds(500))
        {
            lastShown = std::chrono::steady_clock::now();
            objPtr.asPtr<IPropertyObjectProtected>().setProtectedPropertyValue("LastLine", String(lastLine));
        }
        emitHeld(next, period, held);
    }
}

// ---------------------------------------------------------------- UsbInputChannel

UsbInputChannel::UsbInputChannel(const ContextPtr& ctx,
                                 const ComponentPtr& parent,
                                 const StringPtr& localId,
                                 std::shared_ptr<PhoneClock> clock,
                                 std::shared_ptr<UsbConnection> connection)
    : UsbDeviceChannel(ctx,
                       parent,
                       localId,
                       FunctionBlockType("PhoneUsbInput", "USB input device", "An input device on the phone's USB port, such as a game controller"),
                       std::move(clock),
                       std::move(connection),
                       {10, 20, 50, 100},
                       2)
{
    for (const auto& item : this->connection->description.interfaces)
    {
        if (item.cls != 3 || endpoint != 0)
            continue;
        for (const auto& e : item.endpoints)
            if (e.in() && e.type() == 3)
            {
                endpoint = e.address;
                reportBytes = std::min<size_t>(e.maxPacket & 0x7ff, 16);
                break;
            }
    }
    for (size_t i = 0; i < reportBytes; i++)
    {
        values.push_back(addValueSignal("Byte" + std::to_string(i), floatDescriptor()));
        values.back().setDescription("Byte " + std::to_string(i) + " of the device's newest report, from 0 to 255.");
    }
    startAcquisition();
}

UsbInputChannel::~UsbInputChannel()
{
    stopAcquisition();
}

void UsbInputChannel::acquire(int rate)
{
    if (endpoint == 0)
        throw std::runtime_error("The input device has no report endpoint.");
    const int64_t period = 1'000'000 / rate;
    time.setDescriptor(timeDescriptor(1'000'000, period));
    reportOk();

    std::vector<uint8_t> report(64);
    std::vector<float> held(reportBytes, 0.0f);
    int64_t next = (clock->nowUnixMicroseconds() / period + 1) * period;
    while (keepGoing(rate))
    {
        const int n = connection->transfer(endpoint, report.data(), report.size(), 20);
        if (n < 0)
            throw std::runtime_error("The input device was disconnected.");
        for (size_t i = 0; i < static_cast<size_t>(n) && i < reportBytes; i++)
            held[i] = report[i];
        emitHeld(next, period, held);
    }
}

}  // namespace daq::modules::android_device
