#include "usb_host.h"

#include <jni.h>
#include <linux/usbdevice_fs.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace daq::modules::android_device
{

// ---------------------------------------------------------------- descriptors

UsbDescription UsbDescription::parse(const std::vector<uint8_t>& raw)
{
    UsbDescription out;
    size_t at = 0;
    while (at + 2 <= raw.size())
    {
        const size_t length = raw[at];
        const uint8_t type = raw[at + 1];
        if (length < 2 || at + length > raw.size())
            break;
        const uint8_t* d = raw.data() + at;
        switch (type)
        {
            case 1:  // device
                if (length >= 12)
                {
                    out.vendor = static_cast<uint16_t>(d[8] | d[9] << 8);
                    out.product = static_cast<uint16_t>(d[10] | d[11] << 8);
                }
                break;
            case 4:  // interface
                if (length >= 9)
                {
                    UsbInterface item;
                    item.number = d[2];
                    item.alternate = d[3];
                    item.cls = d[5];
                    item.subclass = d[6];
                    item.protocol = d[7];
                    out.interfaces.push_back(item);
                }
                break;
            case 5:  // endpoint
                if (length >= 7 && !out.interfaces.empty())
                {
                    UsbEndpoint endpoint;
                    endpoint.address = d[2];
                    endpoint.attributes = d[3];
                    endpoint.maxPacket = static_cast<uint16_t>(d[4] | d[5] << 8);
                    out.interfaces.back().endpoints.push_back(endpoint);
                }
                break;
            case 0x30:  // SuperSpeed endpoint companion
                if (length >= 4 && !out.interfaces.empty() && !out.interfaces.back().endpoints.empty())
                {
                    auto& endpoint = out.interfaces.back().endpoints.back();
                    endpoint.burst = d[2];
                    endpoint.mult = d[3] & 3;
                }
                break;
            case 0x24:  // class-specific interface
                if (!out.interfaces.empty())
                    out.interfaces.back().classSpecific.emplace_back(d, d + length);
                break;
            default:
                break;
        }
        at += length;
    }
    return out;
}

// ---------------------------------------------------------------- connection

UsbConnection::UsbConnection(std::string key, std::string name, std::string kind, int fd, std::vector<uint8_t> descriptors)
    : key(std::move(key))
    , name(std::move(name))
    , kind(std::move(kind))
    , description(UsbDescription::parse(descriptors))
    , descriptor(dup(fd))
{
}

UsbConnection::~UsbConnection()
{
    if (descriptor >= 0)
        close(descriptor);
}

int UsbConnection::control(uint8_t requestType, uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t length, int timeoutMs)
{
    usbdevfs_ctrltransfer transfer{};
    transfer.bRequestType = requestType;
    transfer.bRequest = request;
    transfer.wValue = value;
    transfer.wIndex = index;
    transfer.wLength = length;
    transfer.timeout = static_cast<uint32_t>(timeoutMs);
    transfer.data = data;
    return ioctl(descriptor, USBDEVFS_CONTROL, &transfer);
}

int UsbConnection::transfer(uint8_t endpoint, uint8_t* data, size_t length, int timeoutMs)
{
    usbdevfs_bulktransfer bulk{};
    bulk.ep = endpoint;
    bulk.len = static_cast<unsigned>(length);
    bulk.timeout = static_cast<unsigned>(timeoutMs);
    bulk.data = data;
    const int result = ioctl(descriptor, USBDEVFS_BULK, &bulk);
    if (result < 0)
        return errno == ETIMEDOUT ? 0 : -1;
    return result;
}

bool UsbConnection::setInterface(unsigned number, unsigned alternate)
{
    usbdevfs_setinterface setting{number, alternate};
    return ioctl(descriptor, USBDEVFS_SETINTERFACE, &setting) == 0;
}

bool UsbConnection::readIsochronousImpl(uint8_t endpoint,
                                        size_t packetBytes,
                                        void (*packet)(void*, const uint8_t*, size_t),
                                        void* packetContext,
                                        bool (*keepGoing)(void*),
                                        void* keepGoingContext,
                                        std::string& error)
{
    // Eight transfers of 32 packets each stay in flight, so the camera never
    // waits for this thread.
    constexpr int kTransfers = 8;
    constexpr int kPackets = 32;
    struct Transfer
    {
        usbdevfs_urb* urb = nullptr;
        std::vector<uint8_t> buffer;
        bool pending = false;
    };
    std::vector<Transfer> transfers(kTransfers);
    const auto submit = [&](Transfer& t)
    {
        usbdevfs_urb* urb = t.urb;
        urb->type = USBDEVFS_URB_TYPE_ISO;
        urb->endpoint = endpoint;
        urb->flags = USBDEVFS_URB_ISO_ASAP;
        urb->buffer = t.buffer.data();
        urb->buffer_length = static_cast<int>(t.buffer.size());
        urb->number_of_packets = kPackets;
        urb->usercontext = &t;
        for (int i = 0; i < kPackets; i++)
        {
            urb->iso_frame_desc[i].length = static_cast<unsigned>(packetBytes);
            urb->iso_frame_desc[i].actual_length = 0;
            urb->iso_frame_desc[i].status = 0;
        }
        t.pending = ioctl(descriptor, USBDEVFS_SUBMITURB, urb) == 0;
        return t.pending;
    };

    bool ok = true;
    for (auto& t : transfers)
    {
        t.urb = static_cast<usbdevfs_urb*>(calloc(1, sizeof(usbdevfs_urb) + kPackets * sizeof(usbdevfs_iso_packet_desc)));
        t.buffer.resize(packetBytes * kPackets);
        if (!submit(t))
        {
            error = std::string("the camera's stream could not be started: ") + strerror(errno);
            ok = false;
            break;
        }
    }

    while (ok && keepGoing(keepGoingContext))
    {
        pollfd waiting{descriptor, POLLOUT, 0};
        if (poll(&waiting, 1, 100) < 0)
            continue;
        if (waiting.revents & (POLLERR | POLLHUP))
        {
            error = "the camera was disconnected";
            ok = false;
            break;
        }
        usbdevfs_urb* reaped = nullptr;
        while (ioctl(descriptor, USBDEVFS_REAPURBNDELAY, &reaped) == 0 && reaped)
        {
            auto* t = static_cast<Transfer*>(reaped->usercontext);
            t->pending = false;
            if (reaped->status == -ENODEV || reaped->status == -ESHUTDOWN)
            {
                error = "the camera was disconnected";
                ok = false;
                break;
            }
            for (int i = 0; i < reaped->number_of_packets; i++)
            {
                const auto& frame = reaped->iso_frame_desc[i];
                if (frame.status == 0 && frame.actual_length > 0)
                    packet(packetContext, t->buffer.data() + static_cast<size_t>(i) * packetBytes, frame.actual_length);
            }
            if (!submit(*t))
            {
                error = std::string("the camera's stream stopped: ") + strerror(errno);
                ok = false;
                break;
            }
        }
        if (errno == ENODEV)
        {
            error = "the camera was disconnected";
            ok = false;
        }
    }

    // Cancels the transfers still in flight and waits for them, briefly.
    for (auto& t : transfers)
        if (t.pending)
            ioctl(descriptor, USBDEVFS_DISCARDURB, t.urb);
    for (int tries = 0; tries < 50 && std::any_of(transfers.begin(), transfers.end(), [](const Transfer& t) { return t.pending; }); tries++)
    {
        usbdevfs_urb* reaped = nullptr;
        if (ioctl(descriptor, USBDEVFS_REAPURBNDELAY, &reaped) == 0 && reaped)
            static_cast<Transfer*>(reaped->usercontext)->pending = false;
        else
            usleep(10'000);
    }
    for (auto& t : transfers)
        if (!t.pending)
            free(t.urb);
    return ok;
}

// ---------------------------------------------------------------- registry

namespace
{

std::mutex registryLock;
std::vector<std::shared_ptr<UsbConnection>> registry;

}  // namespace

std::vector<std::shared_ptr<UsbConnection>> UsbHost::connections()
{
    std::lock_guard guard(registryLock);
    return registry;
}

void UsbHost::add(std::shared_ptr<UsbConnection> connection)
{
    std::lock_guard guard(registryLock);
    registry.erase(std::remove_if(registry.begin(), registry.end(), [&](const auto& c) { return c->key == connection->key; }), registry.end());
    registry.push_back(std::move(connection));
}

void UsbHost::remove(const std::string& key)
{
    std::lock_guard guard(registryLock);
    registry.erase(std::remove_if(registry.begin(), registry.end(), [&](const auto& c) { return c->key == key; }), registry.end());
}

// ---------------------------------------------------------------- MJPEG

namespace
{

/// The standard Huffman tables of JPEG (ITU T.81, annex K.3), as one DHT
/// segment, which MJPEG cameras may leave out.
const uint8_t kStandardTables[] = {
    0xFF, 0xC4, 0x01, 0xA2,
    // DC luminance
    0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    // DC chrominance
    0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    // AC luminance
    0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7D,
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5,
    0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
    0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA,
    // AC chrominance
    0x11, 0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77,
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0,
    0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
    0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5,
    0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
    0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
    0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA,
};

}  // namespace

std::vector<uint8_t> completeMjpeg(const uint8_t* data, size_t length)
{
    // Walk the markers up to the start of the scan; with a DHT among them the
    // picture is complete already.
    size_t at = 2;
    while (at + 4 <= length && data[at] == 0xFF)
    {
        const uint8_t marker = data[at + 1];
        if (marker == 0xC4)
            return {data, data + length};
        if (marker == 0xDA)
        {
            std::vector<uint8_t> out;
            out.reserve(length + sizeof(kStandardTables));
            out.insert(out.end(), data, data + at);
            out.insert(out.end(), std::begin(kStandardTables), std::end(kStandardTables));
            out.insert(out.end(), data + at, data + length);
            return out;
        }
        at += 2 + static_cast<size_t>(data[at + 2] << 8 | data[at + 3]);
    }
    return {data, data + length};
}

// ---------------------------------------------------------------- uncompressed pictures

std::optional<PictureLayout> uncompressedLayout(const uint8_t* guid)
{
    // The GUID of an uncompressed format starts with its four-character code.
    const std::string code(reinterpret_cast<const char*>(guid), 4);
    if (code == "YUY2" || code == "YUYV")
        return PictureLayout::Yuy2;
    if (code == "UYVY")
        return PictureLayout::Uyvy;
    if (code == "NV12")
        return PictureLayout::Nv12;
    if (code == "I420" || code == "IYUV")
        return PictureLayout::I420;
    if (code == "YV12")
        return PictureLayout::Yv12;
    if (code == "Y800" || code == "GREY" || code == "Y8  ")
        return PictureLayout::Gray;
    return std::nullopt;
}

const char* layoutName(PictureLayout layout)
{
    switch (layout)
    {
        case PictureLayout::Mjpeg: return "MJPEG";
        case PictureLayout::Yuy2: return "YUY2";
        case PictureLayout::Uyvy: return "UYVY";
        case PictureLayout::Nv12: return "NV12";
        case PictureLayout::I420: return "I420";
        case PictureLayout::Yv12: return "YV12";
        case PictureLayout::Gray: return "Y800";
    }
    return "";
}

size_t uncompressedBytes(PictureLayout layout, int width, int height)
{
    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    switch (layout)
    {
        case PictureLayout::Yuy2:
        case PictureLayout::Uyvy: return pixels * 2;
        case PictureLayout::Nv12:
        case PictureLayout::I420:
        case PictureLayout::Yv12: return pixels * 3 / 2;
        case PictureLayout::Gray: return pixels;
        case PictureLayout::Mjpeg: return 0;
    }
    return 0;
}

namespace
{

/// The order of the coefficients in the stream: the natural index of each.
const uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                             41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                             30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/// The example quantization tables of ITU T.81, annex K.1, in natural order.
const uint8_t kLuminanceQuality[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                                       14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                                       18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                                       49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
const uint8_t kChrominanceQuality[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
                                         99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                         99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

/// A Huffman table as codes and their lengths, for each symbol.
struct HuffmanCode
{
    uint16_t code[256] = {};
    uint8_t length[256] = {};
};

/// The four standard tables, read from the DHT segment above: DC and AC of
/// luminance and of chrominance.
struct StandardCodes
{
    HuffmanCode dcLuminance, dcChrominance, acLuminance, acChrominance;

    StandardCodes()
    {
        size_t at = 4;
        while (at + 17 <= sizeof(kStandardTables))
        {
            const uint8_t table = kStandardTables[at];
            const uint8_t* bits = kStandardTables + at + 1;
            const uint8_t* values = bits + 16;
            HuffmanCode& out = table == 0x00 ? dcLuminance : table == 0x01 ? dcChrominance : table == 0x10 ? acLuminance : acChrominance;
            uint16_t code = 0;
            size_t count = 0;
            for (int length = 1; length <= 16; length++)
            {
                for (int i = 0; i < bits[length - 1]; i++, count++, code++)
                {
                    out.code[values[count]] = code;
                    out.length[values[count]] = static_cast<uint8_t>(length);
                }
                code = static_cast<uint16_t>(code << 1);
            }
            at += 17 + count;
        }
    }
};

/// The entropy-coded data, with a zero byte stuffed after each 0xFF.
class BitWriter
{
public:
    explicit BitWriter(std::vector<uint8_t>& out)
        : out(out)
    {
    }

    void write(uint32_t value, int count)
    {
        for (int i = count - 1; i >= 0; i--)
        {
            byte = static_cast<uint8_t>(byte << 1 | (value >> i & 1));
            if (++filled == 8)
                flushByte();
        }
    }

    void finish()
    {
        // The last byte is padded with ones.
        while (filled != 0)
            write(1, 1);
    }

private:
    void flushByte()
    {
        out.push_back(byte);
        if (byte == 0xFF)
            out.push_back(0);
        byte = 0;
        filled = 0;
    }

    std::vector<uint8_t>& out;
    uint8_t byte = 0;
    int filled = 0;
};

/// The bits a value needs, its category.
int category(int value)
{
    int magnitude = value < 0 ? -value : value;
    int bits = 0;
    while (magnitude)
    {
        bits++;
        magnitude >>= 1;
    }
    return bits;
}

/// The cosines of the forward DCT, with its scale factors folded in.
struct DctTable
{
    float c[8][8];

    DctTable()
    {
        for (int u = 0; u < 8; u++)
            for (int x = 0; x < 8; x++)
                c[u][x] = static_cast<float>((u == 0 ? std::sqrt(0.5) : 1.0) * 0.5 * std::cos((2 * x + 1) * u * M_PI / 16));
    }
};

/// Encodes one 8 × 8 block of samples into the stream.
void encodeBlock(BitWriter& bits,
                 const float (&samples)[64],
                 const float (&divisors)[64],
                 const HuffmanCode& dc,
                 const HuffmanCode& ac,
                 int& previousDc)
{
    static const DctTable table;
    float rows[64];
    for (int y = 0; y < 8; y++)
        for (int u = 0; u < 8; u++)
        {
            float sum = 0;
            for (int x = 0; x < 8; x++)
                sum += table.c[u][x] * samples[y * 8 + x];
            rows[y * 8 + u] = sum;
        }
    int quantized[64];
    for (int v = 0; v < 8; v++)
        for (int u = 0; u < 8; u++)
        {
            float sum = 0;
            for (int y = 0; y < 8; y++)
                sum += table.c[v][y] * rows[y * 8 + u];
            quantized[v * 8 + u] = static_cast<int>(std::lround(sum / divisors[v * 8 + u]));
        }

    const int difference = quantized[0] - previousDc;
    previousDc = quantized[0];
    int size = category(difference);
    bits.write(dc.code[size], dc.length[size]);
    if (size)
        bits.write(static_cast<uint32_t>(difference < 0 ? difference - 1 : difference) & ((1u << size) - 1), size);

    int run = 0;
    for (int k = 1; k < 64; k++)
    {
        const int value = quantized[kZigzag[k]];
        if (value == 0)
        {
            run++;
            continue;
        }
        while (run > 15)
        {
            bits.write(ac.code[0xF0], ac.length[0xF0]);
            run -= 16;
        }
        size = category(value);
        const int symbol = run << 4 | size;
        bits.write(ac.code[symbol], ac.length[symbol]);
        bits.write(static_cast<uint32_t>(value < 0 ? value - 1 : value) & ((1u << size) - 1), size);
        run = 0;
    }
    if (run > 0)
        bits.write(ac.code[0x00], ac.length[0x00]);
}

void put16(std::vector<uint8_t>& out, size_t value)
{
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

}  // namespace

std::vector<uint8_t> encodeJpeg(PictureLayout layout, const uint8_t* data, int width, int height, int quality)
{
    // The picture as three planes, the chroma at half size in both
    // directions; a layout with chroma on every row has two rows averaged.
    const int cw = (width + 1) / 2;
    const int ch = (height + 1) / 2;
    std::vector<uint8_t> luma(static_cast<size_t>(width) * height);
    std::vector<uint8_t> cb(static_cast<size_t>(cw) * ch, 128);
    std::vector<uint8_t> cr(static_cast<size_t>(cw) * ch, 128);
    const size_t pixels = static_cast<size_t>(width) * height;
    switch (layout)
    {
        case PictureLayout::Yuy2:
        case PictureLayout::Uyvy:
        {
            const int y0 = layout == PictureLayout::Yuy2 ? 0 : 1;
            const int u = layout == PictureLayout::Yuy2 ? 1 : 0;
            const int v = layout == PictureLayout::Yuy2 ? 3 : 2;
            const size_t stride = static_cast<size_t>(width) * 2;
            for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++)
                    luma[static_cast<size_t>(y) * width + x] = data[y * stride + static_cast<size_t>(x) * 2 + y0];
            for (int y = 0; y < ch; y++)
            {
                const uint8_t* a = data + static_cast<size_t>(2 * y) * stride;
                const uint8_t* b = data + static_cast<size_t>(std::min(2 * y + 1, height - 1)) * stride;
                for (int x = 0; x < cw && 4 * x + 3 < static_cast<int>(stride); x++)
                {
                    cb[static_cast<size_t>(y) * cw + x] = static_cast<uint8_t>((a[4 * x + u] + b[4 * x + u] + 1) / 2);
                    cr[static_cast<size_t>(y) * cw + x] = static_cast<uint8_t>((a[4 * x + v] + b[4 * x + v] + 1) / 2);
                }
            }
            break;
        }
        case PictureLayout::Nv12:
        {
            std::memcpy(luma.data(), data, pixels);
            const uint8_t* uv = data + pixels;
            for (int y = 0; y < height / 2; y++)
                for (int x = 0; x < width / 2; x++)
                {
                    cb[static_cast<size_t>(y) * cw + x] = uv[static_cast<size_t>(y) * width + 2 * x];
                    cr[static_cast<size_t>(y) * cw + x] = uv[static_cast<size_t>(y) * width + 2 * x + 1];
                }
            break;
        }
        case PictureLayout::I420:
        case PictureLayout::Yv12:
        {
            std::memcpy(luma.data(), data, pixels);
            const size_t quarter = static_cast<size_t>(width / 2) * (height / 2);
            const uint8_t* first = data + pixels;
            const uint8_t* u = layout == PictureLayout::I420 ? first : first + quarter;
            const uint8_t* v = layout == PictureLayout::I420 ? first + quarter : first;
            for (int y = 0; y < height / 2; y++)
                for (int x = 0; x < width / 2; x++)
                {
                    cb[static_cast<size_t>(y) * cw + x] = u[static_cast<size_t>(y) * (width / 2) + x];
                    cr[static_cast<size_t>(y) * cw + x] = v[static_cast<size_t>(y) * (width / 2) + x];
                }
            break;
        }
        case PictureLayout::Gray:
            std::memcpy(luma.data(), data, pixels);
            break;
        case PictureLayout::Mjpeg:
            return {};
    }

    // The quantization tables at the quality, as the IJG scales them.
    quality = std::clamp(quality, 1, 100);
    const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    uint8_t tables[2][64];
    float divisors[2][64];
    for (int i = 0; i < 64; i++)
    {
        tables[0][i] = static_cast<uint8_t>(std::clamp((kLuminanceQuality[i] * scale + 50) / 100, 1, 255));
        tables[1][i] = static_cast<uint8_t>(std::clamp((kChrominanceQuality[i] * scale + 50) / 100, 1, 255));
        divisors[0][i] = tables[0][i];
        divisors[1][i] = tables[1][i];
    }

    std::vector<uint8_t> out;
    out.reserve(pixels / 4 + 1024);
    const uint8_t start[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
    out.insert(out.end(), std::begin(start), std::end(start));
    out.insert(out.end(), {0xFF, 0xDB});
    put16(out, 2 + 2 * 65);
    for (int t = 0; t < 2; t++)
    {
        out.push_back(static_cast<uint8_t>(t));
        for (int k = 0; k < 64; k++)
            out.push_back(tables[t][kZigzag[k]]);
    }
    out.insert(out.end(), {0xFF, 0xC0});
    put16(out, 17);
    out.push_back(8);
    put16(out, static_cast<size_t>(height));
    put16(out, static_cast<size_t>(width));
    out.insert(out.end(), {3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1});
    out.insert(out.end(), std::begin(kStandardTables), std::end(kStandardTables));
    out.insert(out.end(), {0xFF, 0xDA, 0x00, 0x0C, 3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0});

    static const StandardCodes codes;
    BitWriter bits(out);
    int dcY = 0;
    int dcCb = 0;
    int dcCr = 0;
    float block[64];
    // Samples beyond the edge repeat the last row or column.
    const auto load = [&block](const std::vector<uint8_t>& plane, int planeWidth, int planeHeight, int left, int top)
    {
        for (int y = 0; y < 8; y++)
        {
            const size_t row = static_cast<size_t>(std::min(top + y, planeHeight - 1)) * planeWidth;
            for (int x = 0; x < 8; x++)
                block[y * 8 + x] = static_cast<float>(plane[row + std::min(left + x, planeWidth - 1)]) - 128.0f;
        }
    };
    for (int top = 0; top < height; top += 16)
        for (int left = 0; left < width; left += 16)
        {
            for (int part = 0; part < 4; part++)
            {
                load(luma, width, height, left + (part & 1) * 8, top + (part >> 1) * 8);
                encodeBlock(bits, block, divisors[0], codes.dcLuminance, codes.acLuminance, dcY);
            }
            load(cb, cw, ch, left / 2, top / 2);
            encodeBlock(bits, block, divisors[1], codes.dcChrominance, codes.acChrominance, dcCb);
            load(cr, cw, ch, left / 2, top / 2);
            encodeBlock(bits, block, divisors[1], codes.dcChrominance, codes.acChrominance, dcCr);
        }
    bits.finish();
    out.insert(out.end(), {0xFF, 0xD9});
    return out;
}

}  // namespace daq::modules::android_device

using daq::modules::android_device::UsbConnection;
using daq::modules::android_device::UsbHost;

namespace
{

std::string jniText(JNIEnv* env, jstring value)
{
    if (!value)
        return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string out = chars ? chars : "";
    env->ReleaseStringUTFChars(value, chars);
    return out;
}

}  // namespace

// The app's UsbReader (io.opendaq.androiddevice.UsbReader) hands over the USB
// devices it opened, and takes them back when they are disconnected.

extern "C" JNIEXPORT void JNICALL Java_io_opendaq_androiddevice_UsbReader_opened(JNIEnv* env,
                                                                                jclass,
                                                                                jstring key,
                                                                                jstring name,
                                                                                jstring kind,
                                                                                jint fd,
                                                                                jbyteArray descriptors)
{
    const jsize length = env->GetArrayLength(descriptors);
    std::vector<uint8_t> raw(static_cast<size_t>(length));
    env->GetByteArrayRegion(descriptors, 0, length, reinterpret_cast<jbyte*>(raw.data()));
    UsbHost::add(std::make_shared<UsbConnection>(jniText(env, key), jniText(env, name), jniText(env, kind), fd, std::move(raw)));
}

extern "C" JNIEXPORT void JNICALL Java_io_opendaq_androiddevice_UsbReader_closed(JNIEnv* env, jclass, jstring key)
{
    UsbHost::remove(jniText(env, key));
}
