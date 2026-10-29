#include "sensor_channels.h"

#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCaptureRequest.h>
#include <media/NdkImageReader.h>

#include <climits>
#include <cstdlib>

namespace daq::modules::android_device
{

namespace
{

struct Size
{
    int width;
    int height;
};

/// The resolutions on offer. The camera's nearest supported JPEG size is used.
const std::vector<Size> kResolutions{{320, 240}, {640, 480}, {1280, 720}};

/// Everything the camera needs open, closed in reverse order however
/// acquire() leaves.
struct CameraSession
{
    ACameraManager* manager = nullptr;
    ACameraDevice* device = nullptr;
    AImageReader* reader = nullptr;
    ACaptureSessionOutputContainer* outputs = nullptr;
    ACaptureSessionOutput* output = nullptr;
    ACameraOutputTarget* target = nullptr;
    ACaptureRequest* request = nullptr;
    ACameraCaptureSession* session = nullptr;

    /// Set by the camera's callbacks when it disconnects or fails.
    std::atomic<int> lost{0};

    ~CameraSession()
    {
        if (session)
        {
            ACameraCaptureSession_stopRepeating(session);
            ACameraCaptureSession_close(session);
        }
        if (request)
            ACaptureRequest_free(request);
        if (target)
            ACameraOutputTarget_free(target);
        if (outputs)
            ACaptureSessionOutputContainer_free(outputs);
        if (output)
            ACaptureSessionOutput_free(output);
        if (device)
            ACameraDevice_close(device);
        if (reader)
            AImageReader_delete(reader);
        if (manager)
            ACameraManager_delete(manager);
    }
};

void check(camera_status_t status, const std::string& what)
{
    if (status == ACAMERA_ERROR_PERMISSION_DENIED)
        throw std::runtime_error("The app may not use the camera. Allow it in the phone's settings.");
    if (status == ACAMERA_ERROR_CAMERA_IN_USE || status == ACAMERA_ERROR_MAX_CAMERA_IN_USE)
        throw std::runtime_error("Another app is using the camera.");
    if (status != ACAMERA_OK)
        throw std::runtime_error(what + " (camera error " + std::to_string(status) + ")");
}

/// The id of the phone's first camera that faces the given way, and its
/// characteristics, or an empty id.
std::string findCamera(ACameraManager* manager, acamera_metadata_enum_android_lens_facing_t facing, ACameraMetadata** characteristics)
{
    ACameraIdList* ids = nullptr;
    if (ACameraManager_getCameraIdList(manager, &ids) != ACAMERA_OK || !ids)
        return {};
    std::string found;
    for (int i = 0; i < ids->numCameras && found.empty(); i++)
    {
        ACameraMetadata* metadata = nullptr;
        if (ACameraManager_getCameraCharacteristics(manager, ids->cameraIds[i], &metadata) != ACAMERA_OK)
            continue;
        ACameraMetadata_const_entry entry{};
        if (ACameraMetadata_getConstEntry(metadata, ACAMERA_LENS_FACING, &entry) == ACAMERA_OK && entry.count &&
            entry.data.u8[0] == facing)
        {
            found = ids->cameraIds[i];
            *characteristics = metadata;
        }
        else
        {
            ACameraMetadata_free(metadata);
        }
    }
    ACameraManager_deleteCameraIdList(ids);
    return found;
}

/// The camera's supported JPEG size nearest to the requested one.
Size nearestJpegSize(const ACameraMetadata* characteristics, Size wanted)
{
    ACameraMetadata_const_entry entry{};
    if (ACameraMetadata_getConstEntry(characteristics, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &entry) != ACAMERA_OK)
        return wanted;
    Size best = wanted;
    long bestDistance = LONG_MAX;
    // Entries are (format, width, height, input) quadruples.
    for (uint32_t i = 0; i + 3 < entry.count; i += 4)
    {
        if (entry.data.i32[i] != AIMAGE_FORMAT_JPEG || entry.data.i32[i + 3] != 0)
            continue;
        const Size size{entry.data.i32[i + 1], entry.data.i32[i + 2]};
        const long distance = std::labs(static_cast<long>(size.width) * size.height - static_cast<long>(wanted.width) * wanted.height) +
                              std::labs(static_cast<long>(size.width) - wanted.width) * 1000;
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = size;
        }
    }
    return best;
}

/// The length of the JPEG in a camera buffer. The buffer has the camera's
/// maximum JPEG size; the camera writes the picture's real size in a trailer
/// at the buffer's end (camera3_jpeg_blob: the id 0x00FF, then the size). If
/// the trailer is missing, the picture ends at its last end-of-image marker.
size_t jpegLength(const uint8_t* data, size_t length)
{
    if (length >= 8)
    {
        uint16_t id = 0;
        uint32_t size = 0;
        std::memcpy(&id, data + length - 8, sizeof(id));
        std::memcpy(&size, data + length - 4, sizeof(size));
        if (id == 0x00FF && size > 0 && size <= length - 8)
            return size;
    }
    for (size_t i = length; i >= 2; i--)
        if (data[i - 2] == 0xFF && data[i - 1] == 0xD9)
            return i;
    return length;
}

/// A picture's stamp, from the camera's own clock, as a boot clock stamp. A
/// camera stamps with the boot clock when it says so, and most others with
/// the monotonic clock; a stamp that matches neither is replaced by the time
/// of arrival.
int64_t bootStamp(int64_t stamp, bool realtimeSource)
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int64_t monotonic = static_cast<int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
    const int64_t boot = PhoneClock::bootNanoseconds();
    const auto near = [](int64_t a, int64_t b) { return std::llabs(a - b) < 1'000'000'000; };
    if (realtimeSource || near(stamp, boot))
        return near(stamp, boot) ? stamp : boot;
    if (near(stamp, monotonic))
        return stamp + (boot - monotonic);
    return boot;
}

}  // namespace

