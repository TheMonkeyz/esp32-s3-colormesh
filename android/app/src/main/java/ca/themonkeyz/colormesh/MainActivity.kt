package ca.themonkeyz.colormesh

import android.Manifest
import android.app.Activity
import android.bluetooth.BluetoothAdapter
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaRecorder
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.WindowInsets
import android.view.WindowManager
import android.widget.Button
import android.widget.GridLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import kotlin.random.Random

// One screen (the user's pick, 2026-11-01): the count of boards near, the main circle (MeshView), the 12 colour
// swatches of the board's pick screen, and the Call button. The mesh runs while the app is in front.
class MainActivity : Activity() {
    companion object {
        const val TAG = "colormesh"
        const val FRAME_MS = 33L
        const val PREFS = "colormesh"
        // The board's swatches (main/ui.c sw_rgb)
        val SWATCHES = intArrayOf(
            0xFF0000, 0xFF8000, 0xFFFF00, 0x80FF00, 0x00FF00, 0x00FF80,
            0x00FFFF, 0x0080FF, 0x0000FF, 0x8000FF, 0xFF00FF, 0xFF0080,
        )
    }

    private lateinit var mesh: PhoneMesh
    private lateinit var view: MeshView
    private lateinit var near: TextView
    private lateinit var status: TextView
    private lateinit var callBtn: Button
    private lateinit var talkBtn: Button
    private var rec: AudioRecord? = null
    private var recThread: Thread? = null
    @Volatile private var recPcm = ShortArray(0)
    @Volatile private var recN = 0
    private var recStart = 0L
    private var clipId = Random.nextInt(256)
    private val swatchViews = ArrayList<View>()
    private var radio: Radio? = null
    private val ui = Handler(Looper.getMainLooper())
    private var ringTrack: AudioTrack? = null
    private var shownRing: PhoneMesh.Caller? = null
    private var lastLog = 0L

    private fun now() = SystemClock.elapsedRealtime()

    override fun onCreate(saved: Bundle?) {
        super.onCreate(saved)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        window.statusBarColor = Color.BLACK
        window.navigationBarColor = Color.BLACK
        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        var origin = prefs.getInt("origin", -1)
        if (origin !in 1..254) {
            origin = 1 + Random.nextInt(254)
            prefs.edit().putInt("origin", origin).apply()
        }
        mesh = PhoneMesh(origin)
        val c = prefs.getInt("colour", SWATCHES[7])
        mesh.setColour((c shr 16) and 0xFF, (c shr 8) and 0xFF, c and 0xFF)
        buildUi()
    }

