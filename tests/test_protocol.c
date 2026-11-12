#include "log.h"
#include "protocol.h"
#include "test.h"
#include <string.h>

static size_t make_frame(uint8_t *out, uint8_t type, uint32_t seq, const char *payload)
{
    size_t n = payload ? strlen(payload) : 0;
    mg_encode_header(out, type, seq, (uint32_t)n);
    memcpy(out + MG_HDR_LEN, payload, n);
    return MG_HDR_LEN + n;
}

static mg_reader r;
static mg_msg m;

int main(void)
{
    mg_log_level = -1; /* silence expected warnings from bad-frame cases */
    uint8_t f1[64], f2[64];
    size_t n1 = make_frame(f1, MSG_SYSINFO_RESP, 7, "hello");
    size_t n2 = make_frame(f2, MSG_PING, 8, NULL);

    /* header round trip, big-endian on the wire */
    CHECK(f1[0] == 'M' && f1[1] == 'G' && f1[2] == 1 && f1[3] == MSG_SYSINFO_RESP);
    CHECK(f1[7] == 7 && f1[11] == 5);
    uint8_t t; uint32_t s, l;
    CHECK(mg_decode_header(f1, &t, &s, &l) == 0 && t == MSG_SYSINFO_RESP && s == 7 && l == 5);

    /* partial delivery: one byte at a time, message only appears at the end */
    mg_reader_init(&r);
    int got = 0;
    for (size_t i = 0; i < n1; i++) {
        mg_reader_push(&r, f1 + i, 1);
        int rc = mg_reader_next(&r, &m);
        if (i + 1 < n1) CHECK(rc == 0);
        else got = rc;
    }
    CHECK(got == 1 && m.type == MSG_SYSINFO_RESP && m.len == 5 && memcmp(m.payload, "hello", 5) == 0);
    CHECK(r.len == 0);

    /* coalesced delivery: two frames in one chunk yield two messages */
    uint8_t both[128];
    memcpy(both, f1, n1);
    memcpy(both + n1, f2, n2);
    mg_reader_init(&r);
    mg_reader_push(&r, both, n1 + n2);
    CHECK(mg_reader_next(&r, &m) == 1 && m.seq == 7);
    CHECK(mg_reader_next(&r, &m) == 1 && m.seq == 8 && m.type == MSG_PING && m.len == 0);
    CHECK(mg_reader_next(&r, &m) == 0);

    /* validation: bad magic, bad version, oversized length */
    uint8_t bad[MG_HDR_LEN];
    memcpy(bad, f2, MG_HDR_LEN); bad[0] = 'G';
    CHECK(mg_decode_header(bad, &t, &s, &l) == -1);
    memcpy(bad, f2, MG_HDR_LEN); bad[2] = 9;
    CHECK(mg_decode_header(bad, &t, &s, &l) == -1);
    mg_encode_header(bad, MSG_PING, 1, MG_MAX_PAYLOAD + 1);
    CHECK(mg_decode_header(bad, &t, &s, &l) == -1);
    mg_reader_init(&r);
    mg_reader_push(&r, "GET / HTTP/1.0\r\n", 16);
    CHECK(mg_reader_next(&r, &m) == -1);

    /* integer helpers */
    uint8_t b[4];
    mg_put_u32(b, 0x01020304); CHECK(b[0] == 1 && b[3] == 4 && mg_get_u32(b) == 0x01020304);
    mg_put_u16(b, 0xBEEF); CHECK(mg_get_u16(b) == 0xBEEF);
    CHECK(strcmp(mg_type_name(MSG_DIG), "DIG") == 0);

    return TEST_DONE("test_protocol");
}
