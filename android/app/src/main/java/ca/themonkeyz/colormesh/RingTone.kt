package ca.themonkeyz.colormesh

import kotlin.math.sin
import kotlin.math.sqrt

// The call tone and the "answered" doo-di-dit: a port of main/ringtone.c (timing, buzzer response, 2 ms anti-click
// fades), so the phone rings like the boards. Pure Kotlin: whole sequences rendered to 16-bit mono at RATE.
object RingTone {
    const val RATE = 16000
    private const val E8 = 117
    private const val Q4 = 242
    private const val LAST = 813
    private const val FADE = 32
    private const val PARTIAL_MAX_HZ = 7800
    private const val PARTIALS = 16
    private const val RMS = 0.3

    val RING = arrayOf(
        1319 to E8, 1175 to E8, 740 to Q4, 831 to Q4,
        1109 to E8, 988 to E8, 587 to Q4, 659 to Q4,
        988 to E8, 880 to E8, 554 to Q4, 659 to Q4,
        880 to LAST,
    )
    val ANSWERED = arrayOf(880 to 90, 1109 to 90, 1319 to 140)

    private val resp = doubleArrayOf(
        0.0014, 0.0044, 0.0092, 0.0161, 0.0272, 0.0526, 0.0981, 0.1342, 0.1907, 0.2097, 0.1680, 0.1256,
        0.1312, 0.1171, 0.1205, 0.1684, 0.1722, 0.3128, 0.2602, 0.3009, 0.2159, 0.0837, 0.0457, 0.0179,
        0.0267, 0.0269, 0.0225, 0.0110, 0.0070, 0.0071,
    )

    private fun response(hz: Double): Double {
        val x = (hz - 625.0) / 250.0
        if (x <= 0) return resp[0]
        if (x >= resp.size - 1) return resp[resp.size - 1]
        val i = x.toInt()
        return resp[i] + (resp[i + 1] - resp[i]) * (x - i)
    }

    private fun len(ms: Int) = ms * RATE / 1000

    // The notes, then `gapMs` of silence, `times` times (no gap after the last)
    fun render(notes: Array<Pair<Int, Int>>, times: Int = 1, gapMs: Int = 0, amp: Int = 20000): ShortArray {
        val one = notes.sumOf { len(it.second) }
        val gap = len(gapMs)
        val out = ShortArray(one * times + gap * (times - 1))
        var pos = 0
        repeat(times) { rep ->
            var phase = 0.0
            for ((f, ms) in notes) {
                val w = ArrayList<Double>()
                var sq = 0.0
                var k = 1
                while (k <= PARTIALS && k * f < PARTIAL_MAX_HZ) {
                    val v = response(k.toDouble() * f)
                    w.add(v)
                    sq += v * v / 2
                    k++
                }
                val g = if (sq > 0) RMS / sqrt(sq) else 0.0
                val n = len(ms)
                for (at in 0 until n) {
                    val fade = when {
                        at < FADE -> at.toDouble() / FADE
                        n - at <= FADE -> (n - at).toDouble() / FADE
                        else -> 1.0
                    }
                    var s = 0.0
                    for (j in w.indices) s += w[j] * g * sin((j + 1) * phase)
                    out[pos++] = (s.coerceIn(-1.0, 1.0) * fade * amp).toInt().toShort()
                    phase += 2 * Math.PI * f / RATE
                    if (phase >= 2 * Math.PI) phase -= 2 * Math.PI
                }
            }
            if (rep < times - 1) pos += gap
        }
        return out
    }
}
