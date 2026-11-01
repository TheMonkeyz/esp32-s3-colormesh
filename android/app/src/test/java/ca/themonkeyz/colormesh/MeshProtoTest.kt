package ca.themonkeyz.colormesh

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

// The phone's port of the protocol against tests/fixtures/mesh_vectors.txt, which the firmware's C code made (and
// tests/host/test_mesh.c checks): the same adverts, mixes, opposite colours and hold expiries.
class MeshProtoTest {
    private val vectors = File("../../tests/fixtures/mesh_vectors.txt").readLines().filter { !it.startsWith("#") }

    private fun hex(s: String) = ByteArray(s.length / 2) { s.substring(2 * it, 2 * it + 2).toInt(16).toByte() }

    @Test fun adverts() {
        val ads = vectors.filter { it.startsWith("ad ") }
        assertTrue(ads.size >= 8)
        for (line in ads) {
            val p = line.split(" ")
            val ad = hex(p[1])
            val want = p.drop(2).map { it.toInt() }.toIntArray()
            val st = MeshProto.decodeAd(ad)
            assertNotNull(line, st)
            assertArrayEquals(line, want, st!!.fields())
            assertArrayEquals(line, ad, MeshProto.encodeAd(st))
            assertArrayEquals(line, ad.copyOfRange(4, 30), MeshProto.encodeValue(st))
        }
    }

    @Test fun mixes() {
        val mixes = vectors.filter { it.startsWith("mix ") }
        assertTrue(mixes.size >= 20)
        for (line in mixes) {
            val (left, right) = line.removePrefix("mix ").split(" = ")
            val n = left.split(" ").map { it.toInt() }
            val cols = (0 until n[0]).map { intArrayOf(n[1 + 3 * it], n[2 + 3 * it], n[3 + 3 * it]) }
            assertArrayEquals(line, right.split(" ").map { it.toInt() }.toIntArray(), MeshProto.mix(cols))
        }
        assertArrayEquals(intArrayOf(0, 0, 0), MeshProto.mix(emptyList()))
    }

    @Test fun opposites() {
        val ops = vectors.filter { it.startsWith("opposite ") }
        assertTrue(ops.size >= 8)
        for (line in ops) {
            val (l, r) = line.removePrefix("opposite ").split(" = ").map { s -> s.split(" ").map { it.toInt() } }
            assertArrayEquals(line, r.toIntArray(), MeshProto.opposite(l[0], l[1], l[2]))
        }
    }

    @Test fun holdExpiry() {
        val holds = vectors.filter { it.startsWith("hold ") }
        assertTrue(holds.size >= 8)
        for (line in holds) {
            val (l, r) = line.removePrefix("hold ").split(" = ")
            val (ds, tbl) = l.split(" ").map { it.toLong() }
            assertEquals(line, r.toLong(), MeshProto.holdExpiryMs(ds.toInt(), tbl))
        }
    }

    @Test fun rejects() {
        val good = MeshProto.encodeValue(MeshProto.State(origin = 5))
        assertNotNull(MeshProto.decodeValue(good))
        assertNull(MeshProto.decodeValue(good.copyOf(25)))
        assertNull(MeshProto.decodeValue(good.copyOf().also { it[0] = 0x44 }))
        assertNull(MeshProto.decodeValue(null))
    }

    @Test fun tableSeqAndExpiry() {
        val t = MeshProto.Table()
        val h = MeshProto.State(seq = 5, flags = MeshProto.F_HOLDING, r = 255)
        assertTrue(t.update("A", h, 0, 1000))
        assertEquals(false, t.update("A", h.copy(seq = 4), 0, 1010))          // older: ignored
        assertEquals(1, t.holders())
        assertEquals(0, t.expire(1250))                                       // the old report at 1010 still counts as heard
        assertEquals(1, t.expire(1260))                                       // 250 ms after it: hold cleared
        val slow = MeshProto.State(seq = 1, flags = MeshProto.F_HOLDING, advDs = 10)
        t.update("B", slow, 0, 2000)
        assertEquals(0, t.expire(2399))
        assertEquals(1, t.expire(2400))                                       // 100 ms sender: 400 ms
        assertEquals(2, t.expire(20000))                                      // both gone after 10 s
        assertEquals(0, t.peers.size)
    }
}
