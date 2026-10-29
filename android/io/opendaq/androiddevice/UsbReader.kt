package io.opendaq.androiddevice

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log

/// Hands the devices on the USB port that AndroidDeviceModule reads itself to
/// it: a camera (UVC) where Android does not offer USB cameras to apps, a
/// serial device (CDC ACM) such as an Arduino, and an input device (HID) other
/// than a keyboard or a mouse. Android asks the user once for each device;
/// this then opens it, claims its interfaces and gives the module its file
/// descriptor. Both openDAQ Studio for Android and the Device Server build
/// this file from the module's folder.
class UsbReader(context: Context) {
    companion object {
        private const val TAG = "UsbReader"
        private const val ACTION_PERMISSION = "io.opendaq.androiddevice.USB_PERMISSION"

        private val loaded: Boolean by lazy {
            try {
                System.loadLibrary("android_device_module.module")
                true
            } catch (error: UnsatisfiedLinkError) {
                Log.w(TAG, "The Android device module is not available: ${error.message}")
                false
            }
        }

        @JvmStatic private external fun opened(key: String, name: String, kind: String, fd: Int, descriptors: ByteArray)
        @JvmStatic private external fun closed(key: String)

        private fun isKeyboardOrMouse(item: UsbInterface) =
            item.interfaceSubclass == 1 && (item.interfaceProtocol == 1 || item.interfaceProtocol == 2)
    }

    private val context = context.applicationContext
    private val manager = this.context.getSystemService(UsbManager::class.java)
    private val handler = Handler(Looper.getMainLooper())
    private val open = mutableMapOf<String, Pair<UsbDeviceConnection, List<UsbInterface>>>()
    private val asked = mutableSetOf<String>()
    private var running = false

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            check()
        }
    }

    fun start() {
        if (running || manager == null || !loaded) return
        running = true
        val filter = IntentFilter(ACTION_PERMISSION).apply { addAction(UsbManager.ACTION_USB_DEVICE_DETACHED) }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            context.registerReceiver(receiver, filter)
        }
        poll()
    }

    fun stop() {
        if (!running) return
        running = false
        handler.removeCallbacksAndMessages(null)
        context.unregisterReceiver(receiver)
        for (key in open.keys.toList()) close(key)
    }

    private fun poll() {
        if (!running) return
        check()
        handler.postDelayed({ poll() }, 2000)
    }

    /// The kind of device the module reads, and the interfaces it needs.
    private fun kindOf(device: UsbDevice, androidCameras: Boolean): Pair<String, List<UsbInterface>>? {
        val interfaces = (0 until device.interfaceCount).map { device.getInterface(it) }
        val video = interfaces.filter { it.interfaceClass == UsbConstants.USB_CLASS_VIDEO }
        if (video.isNotEmpty() && !androidCameras) return "camera" to video
        val serial = interfaces.filter { it.interfaceClass == UsbConstants.USB_CLASS_COMM || it.interfaceClass == UsbConstants.USB_CLASS_CDC_DATA }
        if (serial.any { it.interfaceClass == UsbConstants.USB_CLASS_CDC_DATA }) return "serial" to serial
        val input = interfaces.filter { it.interfaceClass == UsbConstants.USB_CLASS_HID && !isKeyboardOrMouse(it) }
        if (input.isNotEmpty() && interfaces.none { it.interfaceClass == UsbConstants.USB_CLASS_HID && isKeyboardOrMouse(it) }) {
            return "input" to input
        }
        return null
    }

    /// Whether Android itself offers a USB camera to apps; the module then
    /// reads it through Android's camera API instead.
    private fun androidHasUsbCameras(): Boolean = try {
        val cameras = context.getSystemService(CameraManager::class.java)
        cameras?.cameraIdList?.any {
            cameras.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_EXTERNAL
        } ?: false
    } catch (error: Exception) {
        false
    }

    private fun check() {
        val usb = manager ?: return
        val devices = usb.deviceList.values
        for (key in open.keys.toList()) {
            if (devices.none { it.deviceName == key }) close(key)
        }
        asked.retainAll(devices.map { it.deviceName }.toSet())
        val androidCameras = androidHasUsbCameras()
        for (device in devices) {
            if (device.deviceName in open) continue
            val (kind, interfaces) = kindOf(device, androidCameras) ?: continue
            if (!usb.hasPermission(device)) {
                if (asked.add(device.deviceName)) requestPermission(usb, device)
                continue
            }
            val connection = usb.openDevice(device) ?: continue
            val claimed = interfaces.distinctBy { it.id }.filter { connection.claimInterface(it, true) }
            val name = try {
                device.productName
            } catch (error: SecurityException) {
                null
            } ?: when (kind) {
                "camera" -> "USB camera"
                "serial" -> "USB serial device"
                else -> "USB input device"
            }
            opened(device.deviceName, name, kind, connection.fileDescriptor, connection.rawDescriptors ?: ByteArray(0))
            open[device.deviceName] = connection to claimed
            Log.i(TAG, "Reading $name as a $kind device")
        }
    }

    private fun requestPermission(usb: UsbManager, device: UsbDevice) {
        val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0
        val intent = PendingIntent.getBroadcast(context, 0, Intent(ACTION_PERMISSION).setPackage(context.packageName), flags)
        usb.requestPermission(device, intent)
    }

    private fun close(key: String) {
        val (connection, claimed) = open.remove(key) ?: return
        closed(key)
        for (item in claimed) connection.releaseInterface(item)
        connection.close()
    }
}