CameraChannel::CameraChannel(const ContextPtr& ctx,
                             const ComponentPtr& parent,
                             const StringPtr& localId,
                             std::shared_ptr<PhoneClock> clock,
                             bool startActive,
                             const std::string& externalId)
    : PhoneChannel(ctx,
                   parent,
                   localId,
                   externalId.empty() ? FunctionBlockType("PhoneCamera", "Camera", "The phone's camera")
                                      : FunctionBlockType("PhoneUsbCamera", "USB camera", "A camera connected to the phone's USB port"),
                   std::move(clock),
                   {1, 2, 5, 10, 15},
                   3,
                   startActive)
    , externalId(externalId)
{
    // The frame rate is the channel's SampleRate. The camera, the resolution and
    // the JPEG quality are its other settings. A USB camera is the one camera
    // of its channel.
    if (externalId.empty())
        objPtr.addProperty(SelectionPropertyBuilder("Camera", List<IString>("Back", "Front"), 0)
                               .setDescription("Which of the phone's cameras the channel reads.")
                               .build());
    auto sizes = List<IString>();
    for (const auto& size : kResolutions)
        sizes.pushBack(std::to_string(size.width) + " × " + std::to_string(size.height));
    objPtr.addProperty(SelectionPropertyBuilder("Resolution", sizes, 1)
                           .setDescription("The picture size. The camera's nearest supported JPEG size is used.")
                           .build());
    objPtr.addProperty(IntPropertyBuilder("Quality", 70)
                           .setMinValue(10)
                           .setMaxValue(100)
                           .setDescription("The JPEG quality, from 10 to 100. Higher is sharper and larger.")
                           .build());
    if (externalId.empty())
    {
        objPtr.getOnPropertyValueWrite("Camera") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
        {
            facing = static_cast<int>(static_cast<Int>(args.getValue()));
            version++;
        };
    }
    objPtr.getOnPropertyValueWrite("Resolution") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
    {
        resolution = static_cast<int>(static_cast<Int>(args.getValue()));
        version++;
    };
    objPtr.getOnPropertyValueWrite("Quality") += [this](PropertyObjectPtr&, PropertyValueEventArgsPtr& args)
    {
        quality = static_cast<int>(static_cast<Int>(args.getValue()));
        version++;
    };

    image = addValueSignal("Image", DataDescriptorBuilder().setSampleType(SampleType::Binary).setUnit(Unit("jpeg")).build());
    image.setDescription("One JPEG picture per sample, with the time its exposure started. The data descriptor's metadata holds its "
                         "width and height.");
    startAcquisition();
}

CameraChannel::~CameraChannel()
{
    stopAcquisition();
}

