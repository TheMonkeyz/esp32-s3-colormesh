package ca.themonkeyz.colormesh

// A voice clip as the boards send it (main/adpcm.c, main/clip_chunks.c): IMA ADPCM (4 bits a sample, low nibble
// first), cut into chunks of CHUNK_DATA bytes, each in one manufacturer-data AD:
//   [len] [0xFF] [0xFF 0xFF] 0x43 'V' origin id chunk(u16) total(u16) len(u32) crc32(u32) data...
// Android sends one periodic packet per 100 ms at best (a board: 10 ms) and only one HCI command of data (one chunk)
// while the train runs, so the phone records at 8 kHz (the user's choice over 16 kHz, 2026-11-01: a 5 s clip arrives
// in about 9 s instead of 18) and says so in its beacon; the boards double the samples. Pure Kotlin; the JVM tests
// check it against tests/fixtures/clip_vectors.txt, made by the firmware's C code.
object Clip {
    const val RATE = 8000
    const val MAX_MS = 5000
    const val MIN_MS = 300                 // shorter = a tap, not a clip (clip.c REC_MIN_MS)
    const val HDR = 20
    const val CHUNK_DATA = 224
    const val PKT_MAX = HDR + CHUNK_DATA
    const val ROUNDS = 2                   // every chunk twice: the boards play as soon as the clip is whole
    const val PEAK_TARGET = 26000
    const val MAX_BOOST = 8
    private const val KIND = 'V'.code
    private const val BEACON = 'B'.code

    private val STEPS = intArrayOf(
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
        118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
        1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
        6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
        32767,
    )
    private val INDEX_ADJ = intArrayOf(-1, -1, -1, -1, 2, 4, 6, 8)

    // adpcm_encode from a fresh state: n samples (n even) to n / 2 bytes
    fun adpcmEncode(pcm: ShortArray): ByteArray {
        var pred = 0
        var index = 0
        fun one(x: Int): Int {
            var step = STEPS[index]
            var diff = x - pred
            var code = 0
            if (diff < 0) { code = 8; diff = -diff }
            if (diff >= step) { code = code or 4; diff -= step }
            step = step shr 1
            if (diff >= step) { code = code or 2; diff -= step }
            step = step shr 1
            if (diff >= step) code = code or 1
            // step_apply: track what the decoder will hear
            val s = STEPS[index]
            var d = s shr 3
            if (code and 4 != 0) d += s
            if (code and 2 != 0) d += s shr 1
            if (code and 1 != 0) d += s shr 2
            pred = (if (code and 8 != 0) pred - d else pred + d).coerceIn(-32768, 32767)
            index = (index + INDEX_ADJ[code and 7]).coerceIn(0, 88)
            return code
        }
        val out = ByteArray(pcm.size / 2)
        for (i in 0 until pcm.size / 2) {
            val lo = one(pcm[2 * i].toInt())
            val hi = one(pcm[2 * i + 1].toInt())
            out[i] = (lo or (hi shl 4)).toByte()
        }
        return out
    }

    fun crc32(p: ByteArray): Long {
        var c = 0xFFFFFFFFL
        for (b in p) {
            c = c xor (b.toLong() and 0xFF)
            repeat(8) { c = (c ushr 1) xor (0xEDB88320L and -(c and 1)) }
        }
        return c.inv() and 0xFFFFFFFFL
    }

    fun chunks(len: Int) = (len + CHUNK_DATA - 1) / CHUNK_DATA

    // clip_pack: chunk `chunk` of the clip as one AD
    fun pack(origin: Int, id: Int, clip: ByteArray, crc: Long, chunk: Int): ByteArray {
        val off = chunk * CHUNK_DATA
        val n = minOf(CHUNK_DATA, clip.size - off)
        val out = ByteArray(HDR + n)
        val total = chunks(clip.size)
        val len = clip.size.toLong()
        val h = intArrayOf(HDR - 1 + n, 0xFF, 0xFF, 0xFF, MeshProto.PROTO, KIND, origin, id,
            chunk and 0xFF, chunk shr 8, total and 0xFF, total shr 8,
            (len and 0xFF).toInt(), (len shr 8 and 0xFF).toInt(), (len shr 16 and 0xFF).toInt(), (len shr 24 and 0xFF).toInt(),
            (crc and 0xFF).toInt(), (crc shr 8 and 0xFF).toInt(), (crc shr 16 and 0xFF).toInt(), (crc shr 24 and 0xFF).toInt())
        for (i in h.indices) out[i] = h[i].toByte()
        System.arraycopy(clip, off, out, HDR, n)
        return out
    }

    // The periodic packets: every chunk ROUNDS times, as many whole ADs per packet as fit in maxLen bytes (at most
    // MAX_PER_PKT). Android's AdvertiseData holds one manufacturer AD per company id, so the k-th AD of a packet
    // goes out with company id 0xFFFF - k (companyOf); the boards take a clip chunk with any 0xFFxx id.
    const val MAX_PER_PKT = 8
    fun companyOf(k: Int) = 0xFFFF - k
    fun packets(origin: Int, id: Int, clip: ByteArray, maxLen: Int, rounds: Int = ROUNDS): List<List<ByteArray>> {
        val crc = crc32(clip)
        val per = (maxLen / PKT_MAX).coerceIn(1, MAX_PER_PKT)
        val all = (0 until rounds).flatMap { (0 until chunks(clip.size)).toList() }
        return all.chunked(per).map { group -> group.map { pack(origin, id, clip, crc, it) } }
    }

    // The clip beacon: the extended advertising data of the phone's clip set. Android gives that set its own address
    // and SID, so a board cannot find the train from the phone's flood advert as it does for a board (address +
    // SID 1); it syncs to the set that carries this AD instead: [9] [0xFF] [0xFF 0xFF] 0x43 'B' origin id tenths kHz
    fun beacon(origin: Int, id: Int, ds: Int): ByteArray =
        intArrayOf(9, 0xFF, 0xFF, 0xFF, MeshProto.PROTO, BEACON, origin, id, ds, RATE / 1000).map { it.toByte() }.toByteArray()

    // A quiet clip scaled up to PEAK_TARGET, at most MAX_BOOST times (clip.c)
    fun normalise(pcm: ShortArray): ShortArray {
        val peak = pcm.maxOfOrNull { kotlin.math.abs(it.toInt()) } ?: 0
        if (peak == 0) return pcm
        val g = minOf(MAX_BOOST.toDouble(), PEAK_TARGET.toDouble() / peak).coerceAtLeast(1.0)
        return ShortArray(pcm.size) { (pcm[it] * g).toInt().coerceIn(-32768, 32767).toShort() }
    }
}
