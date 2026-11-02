package ca.themonkeyz.colormesh

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.random.Random

// The phone as a member: its advert, the effects it sends and hears, calls and answers, an origin clash
class PhoneMeshTest {
    private fun board(origin: Int, seq: Int, hold: Boolean = false, effect: Int = 0, effectSeq: Int = 0,
                      answerTo: Int = 0, rgb: IntArray = intArrayOf(0, 255, 0)) =
        MeshProto.encodeValue(MeshProto.State(origin = origin, seq = seq, flags = if (hold) MeshProto.F_HOLDING else 0,
            r = rgb[0], g = rgb[1], b = rgb[2], effect = effect, effectSeq = effectSeq, answerTo = answerTo, advDs = 2))

    @Test fun ownAdvert() {
        val m = PhoneMesh(0x42, Random(1))
        assertTrue(m.takeAdvertChange())
        assertFalse(m.takeAdvertChange())
        m.setColour(0, 0, 255)
        m.setHolding(true)
        assertTrue(m.takeAdvertChange())
        val st = MeshProto.decodeValue(m.advertValue())!!
        assertEquals(0x42, st.origin)
        assertTrue(st.holding)
        assertEquals(255, st.b)
        assertEquals(10, st.advDs)                       // byte 29: 100 ms
        assertEquals(MeshProto.TARGET_NONE, st.target)   // never a firmware peer
        assertEquals(0, st.flags and 0xFE)               // no dev, servable, clip...
        assertEquals(0, st.ver.sum())
        m.setHolding(true)
        assertFalse(m.takeAdvertChange())                // no change, no seq bump
    }

    @Test fun waveSentForOneSecond() {
        val m = PhoneMesh(0x42, Random(1))
        val s0 = MeshProto.decodeValue(m.advertValue())!!.effectSeq
        m.startWave(1, 1000)
        assertEquals(1, m.waves.size)
        var st = MeshProto.decodeValue(m.advertValue())!!
        assertEquals(1, st.effect)
        assertEquals((s0 + 1) and 0xFF, st.effectSeq)
        m.tick(1999)
        assertEquals(1, MeshProto.decodeValue(m.advertValue())!!.effect)
        m.tick(2000)
        st = MeshProto.decodeValue(m.advertValue())!!
        assertEquals(0, st.effect)
        m.tick(5000)
        assertEquals(0, m.waves.size)                    // the 4 s wave is over
    }

    @Test fun peersMixAndWaves() {
        val m = PhoneMesh(0x42, Random(1))
        m.setColour(255, 0, 0)
        m.onReport("AA", board(0x9a, 1, hold = true), -50, 0)
        assertEquals(1, m.table.peers.size)
        m.setHolding(true)
        val mix = m.mix()
        assertEquals(listOf(255, 255, 0), mix.toList())  // red + green
        val ev = m.onReport("AA", board(0x9a, 2, effect = 3, effectSeq = 7), -50, 100)
        assertEquals(listOf(PhoneMesh.Event.WaveHeard(3, 0x9a)), ev)
        assertTrue(m.onReport("AA", board(0x9a, 3, effect = 3, effectSeq = 7), -50, 120).isEmpty())   // same wave
        assertEquals(1, m.waves.size)
        assertEquals(1, m.onReport("AA", board(0x9a, 4, effect = 4, effectSeq = 8), -50, 140).size)    // a new one
    }

    @Test fun ringAndAnswer() {
        val m = PhoneMesh(0x42, Random(1))
        val ev = m.onReport("AA", board(0x9a, 1, effect = MeshProto.EFFECT_RING, effectSeq = 1), -50, 0)
        assertTrue(ev[0] is PhoneMesh.Event.Ring)
        assertEquals(0x9a, m.ringing!!.origin)
        m.stopRing(true, 500)
        assertNull(m.ringing)
        val st = MeshProto.decodeValue(m.advertValue())!!
        assertEquals(MeshProto.EFFECT_ANSWER, st.effect)
        assertEquals(0x9a, st.answerTo)
        // the phone calls: answers to the phone count once each, answers to others not at all
        m.ring(10000)
        assertEquals(MeshProto.EFFECT_RING, MeshProto.decodeValue(m.advertValue())!!.effect)
        assertEquals(PhoneMesh.CallPhase.OUT, m.callPhase(10000))
        assertTrue(m.onReport("BB", board(0x76, 1, effect = 7, effectSeq = 3, answerTo = 0x11), -50, 11000).isEmpty())
        assertEquals(1, m.onReport("BB", board(0x76, 2, effect = 7, effectSeq = 4, answerTo = 0x42), -50, 11100).size)
        assertTrue(m.onReport("BB", board(0x76, 3, effect = 7, effectSeq = 5, answerTo = 0x42), -50, 13500).isEmpty())
        assertEquals(1, m.answers.size)
        assertEquals(PhoneMesh.CallPhase.ANSWERED, m.callPhase(12000))
        m.cancelCall()
        assertEquals(PhoneMesh.CallPhase.ANSWERED, m.callPhase(12000))   // "Answered" stays its 4 s
        assertEquals(PhoneMesh.CallPhase.NONE, m.callPhase(15200))
        m.onReport("AA", board(0x9a, 9, effect = MeshProto.EFFECT_RING, effectSeq = 9), -50, 20000)
        m.tick(20000 + PhoneMesh.RING_MS)
        assertNull(m.ringing)                            // a ring nobody answers ends
    }

    @Test fun originClash() {
        val m = PhoneMesh(0x9a, Random(3))
        m.takeAdvertChange()
        m.onReport("AA", board(0x9a, 1), -50, 0)
        assertNotEquals(0x9a, m.origin)
        assertTrue(m.takeAdvertChange())
        assertEquals(m.origin, MeshProto.decodeValue(m.advertValue())!!.origin)
    }

    @Test fun peerHoldLastsForASlowScan() {
        val m = PhoneMesh(0x42, Random(1))
        m.onReport("AA", board(0x9a, 1, hold = true), -50, 0)
        m.tick(500)
        assertEquals(1, m.holders())                     // the phone keeps a board's hold 600 ms
        m.tick(600)
        assertEquals(0, m.holders())
    }

    // A board's clip on air (F_CLIP + id): one event per clip id per address; a peer that left starts afresh
    @Test fun clipHeardOncePerId() {
        val m = PhoneMesh(0x42, Random(1))
        fun adv(seq: Int, clip: Boolean, id: Int) = MeshProto.encodeValue(MeshProto.State(origin = 0x9a, seq = seq,
            flags = if (clip) MeshProto.F_CLIP else 0, clipId = id, clipDs = 50, advDs = 2))
        assertTrue(m.onReport("AA", adv(1, false, 0), -50, 1000).isEmpty())
        assertEquals(listOf<PhoneMesh.Event>(PhoneMesh.Event.ClipHeard("AA", 0x9a, 7, 50)),
            m.onReport("AA", adv(2, true, 7), -50, 1100))
        assertTrue(m.onReport("AA", adv(3, true, 7), -50, 1200).isEmpty())        // the same clip, still on air
        assertTrue(m.onReport("AA", adv(4, false, 7), -50, 1300).isEmpty())       // over
        assertEquals(1, m.onReport("AA", adv(5, true, 8), -50, 1400).size)        // the next one
        assertEquals(1, m.onReport("BB", adv(5, true, 8), -50, 1400).size)        // another board, its own ids
        m.tick(20000)                                                             // both left (10 s)
        assertEquals(1, m.onReport("AA", adv(1, true, 8), -50, 20100).size)       // a restarted board: id 8 is new
    }
}