    private fun buildUi() {
        val dp = resources.displayMetrics.density
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.BLACK)
            setPadding((16 * dp).toInt(), (16 * dp).toInt(), (16 * dp).toInt(), (16 * dp).toInt())
        }
        near = TextView(this).apply {
            setTextColor(0xFF8B95A1.toInt())
            textSize = 15f
            gravity = Gravity.CENTER
        }
        root.addView(near)
        view = MeshView(this).apply {
            mesh = this@MainActivity.mesh
            now = { this@MainActivity.now() }
            onHold = { on -> this@MainActivity.mesh.setHolding(on); push() }
            onWave = { kind -> this@MainActivity.mesh.startWave(kind, now()); push() }
            onRingTap = { this@MainActivity.mesh.stopRing(true, now()); stopRingSound(); push() }
            onCallTap = { this@MainActivity.mesh.cancelCall() }
        }
        root.addView(view, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        val grid = GridLayout(this).apply { columnCount = 6 }
        val size = (40 * dp).toInt()
        val pad = (6 * dp).toInt()
        for (rgb in SWATCHES) {
            val v = View(this)
            v.background = GradientDrawable().apply { shape = GradientDrawable.OVAL; setColor(0xFF000000.toInt() or rgb) }
            v.contentDescription = String.format("#%06X", rgb)
            v.setOnClickListener { pickColour(rgb) }
            grid.addView(v, GridLayout.LayoutParams().apply { width = size; height = size; setMargins(pad, pad, pad, pad) })
            swatchViews.add(v)
        }
        root.addView(grid, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT).apply { gravity = Gravity.CENTER_HORIZONTAL; topMargin = (8 * dp).toInt() })
        callBtn = Button(this).apply {
            text = getString(R.string.call)
            setTextColor(0xFF04342C.toInt())
            textSize = 18f
            isAllCaps = false
            background = GradientDrawable().apply { cornerRadius = 28 * dp; setColor(0xFF2BB24C.toInt()) }
            setOnClickListener { mesh.ring(now()); push() }
        }
        // Hold to talk, as the board's voice page: up to 5 s, sent when the finger lifts
        talkBtn = Button(this).apply {
            text = getString(R.string.talk)
            setTextColor(0xFFF2F4F7.toInt())
            textSize = 18f
            isAllCaps = false
            background = GradientDrawable().apply { cornerRadius = 28 * dp; setColor(0xFF2A3340.toInt()) }
            setOnTouchListener { v, e ->
                when (e.actionMasked) {
                    MotionEvent.ACTION_DOWN -> { v.isPressed = true; talkDown() }
                    MotionEvent.ACTION_UP -> { v.isPressed = false; talkUp(true) }
                    MotionEvent.ACTION_CANCEL -> { v.isPressed = false; talkUp(false) }
                }
                true
            }
        }
        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER }
        val half = LinearLayout.LayoutParams(0, (56 * dp).toInt(), 1f).apply { setMargins(pad, 0, pad, 0) }
        row.addView(callBtn, half)
        row.addView(talkBtn, LinearLayout.LayoutParams(half))
        root.addView(row, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT).apply { topMargin = (12 * dp).toInt() })
        status = TextView(this).apply {
            setTextColor(0xFF8B95A1.toInt())
            textSize = 12f
            gravity = Gravity.CENTER
        }
        root.addView(status)
        // Android 15+ draws the app under the status and navigation bars: keep the content clear of them
        val edge = (16 * dp).toInt()
        root.setOnApplyWindowInsetsListener { v, insets ->
            @Suppress("DEPRECATION")
            val top = if (Build.VERSION.SDK_INT >= 30) insets.getInsets(WindowInsets.Type.systemBars()).top
                      else insets.systemWindowInsetTop
            @Suppress("DEPRECATION")
            val bottom = if (Build.VERSION.SDK_INT >= 30) insets.getInsets(WindowInsets.Type.systemBars()).bottom
                         else insets.systemWindowInsetBottom
            v.setPadding(edge, edge + top, edge, edge + bottom)
            insets
        }
        setContentView(ScrollView(this).apply { isFillViewport = true; addView(root) })
        markSwatch()
    }

    private fun pickColour(rgb: Int) {
        mesh.setColour((rgb shr 16) and 0xFF, (rgb shr 8) and 0xFF, rgb and 0xFF)
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt("colour", rgb).apply()
        markSwatch()
        push()
    }

    private fun markSwatch() {
        val own = (mesh.colour[0] shl 16) or (mesh.colour[1] shl 8) or mesh.colour[2]
        val dp = resources.displayMetrics.density
        swatchViews.forEachIndexed { i, v ->
            (v.background as GradientDrawable).setStroke(if (SWATCHES[i] == own) (3 * dp).toInt() else 0, Color.WHITE)
        }
    }

    // ---- permissions and the radio ----

    private fun needed(): Array<String> =
        if (Build.VERSION.SDK_INT >= 31) arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_ADVERTISE)
        else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)

    override fun onResume() {
        super.onResume()
        val missing = needed().filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (missing.isNotEmpty()) {
            status.text = getString(R.string.need_permission)
            requestPermissions(missing.toTypedArray(), 1)
            return
        }
        startRadio()
    }

    override fun onRequestPermissionsResult(code: Int, perms: Array<out String>, results: IntArray) {
        if (code == 2) return                           // the microphone: the next press records
        if (results.isNotEmpty() && results.all { it == PackageManager.PERMISSION_GRANTED }) startRadio()
        else status.text = getString(R.string.need_permission)
    }

    private fun startRadio() {
        if (radio != null) return
        val r = Radio(this, { addr, value, rssi -> onReport(addr, value, rssi) }) { ok, msg ->
            status.text = if (ok) "" else getString(R.string.radio_error, msg)
        }
        if (!r.enabled) {
            status.text = getString(R.string.bt_off)
            @Suppress("DEPRECATION")
            try { startActivity(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE)) } catch (e: Exception) { }
            return
        }
        radio = r
        mesh.takeAdvertChange()
        r.start(mesh.advertValue())
        Log.i(TAG, "phone origin %02x".format(mesh.origin))
        ui.post(frame)
    }

    override fun onPause() {
        super.onPause()
        ui.removeCallbacks(frame)
        mesh.setHolding(false)
        radio?.update(mesh.advertValue())
        talkUp(false)
        radio?.stop()
        radio = null
        stopRingSound()
    }

    private fun onReport(addr: String, value: ByteArray?, rssi: Int) {
        for (e in mesh.onReport(addr, value, rssi, now())) {
            when (e) {
                is PhoneMesh.Event.Ring -> Log.i(TAG, "ring from %02x".format(e.origin))
                is PhoneMesh.Event.Answer -> { Log.i(TAG, "answer from %02x".format(e.origin)); play(RingTone.ANSWERED) }
                is PhoneMesh.Event.WaveHeard -> Log.i(TAG, "wave %d from %02x".format(e.kind, e.origin))
                is PhoneMesh.Event.ClipHeard -> hearClip(e)
            }
        }
    }

    // A peer's clip is on its periodic train: sync to it and play it, as the boards do (Radio.listenClip)
    private fun hearClip(e: PhoneMesh.Event.ClipHeard) {
        val r = radio ?: return
        if (!r.canHearClips) {
            Log.w(TAG, "clip ${e.id} from %02x: this phone cannot hear clips (${Hidden.why})".format(e.origin))
            return
        }
        status.text = getString(R.string.receiving)
        r.listenClip(e.addr, e.origin, e.id, e.ds) { bytes, khz ->
            if (bytes == null) { status.text = getString(R.string.clip_lost); return@listenClip }
            val pcm = Clip.adpcmDecode(bytes)
            status.text = getString(R.string.playing)
            Log.i(TAG, "clip ${e.id}: playing ${pcm.size / khz} ms at $khz kHz")
            play(pcm, khz * 1000, AudioAttributes.USAGE_MEDIA) { status.text = "" }
        }
    }

    // Send the advert now if it changed
    private fun push() {
        if (mesh.takeAdvertChange()) {
            radio?.update(mesh.advertValue())
            val s = mesh.state()
            Log.i(TAG, "advert origin=%02x seq=%d hold=%b rgb=%02x%02x%02x effect=%d/%d".format(
                s.origin, s.seq, s.holding, s.r, s.g, s.b, s.effect, s.effectSeq))
        }
    }

    private val frame = object : Runnable {
        override fun run() {
            val t = now()
            mesh.tick(t)
            push()
            radio?.clipTick(t)
            val n = mesh.table.peers.size
            near.text = resources.getQuantityString(R.plurals.near, n, n)
            if (mesh.ringing != shownRing) {
                shownRing = mesh.ringing
                if (shownRing != null) playRing() else stopRingSound()
            }
            if (t - lastLog > 10000) {
                lastLog = t
                Log.i(TAG, "peers=$n holders=${mesh.holders()} sent=${radio?.sent} heard=${radio?.heard}")
            }
            view.invalidate()
            ui.postDelayed(this, FRAME_MS)
        }
    }

    // ---- voice clips ----

    private fun talkDown() {
        val r = radio ?: return
        if (!r.canSendClips) { status.text = getString(R.string.no_clips); return }
        if (r.sendingClip || rec != null) return
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), 2)
            return
        }
        val max = Clip.RATE * Clip.MAX_MS / 1000
        val buf = maxOf(AudioRecord.getMinBufferSize(Clip.RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT), 4096)
        val a = try {
            AudioRecord(MediaRecorder.AudioSource.VOICE_RECOGNITION, Clip.RATE, AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT, buf)
        } catch (e: SecurityException) { return }
        if (a.state != AudioRecord.STATE_INITIALIZED) { a.release(); status.text = getString(R.string.no_mic); return }
        recPcm = ShortArray(max)
        recN = 0
        rec = a
        recStart = now()
        a.startRecording()
        talkBtn.text = getString(R.string.talking)
        recThread = Thread {
            val tmp = ShortArray(512)
            while (rec === a && recN < max) {
                val n = a.read(tmp, 0, minOf(tmp.size, max - recN))
                if (n <= 0) break
                System.arraycopy(tmp, 0, recPcm, recN, n)
                recN += n
            }
        }.also { it.start() }
    }

    private fun talkUp(send: Boolean) {
        val a = rec ?: return
        rec = null
        try { a.stop() } catch (e: Exception) { }
        recThread?.join(500)
        a.release()
        talkBtn.text = getString(R.string.talk)
        val n = recN and 1.inv()
        if (!send || n * 1000L / Clip.RATE < Clip.MIN_MS) return
        val r = radio ?: return
        val pcm = Clip.normalise(recPcm.copyOf(n))
        val clip = Clip.adpcmEncode(pcm)
        clipId = (clipId + 1) and 0xFF
        val ds = ((n * 10L + Clip.RATE / 2) / Clip.RATE).toInt()
        val pkts = Clip.packets(mesh.origin, clipId, clip, r.maxClipData)
        Log.i(TAG, "clip $clipId: ${n * 1000L / Clip.RATE} ms, ${clip.size} bytes, ${pkts.size} packets of up to ${pkts[0].size} chunks")
        talkBtn.text = getString(R.string.sending)
        r.sendClip(mesh.origin, clipId, ds, pkts) { ok ->
            talkBtn.text = getString(R.string.talk)
            if (!ok) status.text = getString(R.string.clip_failed)
        }
    }

    // ---- sound ----

    // A mono 16-bit clip, whole in memory (the ring tones at RingTone.RATE; a voice clip at its own rate)
    private fun track(pcm: ShortArray, rate: Int = RingTone.RATE, usage: Int = AudioAttributes.USAGE_NOTIFICATION_RINGTONE): AudioTrack {
        val content = if (usage == AudioAttributes.USAGE_MEDIA) AudioAttributes.CONTENT_TYPE_SPEECH
                      else AudioAttributes.CONTENT_TYPE_SONIFICATION
        val t = AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder().setUsage(usage).setContentType(content).build())
            .setAudioFormat(AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(rate).setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
            .setTransferMode(AudioTrack.MODE_STATIC)
            .setBufferSizeInBytes(pcm.size * 2)
            .build()
        t.write(pcm, 0, pcm.size)
        return t
    }

    private fun playRing() {
        stopRingSound()
        ringTrack = track(RingTone.render(RingTone.RING, times = 3, gapMs = 3200)).also { it.play() }
    }

    private fun stopRingSound() {
        ringTrack?.let { try { it.stop() } catch (e: Exception) { }; it.release() }
        ringTrack = null
    }

    private fun play(notes: Array<Pair<Int, Int>>) = play(RingTone.render(notes), RingTone.RATE)

    // Play once and free the track at the end
    private fun play(pcm: ShortArray, rate: Int, usage: Int = AudioAttributes.USAGE_NOTIFICATION_RINGTONE,
                     onEnd: () -> Unit = {}) {
        if (pcm.isEmpty()) return onEnd()
        val t = track(pcm, rate, usage)
        t.setNotificationMarkerPosition(pcm.size - 1)
        t.setPlaybackPositionUpdateListener(object : AudioTrack.OnPlaybackPositionUpdateListener {
            override fun onMarkerReached(tr: AudioTrack) { tr.release(); onEnd() }
            override fun onPeriodicNotification(tr: AudioTrack) {}
        })
        t.play()
    }
}
