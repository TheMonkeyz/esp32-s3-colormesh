package ca.themonkeyz.colormesh

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

// The phone's voice clip code against tests/fixtures/clip_vectors.txt (made by the firmware's adpcm.c and
// clip_chunks.c), and the packets the phone sends: every chunk ROUNDS times, whole ADs, within the length
class ClipTest {
    private val lines = File("../../tests/fixtures/clip_vectors.txt").readLines().filter { !it.startsWith("#") }
    private fun hex(s: String) = ByteArray(s.length / 2) { s.substring(2 * it, 2 * it + 2).toInt(16).toByte() }
    private fun pcm(n: Int) = ShortArray(n) { i ->
        ((((i.toLong() * 1103515245L + 12345L) and 0xFFFFFFFFL) shr 16 and 0x3FFF) - 8192).toInt().toShort()
    }

    @Test fun adpcmCrcAndPack() {
        val want = hex(lines.first { it.startsWith("adpcm ") }.split(" ")[2])
        val code = Clip.adpcmEncode(pcm(1000))
        assertArrayEquals(want, code)
        val crc = lines.first { it.startsWith("crc ") }.split(" ")[1].toLong()
        assertEquals(crc, Clip.crc32(code))
        assertEquals(0xCBF43926L, Clip.crc32("123456789".toByteArray()))
        val packs = lines.filter { it.startsWith("pack ") }
        assertEquals(3, packs.size)
        for (line in packs) {
            val p = line.split(" ")
            assertArrayEquals(line, hex(p[4]), Clip.pack(p[1].toInt(), p[2].toInt(), code, crc, p[3].toInt()))
        }
    }

    @Test fun packets() {
        val clip = ByteArray(40000) { (it * 7).toByte() }
        val chunks = Clip.chunks(clip.size)
        assertEquals(179, chunks)
        val pkts = Clip.packets(0x3e, 5, clip, 1650)
        assertTrue(pkts.all { p -> p.size in 1..6 && p.sumOf { it.size } <= 1650 })
        assertEquals(Clip.ROUNDS * chunks, pkts.drop(Clip.LEAD).sumOf { it.size })
        assertEquals(Clip.LEAD + (Clip.ROUNDS * chunks + 5) / 6, pkts.size)
        assertTrue(pkts.take(Clip.LEAD + 1).all { it === pkts[0] })      // the lead-in: the first packet held
        val one = Clip.packets(0x3e, 5, ByteArray(Clip.RATE * 5 / 2), 251)   // what the phone sends: 5 s at 8 kHz
        assertTrue(one.all { it.size == 1 && it[0].size <= 251 })
        assertEquals(Clip.LEAD + Clip.ROUNDS * 90, one.size)        // 90 chunks a round: 9 s at 100 ms
        // each AD: [len] FF FF FF 43 'V' (the radio swaps in company 0xFFFF - k)
        val ad = pkts[0][1]
        assertEquals(ad.size - 1, ad[0].toInt() and 0xFF)
        assertEquals('V'.code, ad[5].toInt())
        assertEquals(1, ad[8].toInt())                              // chunk 1
        assertEquals(1, Clip.packets(1, 1, clip, 100)[0].size)      // a short limit: still one AD a packet
        assertEquals(Clip.MAX_PER_PKT, Clip.packets(1, 1, clip, 100000)[0].size)
    }

    // The decoder is step_apply from a fresh state: the first nibbles of the fixture's stream by hand (F, F, F, F:
    // steps 7, 16, 34, 73), and a smooth signal comes back close to what went in
    @Test fun decode() {
        assertArrayEquals(shortArrayOf(-11, -41, -104, -240), Clip.adpcmDecode(hex("ffff")))
        val sine = ShortArray(1600) { (8000 * kotlin.math.sin(it * 2 * Math.PI * 440 / 16000)).toInt().toShort() }
        val back = Clip.adpcmDecode(Clip.adpcmEncode(sine))
        assertEquals(sine.size, back.size)
        var err = 0L
        for (i in 100 until sine.size) err += kotlin.math.abs(sine[i] - back[i])
        assertTrue("mean error ${err / 1500}", err / 1500 < 200)
        assertEquals(2, Clip.adpcmDecode(hex("ffff"), 1).size)       // n bytes of a longer buffer
    }