std::vector<std::string> CameraChannel::externalCameras()
{
    std::vector<std::string> found;
    // One manager for the whole run: the check repeats every two seconds.
    static ACameraManager* const manager = ACameraManager_create();
    ACameraIdList* ids = nullptr;
    if (ACameraManager_getCameraIdList(manager, &ids) == ACAMERA_OK && ids)
    {
        for (int i = 0; i < ids->numCameras; i++)
        {
            ACameraMetadata* metadata = nullptr;
            if (ACameraManager_getCameraCharacteristics(manager, ids->cameraIds[i], &metadata) != ACAMERA_OK)
                continue;
            ACameraMetadata_const_entry entry{};
            if (ACameraMetadata_getConstEntry(metadata, ACAMERA_LENS_FACING, &entry) == ACAMERA_OK && entry.count &&
                entry.data.u8[0] == ACAMERA_LENS_FACING_EXTERNAL)
                found.emplace_back(ids->cameraIds[i]);
            ACameraMetadata_free(metadata);
        }
        ACameraManager_deleteCameraIdList(ids);
    }
    return found;
}

bool CameraChannel::available()
{
    ACameraManager* manager = ACameraManager_create();
    ACameraIdList* ids = nullptr;
    const bool any = ACameraManager_getCameraIdList(manager, &ids) == ACAMERA_OK && ids && ids->numCameras > 0;
    if (ids)
        ACameraManager_deleteCameraIdList(ids);
    ACameraManager_delete(manager);
    return any;
}

