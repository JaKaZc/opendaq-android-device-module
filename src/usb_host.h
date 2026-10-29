// USB devices on the phone's port, read by the module itself through Linux's
// usbfs: a camera (UVC), a serial device (CDC ACM) or another input device
// (HID). The app's UsbReader.kt asks the user for each device, opens it,
// claims its interfaces and hands its file descriptor and descriptors here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace daq::modules::android_device
{

struct UsbEndpoint
{
    uint8_t address = 0;
    uint8_t attributes = 0;
    uint16_t maxPacket = 0;
    // From a SuperSpeed endpoint companion descriptor.
    uint8_t burst = 0;
    uint8_t mult = 0;

    bool in() const { return (address & 0x80) != 0; }
    /// 0 control, 1 isochronous, 2 bulk, 3 interrupt.
    int type() const { return attributes & 3; }
    /// The bytes one isochronous packet carries, with high bandwidth and bursts.
    size_t packetBytes() const
    {
        return static_cast<size_t>(maxPacket & 0x7ff) * (((maxPacket >> 11) & 3) + 1) * (burst + 1u) * (mult + 1u);
    }
};

/// One alternate setting of an interface, with its class-specific
/// descriptors, which describe a camera's formats and frame sizes.
struct UsbInterface
{
    uint8_t number = 0;
    uint8_t alternate = 0;
    uint8_t cls = 0;
    uint8_t subclass = 0;
    uint8_t protocol = 0;
    std::vector<UsbEndpoint> endpoints;
    std::vector<std::vector<uint8_t>> classSpecific;
};

/// The device and active configuration descriptors, as Android's
/// UsbDeviceConnection.getRawDescriptors() returns them.
struct UsbDescription
{
    uint16_t vendor = 0;
    uint16_t product = 0;
    std::vector<UsbInterface> interfaces;

    static UsbDescription parse(const std::vector<uint8_t>& raw);
};

/// A device the app opened, owning a duplicate of its file descriptor.
class UsbConnection
{
public:
    UsbConnection(std::string key, std::string name, std::string kind, int fd, std::vector<uint8_t> descriptors);
    ~UsbConnection();

    UsbConnection(const UsbConnection&) = delete;
    UsbConnection& operator=(const UsbConnection&) = delete;

    const std::string key;       // Android's device name, such as /dev/bus/usb/001/002
    const std::string name;      // the product name
    const std::string kind;      // "camera", "serial" or "input"
    const UsbDescription description;

    /// A control transfer; the number of bytes moved, or -1.
    int control(uint8_t requestType, uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t length, int timeoutMs = 1000);
    /// A bulk or interrupt transfer; the number of bytes moved, 0 on a timeout,
    /// or -1 on an error.
    int transfer(uint8_t endpoint, uint8_t* data, size_t length, int timeoutMs);
    bool setInterface(unsigned number, unsigned alternate);

    /// Reads an isochronous endpoint with several transfers in flight, and
    /// hands each packet to `packet` until `keepGoing` turns false. Returns
    /// false with the reason when the device stops answering.
    template <typename Packet, typename KeepGoing>
    bool readIsochronous(uint8_t endpoint, size_t packetBytes, Packet&& packet, KeepGoing&& keepGoing, std::string& error);

    int fd() const { return descriptor; }

private:
    bool readIsochronousImpl(uint8_t endpoint,
                             size_t packetBytes,
                             void (*packet)(void*, const uint8_t*, size_t),
                             void* packetContext,
                             bool (*keepGoing)(void*),
                             void* keepGoingContext,
                             std::string& error);

    int descriptor;
};

template <typename Packet, typename KeepGoing>
bool UsbConnection::readIsochronous(uint8_t endpoint, size_t packetBytes, Packet&& packet, KeepGoing&& keepGoing, std::string& error)
{
    return readIsochronousImpl(
        endpoint,
        packetBytes,
        [](void* context, const uint8_t* data, size_t length) { (*static_cast<std::remove_reference_t<Packet>*>(context))(data, length); },
        const_cast<void*>(static_cast<const void*>(&packet)),
        [](void* context) { return static_cast<bool>((*static_cast<std::remove_reference_t<KeepGoing>*>(context))()); },
        const_cast<void*>(static_cast<const void*>(&keepGoing)),
        error);
}

/// The devices the app has opened, for the phone device to give channels.
class UsbHost
{
public:
    static std::vector<std::shared_ptr<UsbConnection>> connections();
    static void add(std::shared_ptr<UsbConnection> connection);
    static void remove(const std::string& key);
};

/// A JPEG from a camera's MJPEG stream with the standard Huffman tables
/// inserted where the camera left them out, as MJPEG allows; decoders need
/// them.
std::vector<uint8_t> completeMjpeg(const uint8_t* data, size_t length);

/// How a camera lays out the pixels of a picture: compressed by the camera
/// (MJPEG) or uncompressed in one of the common YUV layouts.
enum class PictureLayout
{
    Mjpeg,
    Yuy2,  // Y0 U Y1 V for each pair of pixels
    Uyvy,  // U Y0 V Y1
    Nv12,  // the Y plane, then U and V interleaved at half size
    I420,  // the Y plane, then the U and V planes at half size
    Yv12,  // the Y plane, then the V and U planes at half size
    Gray,  // the Y plane alone
};

/// The layout an uncompressed format's GUID names, or none for one this
/// module does not read.
std::optional<PictureLayout> uncompressedLayout(const uint8_t* guid);

/// A short name of the layout, for diagnostics.
const char* layoutName(PictureLayout layout);

/// The bytes of one uncompressed picture.
size_t uncompressedBytes(PictureLayout layout, int width, int height);

/// One uncompressed picture as a baseline JPEG with 4:2:0 chroma, at quality
/// 1 to 100 as the IJG scales the standard tables.
std::vector<uint8_t> encodeJpeg(PictureLayout layout, const uint8_t* data, int width, int height, int quality);

}  // namespace daq::modules::android_device
