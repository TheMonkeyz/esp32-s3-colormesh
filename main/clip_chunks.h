#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A voice clip cut into chunks for the periodic train (clip.c). Each periodic packet is one manufacturer-data AD:
//   [len] [0xFF] [0xFF 0xFF]  proto 0x43 · 'V' · origin · clip id · chunk u16 · total u16 · clip len u32 · crc32 u32
//   then up to CLIP_CHUNK_DATA bytes of the ADPCM stream (little-endian numbers).
// The Android app (android/, Clip.kt) sends the same ADs, but Android holds one manufacturer AD per company id, so
// the k-th chunk of one periodic packet carries company id 0xFFFF - k: any 0xFFxx company id is a clip chunk. Its
// packets hold several ADs and come in several HCI fragments (clip_frag_*, clip_next_ad).
// The receiver keeps a bitmap of the chunks it has; the clip is whole when every bit is set and the CRC-32 of the
// assembled bytes matches. Pure C99, host-tested (tests/host/test_clip_chunks.c).
#define CLIP_PKT_HDR     20                 // the AD header (4) and the clip header (16)
#define CLIP_CHUNK_DATA  224
#define CLIP_PKT_MAX     (CLIP_PKT_HDR + CLIP_CHUNK_DATA)   // 244, within one AUX_SYNC_IND (247)
#define CLIP_MAX_BYTES   (64 * 1024)
#define CLIP_MAX_CHUNKS  ((CLIP_MAX_BYTES + CLIP_CHUNK_DATA - 1) / CLIP_CHUNK_DATA)

typedef struct {
    uint8_t origin, id;
    uint16_t chunk, total;
    uint32_t len, crc;
    const uint8_t *data;                    // into the packet
    uint16_t data_len;
} clip_pkt_t;

uint32_t clip_crc32(const uint8_t *p, size_t n);
uint16_t clip_chunks(uint32_t len);         // how many chunks a clip of len bytes takes
// Packet `chunk` of a clip into out (CLIP_PKT_MAX bytes); returns its length, 0 if chunk is out of range
int clip_pack(uint8_t origin, uint8_t id, const uint8_t *clip, uint32_t len, uint32_t crc, uint16_t chunk,
              uint8_t *out);
// A received periodic report: true if it is a well-formed clip packet (the header checked against itself)
bool clip_parse(const uint8_t *p, size_t n, clip_pkt_t *out);

// One periodic packet put back together from its HCI fragments (data status 0 = complete, 1 = more to come,
// 2 = truncated). clip_frag_add returns the packet's length once it is complete (in f->buf), else 0; a truncated
// packet or one longer than CLIP_FRAG_MAX is dropped. Not reentrant (one sync callback).
#define CLIP_FRAG_MAX 2048
typedef struct {
    uint8_t buf[CLIP_FRAG_MAX];
    uint16_t len;
    bool bad;                               // this packet overflowed: drop the rest of it
} clip_frag_t;
void clip_frag_reset(clip_frag_t *f);
int clip_frag_add(clip_frag_t *f, const uint8_t *data, size_t n, int status);
// The AD at *off in p (n bytes): *ad and *ad_len (its length byte included), and *off moves past it. False at the
// end, on a zero length, or on an AD that runs past n.
bool clip_next_ad(const uint8_t *p, size_t n, size_t *off, const uint8_t **ad, size_t *ad_len);
// The Android app's clip beacon, in its clip set's extended advertising data (Android gives that set its own address
// and SID, so a board cannot find it from the phone's flood advert):
//   [9] [0xFF] [0xFF 0xFF] 0x43 'B' origin id tenths kHz   (kHz: the clip's sample rate, 8 or 16; a beacon without
// that byte, [8] ..., is 16). The phone records at 8 kHz: Android sends one chunk per 100 ms, 4x slower than a board.
bool clip_beacon_parse(const uint8_t *p, size_t n, uint8_t *origin, uint8_t *id, uint8_t *ds, uint8_t *khz);
// An 8 kHz clip played at 16 kHz: n samples at the start of pcm become 2n, each followed by the mean of it and the
// next (the last repeated). In place (pcm holds 2n samples).
void clip_upsample2(int16_t *pcm, size_t n);

// Receiver side: one clip being assembled. buf holds CLIP_MAX_BYTES, bits CLIP_MAX_CHUNKS bits.
typedef struct {
    uint8_t origin, id;
    uint16_t total, have;
    uint32_t len, crc;
    uint8_t *buf;
    uint8_t bits[(CLIP_MAX_CHUNKS + 7) / 8];
    bool active;
} clip_rx_t;

void clip_rx_begin(clip_rx_t *r, uint8_t origin, uint8_t id, uint8_t *buf);
// One packet: copied in when it belongs to this clip and is new. Returns true once the clip is whole and its CRC
// matches (only on the packet that completes it)
bool clip_rx_add(clip_rx_t *r, const clip_pkt_t *p);
uint16_t clip_rx_missing(const clip_rx_t *r);
// Id order with wrap (like the effect seq): a is newer than b
bool clip_id_newer(uint8_t a, uint8_t b);
