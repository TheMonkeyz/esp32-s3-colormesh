package ca.themonkeyz.colormesh

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertisingSet
import android.bluetooth.le.AdvertisingSetCallback
import android.bluetooth.le.AdvertisingSetParameters
import android.bluetooth.le.IPeriodicAdvertisingCallback
import android.bluetooth.le.PeriodicAdvertisingParameters
import android.bluetooth.le.PeriodicAdvertisingReport
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.AttributionSource
import android.content.Context
import android.os.Binder
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Parcel
import android.os.SystemClock
import android.util.Log
import java.io.ByteArrayOutputStream

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
    companion object {
        const val TAG = "colormesh"
        // The sync callback's AIDL descriptor and transaction codes (SyncBinder)
        const val DESCRIPTOR = "android.bluetooth.le.IPeriodicAdvertisingCallback"
        const val T_SYNC_ESTABLISHED = 1
        const val T_REPORT = 2
        const val T_SYNC_LOST = 3
    }

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

    // ---- a peer's voice clip: sync to its periodic train. A board's train is at its flood address, SID 1, and its
    // extended advert carries the train beacon (a phone's clip set: its own address, the 'B' beacon); the flood's
    // legacy-only scan never reports extended adverts, so a short extended scan finds it, then the Bluetooth
    // service's registerSync (hidden, Hidden.kt) locks on. The packets come to SyncBinder and Clip.Rx collects the
    // chunks: onDone(bytes, kHz) once the clip is whole, onDone(null, 0) when it is lost. One clip at a time: a newer
    // one replaces the old. ----
    private val handler = Handler(Looper.getMainLooper())
    private var rxScanCb: ScanCallback? = null
    private var rxSyncCb: SyncBinder? = null
    private var rxScan: Any? = null                  // the scan service and this app's attribution, while synced
    private var rxSrc: AttributionSource? = null
    private var rx: Clip.Rx? = null
    private var rxDone: ((ByteArray?, Int) -> Unit)? = null
    private var rxT0 = 0L
    private var rxKhz = 16
    private var rxHeard = 0
    private val rxFrag = ByteArrayOutputStream()
    private var rxFragBad = false
    private var rxSeen = 0                   // the train scan's reports: all, the sender's legacy ones, other trains
    private var rxLegacy = 0
    private var rxExt = 0
    private val rxTimeout = Runnable {
        rxEnd(null, "no sync within ${Clip.SYNC_WAIT_MS} ms (scan: $rxSeen reports, $rxLegacy legacy from the sender)")
    }

    val canHearClips: Boolean get() = Hidden.possible && adapter?.isLePeriodicAdvertisingSupported == true
    val hearingClip get() = rx != null

    private fun since() = SystemClock.elapsedRealtime() - rxT0

    fun listenClip(addr: String, origin: Int, id: Int, ds: Int, onDone: (ByteArray?, Int) -> Unit) {
        val scanner = adapter?.bluetoothLeScanner ?: return onDone(null, 0)
        if (!canHearClips) { Log.w(TAG, "clip $id: cannot hear clips: ${Hidden.why}"); return onDone(null, 0) }
        if (rx != null) rxEnd(null, "replaced by clip $id")
        rx = Clip.Rx(origin, id)
        rxDone = onDone
        rxT0 = SystemClock.elapsedRealtime()
        rxKhz = 16
        rxHeard = 0
        rxFrag.reset()
        rxFragBad = false
        rxSeen = 0
        rxLegacy = 0
        rxExt = 0
        val cb = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, r: ScanResult) {
                if (rxScanCb !== this) return
                rxSeen++
                val mine = r.device.address.equals(addr, ignoreCase = true)
                if (r.isLegacy) { if (mine) rxLegacy++; return }
                // the sender's train: its flood address (a board) or its beacon with this origin and id
                val b = r.scanRecord?.bytes?.let { Clip.beaconParse(it) }
                if (!mine && (b == null || b.origin != origin || b.id != id)) {
                    if (r.periodicAdvertisingInterval != ScanResult.PERIODIC_INTERVAL_NOT_PRESENT && rxExt++ < 3)
                        Log.i(TAG, "clip $id: another train on air: ${r.device.address} sid=${r.advertisingSid} " +
                            "interval=${r.periodicAdvertisingInterval}")
                    return
                }
                Log.i(TAG, "clip $id: extended report of ${r.device.address} after ${since()} ms: sid=${r.advertisingSid} " +
                    "interval=${r.periodicAdvertisingInterval}x1.25ms status=${r.dataStatus} " +
                    "phy=${r.primaryPhy}/${r.secondaryPhy} rssi=${r.rssi} ${r.scanRecord?.bytes?.size ?: 0} bytes " +
                    "beacon=${b != null}")
                if (r.periodicAdvertisingInterval == ScanResult.PERIODIC_INTERVAL_NOT_PRESENT) return
                if (b != null) rxKhz = b.khz
                stopRxScan()
                sync(r, id)
            }
            override fun onScanFailed(errorCode: Int) { rxEnd(null, "train scan failed $errorCode") }
        }
        rxScanCb = cb
        // No filter (matched here by address): a filter on the address found nothing on a Pixel 8 Pro (2026-11-01)
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .setCallbackType(ScanSettings.CALLBACK_TYPE_ALL_MATCHES)
            .setReportDelay(0)
            .setLegacy(false)
            .build()
        try { scanner.startScan(null, settings, cb) } catch (e: Exception) { return rxEnd(null, "train scan: $e") }
        handler.postDelayed(rxTimeout, Clip.SYNC_WAIT_MS)
        Log.i(TAG, "clip $id from %02x ($addr), ${ds / 10}.${ds % 10} s: looking for its train".format(origin))
    }

    private fun stopRxScan() {
        val cb = rxScanCb ?: return
        rxScanCb = null
        try { adapter?.bluetoothLeScanner?.stopScan(cb) } catch (e: Exception) { }
    }

    private fun sync(r: ScanResult, id: Int) {
        val a = adapter ?: return rxEnd(null, "no adapter")
        val scan = Hidden.scanService(a) ?: return rxEnd(null, "no scan service: ${Hidden.why}")
        val src = Hidden.attribution(a) ?: return rxEnd(null, "no attribution source: ${Hidden.why}")
        val cb = SyncBinder(id)
        rxSyncCb = cb
        rxScan = scan
        rxSrc = src
        if (!Hidden.registerSync(scan, r, 0, Clip.SYNC_TIMEOUT, cb, src)) rxEnd(null, "registerSync: ${Hidden.why}")
        else Log.i(TAG, "clip $id: registerSync sent after ${since()} ms")
    }

    // The sync callback: this app's own Binder behind the service's IPeriodicAdvertisingCallback (oneway). The
    // transaction codes and parcel layouts are those of Android 17's framework-bluetooth.jar (Pixel 8 Pro,
    // 2026-11-01): 1 onSyncEstablished(int, BluetoothDevice, int, int, int, int), 2 onPeriodicAdvertisingReport
    // (present flag, then syncHandle, txPower, rssi, dataStatus, a flag and the bytes), 3 onSyncLost(int); 4 and 5
    // (sync transferred, BIG info) are not for us. Binder thread in, main thread out.
    private inner class SyncBinder(val clipId: Int) : Binder(), IPeriodicAdvertisingCallback {
        init { attachInterface(this, DESCRIPTOR) }

        override fun asBinder(): IBinder = this

        override fun onTransact(code: Int, data: Parcel, reply: Parcel?, flags: Int): Boolean {
            if (code < IBinder.FIRST_CALL_TRANSACTION || code > IBinder.LAST_CALL_TRANSACTION)
                return super.onTransact(code, data, reply, flags)
            data.enforceInterface(DESCRIPTOR)
            when (code) {
                T_SYNC_ESTABLISHED -> {
                    val h = data.readInt()
                    val dev = data.readTypedObject(BluetoothDevice.CREATOR)
                    val sid = data.readInt()
                    val skip = data.readInt()
                    val timeout = data.readInt()
                    val status = data.readInt()
                    handler.post { onSyncEstablished(h, dev, sid, skip, timeout, status) }
                }
                T_REPORT -> {
                    if (data.readInt() == 0) return true                // a null report
                    data.readInt()                                      // sync handle
                    data.readInt()                                      // tx power
                    data.readInt()                                      // rssi
                    val status = data.readInt()
                    val bytes = if (data.readInt() == 1) data.createByteArray() else null
                    handler.post { report(status, bytes) }
                }
                T_SYNC_LOST -> {
                    val h = data.readInt()
                    handler.post { onSyncLost(h) }
                }
                else -> Log.i(TAG, "clip $clipId: sync callback $code ignored")
            }
            return true
        }

        override fun onSyncEstablished(syncHandle: Int, device: BluetoothDevice?, advertisingSid: Int, skip: Int,
                                       timeout: Int, status: Int) {
            if (rxSyncCb !== this) return
            Log.i(TAG, "clip $clipId: sync status=$status handle=$syncHandle sid=$advertisingSid " +
                "from ${device?.address} after ${since()} ms")
            if (status != 0) rxEnd(null, "sync failed $status") else handler.removeCallbacks(rxTimeout)
        }

        fun report(status: Int, bytes: ByteArray?) {
            if (rxSyncCb === this) packet(bytes ?: ByteArray(0), status)
        }

        override fun onPeriodicAdvertisingReport(report: PeriodicAdvertisingReport?) {}   // read in onTransact
        override fun onSyncLost(syncHandle: Int) { if (rxSyncCb === this) rxEnd(null, "train lost") }
        override fun onSyncTransferred(device: BluetoothDevice?, status: Int) {}
        override fun onBigInfoAdvertisingReport(syncHandle: Int, encrypted: Boolean) {}
    }

    // One periodic report: fragments put back together (data status 0 complete, 1 more to come, 2 truncated), then
    // every chunk AD of the packet goes to the clip
    private fun packet(data: ByteArray, status: Int) {
        val r = rx ?: return
        if (!rxFragBad && rxFrag.size() + data.size <= Clip.FRAG_MAX) rxFrag.write(data) else rxFragBad = true
        if (status == PeriodicAdvertisingReport.DATA_INCOMPLETE_MORE_TO_COME) return
        val whole = if (status == PeriodicAdvertisingReport.DATA_COMPLETE && !rxFragBad) rxFrag.toByteArray() else null
        rxFrag.reset()
        rxFragBad = false
        if (whole == null) return
        for (ad in Clip.ads(whole)) {
            val p = Clip.parse(ad) ?: continue
            rxHeard++
            if (rxHeard == 1) Log.i(TAG, "clip ${r.id}: first chunk after ${since()} ms (${p.total} chunks, ${p.len} bytes)")
            if (r.add(p)) {
                val bytes = r.bytes()
                Log.i(TAG, "clip ${r.id}: whole after ${since()} ms, ${r.total} chunks, ${bytes.size} bytes, " +
                    "$rxHeard heard, crc ok, $rxKhz kHz")
                rxEnd(bytes, "")
                return
            }
            if (!r.active) { rxEnd(null, "CRC mismatch"); return }
        }
    }

    private fun rxEnd(bytes: ByteArray?, why: String) {
        handler.removeCallbacks(rxTimeout)
        stopRxScan()
        val s = rxSyncCb
        rxSyncCb = null
        if (s != null) { val sc = rxScan; val src = rxSrc; if (sc != null && src != null) Hidden.unregisterSync(sc, s, src) }
        rxScan = null
        rxSrc = null
        val r = rx ?: return
        rx = null
        if (bytes == null) Log.w(TAG, "clip ${r.id}: dropped: $why (${r.have} of ${r.total} chunks, $rxHeard heard, ${since()} ms)")
        val cb = rxDone
        rxDone = null
        cb?.invoke(bytes, rxKhz)
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
        if (clipDone != null) clipEnd(false)
        if (rx != null) rxEnd(null, "radio stopped")
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
