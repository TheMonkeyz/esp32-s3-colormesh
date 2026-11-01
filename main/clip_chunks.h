#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A voice clip cut into chunks for the periodic train (clip.c). Each periodic packet is one manufacturer-data AD:
//   [len] [0xFF] [0xFF 0xFF]  proto 0x43 · 'V' · origin · clip id · chunk u16 · total u16 · clip len u32 · crc32 u32
//   then up to CLIP_CHUNK_DATA bytes of the ADPCM stream (little-endian numbers).
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
