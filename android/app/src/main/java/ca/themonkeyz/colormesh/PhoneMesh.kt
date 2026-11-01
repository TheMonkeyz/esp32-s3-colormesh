package ca.themonkeyz.colormesh

import kotlin.random.Random

// The phone as a mesh member: its own advert (colour, hold, waves, calls), the peers heard, their waves and calls.
// The rules follow main/mesh.c (peer_effect, do_refresh) and main/calls.c. Pure Kotlin, no Android, single thread
// (the activity's main thread); time is an argument. The radio (Radio.kt) sends advertValue() whenever
// takeAdvertChange() says it changed, and hands every ColorMesh report to onReport().
class PhoneMesh(origin: Int, private val rnd: Random = Random.Default) {
    companion object {
        const val ADV_DS = 10                       // byte 29: Android advertises every 100 ms at best
        const val PEER_HOLD_MS = 600L               // the phone hears fewer reports than a board: a longer floor
        const val WAVE_MAX = 8
        const val WAVE_DURATION_MS = 4000L
        const val CALL_OUT_MS = 3 * 2970L + 2 * 3200L + 2000L   // calls.h: the callees' ring + a margin
        const val ANSWERED_MS = 4000L
        const val RING_MS = 3 * 2970L + 2 * 3200L               // the incoming ring: 3 phrases, 2 gaps
        const val CALL_MAX_ANS = 8
    }

    data class Wave(val kind: Int, val a: IntArray, val b: IntArray, val startMs: Long)
    data class Caller(val origin: Int, val rgb: IntArray, val startMs: Long)
    sealed class Event {
        data class WaveHeard(val kind: Int, val origin: Int) : Event()
        data class Ring(val origin: Int, val rgb: IntArray) : Event()
        data class Answer(val origin: Int, val rgb: IntArray) : Event()
    }
    enum class CallPhase { NONE, OUT, ANSWERED }

    var origin = origin and 0xFF
        private set
    var colour = intArrayOf(255, 0, 0)
        private set
    var holding = false
        private set
    val table = MeshProto.Table(holdExpiryMs = PEER_HOLD_MS)
    val waves = ArrayList<Wave>()

    private var seq = rnd.nextInt(256)
    private var effect = MeshProto.EFFECT_NONE
    private var effectSeq = rnd.nextInt(256)
    private var effectEndMs = 0L
    private var answerTo = 0
    private var advDirty = true

    private class Eff(var active: Boolean, var seq: Int, var lastMs: Long)
    private val eff = HashMap<String, Eff>()

    // Outgoing call (calls.c) and an incoming ring
    private var callActive = false
    private var callStartMs = 0L
    private var callLastMs = 0L
    val answers = ArrayList<Pair<Int, IntArray>>()
    var ringing: Caller? = null
        private set

    // ---- own state ----

    fun state(): MeshProto.State = MeshProto.State(
        origin = origin, seq = seq, flags = if (holding) MeshProto.F_HOLDING else 0,
        r = colour[0], g = colour[1], b = colour[2], target = MeshProto.TARGET_NONE,
        effect = effect, effectSeq = effectSeq, answerTo = if (effect == MeshProto.EFFECT_ANSWER) answerTo else 0,
        advDs = ADV_DS)

    fun advertValue(): ByteArray = MeshProto.encodeValue(state())

    // True once after each change of the own advert
    fun takeAdvertChange(): Boolean = advDirty.also { advDirty = false }

    private fun changed() {
        seq = (seq + 1) and 0xFF
        advDirty = true
    }

    fun setColour(r: Int, g: Int, b: Int) {
        if (colour[0] == r && colour[1] == g && colour[2] == b) return
        colour = intArrayOf(r, g, b)
        changed()
    }

    fun setHolding(on: Boolean) {
        if (holding == on) return
        holding = on
        changed()
    }

    private fun startEffect(e: Int, nowMs: Long) {
        effect = e
        effectSeq = (effectSeq + 1) and 0xFF
        effectEndMs = nowMs + MeshProto.EFFECT_MS
        changed()
    }

    // A wave of the own colour here at once and on every board (kind 1-5: centre, left, right, top, bottom)
    fun startWave(kind: Int, nowMs: Long) {
        if (kind < MeshProto.EFFECT_WAVE_CENTRE || kind > MeshProto.EFFECT_WAVE_BOTTOM) return
        addWave(kind, colour, null, nowMs)
        startEffect(kind, nowMs)
    }

    // A call: every board rings
    fun ring(nowMs: Long) {
        callActive = true
        callStartMs = nowMs
        callLastMs = 0L
        answers.clear()
        startEffect(MeshProto.EFFECT_RING, nowMs)
    }

    fun cancelCall() {
        callActive = false
        answers.clear()
    }

