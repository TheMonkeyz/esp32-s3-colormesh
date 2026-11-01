// Voice clip chunks (clip_chunks.h)
#include "clip_chunks.h"
#include <string.h>

#define PROTO 0x43
#define KIND  'V'
#define BEACON 'B'

uint32_t clip_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

uint16_t clip_chunks(uint32_t len) { return (uint16_t)((len + CLIP_CHUNK_DATA - 1) / CLIP_CHUNK_DATA); }

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }

int clip_pack(uint8_t origin, uint8_t id, const uint8_t *clip, uint32_t len, uint32_t crc, uint16_t chunk,
              uint8_t *out)
{
    uint16_t total = clip_chunks(len);
    if (len == 0 || len > CLIP_MAX_BYTES || chunk >= total) return 0;
    uint32_t off = (uint32_t)chunk * CLIP_CHUNK_DATA;
    uint16_t n = (uint16_t)(len - off < CLIP_CHUNK_DATA ? len - off : CLIP_CHUNK_DATA);
    out[0] = (uint8_t)(CLIP_PKT_HDR - 1 + n);        // AD length: type byte onwards
    out[1] = 0xFF;
    out[2] = out[3] = 0xFF;
    out[4] = PROTO;
    out[5] = KIND;
    out[6] = origin;
    out[7] = id;
    put16(out + 8, chunk);
    put16(out + 10, total);
    put32(out + 12, len);
    put32(out + 16, crc);
    memcpy(out + CLIP_PKT_HDR, clip + off, n);
    return CLIP_PKT_HDR + n;
}

bool clip_parse(const uint8_t *p, size_t n, clip_pkt_t *out)
{
    if (n < CLIP_PKT_HDR + 1 || p[0] + 1u != n || p[1] != 0xFF || p[3] != 0xFF || p[4] != PROTO ||   // company 0xFFxx
        p[5] != KIND)
        return false;
    clip_pkt_t c = { .origin = p[6], .id = p[7], .chunk = get16(p + 8), .total = get16(p + 10), .len = get32(p + 12),
                     .crc = get32(p + 16), .data = p + CLIP_PKT_HDR, .data_len = (uint16_t)(n - CLIP_PKT_HDR) };
    if (c.len == 0 || c.len > CLIP_MAX_BYTES || c.total != clip_chunks(c.len) || c.chunk >= c.total) return false;
    uint32_t off = (uint32_t)c.chunk * CLIP_CHUNK_DATA;
    uint32_t want = c.len - off < CLIP_CHUNK_DATA ? c.len - off : CLIP_CHUNK_DATA;
    if (c.data_len != want) return false;
    *out = c;
    return true;
}

void clip_frag_reset(clip_frag_t *f)
{
    f->len = 0;
    f->bad = false;
}

int clip_frag_add(clip_frag_t *f, const uint8_t *data, size_t n, int status)
{
    if (!f->bad && f->len + n <= CLIP_FRAG_MAX) {
        memcpy(f->buf + f->len, data, n);
        f->len = (uint16_t)(f->len + n);
    } else {
        f->bad = true;
    }
    if (status == 1) return 0;                       // more to come
    int whole = status == 0 && !f->bad ? f->len : 0;
    clip_frag_reset(f);                              // the next report starts a new packet
    return whole;
}

bool clip_next_ad(const uint8_t *p, size_t n, size_t *off, const uint8_t **ad, size_t *ad_len)
{
    if (*off >= n || p[*off] == 0 || *off + 1 + p[*off] > n) return false;
    *ad = p + *off;
    *ad_len = (size_t)p[*off] + 1;
    *off += *ad_len;
    return true;
}

bool clip_beacon_parse(const uint8_t *p, size_t n, uint8_t *origin, uint8_t *id, uint8_t *ds, uint8_t *khz)
{
    size_t off = 0, len;
    const uint8_t *a;
    while (clip_next_ad(p, n, &off, &a, &len)) {
        if ((len == 9 || len == 10) && a[1] == 0xFF && a[2] == 0xFF && a[3] == 0xFF && a[4] == PROTO &&
            a[5] == BEACON) {
            *origin = a[6];
            *id = a[7];
            *ds = a[8];
            *khz = len == 10 && a[9] == 8 ? 8 : 16;
            return true;
        }
    }
    return false;
}

void clip_upsample2(int16_t *pcm, size_t n)
{
    for (size_t i = n; i-- > 0;) {                   // from the end: 2i >= i, nothing is read after it is written
        int32_t next = i + 1 < n ? pcm[i + 1] : pcm[i];
        int16_t x = pcm[i];
        pcm[2 * i + 1] = (int16_t)((x + next) / 2);
        pcm[2 * i] = x;
    }
}

void clip_rx_begin(clip_rx_t *r, uint8_t origin, uint8_t id, uint8_t *buf)
{
    memset(r, 0, sizeof(*r));
    r->origin = origin;
    r->id = id;
    r->buf = buf;
    r->active = true;
}

bool clip_rx_add(clip_rx_t *r, const clip_pkt_t *p)
{
    if (!r->active || p->origin != r->origin || p->id != r->id) return false;
    if (r->total == 0) {                             // the first packet fixes the clip's size
        r->total = p->total;
        r->len = p->len;
        r->crc = p->crc;
    } else if (p->total != r->total || p->len != r->len || p->crc != r->crc) {
        return false;                                // another clip under the same id: ignore it
    }
    uint8_t m = (uint8_t)(1u << (p->chunk & 7));
    if (r->bits[p->chunk >> 3] & m) return false;
    memcpy(r->buf + (uint32_t)p->chunk * CLIP_CHUNK_DATA, p->data, p->data_len);
    r->bits[p->chunk >> 3] |= m;
    if (++r->have < r->total) return false;
    r->active = false;
    return clip_crc32(r->buf, r->len) == r->crc;
}

uint16_t clip_rx_missing(const clip_rx_t *r) { return r->total ? (uint16_t)(r->total - r->have) : 0xFFFF; }

bool clip_id_newer(uint8_t a, uint8_t b) { return a != b && (uint8_t)(a - b) < 128; }
