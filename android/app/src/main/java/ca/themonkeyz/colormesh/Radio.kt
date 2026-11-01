package ca.themonkeyz.colormesh

import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertisingSet
import android.bluetooth.le.AdvertisingSetCallback
import android.bluetooth.le.AdvertisingSetParameters
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.util.Log

// The phone's radio: one legacy, non-connectable, non-scannable advertising set (no Flags AD, so the 30-byte
// ColorMesh AD fits the 31-byte legacy payload) every 100 ms (Android's fastest), its data changed in place; one
// continuous low-latency scan for ColorMesh adverts, started once (Android allows 5 scan starts per 30 s).
// Callbacks arrive on the main thread. The caller checks the permissions first.
@SuppressLint("MissingPermission")
class Radio(
    ctx: Context,
    private val onReport: (addr: String, value: ByteArray?, rssi: Int) -> Unit,
    private val onState: (ok: Boolean, msg: String) -> Unit,
) {
    companion object { const val TAG = "colormesh" }

    private val adapter = (ctx.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
    private var set: AdvertisingSet? = null
    private var pending: ByteArray? = null
    private var running = false
    var sent = 0
        private set
    var heard = 0
        private set

    val enabled get() = adapter?.isEnabled == true

    private fun data(value: ByteArray) = AdvertiseData.Builder()
        .addManufacturerData(MeshProto.COMPANY, value)
        .setIncludeDeviceName(false)
        .setIncludeTxPowerLevel(false)
        .build()

    private val advCb = object : AdvertisingSetCallback() {
        override fun onAdvertisingSetStarted(s: AdvertisingSet?, txPower: Int, status: Int) {
            if (status != ADVERTISE_SUCCESS) {
                Log.w(TAG, "adv start failed status=$status")
                onState(false, "adv $status")
                return
            }
            set = s
            Log.i(TAG, "advertising 100 ms tx=$txPower")
            pending?.let { update(it) }
        }
        override fun onAdvertisingDataSet(s: AdvertisingSet?, status: Int) {
            if (status != ADVERTISE_SUCCESS) Log.w(TAG, "adv data status=$status")
        }
        override fun onAdvertisingSetStopped(s: AdvertisingSet?) { set = null }
    }

    private val scanCb = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, r: ScanResult) {
            heard++
            onReport(r.device.address, r.scanRecord?.getManufacturerSpecificData(MeshProto.COMPANY), r.rssi)
        }
        override fun onBatchScanResults(results: MutableList<ScanResult>) { results.forEach { onScanResult(0, it) } }
        override fun onScanFailed(errorCode: Int) {
            Log.w(TAG, "scan failed $errorCode")
            onState(false, "scan $errorCode")
        }
    }

    fun start(value: ByteArray) {
        if (running) return
        val adv = adapter?.bluetoothLeAdvertiser
        val scanner = adapter?.bluetoothLeScanner
        if (adv == null || scanner == null) { onState(false, "no Bluetooth LE"); return }
        running = true
        pending = value
        val params = AdvertisingSetParameters.Builder()
            .setLegacyMode(true)
            .setConnectable(false)
            .setScannable(false)
            .setInterval(AdvertisingSetParameters.INTERVAL_LOW)
            .setTxPowerLevel(AdvertisingSetParameters.TX_POWER_HIGH)
            .build()
        adv.startAdvertisingSet(params, data(value), null, null, null, advCb)
        val filter = ScanFilter.Builder()
            .setManufacturerData(MeshProto.COMPANY, byteArrayOf(MeshProto.PROTO.toByte()), byteArrayOf(0xFF.toByte()))
            .build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .setCallbackType(ScanSettings.CALLBACK_TYPE_ALL_MATCHES)
            .setReportDelay(0)
            .setLegacy(true)
            .build()
        scanner.startScan(listOf(filter), settings, scanCb)
        Log.i(TAG, "scan started")
        onState(true, "")
    }

    // New advert data (a change of colour, hold, effect...)
    fun update(value: ByteArray) {
        pending = value
        val s = set ?: return
        s.setAdvertisingData(data(value))
        sent++
    }

    fun stop() {
        if (!running) return
        running = false
        try {
            adapter?.bluetoothLeScanner?.stopScan(scanCb)
            adapter?.bluetoothLeAdvertiser?.stopAdvertisingSet(advCb)
        } catch (e: Exception) {
            Log.w(TAG, "stop: $e")
        }
        set = null
        Log.i(TAG, "radio stopped")
    }
}
