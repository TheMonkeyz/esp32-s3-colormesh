package ca.themonkeyz.colormesh

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.RectF
import android.text.Layout
import android.text.StaticLayout
import android.text.TextPaint
import android.view.MotionEvent
import android.view.View
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.min

// The main circle, like the board's main screen: the mix of every holder (the phone's own colour as an edge ring),
// the waves, the peers as dots around the edge, and the call overlays. Touch: the finger down = hold (the phone's
// colour joins the mix), a short tap = a wave from that zone (centre: rings; near an edge: bands from that side).
// While a call rings, a tap answers; while the phone's own call shows, a tap closes it.
class MeshView(ctx: Context) : View(ctx) {
    var mesh: PhoneMesh? = null
    var now: () -> Long = { System.currentTimeMillis() }
    var onHold: (Boolean) -> Unit = {}
    var onWave: (Int) -> Unit = {}
    var onRingTap: () -> Unit = {}
    var onCallTap: () -> Unit = {}

    private val dp = resources.displayMetrics.density
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val text = TextPaint(Paint.ANTI_ALIAS_FLAG).apply { textSize = 20 * dp; color = 0xFFF2F4F7.toInt() }
    private val bmpN = 160
    private val bmp = Bitmap.createBitmap(bmpN, bmpN, Bitmap.Config.ARGB_8888)
    private val px = IntArray(bmpN * bmpN)
    private val bmpPaint = Paint(Paint.FILTER_BITMAP_FLAG)

    private var downMs = 0L
    private var downX = 0f
    private var downY = 0f
    private var moved = false
    private var held = false

    override fun onMeasure(wSpec: Int, hSpec: Int) {
        val w = MeasureSpec.getSize(wSpec)
        val h = MeasureSpec.getSize(hSpec)
        val s = if (MeasureSpec.getMode(hSpec) == MeasureSpec.UNSPECIFIED) w else min(w, h)
        setMeasuredDimension(w, s)
    }

    private fun cx() = width / 2f
    private fun cy() = height / 2f
    private fun radius() = min(width, height) / 2f - 14 * dp

    override fun onDraw(c: Canvas) {
        val m = mesh ?: return
        val t = now()
        val r = radius()
        val disc = RectF(cx() - r, cy() - r, cx() + r, cy() + r)
        val mix = m.mix()
        val anyHold = m.holders() > 0
        if (m.waves.isNotEmpty()) {
            WaveField.render(px, bmpN, m.waves, t, mix, anyHold)
            bmp.setPixels(px, 0, bmpN, 0, 0, bmpN, bmpN)
            c.save()
            val clip = android.graphics.Path().apply { addOval(disc, android.graphics.Path.Direction.CW) }
            c.clipPath(clip)
            c.drawBitmap(bmp, Rect(0, 0, bmpN, bmpN), disc, bmpPaint)
            c.restore()
        } else {
            fill.color = if (anyHold) Color.rgb(mix[0], mix[1], mix[2]) else Color.BLACK
            c.drawOval(disc, fill)
        }
        // the own colour as an edge ring (WAVE_RING_PX 14 of 233 on the board)
        stroke.strokeWidth = r * 14f / 233f
        stroke.color = Color.rgb(m.colour[0], m.colour[1], m.colour[2])
        c.drawCircle(cx(), cy(), r - stroke.strokeWidth / 2, stroke)
        // the peers: one dot each around the outside, full when holding
        val peers = m.table.peers.sortedBy { it.st.origin }
        peers.forEachIndexed { i, p ->
            val a = Math.toRadians(-90.0 + 360.0 * i / maxOf(peers.size, 1))
            val x = cx() + (r + 8 * dp) * Math.cos(a).toFloat()
            val y = cy() + (r + 8 * dp) * Math.sin(a).toFloat()
            fill.color = Color.rgb(p.st.r, p.st.g, p.st.b)
            fill.alpha = if (p.st.holding) 255 else 110
            c.drawCircle(x, y, (if (p.st.holding) 6 else 4) * dp, fill)
            fill.alpha = 255
        }
        // overlays: incoming call, own call, else the hint while nobody holds
        val ring = m.ringing
        val phase = m.callPhase(t)
        val ink = if (anyHold && luma(mix) > 150) 0xFF1A1A1A.toInt() else 0xFFF2F4F7.toInt()
        when {
            ring != null -> {
                fill.color = Color.rgb(ring.rgb[0], ring.rgb[1], ring.rgb[2])
                c.drawCircle(cx(), cy() - r * 0.35f, r * 0.12f, fill)
                centreText(c, context.getString(R.string.call_in) + "\n\n" + context.getString(R.string.call_answer), ink, r)
            }
            phase != PhoneMesh.CallPhase.NONE -> {
                val dots = m.answers
                dots.forEachIndexed { i, a ->
                    fill.color = Color.rgb(a.second[0], a.second[1], a.second[2])
                    c.drawCircle(cx() + (i - (dots.size - 1) / 2f) * 26 * dp, cy() - r * 0.35f, 9 * dp, fill)
                }
                val head = context.getString(if (phase == PhoneMesh.CallPhase.ANSWERED) R.string.call_answered else R.string.call_out)
                centreText(c, head + "\n\n" + context.getString(R.string.call_close), ink, r)
            }
            !anyHold && m.waves.isEmpty() -> centreText(c, context.getString(R.string.hold_hint), 0xFF8B95A1.toInt(), r)
        }
    }

    private fun luma(c: IntArray) = (c[0] * 299 + c[1] * 587 + c[2] * 114) / 1000

    private fun centreText(c: Canvas, s: String, color: Int, r: Float) {
        text.color = color
        val w = (r * 1.3f).toInt()
        val l = StaticLayout.Builder.obtain(s, 0, s.length, text, w).setAlignment(Layout.Alignment.ALIGN_CENTER).build()
        c.save()
        c.translate(cx() - w / 2f, cy() - l.height / 2f)
        l.draw(c)
        c.restore()
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        val m = mesh ?: return false
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                if (hypot(e.x - cx(), e.y - cy()) > radius() + 20 * dp) return false
                downMs = now()
                downX = e.x
                downY = e.y
                moved = false
                held = m.ringing == null && m.callPhase(downMs) == PhoneMesh.CallPhase.NONE
                if (held) onHold(true)
                parent?.requestDisallowInterceptTouchEvent(true)
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                if (hypot(e.x - downX, e.y - downY) > 24 * dp) moved = true
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                val t = now()
                val tap = e.actionMasked == MotionEvent.ACTION_UP && !moved && t - downMs < 300
                if (held) onHold(false)                 // always released, even if a call came in meanwhile
                when {
                    !tap -> {}
                    m.ringing != null -> onRingTap()
                    m.callPhase(t) != PhoneMesh.CallPhase.NONE -> onCallTap()
                    held -> onWave(zone(downX, downY))
                }
                held = false
                return true
            }
        }
        return false
    }

    // The board's zones: within TAP_CENTRE_R (100 of 233) of the centre = rings, else the nearest side's bands
    private fun zone(x: Float, y: Float): Int {
        val dx = x - cx()
        val dy = y - cy()
        if (hypot(dx, dy) < radius() * 100f / 233f) return 1
        return if (abs(dx) > abs(dy)) (if (dx < 0) 2 else 3) else (if (dy < 0) 4 else 5)
    }
}
