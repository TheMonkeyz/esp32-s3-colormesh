package ca.themonkeyz.colormesh

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertisingSet
import android.bluetooth.le.AdvertisingSetCallback
import android.bluetooth.le.AdvertisingSetParameters
import android.bluetooth.le.PeriodicAdvertisingParameters
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

    // ---- a voice clip: a second set, extended (not legacy) with the clip beacon, and a periodic train of packets
    // (Clip.packets), one every 100 ms (Android's shortest periodic interval). onDone(ok) when the last went out. ----
    private var clipSet: AdvertisingSet? = null
    private var clipPkts: List<List<ByteArray>> = emptyList()
    private var clipNext = 0
    private var clipDone: ((Boolean) -> Unit)? = null
    private var clipBusy = false
    var clipProgress = 0f
        private set
    private var clipRefused = 0

    // The longest periodic data this phone sends: a controller refuses new periodic data in fragments while the train
    // runs (LE Set Periodic Advertising Data, Operation 3 only: COMMAND_DISALLOWED seen on a Pixel 8 Pro), so one
    // HCI command's worth, 251 bytes: one clip chunk a packet
    val maxClipData: Int get() = 251
    val canSendClips: Boolean get() = adapter?.isLePeriodicAdvertisingSupported == true &&
        adapter.isLeExtendedAdvertisingSupported

    private fun pktData(ads: List<ByteArray>): AdvertiseData {
        val b = AdvertiseData.Builder().setIncludeDeviceName(false).setIncludeTxPowerLevel(false)
        ads.forEachIndexed { k, ad -> b.addManufacturerData(Clip.companyOf(k), ad.copyOfRange(4, ad.size)) }
        return b.build()
    }

    private val clipCb = object : AdvertisingSetCallback() {
        override fun onAdvertisingSetStarted(s: AdvertisingSet?, txPower: Int, status: Int) {
            if (status != ADVERTISE_SUCCESS) {
                Log.w(TAG, "clip set failed status=$status")
                clipEnd(false)
                return
            }
            clipSet = s
            clipNext = 1
            Log.i(TAG, "clip set up: ${clipPkts.size} packets")
        }
        override fun onPeriodicAdvertisingDataSet(s: AdvertisingSet?, status: Int) {
            clipBusy = false
            if (status != ADVERTISE_SUCCESS) { Log.w(TAG, "periodic data status=$status"); clipRefused++ }
        }
        override fun onAdvertisingSetStopped(s: AdvertisingSet?) { clipSet = null }
    }

    fun sendClip(origin: Int, id: Int, ds: Int, pkts: List<List<ByteArray>>, onDone: (Boolean) -> Unit) {
        val adv = adapter?.bluetoothLeAdvertiser ?: return onDone(false)
        if (clipDone != null || pkts.isEmpty()) return onDone(false)
        clipPkts = pkts
        clipNext = 0
        clipDone = onDone
        clipBusy = false
        clipProgress = 0f
        val params = AdvertisingSetParameters.Builder()
            .setLegacyMode(false)
            .setConnectable(false)
            .setScannable(false)
            .setInterval(AdvertisingSetParameters.INTERVAL_LOW)
            .setTxPowerLevel(AdvertisingSetParameters.TX_POWER_HIGH)
            .setPrimaryPhy(BluetoothDevice.PHY_LE_1M)
            .setSecondaryPhy(BluetoothDevice.PHY_LE_2M)
            .build()
        val beacon = Clip.beacon(origin, id, ds)
        val advData = AdvertiseData.Builder().addManufacturerData(MeshProto.COMPANY, beacon.copyOfRange(4, beacon.size))
            .setIncludeDeviceName(false).setIncludeTxPowerLevel(false).build()
        val per = PeriodicAdvertisingParameters.Builder().setInterval(80).setIncludeTxPower(false).build()   // 100 ms
        adv.startAdvertisingSet(params, advData, null, per, pktData(pkts[0]), clipCb)
    }

    private var clipLastMs = 0L

    // Called every frame by the activity while a clip goes out: the next packet every Clip.PKT_MS
    fun clipTick(nowMs: Long) {
        val s = clipSet ?: return
        if (clipBusy || nowMs - clipLastMs < Clip.PKT_MS) return
        clipLastMs = nowMs
        if (clipNext >= clipPkts.size) { clipEnd(true); return }
        clipBusy = true
        s.setPeriodicAdvertisingData(pktData(clipPkts[clipNext]))
        clipNext++
        clipProgress = clipNext.toFloat() / clipPkts.size
    }

    private fun clipEnd(ok: Boolean) {
        try { adapter?.bluetoothLeAdvertiser?.stopAdvertisingSet(clipCb) } catch (e: Exception) { }
        clipSet = null
        val cb = clipDone
        clipDone = null
        Log.i(TAG, "clip sent ok=$ok packets=$clipNext/${clipPkts.size} refused=$clipRefused")
        clipRefused = 0
        cb?.invoke(ok)
    }

    val sendingClip get() = clipDone != null

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
        if (clipDone != null) clipEnd(false)
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
