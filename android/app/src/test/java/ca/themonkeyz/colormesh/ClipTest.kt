package ca.themonkeyz.colormesh

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
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
        assertEquals(Clip.ROUNDS * chunks, pkts.sumOf { it.size })
        assertEquals((Clip.ROUNDS * chunks + 5) / 6, pkts.size)    // 90 packets: 9 s at 100 ms
        // each AD: [len] FF FF FF 43 'V' (the radio swaps in company 0xFFFF - k)
        val ad = pkts[0][1]
        assertEquals(ad.size - 1, ad[0].toInt() and 0xFF)
        assertEquals('V'.code, ad[5].toInt())
        assertEquals(1, ad[8].toInt())                              // chunk 1
        assertEquals(1, Clip.packets(1, 1, clip, 100)[0].size)      // a short limit: still one AD a packet
        assertEquals(Clip.MAX_PER_PKT, Clip.packets(1, 1, clip, 100000)[0].size)
    }

    @Test fun beaconAndNormalise() {
        assertArrayEquals(hex("08ffffff43423e091f"), Clip.beacon(0x3e, 9, 31))
        val quiet = ShortArray(100) { if (it == 50) 1000 else 0 }
        assertEquals(8000, Clip.normalise(quiet)[50].toInt())       // at most x8
        val loud = ShortArray(100) { if (it == 50) (-13000).toShort() else 0 }
        assertEquals(-26000, Clip.normalise(loud)[50].toInt())      // up to 26000
    }
}
