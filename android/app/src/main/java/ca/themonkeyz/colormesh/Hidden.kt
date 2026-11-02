package ca.themonkeyz.colormesh

import android.bluetooth.BluetoothAdapter
import android.bluetooth.le.IPeriodicAdvertisingCallback
import android.bluetooth.le.ScanResult
import android.content.AttributionSource
import android.os.Build
import android.util.Log
import org.lsposed.hiddenapibypass.HiddenApiBypass

// Android has no public API to hear another device's periodic advertising train, and Android 17 dropped even the
// hidden Java wrapper (PeriodicAdvertisingManager is gone from framework-bluetooth.jar on a Pixel 8 Pro, 2026-11-01).
// What is left is the Bluetooth service's own interface, IBluetoothScan.registerSync(ScanResult, skip, timeout,
// IPeriodicAdvertisingCallback, AttributionSource), the call the LE Audio broadcast assistant uses. This object
// reaches it with HiddenApiBypass (LSPosed): its invoke() finds a method through ART's own tables, not through
// reflection, so the non-SDK restriction never sees it. The callback is this app's own Binder (Radio.SyncBinder),
// which implements the hidden interface (a compile-time stub in the `hidden` module) and reads the transactions
// itself. Android 12+ (the service signature with an AttributionSource); tested on Android 17. `why` says what
// failed.
object Hidden {
    const val TAG = "colormesh"
    var why = ""
        private set

    val possible get() = Build.VERSION.SDK_INT >= 31

    private fun call(cls: Class<*>, thiz: Any?, name: String, vararg args: Any?): Any? = try {
        HiddenApiBypass.invoke(cls, thiz, name, *args)
    } catch (e: Throwable) {
        why = "$name: $e"
        Log.w(TAG, why)
        null
    }

    // The scan service (IBluetoothScan) and this app's attribution, as BluetoothLeScanner takes them from the adapter
    fun scanService(adapter: BluetoothAdapter?): Any? =
        if (adapter == null || !possible) null else call(BluetoothAdapter::class.java, adapter, "getBluetoothScan")

    fun attribution(adapter: BluetoothAdapter): AttributionSource? =
        call(BluetoothAdapter::class.java, adapter, "getAttributionSource") as? AttributionSource

    // IBluetoothScan.registerSync: true when the call went through (the outcome comes to the callback)
    fun registerSync(scan: Any, r: ScanResult, skip: Int, timeout: Int, cb: IPeriodicAdvertisingCallback,
                     src: AttributionSource): Boolean {
        why = ""
        call(scan.javaClass, scan, "registerSync", r, skip, timeout, cb, src)
        return why.isEmpty()
    }

    fun unregisterSync(scan: Any, cb: IPeriodicAdvertisingCallback, src: AttributionSource) {
        call(scan.javaClass, scan, "unregisterSync", cb, src)
    }
}