    // clip_parse and the receiver: chunks in any order, repeats ignored, whole once every chunk is in and the CRC
    // matches; a packet of several ADs; a damaged chunk dropped
    @Test fun receive() {
        val clip = ByteArray(1000) { (it * 13).toByte() }
        val crc = Clip.crc32(clip)
        val total = Clip.chunks(clip.size)
        assertEquals(5, total)
        val rx = Clip.Rx(0x3e, 9)
        assertEquals(-1, rx.missing)
        for (c in listOf(3, 1, 1, 0, 4)) {
            val p = Clip.parse(Clip.pack(0x3e, 9, clip, crc, c))!!
            assertEquals(c, p.chunk)
            assertEquals(if (c == 4) 1000 - 4 * Clip.CHUNK_DATA else Clip.CHUNK_DATA, p.data.size)
            assertFalse(rx.add(p))
        }
        assertEquals(1, rx.missing)
        assertFalse(rx.add(Clip.parse(Clip.pack(0x3e, 8, clip, crc, 2))!!))       // another clip
        assertFalse(rx.add(Clip.parse(Clip.pack(0x3e, 9, clip, crc + 1, 2))!!))   // the same id, another clip
        assertTrue(rx.add(Clip.parse(Clip.pack(0x3e, 9, clip, crc, 2))!!))
        assertFalse(rx.active)
        assertArrayEquals(clip, rx.bytes())
        // a packet of two ADs (what a phone sends), each parsed on its own
        val two = Clip.pack(0x3e, 9, clip, crc, 0) + Clip.pack(0x3e, 9, clip, crc, 1)
        assertEquals(listOf(0, 1), Clip.ads(two).map { Clip.parse(it)!!.chunk })
        assertEquals(1, Clip.ads(two.copyOf(two.size - 1)).size)              // the second runs past the end
        assertTrue(Clip.ads(ByteArray(3)).isEmpty())                            // a zero length
        // not a chunk: a bad length byte, the wrong kind, a chunk past the end
        val ad = Clip.pack(0x3e, 9, clip, crc, 0)
        assertNull(Clip.parse(ad.copyOf(ad.size - 1)))
        assertNull(Clip.parse(ad.copyOf().also { it[5] = 'B'.code.toByte() }))
        assertNull(Clip.parse(ad.copyOf().also { it[8] = 5 }))
        assertNull(Clip.parse(Clip.beacon(0x3e, 9, 31)))
        // a damaged byte: whole, but the CRC fails
        val bad = Clip.Rx(0x3e, 9)
        for (c in 0 until total) {
            val p = Clip.pack(0x3e, 9, clip, crc, c)
            if (c == 2) p[Clip.HDR + 3] = (p[Clip.HDR + 3] + 1).toByte()
            assertFalse(bad.add(Clip.parse(p)!!))
        }
        assertFalse(bad.active)
        assertEquals(0, bad.missing)
        // the beacon of a phone's clip set, among other ADs
        val b = Clip.beaconParse(byteArrayOf(2, 1, 6) + Clip.beacon(0x3e, 9, 31))!!
        assertEquals(listOf(0x3e, 9, 31, 8), listOf(b.origin, b.id, b.ds, b.khz))
        assertNull(Clip.beaconParse(byteArrayOf(2, 1, 6)))
        val t = Clip.beaconParse(hex("09ffffff43549a073210"))!!         // a board's train beacon ('T', 16 kHz)
        assertEquals(listOf(0x9a, 7, 50, 16), listOf(t.origin, t.id, t.ds, t.khz))
    }

    @Test fun beaconAndNormalise() {
        assertArrayEquals(hex("09ffffff43423e091f08"), Clip.beacon(0x3e, 9, 31))   // 8 kHz
        val quiet = ShortArray(100) { if (it == 50) 1000 else 0 }
        assertEquals(8000, Clip.normalise(quiet)[50].toInt())       // at most x8
        val loud = ShortArray(100) { if (it == 50) (-13000).toShort() else 0 }
        assertEquals(-26000, Clip.normalise(loud)[50].toInt())      // up to 26000
    }
}
