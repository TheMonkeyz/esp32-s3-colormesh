package ca.themonkeyz.colormesh

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs

// The ring tone's length and level, and the wave field's disc, mix and fade
class RenderTest {
    @Test fun ringTone() {
        val one = RingTone.render(RingTone.RING)
        assertEquals(2967 * 16, one.size)                // ringtone_samples(): 2967 ms at 16 kHz
        val three = RingTone.render(RingTone.RING, times = 3, gapMs = 3200)
        assertEquals(3 * 2967 * 16 + 2 * 3200 * 16, three.size)
        assertTrue(one.maxOf { abs(it.toInt()) } in 5000..20000)
        assertEquals(0, one[0].toInt())                  // the fade in starts at 0
        assertEquals(320 * 16, RingTone.render(RingTone.ANSWERED).size)
    }

    @Test fun waveField() {
        val n = 32
        val px = IntArray(n * n)
        WaveField.render(px, n, emptyList(), 0, intArrayOf(0, 0, 255), true)
        assertEquals(0xFF000000.toInt(), px[0])          // corner: outside the disc
        assertEquals(0xFF0000FF.toInt(), px[n / 2 * n + n / 2])   // a holder alone: its colour
        val w = PhoneMesh.Wave(1, intArrayOf(255, 0, 0), intArrayOf(0, 255, 255), 0)
        WaveField.render(px, n, listOf(w), 1000, intArrayOf(0, 0, 0), false)
        assertNotEquals(0xFF000000.toInt(), px[n / 2 * n + n / 2])
        WaveField.render(px, n, listOf(w), 3999, intArrayOf(0, 0, 0), false)
        val faded = px[n / 2 * n + n / 2]
        assertTrue(maxOf(faded shr 16 and 0xFF, faded shr 8 and 0xFF, faded and 0xFF) < 60)   // almost black at the end
    }
}
