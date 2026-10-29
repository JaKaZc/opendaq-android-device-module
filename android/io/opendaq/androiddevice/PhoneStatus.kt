package io.opendaq.androiddevice

import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.net.TrafficStats
import android.net.wifi.WifiManager
import android.os.BatteryManager
import android.os.Build
import android.os.Environment
import android.os.Handler
import android.os.HandlerThread
import android.os.PowerManager
import android.os.Process
import android.os.StatFs
import android.os.SystemClock
import android.util.Log
import java.io.File
import kotlin.math.abs

/// Reports the phone's state to AndroidDeviceModule, whose System and USB
/// channels show it: CPU, memory, Wi-Fi, network traffic, battery,
/// temperature, storage, and what is connected to the USB port. Android tells
/// most of it only to Java, so the app reports it, as GnssTime does the time.
/// Both openDAQ Studio for Android and the Device Server build this file from
/// the module's folder.
///
/// It reports every two seconds, and as often as the channels read while
/// they do, on a thread of its own.
class PhoneStatus(context: Context) {
    companion object {
        private const val TAG = "PhoneStatus"
        private const val IDLE_INTERVAL = 2000L

        private val loaded: Boolean by lazy {
            try {
                System.loadLibrary("android_device_module.module")
                true
            } catch (error: UnsatisfiedLinkError) {
                Log.w(TAG, "The Android device module is not available: ${error.message}")
                false
            }
        }

        @JvmStatic private external fun wantedInterval(): Int
        @JvmStatic private external fun report(values: FloatArray)
        @JvmStatic private external fun reportUsb(
            powerSource: String,
            charger: String,
            devices: Array<String>,
            audioIds: IntArray,
            audioNames: Array<String>,
        )

        // The System channel's fields, in the module's order (phone_status.cpp).
        private const val CPU = 0
        private const val CPU_FREQUENCY = 1
        private const val MEMORY_USED = 2
        private const val MEMORY_AVAILABLE = 3
        private const val WIFI_SIGNAL = 4
        private const val WIFI_LINK_SPEED = 5
        private const val NETWORK_RECEIVE = 6
        private const val NETWORK_SEND = 7
        private const val BATTERY_LEVEL = 8
        private const val BATTERY_VOLTAGE = 9
        private const val BATTERY_CURRENT = 10
        private const val BATTERY_POWER = 11
        private const val BATTERY_TEMPERATURE = 12
        private const val CHARGING = 13
        private const val THERMAL_HEADROOM = 14
        private const val STORAGE_FREE = 15
        private const val FIELDS = 16
    }

    private val context = context.applicationContext
    private var thread: HandlerThread? = null
    private var handler: Handler? = null

    private val cores = Runtime.getRuntime().availableProcessors()
    private var lastCpu = -1L
    private var lastWall = 0L
    private var lastReceived = -1L
    private var lastSent = -1L
    private var lastTraffic = 0L

    fun start() {
        if (thread != null || !loaded) return
        val started = HandlerThread("PhoneStatus").apply { start() }
        thread = started
        handler = Handler(started.looper).apply { post { tick() } }
    }

    fun stop() {
        handler?.removeCallbacksAndMessages(null)
        thread?.quitSafely()
        thread = null
        handler = null
    }

    private fun tick() {
        try {
            sample()
        } catch (error: RuntimeException) {
            Log.w(TAG, "The phone's state could not be read: ${error.message}")
        }
        val wanted = wantedInterval()
        handler?.postDelayed({ tick() }, if (wanted > 0) wanted.toLong() else IDLE_INTERVAL)
    }

