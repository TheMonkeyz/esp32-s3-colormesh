package ca.themonkeyz.colormesh

// The ColorMesh wire format, peer table and colour mix: a port of main/mesh_proto.c (read its header for the byte
// map). Pure Kotlin, no Android: the JVM tests check it against tests/fixtures/mesh_vectors.txt, which the
// firmware's C code made, so the phone and the boards keep the same rules. Time is always an argument (nowMs).
object MeshProto {
    const val PROTO = 0x43
    const val COMPANY = 0xFFFF
    const val AD_SIZE = 30
    const val MFG_LEN = 28                 // the value after the AD type, company id first
    const val F_HOLDING = 0x01
    const val F_CLIP = 0x20
    const val TARGET_NONE = 0xFF
    const val EFFECT_NONE = 0
    const val EFFECT_WAVE_CENTRE = 1
    const val EFFECT_WAVE_BOTTOM = 5
    const val EFFECT_RING = 6
    const val EFFECT_ANSWER = 7
    const val MAX_PEERS = 16
    const val HOLD_EXPIRY_MS = 250L
    const val HOLD_EXPIRY_MAX_MS = 1000L
    const val PEER_EXPIRY_MS = 10000L
    const val SEQ_RESET_REPORTS = 3
    const val EFFECT_MS = 1000L            // an effect stays in the advert this long (mesh.h MESH_EFFECT_MS)

    // One advert's fields, 0-255 each (mp_state_t)
    data class State(
        var origin: Int = 0, var seq: Int = 0, var ttl: Int = 0, var flags: Int = 0,
        var r: Int = 0, var g: Int = 0, var b: Int = 0,
        var ver: IntArray = IntArray(4), var target: Int = TARGET_NONE,
        var effect: Int = 0, var effectSeq: Int = 0, var b2r: Int = 0, var b2g: Int = 0, var b2b: Int = 0,
        var gameSeq: Int = 0, var gameStarter: Int = 0, var gameLeft: Int = 0, var gamePick: Int = 0,
        var clipId: Int = 0, var clipDs: Int = 0, var answerTo: Int = 0, var advDs: Int = 0,
    ) {
        val holding get() = flags and F_HOLDING != 0
        // The fields in byte order 5..29 (the fixtures' order)
        fun fields(): IntArray = intArrayOf(origin, seq, ttl, flags, r, g, b, ver[0], ver[1], ver[2], ver[3], target,
            effect, effectSeq, b2r, b2g, b2b, gameSeq, gameStarter, gameLeft, gamePick, clipId, clipDs, answerTo, advDs)
        fun copyState() = copy(ver = ver.copyOf())
        override fun equals(other: Any?) = other is State && fields().contentEquals(other.fields())
        override fun hashCode() = fields().contentHashCode()
    }

    // The 26 bytes after the company id: what Android's addManufacturerData(COMPANY, ...) sends and
    // ScanRecord.getManufacturerSpecificData(COMPANY) gives back
    fun encodeValue(s: State): ByteArray {
        val f = s.fields()
        val out = ByteArray(26)
        out[0] = PROTO.toByte()
        for (i in f.indices) out[1 + i] = f[i].toByte()
        return out
    }

    // The whole 30-byte AD structure (length byte first)
    fun encodeAd(s: State): ByteArray =
        byteArrayOf((AD_SIZE - 1).toByte(), 0xFF.toByte(), (COMPANY and 0xFF).toByte(), (COMPANY shr 8).toByte()) +
            encodeValue(s)

    fun decodeValue(v: ByteArray?): State? {
        if (v == null || v.size != 26 || (v[0].toInt() and 0xFF) != PROTO) return null
        val u = IntArray(25) { v[1 + it].toInt() and 0xFF }
        return State(u[0], u[1], u[2], u[3], u[4], u[5], u[6], intArrayOf(u[7], u[8], u[9], u[10]), u[11], u[12], u[13],
            u[14], u[15], u[16], u[17], u[18], u[19], u[20], u[21], u[22], u[23], u[24])
    }

    fun decodeAd(ad: ByteArray): State? {
        if (ad.size != AD_SIZE || (ad[0].toInt() and 0xFF) != AD_SIZE - 1 || (ad[1].toInt() and 0xFF) != 0xFF) return null
        if ((ad[2].toInt() and 0xFF) or ((ad[3].toInt() and 0xFF) shl 8) != COMPANY) return null
        return decodeValue(ad.copyOfRange(4, AD_SIZE))
    }

