cmake_minimum_required(VERSION 3.24)
project(android_device_module LANGUAGES CXX)

# An openDAQ module with the phone's own sensors as the channels of one device
# (android://): the microphone, through AAudio; the accelerometer, the gyroscope,
# the proximity sensor and the light sensors, through the NDK sensor API; and
# the camera, through the NDK camera API. All need Android 8 (API 26). The
# device's clock takes its time from the phone, an NTP server or the phone's
# satellite receiver.
find_package(openDAQ REQUIRED)

add_library(android_device_module SHARED
    src/module.cpp
    src/phone_device.cpp
    src/phone_clock.cpp
    src/phone_channel.cpp
    src/microphone_channel.cpp
    src/motion_channel.cpp
    src/on_change_channels.cpp
    src/camera_channel.cpp
    src/phone_status.cpp
    src/system_channels.cpp
    src/usb_host.cpp
    src/usb_channels.cpp
)
target_compile_features(android_device_module PRIVATE cxx_std_17)
target_link_libraries(android_device_module PRIVATE daq::opendaq aaudio android camera2ndk mediandk log)
# The definitions openDAQ's own modules are built with. The last one lets
# calls to API 28 functions compile, guarded by __builtin_available.
target_compile_definitions(android_device_module PRIVATE
    BUILDING_SHARED_LIBRARY
    OPENDAQ_MODULE_EXPORTS
    OPENDAQ_TRACK_SHARED_LIB_OBJECT_COUNT
    __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__
)
set_target_properties(android_device_module PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    PREFIX "lib"
    SUFFIX ".module.so"
)