    // Answer the ringing call (answer = true) or stop the ring
    fun stopRing(answer: Boolean, nowMs: Long) {
        val c = ringing ?: return
        ringing = null
        if (answer) {
            answerTo = c.origin
            startEffect(MeshProto.EFFECT_ANSWER, nowMs)
        }
    }

    fun callPhase(nowMs: Long): CallPhase {
        if (callActive && nowMs - callStartMs >= CALL_OUT_MS) callActive = false
        if (callLastMs != 0L && nowMs - callLastMs < ANSWERED_MS) return CallPhase.ANSWERED
        return if (callActive) CallPhase.OUT else CallPhase.NONE
    }

    // ---- what the phone hears ----

    fun onReport(addr: String, value: ByteArray?, rssi: Int, nowMs: Long): List<Event> {
        val st = MeshProto.decodeValue(value) ?: return emptyList()
        if (st.origin == origin) {
            if (st.seq == seq && st.r == colour[0] && st.g == colour[1] && st.b == colour[2]) return emptyList()  // own echo
            pickOrigin(st.origin)                   // a board uses this origin: take another one
        }
        table.update(addr, st, rssi, nowMs)
        return peerEffect(addr, st, nowMs)
    }

    // A free origin byte (not 0, not 0xFF, not a peer's)
    private fun pickOrigin(taken: Int) {
        val used = table.peers.map { it.st.origin }.toHashSet().apply { add(taken) }
        var o: Int
        do o = 1 + rnd.nextInt(254) while (o in used)
        origin = o
        changed()
    }

    private fun peerEffect(addr: String, st: MeshProto.State, nowMs: Long): List<Event> {
        var e = eff[addr]
        if (e == null) {
            if (st.effect == MeshProto.EFFECT_NONE) return emptyList()
            e = Eff(false, 0, nowMs)
            eff[addr] = e
        }
        if (e.active && nowMs - e.lastMs > 2 * MeshProto.EFFECT_MS) e.active = false   // a long gap: forget it
        e.lastMs = nowMs
        if (st.effect == MeshProto.EFFECT_NONE) {
            e.active = false
            return emptyList()
        }
        if (e.active && e.seq == st.effectSeq) return emptyList()
        e.active = true
        e.seq = st.effectSeq
        val rgb = intArrayOf(st.r, st.g, st.b)
        return when (st.effect) {
            MeshProto.EFFECT_ANSWER -> {
                if (st.answerTo != origin || !callActive) return emptyList()   // another board's call
                if (answers.any { it.first == st.origin } || answers.size >= CALL_MAX_ANS) return emptyList()
                answers.add(st.origin to rgb)
                callLastMs = nowMs
                listOf(Event.Answer(st.origin, rgb))
            }
            MeshProto.EFFECT_RING -> {
                ringing = Caller(st.origin, rgb, nowMs)
                listOf(Event.Ring(st.origin, rgb))
            }
            in MeshProto.EFFECT_WAVE_CENTRE..MeshProto.EFFECT_WAVE_BOTTOM -> {
                val b2 = if (st.b2r or st.b2g or st.b2b != 0) intArrayOf(st.b2r, st.b2g, st.b2b) else null
                addWave(st.effect, rgb, b2, nowMs)
                listOf(Event.WaveHeard(st.effect, st.origin))
            }
            else -> emptyList()                     // a kind this app does not know
        }
    }

    private fun addWave(kind: Int, a: IntArray, b2: IntArray?, nowMs: Long) {
        if (waves.size >= WAVE_MAX) waves.removeAt(0)
        waves.add(Wave(kind, a.copyOf(), b2 ?: MeshProto.opposite(a[0], a[1], a[2]), nowMs))
    }

    // Every frame: expiry, the effect's end, finished waves and rings. True when the peers or the mix changed.
    fun tick(nowMs: Long): Boolean {
        val n = table.expire(nowMs)
        if (effect != MeshProto.EFFECT_NONE && nowMs >= effectEndMs) {
            effect = MeshProto.EFFECT_NONE
            changed()
        }
        waves.removeAll { nowMs - it.startMs >= WAVE_DURATION_MS }
        ringing?.let { if (nowMs - it.startMs >= RING_MS) ringing = null }
        eff.entries.removeAll { nowMs - it.value.lastMs > MeshProto.PEER_EXPIRY_MS }
        return n > 0
    }

    // The mix of every holder, the phone included
    fun mix(): IntArray {
        val c = table.peers.filter { it.st.holding }.map { intArrayOf(it.st.r, it.st.g, it.st.b) }.toMutableList()
        if (holding) c.add(colour)
        return MeshProto.mix(c)
    }

    fun holders() = table.holders() + if (holding) 1 else 0
}