    // mp_hold_expiry_ms: 3 of the sender's intervals + 100 ms, at least tableMs, at most HOLD_EXPIRY_MAX_MS
    fun holdExpiryMs(advDs: Int, tableMs: Long = HOLD_EXPIRY_MS): Long =
        (30L * advDs + 100L).coerceAtLeast(tableMs).coerceAtMost(HOLD_EXPIRY_MAX_MS)

    // wave_opposite: hue + 180 degrees, same saturation and value
    fun opposite(r: Int, g: Int, b: Int): IntArray {
        val mx = maxOf(r, g, b)
        val mn = minOf(r, g, b)
        return intArrayOf(mx + mn - r, mx + mn - g, mx + mn - b)
    }

    // ---- peer table (keyed by address, as on the boards) ----
    class Peer(val addr: String, var st: State, var rssi: Int, var lastMs: Long) {
        var stale = 0
    }

    class Table(val holdExpiryMs: Long = HOLD_EXPIRY_MS, val peerExpiryMs: Long = PEER_EXPIRY_MS) {
        val peers = ArrayList<Peer>()

        // True when the peer is new or its colour, flags, version, target, effect or colour B changed
        fun update(addr: String, st: State, rssi: Int, nowMs: Long): Boolean {
            val p = peers.firstOrNull { it.addr == addr }
            if (p == null) {
                if (peers.size >= MAX_PEERS) peers.remove(peers.minByOrNull { it.lastMs })
                peers.add(Peer(addr, st.copyState(), rssi, nowMs))
                return true
            }
            p.lastMs = nowMs
            p.rssi = rssi
            if (((st.seq - p.st.seq) and 0xFF).toByte() < 0) {       // older: stale, unless the peer restarted
                if (++p.stale < SEQ_RESET_REPORTS) return false
            }
            p.stale = 0
            val o = p.st
            val changed = o.r != st.r || o.g != st.g || o.b != st.b || o.flags != st.flags || o.target != st.target ||
                o.effect != st.effect || o.effectSeq != st.effectSeq || o.b2r != st.b2r || o.b2g != st.b2g ||
                o.b2b != st.b2b || !o.ver.contentEquals(st.ver)
            p.st = st.copyState()
            return changed
        }

        // Holders not heard for their hold expiry stop holding; peers not heard for peerExpiryMs leave
        fun expire(nowMs: Long): Int {
            var n = 0
            val it = peers.iterator()
            while (it.hasNext()) {
                val p = it.next()
                val age = (nowMs - p.lastMs).coerceAtLeast(0)
                if (age >= peerExpiryMs) {
                    it.remove()
                    n++
                } else if (p.st.holding && age >= holdExpiryMs(p.st.advDs, holdExpiryMs)) {
                    p.st.flags = p.st.flags and F_HOLDING.inv()
                    n++
                }
            }
            return n
        }

        fun holders() = peers.count { it.st.holding }
    }

    // mp_mix: every holder (and self while holding) summed in linear light, the largest channel full; none -> black
    fun mix(colours: List<IntArray>): IntArray {
        val sum = LongArray(3)
        for (c in colours) for (i in 0..2) sum[i] += TO_LIN[c[i]].toLong()
        val max = sum.max()
        if (max == 0L) return IntArray(3)
        return IntArray(3) { toSrgb(sum[it] * 65535L / max) }
    }

    fun toLin(v: Int) = TO_LIN[v]

    // The largest i with THR[i] <= lin
    fun toSrgb(lin: Long): Int {
        var lo = 0
        var hi = 255
        while (lo < hi) {
            val mid = (lo + hi + 1) / 2
            if (THR[mid] <= lin) lo = mid else hi = mid - 1
        }
        return lo
    }

