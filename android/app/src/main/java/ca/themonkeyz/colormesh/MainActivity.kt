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
import android.media.AudioTrack
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.Gravity
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
        root.addView(callBtn, LinearLayout.LayoutParams((220 * dp).toInt(), (56 * dp).toInt()).apply {
            gravity = Gravity.CENTER_HORIZONTAL; topMargin = (12 * dp).toInt()
        })
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
            }
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

    // ---- sound ----

    private fun track(pcm: ShortArray): AudioTrack {
        val t = AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE)
                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build())
            .setAudioFormat(AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(RingTone.RATE).setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
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

    private fun play(notes: Array<Pair<Int, Int>>) {
        val pcm = RingTone.render(notes)
        val t = track(pcm)
        t.setNotificationMarkerPosition(pcm.size - 1)
        t.setPlaybackPositionUpdateListener(object : AudioTrack.OnPlaybackPositionUpdateListener {
            override fun onMarkerReached(tr: AudioTrack) { tr.release() }
            override fun onPeriodicNotification(tr: AudioTrack) {}
        })
        t.play()
    }
}
