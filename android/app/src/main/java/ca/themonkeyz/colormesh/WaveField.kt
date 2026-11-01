package ca.themonkeyz.colormesh

import kotlin.math.cos
import kotlin.math.sqrt

// The disc's pixels with waves (a port of main/wave.c's rule, in floating point): the holders' mix and every active
// wave summed in linear light, the largest channel full, then scaled by min(1, holding + the waves' weights) so a
// lone wave fades to black. Geometry in the board's 466 px frame, so the phone shows the same pattern at any size.
// Each wave's colour at distance d from its origin: crest = colour A, trough = colour B,
// blend (1 - cos(2 pi (d - speed t) / pitch)) / 2. Pure Kotlin: drawn into an ARGB IntArray, n x n.
object WaveField {
    const val FRAME = 466.0
    const val PITCH = 64.0
    const val SPEED = 160.0                // px/s
    const val FADE_MS = 500L

    fun render(out: IntArray, n: Int, waves: List<PhoneMesh.Wave>, nowMs: Long, mix: IntArray, holding: Boolean) {
        val active = waves.filter { nowMs - it.startMs in 0 until PhoneMesh.WAVE_DURATION_MS }
        val wt = DoubleArray(active.size) {
            val t = nowMs - active[it].startMs
            val left = PhoneMesh.WAVE_DURATION_MS - t
            if (left < FADE_MS) left.toDouble() / FADE_MS else 1.0
        }
        val la = active.map { lin(it.a) }
        val lb = active.map { lin(it.b) }
        val hold = if (holding) lin(mix) else doubleArrayOf(0.0, 0.0, 0.0)
        val scale = minOf(1.0, (if (holding) 1.0 else 0.0) + wt.sum())
        val off = active.map { (nowMs - it.startMs) * SPEED / 1000.0 }
        val step = FRAME / n
        val r2 = (FRAME / 2) * (FRAME / 2)
        for (y in 0 until n) {
            val fy = (y + 0.5) * step
            for (x in 0 until n) {
                val fx = (x + 0.5) * step
                val cx = fx - FRAME / 2
                val cy = fy - FRAME / 2
                if (cx * cx + cy * cy > r2) { out[y * n + x] = 0xFF000000.toInt(); continue }
                var sr = hold[0]
                var sg = hold[1]
                var sb = hold[2]
                for (i in active.indices) {
                    val d = when (active[i].kind) {
                        1 -> sqrt(cx * cx + cy * cy)
                        2 -> fx
                        3 -> FRAME - fx
                        4 -> fy
                        else -> FRAME - fy
                    }
                    val c = (1 - cos(2 * Math.PI * (d - off[i]) / PITCH)) / 2
                    sr += wt[i] * (la[i][0] * (1 - c) + lb[i][0] * c)
                    sg += wt[i] * (la[i][1] * (1 - c) + lb[i][1] * c)
                    sb += wt[i] * (la[i][2] * (1 - c) + lb[i][2] * c)
                }
                val mx = maxOf(sr, sg, sb)
                out[y * n + x] = if (mx <= 0) 0xFF000000.toInt()
                else argb(srgb(sr / mx * scale), srgb(sg / mx * scale), srgb(sb / mx * scale))
            }
        }
    }

    private fun lin(c: IntArray) = DoubleArray(3) { MeshProto.toLin(c[it]) / 65535.0 }
    private fun srgb(l: Double) = MeshProto.toSrgb((l * 65535).toLong().coerceIn(0, 65535))
    private fun argb(r: Int, g: Int, b: Int) = (0xFF shl 24) or (r shl 16) or (g shl 8) or b
}
