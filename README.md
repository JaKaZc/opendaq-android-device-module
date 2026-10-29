# openDAQ Android Device Module

An [openDAQ](https://opendaq.com) module, **AndroidDeviceModule**, that turns an
Android phone into an openDAQ device. The phone is one device, with the
connection string `android://`, and each of its sensors is a channel of that
device: the microphone, the motion sensors, the light and proximity sensors,
the camera, the phone's own state and the devices on its USB port. Every
channel has its own sample rate, settings and acquisition thread, and every
sample carries the time it was measured, from a clock that can follow the
phone, an NTP server or the satellite receiver.

> **Android only.** The module runs only inside an app on an Android phone or
> tablet: Android 8 (API 26) or newer, arm64. It reads the hardware through
> Android's own interfaces (AAudio, the NDK sensor and camera APIs, USB host
> and JNI), so it does not build or run on Windows, macOS, Linux or a
> Raspberry Pi. A program on a computer uses the phone's sensors over the
> network instead; see [Using the phone from other projects](#using-the-phone-from-other-projects).

The module is C++17 against the openDAQ SDK and the Android NDK, plus three
small Kotlin classes for what Android tells only to Java. It does not serve
anything on the network by itself. An app loads it into its openDAQ instance,
and the app's openDAQ server makes the device reachable. The
[openDAQ Device Server](#the-opendaq-phone-projects) app does exactly that.

## The openDAQ phone projects

This repository is one of four that work together. Clone them side by side
into one folder, because the build scripts find each other there.

| Repository | What it is | Needs |
|---|---|---|
| **opendaq-android-device-module** (this one) | openDAQ module, **Android only**: the phone's sensors as one device, `android://` | openDAQ SDK, Android NDK |
| opendaq-universal-function-blocks | openDAQ module, UniversalFunctionBlocksModule: Universal renderer, Level, Spectrum, Vibration, Sound level meter and Dashboard | openDAQ SDK |
| opendaq-device-server | Android app that serves the phone as an openDAQ device on the network | both modules, Android 8 or newer, arm64 |
| opendaq-studio-desktop | openDAQ Studio, the desktop client for macOS and Windows | the function blocks module |

```
 Android phone: opendaq-device-server                 Computer: opendaq-studio-desktop
 ┌──────────────────────────────────────────┐         ┌──────────────────────────────────────────┐
 │ openDAQ instance                         │         │ openDAQ Studio                           │
 │  android:// device  <- this module       │   TCP   │  openDAQ instance, native client         │
 │  function blocks    <- universal         │   7420  │  UniversalFunctionBlocksModule, so       │
 │                        function blocks   │ ──────▶ │  the phone's blocks can be set up        │
 │  native streaming server, mDNS           │         │  NTP time server, UDP 12300              │
 └──────────────────────────────────────────┘         └──────────────────────────────────────────┘
```

All four are built against the same openDAQ SDK release, **v3.40.3**.
openDAQ's native protocol does not connect a client of one release to a server
of another, so the phone and the computer must use the same release.

## The device

The module has one device type, Android device (`AndroidDevice`), with the connection string
`android://`. The module creates one device at a time, because a second one
would compete for the microphone and the camera. Options in the connection
string, such as `android://?Microphone=true&Camera=false`, or the same names in
the device's configuration, decide which channels start active.

| Configuration | Default | Meaning |
|---|---|---|
| Microphone | true | The Microphone channel starts active |
| Camera | false | The Camera channel starts active. It is off by default, because a running camera keeps Android's privacy indicator on |
| Dashboard | false | The device adds a Dashboard function block, a web page of its main signals on port 8090. It needs UniversalFunctionBlocksModule |
| Name, LocalId, SerialNumber | empty | The device's name and identity, for an app that makes the phone the root device of its instance |

An app normally makes the device the root device of its instance
(`Instance::setRootDevice`), named after the phone. Other clients then see the
phone itself as the device, with the sensors as its channels directly inside
it. Devices and function blocks that a client adds sit in it too.

### Channels

A channel the phone lacks is left out.

| Channel | Sensor | Signals | Sample rate |
|---|---|---|---|
| Microphone | The microphone, through AAudio | `Audio`, Float32 from −1 to 1, or Pa when calibrated | 8000, 16 000, 44 100 or 48 000 Hz (default 48 000) |
| Accelerometer | Through the NDK sensor API | `X`, `Y`, `Z` and `Magnitude`, Float32 in m/s², gravity included | 25, 50, 100 or 200 Hz (default 100) |
| Gyroscope | Through the NDK sensor API | `X`, `Y`, `Z` and `Magnitude`, Float32 in rad/s | 25, 50, 100 or 200 Hz (default 100) |
| Magnetometer | Through the NDK sensor API | `X`, `Y`, `Z` and `Magnitude`, Float32 in µT | 25, 50, 100 or 200 Hz (default 100) |
| LinearAcceleration | Android's acceleration without gravity | `X`, `Y`, `Z` and `Magnitude`, Float32 in m/s² | 25, 50, 100 or 200 Hz (default 100) |
| Orientation | Android's rotation vector | `Azimuth`, `Pitch` and `Roll`, Float32 in degrees | 25, 50, 100 or 200 Hz (default 100) |
| Proximity | Through the NDK sensor API | `Near`: 1 when an object is near and 0 when not | 5, 10, 20 or 50 Hz (default 10) |
| Light | The ambient light sensor | `Illuminance`, Float32 in lux | 5, 10, 20 or 50 Hz (default 10) |
| InfraredLight | Samsung's infrared light stream, on Samsung phones only | `Field0`, `Field4`, `Field5` and `Field6`, the raw event fields | 5, 10, 20 or 50 Hz (default 10) |
| Pressure, Humidity, Temperature | The barometer, the humidity sensor and the ambient temperature sensor, where the phone has them | One signal each, in hPa, % and °C | 5, 10, 20 or 50 Hz (default 10) |
| Camera | A camera, through the NDK camera API | `Image`: sample type Binary, one JPEG picture per sample | 1, 2, 5, 10 or 15 pictures per second (default 10) |
| System | Android's state, reported by `PhoneStatus.kt` | `Cpu`, `CpuFrequency`, `MemoryUsed`, `MemoryAvailable`, `WifiSignal`, `WifiLinkSpeed`, `NetworkReceive`, `NetworkSend`, `BatteryLevel`, `BatteryVoltage`, `BatteryCurrent`, `BatteryPower`, `BatteryTemperature`, `Charging`, `ThermalHeadroom`, `StorageFree` | 1, 2, 5 or 10 Hz (default 2) |
| USB | The USB port, reported by `PhoneStatus.kt` | `ChargingPower`, `ChargingCurrent` and `Devices`, and the properties PowerSource, Charger and ConnectedDevices | 1, 2, 5 or 10 Hz (default 2) |

Devices on the USB port get channels of their own:

- **A USB camera.** It is read through Android where Android supports USB
  cameras. Otherwise the module reads it itself with the UVC protocol: MJPEG
  where the camera offers it, and otherwise uncompressed YUY2, UYVY, NV12,
  I420, YV12 or grayscale video, which the module compresses to JPEG. Its
  read-only **Diagnostics** property shows the format, the packet counts and
  the errors, which tells where a black picture comes from.
- **A USB microphone**, through AAudio.
- **A serial device** (CDC ACM), such as an Arduino: each number of a line
  becomes a signal.
- **An input device** (HID): the bytes of its reports.

Android asks the user once for each USB device. `UsbReader.kt` then opens it
and hands its file descriptor to the module.

### Calibration

- **Motion channels** have OffsetX, OffsetY, OffsetZ and Scale. Their **Tare**
  procedure averages one second at rest into the offsets, and
  ResetCalibration clears them.
- **Light and single-value channels** have Offset and Scale.
- **The microphone** has Sensitivity, the pressure in Pa at full scale. Its
  **Calibrate** procedure takes two seconds of a calibrator's tone at
  CalibrationLevel (94 dB by default) and sets the sensitivity. A calibrated
  microphone's `Audio` signal is in Pa, so a sound level meter reads true
  dB SPL.

### Operation modes and status

Idle stops every sensor, as the SDK's reference device does. Operation, the
default, and SafeOperation run the active channels. A channel whose Active
attribute is off stops its sensor. A sensor that cannot be opened, such as a
microphone without permission, sets its channel's status to Error with the
reason, and the channel tries again every second.

### Derived function blocks

When UniversalFunctionBlocksModule is loaded too, the device adds three of its
function blocks, already connected, so every client gets derived signals:

- **Sound level**: the sound level meter on the microphone, with the
  A-weighted Fast level, Leq, the maximum and octave bands.
- **Sound spectrum**: the spectrum of the microphone.
- **Vibration**: velocity RMS in mm/s and its ISO 10816 zone, from the
  accelerometer's three axes.

With the configuration option Dashboard, it also adds a **Dashboard** of the
main signals. Without the function blocks module, the device works the same
and leaves these blocks out.

## Time

Every signal counts time from the Unix epoch, as the SDK's reference device
does, so phone signals and signals from other devices share one time axis.
The microphone's ticks are its samples. The other channels' ticks are
microseconds.

- **Stamps.** Each sample's time is Android's stamp of when it was measured,
  not of when it arrived. All stamps use the boot clock (`CLOCK_BOOTTIME`), so
  the channels agree with each other to within the sensors' own latency.
- **One clock.** The device's clock turns boot clock stamps into Unix time by
  adding an offset. The device property **TimeSource** chooses where the
  offset comes from:

  | TimeSource | Where the time comes from | How exact |
  |---|---|---|
  | Phone clock (default) | Android's own time | Usually within 50 ms of UTC |
  | Network time | The NTP server in NtpServer, `time.android.com` by default, or `host:port` such as openDAQ Studio's own server | About half the shortest round trip; under a millisecond to openDAQ Studio on the same Wi-Fi |
  | GNSS time | The phone's satellite receiver, reported by `GnssTime.kt` | Microseconds with clock measurements, about 20 ms with fixes |

- **State.** The read-only properties TimeStatus, ClockOffset and
  ClockUncertainty show the clock's state. A small change of the offset is
  applied gradually, 1 ms per second, so the time stays continuous. A change
  above 10 ms is a step, after which the channels start their time axis again.
  While the clock does not follow its source, the device's status is Warning.
- **Synchronization component.** Its interfaces PhoneClock, NetworkTime and
  GnssTime are the three sources, and SynchronizationLocked is true while the
  clock follows the chosen one.
- **The phone as a time master.** With **ServeTime** on, the phone answers NTP
  on UDP port 12300 with its own time. A phone that follows GNSS can then give
  that time to phones that cannot see the sky. TimeServer shows the address.
- **openDAQ Studio as the time master.** openDAQ Studio on the computer
  answers NTP on UDP port 12300. When it connects a phone, it sets the phone's
  NtpServer to its own address and TimeSource to Network time, once, so all
  phones follow one clock over one network hop.

GNSS time needs the location permission, and Android gives the location to an
app only while it is in front or runs a foreground service of the location
type.

## How it works

- **Microphone.** The channel reads 20 ms blocks on its own thread with the
  voice-recognition input preset, which has no automatic gain or noise
  suppression. Every block follows the previous one without a gap. The
  microphone's clock and the phone's run at slightly different rates, so the
  channel compares each block's time with AAudio's stamp and catches up once
  the difference exceeds a millisecond.
- **Motion sensors.** Android delivers their events at uneven intervals, while
  openDAQ's linear time domain needs a fixed rate. The channels
  (`MotionChannel`) interpolate the events linearly onto a grid at the chosen
  rate. After a gap of more than half a second, the grid starts again at the
  next event instead of bridging the gap.
- **Proximity, light and similar sensors.** Android reports them only when
  their value changes. The channels (`OnChangeChannel`) hold the latest values
  and repeat them at the chosen rate, so the signal is a step curve. The grid
  lags the clock by 100 ms, so a late event still lands at its own time.
- **Camera.** The channel follows openDAQ's video convention: a signal of
  sample type `Binary` carries one encoded picture per sample, and its unit's
  symbol names the format, `jpeg`. The descriptor's metadata holds the
  picture's `width` and `height`. The camera hardware encodes the JPEGs, and
  the channel takes the newest one at the chosen rate, stamped with the start
  of its exposure. Properties choose the back or front camera, the resolution
  and the JPEG quality.
- **Why proximity is Near and not a distance.** On most phones Android's
  proximity sensor reports only near or far. The channel therefore reports
  `Near`, with Android's own rule for turning the screen off, and the last
  reported distance as the property ReportedDistance.
- **Changing a rate.** The property handler only records the new rate. The
  channel's own thread reopens the sensor and sets a new time descriptor, so
  readers see a new stream. The handler never waits for the thread.

### The Kotlin side

Android tells some things only to Java. Three classes in `android/` report
them to the module through JNI, and the app compiles them into its own code:

| Class | Reports |
|---|---|
| `io.opendaq.androiddevice.GnssTime` | The satellite receiver's time, while the device wants GNSS time |
| `io.opendaq.androiddevice.PhoneStatus` | CPU, memory, Wi-Fi, network traffic, battery, temperature, storage and the USB port, for the System and USB channels |
| `io.opendaq.androiddevice.UsbReader` | USB devices the module reads itself: it asks for permission, opens the device and hands over its file descriptor |

The native side of each is in the module (`phone_clock.cpp`,
`phone_status.cpp`, `usb_host.cpp`), so the app must load the module's library
before it starts these classes. openDAQ loads the same library, so both sides
share one copy.

## Using the phone from other projects

There are three ways to use the phone's sensors from another project.

**Over the network, from any openDAQ client.** This is the usual way, and it
works from any operating system and language that openDAQ supports: Python,
C++, C# and openDAQ Studio. The phone runs the openDAQ Device Server, which
loads this module and serves it over openDAQ's native protocol. The client
needs only the openDAQ SDK of the same release, 3.40.3, and nothing from this
repository:

```python
import opendaq   # pip install opendaq==3.40.3

instance = opendaq.Instance()
phone = instance.add_device("daq.nd://192.168.1.197")   # the address the app shows
signals = list(phone.signals_recursive)
for signal in signals:
    print(signal.global_id)

reader = opendaq.StreamReader(signals[0])
print(reader.read(100, 1000))   # up to 100 samples, waiting up to 1 s
```

```cpp
#include <opendaq/opendaq.h>

auto instance = daq::Instance();
auto phone = instance.addDevice("daq.nd://192.168.1.197");
for (const auto& signal : phone.getSignalsRecursive())
    std::cout << signal.getGlobalId() << std::endl;
```

The client gets the whole device: it reads the signals, changes sample rates,
switches channels on and off, calibrates, sets the time source and uses the
function blocks on the phone. Anyone on the same network can connect, because
the native protocol has no password.

**Inside another Android app.** An Android app in C++ or Kotlin builds this
module into itself, as described in
[Using the module in an app](#using-the-module-in-an-app). openDAQ publishes no
Python wheels for Android, so Python inside an Android app is not practical.

**Without openDAQ.** A client that does not speak openDAQ cannot read the
phone, because the Device Server serves only openDAQ's native protocol. A
browser can open the Dashboard's web page. openDAQ's OPC UA server module
could make the phone readable by OPC UA tools, but the Device Server does not
include it.

## Requirements

The module is built on a computer and runs on the phone.

| What | Version | Notes |
|---|---|---|
| A Mac or Linux computer, to build on | | The build script is a POSIX shell script |
| Android NDK | 28.2 (the version it is built and tested with) | Install it with Android Studio's SDK Manager. The script finds the newest NDK in the Android SDK, or `ANDROID_NDK` |
| CMake | 3.24 or newer | `brew install cmake` on macOS |
| Ninja | | `brew install ninja` on macOS |
| git | | To clone the openDAQ SDK |
| Network access | | On the first build, for the SDK and its dependencies |
| ccache | optional | Used when installed; repeated builds are much faster |

The module runs on Android 8 (API 26) or newer, arm64 only. Other ABIs, such
as 32-bit ARM or the x86_64 emulator images, are not built.

## Building

```bash
tool/build_android.sh
```

The script:

1. clones the openDAQ SDK into `build/openDAQ`, checks out `v3.40.3` and
   applies the patch in `patches/`,
2. builds the SDK's core for Android arm64, API 26, with `c++_shared`,
3. builds the module against it and writes the stripped library to
   `dist/android-arm64/libandroid_device_module.module.so`.

The first run takes about ten minutes. Later runs only rebuild what changed.

| Variable | Default | Meaning |
|---|---|---|
| `OPENDAQ_SOURCE` | `https://github.com/openDAQ/openDAQ.git` | Where to clone the SDK from; a local clone is faster |
| `SDK_REF` | `v3.40.3` | The SDK release |
| `ANDROID_NDK` | the newest NDK in the Android SDK | The NDK |
| `CMAKE`, `NINJA` | `cmake`, `ninja` | The tools, when they are not on the PATH |

### The Android patch

`patches/android-mutex-cast.patch` changes one line of the SDK. openDAQ builds
with hidden symbol visibility, and Android's libc++ compares `type_info` by
address. Each library therefore has its own copy of `MutexImpl`'s type
information, and a `dynamic_cast<MutexImpl*>` in a module returns null. The
patch replaces it with a `static_cast`, which is safe because the object is
always a `MutexImpl`. Every SDK build that loads this module on Android needs
it.

### Using the module in an app

An app builds the module from these sources against its own SDK build, as the
Device Server's `tool/build_native.sh` does, and then:

1. packs `libandroid_device_module.module.so` with the SDK's libraries in its
   `jniLibs/arm64-v8a`, uncompressed, so openDAQ can load it by name,
2. adds `android/` to its Kotlin or Java sources,
3. loads the module into its openDAQ instance and adds the device
   `android://`, usually as the root device,
4. asks for the permissions the channels need: `RECORD_AUDIO`, `CAMERA` and
   location for GNSS time.

The CMake target needs the compile definitions `BUILDING_SHARED_LIBRARY`,
`OPENDAQ_MODULE_EXPORTS`, `OPENDAQ_TRACK_SHARED_LIB_OBJECT_COUNT` and
`__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__`, which `CMakeLists.txt` sets.

## Layout

| Path | |
|---|---|
| `src/module.cpp` | The module and its device type |
| `src/phone_device.cpp`, `.h` | The device: channels, operation modes, time properties, the Synchronization component, derived function blocks |
| `src/phone_clock.cpp`, `.h` | The device's clock: phone time, NTP, GNSS, and the NTP server for ServeTime |
| `src/phone_channel.cpp` | What every channel shares: the thread, the rate, the Active attribute |
| `src/microphone_channel.cpp` | The microphone, through AAudio |
| `src/motion_channel.cpp` | The accelerometer, gyroscope, magnetometer, linear acceleration and orientation |
| `src/on_change_channels.cpp` | Proximity, light, infrared light, pressure, humidity and temperature |
| `src/camera_channel.cpp` | The camera, through the NDK camera API |
| `src/system_channels.cpp`, `src/phone_status.cpp` | The System and USB channels |
| `src/usb_host.cpp`, `src/usb_channels.cpp` | USB cameras (UVC), serial devices and input devices read by the module |
| `src/android_sensors.h`, `src/sensor_channels.h`, `src/common.h` | Shared helpers |
| `android/io/opendaq/androiddevice/` | The Kotlin classes `GnssTime`, `PhoneStatus` and `UsbReader` |
| `patches/` | The SDK change Android needs |
| `tool/build_android.sh`, `tool/prepare_sdk.sh` | The build |

## License

Apache License 2.0, the same as the openDAQ SDK. See [LICENSE](LICENSE).

This is an independent project built on the openDAQ SDK. openDAQ is a
trademark of its respective owners.