void CameraChannel::acquire(int rate)
{
    const int startVersion = version;
    const bool front = facing == 1;
    const Size wanted = kResolutions[std::min<size_t>(static_cast<size_t>(std::max(0, resolution.load())), kResolutions.size() - 1)];

    CameraSession camera;
    camera.manager = ACameraManager_create();
    ACameraMetadata* characteristics = nullptr;
    std::string id;
    if (!externalId.empty())
    {
        if (ACameraManager_getCameraCharacteristics(camera.manager, externalId.c_str(), &characteristics) != ACAMERA_OK)
            throw std::runtime_error("The USB camera is no longer connected.");
        id = externalId;
    }
    else
    {
        id = findCamera(camera.manager, front ? ACAMERA_LENS_FACING_FRONT : ACAMERA_LENS_FACING_BACK, &characteristics);
        if (id.empty())
            throw std::runtime_error(front ? "The phone has no front camera." : "The phone has no back camera.");
    }
    const Size size = nearestJpegSize(characteristics, wanted);
    int32_t orientation = 0;
    ACameraMetadata_const_entry entry{};
    if (ACameraMetadata_getConstEntry(characteristics, ACAMERA_SENSOR_ORIENTATION, &entry) == ACAMERA_OK && entry.count)
        orientation = entry.data.i32[0];
    bool realtimeStamps = false;
    if (ACameraMetadata_getConstEntry(characteristics, ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE, &entry) == ACAMERA_OK && entry.count)
        realtimeStamps = entry.data.u8[0] == ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME;
    ACameraMetadata_free(characteristics);

    ACameraDevice_StateCallbacks deviceCallbacks{};
    deviceCallbacks.context = &camera;
    deviceCallbacks.onDisconnected = [](void* context, ACameraDevice*) { static_cast<CameraSession*>(context)->lost = 1; };
    deviceCallbacks.onError = [](void* context, ACameraDevice*, int) { static_cast<CameraSession*>(context)->lost = 2; };
    check(ACameraManager_openCamera(camera.manager, id.c_str(), &deviceCallbacks, &camera.device), "The camera could not be opened");

    // Four buffers, so the camera never waits for this thread.
    if (AImageReader_new(size.width, size.height, AIMAGE_FORMAT_JPEG, 4, &camera.reader) != AMEDIA_OK)
        throw std::runtime_error("The camera's picture buffer could not be created.");
    ANativeWindow* window = nullptr;
    AImageReader_getWindow(camera.reader, &window);

    check(ACaptureSessionOutputContainer_create(&camera.outputs), "The camera's outputs could not be set up");
    check(ACaptureSessionOutput_create(window, &camera.output), "The camera's output could not be set up");
    check(ACaptureSessionOutputContainer_add(camera.outputs, camera.output), "The camera's output could not be set up");
    check(ACameraOutputTarget_create(window, &camera.target), "The camera's target could not be set up");
    check(ACameraDevice_createCaptureRequest(camera.device, TEMPLATE_PREVIEW, &camera.request), "The camera's request could not be created");
    check(ACaptureRequest_addTarget(camera.request, camera.target), "The camera's request could not be set up");
    const uint8_t jpegQuality = static_cast<uint8_t>(std::clamp(quality.load(), 10, 100));
    ACaptureRequest_setEntry_u8(camera.request, ACAMERA_JPEG_QUALITY, 1, &jpegQuality);
    // Upright pictures with the phone held upright.
    ACaptureRequest_setEntry_i32(camera.request, ACAMERA_JPEG_ORIENTATION, 1, &orientation);

    ACameraCaptureSession_stateCallbacks sessionCallbacks{};
    sessionCallbacks.onClosed = [](void*, ACameraCaptureSession*) {};
    sessionCallbacks.onReady = [](void*, ACameraCaptureSession*) {};
    sessionCallbacks.onActive = [](void*, ACameraCaptureSession*) {};
    check(ACameraDevice_createCaptureSession(camera.device, camera.outputs, &sessionCallbacks, &camera.session),
          "The camera's session could not be started");
    check(ACameraCaptureSession_setRepeatingRequest(camera.session, nullptr, 1, &camera.request, nullptr),
          "The camera could not be started");

    // A rotation by 90 or 270 degrees swaps the picture's sides.
    const bool turned = orientation % 180 != 0;
    const int width = turned ? size.height : size.width;
    const int height = turned ? size.width : size.height;
    const int64_t period = 1'000'000 / rate;
    time.setDescriptor(timeDescriptor(1'000'000, period));
    image.setDescriptor(DataDescriptorBuilder()
                            .setSampleType(SampleType::Binary)
                            .setUnit(Unit("jpeg"))
                            .setName("Image")
                            .setMetadata(Dict<IString, IString>({{"width", std::to_string(width)},
                                                                 {"height", std::to_string(height)},
                                                                 {"camera", !externalId.empty() ? "usb" : front ? "front" : "back"}}))
                            .build());
    reportOk();

    bool delivered = false;
    bool warned = false;
    const auto opened = std::chrono::steady_clock::now();

    // The camera runs at its own rate. Every period, the newest picture is
    // sent and the older ones are dropped.
    int64_t next = clock->nowUnixMicroseconds();
    while (keepGoing(rate) && version == startVersion)
    {
        if (camera.lost)
            throw std::runtime_error(camera.lost == 1 ? "The camera was disconnected, for example by another app."
                                                      : "The camera reported an error.");
        const int64_t wait = next - clock->nowUnixMicroseconds();
        if (wait > 0)
            std::this_thread::sleep_for(std::chrono::microseconds(std::min<int64_t>(wait, 50'000)));
        if (clock->nowUnixMicroseconds() < next)
            continue;
        next += period;
        // After a stall, the next picture is due a period from now, not
        // immediately for every missed one.
        next = std::max(next, clock->nowUnixMicroseconds());

        AImage* picture = nullptr;
        if (AImageReader_acquireLatestImage(camera.reader, &picture) != AMEDIA_OK || !picture)
        {
            // A camera that runs but sends nothing, which a USB camera may do
            // when Android's support for it is incomplete, says so.
            if (!delivered && !warned && std::chrono::steady_clock::now() - opened > std::chrono::seconds(5))
            {
                warned = true;
                setComponentStatusWithMessage(ComponentStatus::Warning,
                                              "The camera is open but sends no pictures. A USB camera may need another resolution.");
            }
            continue;
        }
        if (!delivered)
            reportOk();
        delivered = true;
        uint8_t* data = nullptr;
        int length = 0;
        if (AImage_getPlaneData(picture, 0, &data, &length) == AMEDIA_OK && data && length > 0)
        {
            const size_t bytes = jpegLength(data, static_cast<size_t>(length));
            int64_t stamp = 0;
            AImage_getTimestamp(picture, &stamp);
            const int64_t at = clock->toUnixNanoseconds(bootStamp(stamp, realtimeStamps)) / 1000;
            const auto domainPacket = DataPacket(time.getDescriptor(), 1, at);
            const auto packet = BinaryDataPacket(domainPacket, image.getDescriptor(), bytes);
            std::memcpy(packet.getRawData(), data, bytes);
            time.sendPacket(domainPacket);
            image.sendPacket(packet);
        }
        AImage_delete(picture);
    }
}

}  // namespace daq::modules::android_device