    private fun sample() {
        val values = FloatArray(FIELDS) { Float.NaN }

        // This app's processor time, against the wall clock and all cores.
        val wall = SystemClock.elapsedRealtime()
        val cpu = Process.getElapsedCpuTime()
        if (lastCpu >= 0 && wall > lastWall) values[CPU] = 100f * (cpu - lastCpu) / ((wall - lastWall).toFloat() * cores)
        lastCpu = cpu
        lastWall = wall
        values[CPU_FREQUENCY] = cpuFrequency()

        val memory = ActivityManager.MemoryInfo()
        context.getSystemService(ActivityManager::class.java)?.getMemoryInfo(memory)
        if (memory.totalMem > 0) {
            values[MEMORY_USED] = 100f * (memory.totalMem - memory.availMem) / memory.totalMem
            values[MEMORY_AVAILABLE] = memory.availMem / 1e6f
        }

        @Suppress("DEPRECATION")
        val wifi = context.getSystemService(WifiManager::class.java)?.connectionInfo
        if (wifi != null && wifi.networkId != -1 && wifi.rssi > -127) {
            values[WIFI_SIGNAL] = wifi.rssi.toFloat()
            values[WIFI_LINK_SPEED] = wifi.linkSpeed.toFloat()
        }

        val received = TrafficStats.getTotalRxBytes()
        val sent = TrafficStats.getTotalTxBytes()
        if (received != TrafficStats.UNSUPPORTED.toLong() && lastReceived >= 0 && wall > lastTraffic) {
            val seconds = (wall - lastTraffic) / 1000f
            values[NETWORK_RECEIVE] = (received - lastReceived) / 1000f / seconds
            values[NETWORK_SEND] = (sent - lastSent) / 1000f / seconds
        }
        lastReceived = received
        lastSent = sent
        lastTraffic = wall

        val battery = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val manager = context.getSystemService(BatteryManager::class.java)
        val plugged = battery?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ?: 0
        if (battery != null) {
            val level = battery.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
            val scale = battery.getIntExtra(BatteryManager.EXTRA_SCALE, 100)
            if (level >= 0 && scale > 0) values[BATTERY_LEVEL] = 100f * level / scale
            val millivolts = battery.getIntExtra(BatteryManager.EXTRA_VOLTAGE, -1)
            if (millivolts > 0) values[BATTERY_VOLTAGE] = millivolts / 1000f
            val tenths = battery.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE)
            if (tenths != Int.MIN_VALUE) values[BATTERY_TEMPERATURE] = tenths / 10f
            val status = battery.getIntExtra(BatteryManager.EXTRA_STATUS, BatteryManager.BATTERY_STATUS_UNKNOWN)
            values[CHARGING] = if (plugged != 0 && status == BatteryManager.BATTERY_STATUS_CHARGING) 1f else 0f
        }
        val current = manager?.getIntProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW) ?: Int.MIN_VALUE
        if (current != Int.MIN_VALUE && current != 0) {
            // Android says microamperes; some phones report milliamperes, and
            // a phone draws more than 20 mA, so a smaller number is in mA.
            val milliamperes = if (abs(current) < 20_000) current.toFloat() else current / 1000f
            values[BATTERY_CURRENT] = milliamperes
            if (!values[BATTERY_VOLTAGE].isNaN()) values[BATTERY_POWER] = values[BATTERY_VOLTAGE] * milliamperes / 1000f
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val headroom = context.getSystemService(PowerManager::class.java)?.getThermalHeadroom(0) ?: Float.NaN
            values[THERMAL_HEADROOM] = headroom
        }
        values[STORAGE_FREE] = StatFs(Environment.getDataDirectory().path).availableBytes / 1e9f
        report(values)

        reportUsb(
            powerSource(plugged),
            charger(battery),
            usbDevices(battery),
            audioInputs().map { it.id }.toIntArray(),
            audioInputs().map { it.productName?.toString() ?: "USB microphone" }.toTypedArray(),
        )
    }

    /// The average current frequency of the cores, where Android lets apps read it.
    private fun cpuFrequency(): Float {
        var sum = 0f
        var count = 0
        for (core in 0 until cores) {
            val kilohertz = try {
                File("/sys/devices/system/cpu/cpu$core/cpufreq/scaling_cur_freq").readText().trim().toFloat()
            } catch (error: Exception) {
                continue
            }
            // Some phones and the emulator report 0 for a core they do not track.
            if (kilohertz <= 0f) continue
            sum += kilohertz / 1000f
            count++
        }
        return if (count > 0) sum / count else Float.NaN
    }

    private fun powerSource(plugged: Int) = when (plugged) {
        BatteryManager.BATTERY_PLUGGED_AC -> "Charger"
        BatteryManager.BATTERY_PLUGGED_USB -> "USB"
        BatteryManager.BATTERY_PLUGGED_WIRELESS -> "Wireless charger"
        8 -> "Dock" // BatteryManager.BATTERY_PLUGGED_DOCK, Android 13 and later
        else -> "Battery"
    }

    /// The charger's rating from the extras most phones add to the battery
    /// broadcast, though Android does not document them.
    private fun charger(battery: Intent?): String {
        val microamperes = battery?.getIntExtra("max_charging_current", -1) ?: -1
        val microvolts = battery?.getIntExtra("max_charging_voltage", -1) ?: -1
        if (microamperes <= 0 || microvolts <= 0) return ""
        val amperes = microamperes / 1e6
        val volts = microvolts / 1e6
        return String.format("%.1f V, %.2f A, %.0f W", volts, amperes, volts * amperes)
    }

    /// The USB devices on the port, one line each, and the computer the phone
    /// is a USB device of, if any.
    private fun usbDevices(battery: Intent?): Array<String> {
        val lines = mutableListOf<String>()
        val manager = context.getSystemService(UsbManager::class.java)
        for (device in manager?.deviceList?.values.orEmpty()) lines += describe(device)
        // The phone is itself a USB device when a computer powers it over USB.
        val state = context.registerReceiver(null, IntentFilter("android.hardware.usb.action.USB_STATE"))
        if (state?.getBooleanExtra("connected", false) == true &&
            battery?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) == BatteryManager.BATTERY_PLUGGED_USB
        ) {
            lines += "A computer, which the phone is connected to as a USB device"
        }
        return lines.toTypedArray()
    }

    private fun describe(device: UsbDevice): String {
        val name = try {
            device.productName
        } catch (error: SecurityException) {
            null
        } ?: "USB device"
        val maker = try {
            device.manufacturerName
        } catch (error: SecurityException) {
            null
        }
        val kinds = (0 until device.interfaceCount).map { kind(device.getInterface(it).interfaceClass) }.distinct()
        val id = String.format("%04x:%04x", device.vendorId, device.productId)
        return buildString {
            append(name)
            if (!maker.isNullOrBlank()) append(" ($maker)")
            if (kinds.isNotEmpty()) append(": ").append(kinds.joinToString(", "))
            append(", ").append(id)
        }
    }

    private fun kind(usbClass: Int) = when (usbClass) {
        UsbConstants.USB_CLASS_AUDIO -> "audio"
        UsbConstants.USB_CLASS_COMM, UsbConstants.USB_CLASS_CDC_DATA -> "serial"
        UsbConstants.USB_CLASS_HID -> "input"
        UsbConstants.USB_CLASS_STILL_IMAGE -> "still camera"
        UsbConstants.USB_CLASS_PRINTER -> "printer"
        UsbConstants.USB_CLASS_MASS_STORAGE -> "storage"
        UsbConstants.USB_CLASS_HUB -> "hub"
        UsbConstants.USB_CLASS_VIDEO -> "video"
        UsbConstants.USB_CLASS_WIRELESS_CONTROLLER -> "wireless"
        UsbConstants.USB_CLASS_VENDOR_SPEC -> "vendor-specific"
        else -> "class $usbClass"
    }

    private fun audioInputs(): List<AudioDeviceInfo> {
        val audio = context.getSystemService(AudioManager::class.java) ?: return emptyList()
        return audio.getDevices(AudioManager.GET_DEVICES_INPUTS).filter {
            it.type == AudioDeviceInfo.TYPE_USB_DEVICE || it.type == AudioDeviceInfo.TYPE_USB_HEADSET
        }
    }
}
