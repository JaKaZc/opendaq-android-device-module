package io.opendaq.androiddevice

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.pm.PackageManager
import android.location.GnssMeasurementsEvent
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log

/// Reports the time of the phone's satellite receiver (GNSS) to
/// AndroidDeviceModule, whose phone device uses it when its TimeSource is GNSS
/// time. Both openDAQ Studio for Android and the Device Server build this file
/// from the module's folder.
///
/// openDAQ loads the module from the app's libraries; loading it here gives
/// the same library, which implements the functions below (phone_clock.cpp).
/// While the device wants GNSS time and the app may use the location, this
/// listens to the receiver. Its clock measurements (GnssClock) give the time
/// to within microseconds; on a phone without them, the fixes give it to
/// about twenty milliseconds. A measurement counts only when it agrees with
/// the latest fix to within half a second, so a receiver that gets its own
/// clock wrong, such as the emulator's, cannot move the time.
class GnssTime(context: Context) {
    companion object {
        private const val TAG = "GnssTime"
        private const val PERMISSION_REQUEST = 7421

        /// The GPS epoch, 6 January 1980, in Unix time.
        private const val GPS_EPOCH_NANOS = 315_964_800L * 1_000_000_000L

        /// GPS time is ahead of UTC by the leap seconds since 1980, 18 since
        /// 2017. A receiver that knows the number says so.
        private const val LEAP_SECONDS = 18

        /// How far a clock measurement may differ from the latest fix, and how
        /// old that fix may be.
        private const val AGREEMENT_NANOS = 500_000_000L
        private const val FIX_FRESH_NANOS = 60_000_000_000L

        private val loaded: Boolean by lazy {
            try {
                System.loadLibrary("android_device_module.module")
                true
            } catch (error: UnsatisfiedLinkError) {
                Log.w(TAG, "The Android device module is not available: ${error.message}")
                false
            }
        }

        @JvmStatic private external fun wanted(): Boolean
        @JvmStatic private external fun report(unixNanos: Long, bootNanos: Long, uncertaintyNanos: Long)
        @JvmStatic private external fun problem(message: String?)

        /// Whether a phone device wants GNSS time.
        fun isWanted(): Boolean = loaded && wanted()

        fun mayUseLocation(context: Context): Boolean =
            context.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED

        private var asked = false

        /// Asks the user once per run of the app for the location, when a phone
        /// device wants GNSS time and the app may not use it yet.
        fun askIfWanted(activity: Activity) {
            if (asked || !isWanted() || mayUseLocation(activity)) return
            asked = true
            activity.requestPermissions(
                arrayOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION),
                PERMISSION_REQUEST,
            )
        }
    }

    private val context = context.applicationContext
    private val handler = Handler(Looper.getMainLooper())
    private val locations = this.context.getSystemService(LocationManager::class.java)
    private var running = false
    private var listening = false
    private var askFrom: Activity? = null

    // The latest fix: Unix time minus the boot clock, and when it came.
    private var fixOffset = 0L
    private var fixAt = Long.MIN_VALUE

    /// Called on the main thread before the receiver is started, for a
    /// foreground service to add the location to its types.
    var beforeListening: (() -> Unit)? = null

    /// Starts watching whether a phone device wants GNSS time. An activity
    /// passed here asks the user for the location when it does.
    fun start(askFrom: Activity? = null) {
        this.askFrom = askFrom
        if (running) return
        running = true
        check()
    }

    fun stop() {
        running = false
        askFrom = null
        handler.removeCallbacksAndMessages(null)
        listen(false)
    }

    private fun check() {
        if (!running) return
        askFrom?.let { askIfWanted(it) }
        listen(isWanted())
        handler.postDelayed({ check() }, 2000)
    }

    private fun listen(on: Boolean) {
        if (!on) {
            if (listening) {
                locations.removeUpdates(fixes)
                locations.unregisterGnssMeasurementsCallback(measurements)
                listening = false
            }
            return
        }
        if (listening) return
        if (!mayUseLocation(context)) {
            problem("The app may not use the phone's location, which GNSS time needs. Allow it in the app's settings.")
            return
        }
        if (!locations.isProviderEnabled(LocationManager.GPS_PROVIDER)) {
            problem("The phone's location is off, and GNSS time needs it. Turn it on in the phone's settings.")
            return
        }
        try {
            beforeListening?.invoke()
            locations.requestLocationUpdates(LocationManager.GPS_PROVIDER, 1000L, 0f, fixes, Looper.getMainLooper())
            locations.registerGnssMeasurementsCallback(measurements, handler)
            problem(null)
            listening = true
        } catch (error: SecurityException) {
            problem("The app may not use the phone's location, which GNSS time needs. Allow it in the app's settings.")
        } catch (error: RuntimeException) {
            problem("The phone's satellite receiver could not be started: ${error.message}")
        }
    }

    private val measurements = object : GnssMeasurementsEvent.Callback() {
        override fun onGnssMeasurementsReceived(event: GnssMeasurementsEvent) {
            val clock = event.clock
            // The receiver's time is known once it has the full bias, and
            // comparable with the boot clock where it gives that clock's stamp.
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q || !clock.hasFullBiasNanos() || !clock.hasElapsedRealtimeNanos()) return
            val bias = if (clock.hasBiasNanos()) Math.round(clock.biasNanos) else 0L
            val gpsNanos = clock.timeNanos - clock.fullBiasNanos - bias
            val leap = if (clock.hasLeapSecond()) clock.leapSecond else LEAP_SECONDS
            val unixNanos = gpsNanos + GPS_EPOCH_NANOS - leap * 1_000_000_000L
            var uncertainty = if (clock.hasElapsedRealtimeUncertaintyNanos()) clock.elapsedRealtimeUncertaintyNanos.toLong() else 1_000_000L
            if (clock.hasBiasUncertaintyNanos()) uncertainty += clock.biasUncertaintyNanos.toLong()
            // Measurements and fixes both arrive on the main thread.
            val fresh = fixAt != Long.MIN_VALUE && clock.elapsedRealtimeNanos - fixAt < FIX_FRESH_NANOS
            if (fresh && Math.abs(unixNanos - clock.elapsedRealtimeNanos - fixOffset) < AGREEMENT_NANOS) {
                report(unixNanos, clock.elapsedRealtimeNanos, uncertainty)
            }
        }
    }

    // All methods are overridden, because Android before 11 has no defaults.
    private val fixes = object : LocationListener {
        override fun onLocationChanged(location: Location) {
            val uncertainty =
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && location.hasElapsedRealtimeUncertaintyNanos()) {
                    location.elapsedRealtimeUncertaintyNanos.toLong() + 1_000_000L
                } else {
                    20_000_000L
                }
            fixOffset = location.time * 1_000_000L - location.elapsedRealtimeNanos
            fixAt = location.elapsedRealtimeNanos
            report(location.time * 1_000_000L, location.elapsedRealtimeNanos, uncertainty)
        }

        override fun onProviderEnabled(provider: String) {
            problem(null)
        }

        override fun onProviderDisabled(provider: String) {
            problem("The phone's location is off, and GNSS time needs it. Turn it on in the phone's settings.")
        }

        @Deprecated("Called only before Android 10")
        override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) {
        }
    }
}