    // Copied from main/mesh_proto.c: sRGB to linear (0-65535) and the thresholds back
    private val TO_LIN = intArrayOf(
        0, 1, 2, 4, 7, 11, 17, 24, 32, 42, 53, 65,
        79, 94, 111, 129, 148, 169, 192, 216, 242, 270, 299, 330,
        362, 396, 432, 469, 508, 549, 591, 635, 681, 729, 779, 830,
        883, 938, 995, 1053, 1113, 1175, 1239, 1305, 1373, 1443, 1514, 1587,
        1663, 1740, 1819, 1900, 1983, 2068, 2155, 2243, 2334, 2427, 2521, 2618,
        2717, 2817, 2920, 3024, 3131, 3240, 3350, 3463, 3578, 3694, 3813, 3934,
        4057, 4182, 4309, 4438, 4570, 4703, 4838, 4976, 5115, 5257, 5401, 5547,
        5695, 5845, 5998, 6152, 6309, 6468, 6629, 6792, 6957, 7124, 7294, 7466,
        7640, 7816, 7994, 8175, 8358, 8543, 8730, 8919, 9111, 9305, 9501, 9699,
        9900, 10102, 10307, 10515, 10724, 10936, 11150, 11366, 11585, 11806, 12029, 12254,
        12482, 12712, 12944, 13179, 13416, 13655, 13896, 14140, 14386, 14635, 14885, 15138,
        15394, 15652, 15912, 16174, 16439, 16706, 16975, 17247, 17521, 17798, 18077, 18358,
        18642, 18928, 19216, 19507, 19800, 20095, 20393, 20694, 20996, 21301, 21609, 21919,
        22231, 22546, 22863, 23182, 23504, 23829, 24156, 24485, 24817, 25151, 25487, 25826,
        26168, 26512, 26858, 27207, 27558, 27912, 28268, 28627, 28988, 29351, 29717, 30086,
        30457, 30830, 31206, 31585, 31966, 32349, 32735, 33124, 33514, 33908, 34304, 34702,
        35103, 35507, 35913, 36321, 36732, 37146, 37562, 37981, 38402, 38825, 39252, 39680,
        40112, 40546, 40982, 41421, 41862, 42306, 42753, 43202, 43654, 44108, 44565, 45025,
        45487, 45951, 46418, 46888, 47360, 47835, 48313, 48793, 49275, 49761, 50249, 50739,
        51232, 51728, 52226, 52727, 53230, 53736, 54245, 54756, 55270, 55787, 56306, 56828,
        57352, 57879, 58409, 58941, 59476, 60014, 60554, 61097, 61642, 62190, 62741, 63295,
        63851, 64410, 64971, 65535,
    )
    private val THR = intArrayOf(
        0, 1, 2, 3, 6, 10, 15, 21, 29, 37, 48, 59,
        72, 87, 103, 120, 139, 159, 181, 205, 230, 256, 285, 314,
        346, 379, 414, 451, 489, 529, 570, 614, 659, 706, 754, 805,
        857, 911, 966, 1024, 1083, 1145, 1208, 1273, 1339, 1408, 1479, 1551,
        1625, 1702, 1780, 1860, 1942, 2026, 2111, 2199, 2289, 2381, 2474, 2570,
        2667, 2767, 2869, 2972, 3078, 3186, 3295, 3407, 3521, 3636, 3754, 3874,
        3996, 4120, 4246, 4374, 4504, 4636, 4771, 4907, 5046, 5187, 5329, 5474,
        5621, 5770, 5922, 6075, 6231, 6388, 6548, 6710, 6875, 7041, 7209, 7380,
        7553, 7728, 7905, 8085, 8267, 8450, 8636, 8825, 9015, 9208, 9403, 9600,
        9800, 10001, 10205, 10411, 10620, 10830, 11043, 11258, 11476, 11696, 11918, 12142,
        12368, 12597, 12828, 13062, 13297, 13535, 13776, 14018, 14263, 14511, 14760, 15012,
        15266, 15523, 15782, 16043, 16307, 16573, 16841, 17111, 17384, 17660, 17937, 18218,
        18500, 18785, 19072, 19361, 19653, 19948, 20244, 20544, 20845, 21149, 21455, 21764,
        22075, 22389, 22704, 23023, 23344, 23667, 23992, 24320, 24651, 24984, 25319, 25657,
        25997, 26340, 26685, 27033, 27383, 27735, 28090, 28447, 28807, 29170, 29534, 29902,
        30271, 30644, 31018, 31396, 31775, 32158, 32542, 32929, 33319, 33711, 34106, 34503,
        34903, 35305, 35710, 36117, 36527, 36939, 37354, 37771, 38191, 38614, 39039, 39466,
        39896, 40329, 40764, 41202, 41642, 42085, 42530, 42978, 43428, 43881, 44337, 44795,
        45256, 45719, 46185, 46653, 47124, 47598, 48074, 48553, 49034, 49518, 50005, 50494,
        50986, 51480, 51977, 52477, 52979, 53483, 53991, 54501, 55013, 55529, 56047, 56567,
        57090, 57616, 58144, 58675, 59209, 59745, 60284, 60826, 61370, 61917, 62466, 63018,
        63573, 64130, 64690, 65253,
    )
}
