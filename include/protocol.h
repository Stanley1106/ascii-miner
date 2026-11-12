/*
 * protocol.h — Miner Game application protocol (MGP) v1.
 *
 * Every message = 12-byte header + payload. All integers are big-endian.
 *
 *   0      1      2        3       4..7      8..11
 *  +------+------+--------+------+---------+---------+---------...
 *  | 'M'  | 'G'  | version| type |   seq   | length  | payload
 *  +------+------+--------+------+---------+---------+---------...
 *
 * See docs/protocol.md for each payload layout.
 */
#ifndef MG_PROTOCOL_H
#define MG_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define MG_MAGIC0        0x4D   /* 'M' */
#define MG_MAGIC1        0x47   /* 'G' */
#define MG_PROTO_VERSION 1
#define MG_HDR_LEN       12
#define MG_MAX_PAYLOAD   16384
#define MG_NAME_LEN      16
#define MG_DEFAULT_PORT  5555

enum mg_msg_type {
    MSG_HELLO        = 0x01, /* C->S flags u8, name[16]                     */
    MSG_WELCOME      = 0x02, /* S->C id u16, map_w u16, map_h u16, x u16, y u16 */
    MSG_MOVE         = 0x10, /* C->S dir u8                                  */
    MSG_POS          = 0x11, /* S->C x u16, y u16                            */
    MSG_DIG          = 0x12, /* C->S (empty): dig the cell you stand on      */
    MSG_DIG_RESULT   = 0x13, /* S->C x u16, y u16, status u8, ore u8, value u32, total u32 */
    MSG_INV_REQ      = 0x20, /* C->S (empty)                                 */
    MSG_INV_RESP     = 0x21, /* S->C count u32 x ORE_COUNT, total u32, digs u32 */
    MSG_SYSINFO_REQ  = 0x30, /* C->S (empty)                                 */
    MSG_SYSINFO_RESP = 0x31, /* S->C UTF-8 text "key=value\n" lines          */
    MSG_SNAPSHOT     = 0x40, /* S->C world view, see docs/protocol.md        */
    MSG_PING         = 0x50,
    MSG_PONG         = 0x51,
    MSG_BYE          = 0x60, /* C->S graceful quit                           */
    MSG_ERROR        = 0x7F, /* S->C code u8, text                           */
};

enum mg_dir { DIR_UP = 0, DIR_DOWN = 1, DIR_LEFT = 2, DIR_RIGHT = 3 };
enum mg_dig_status { DIG_NOTHING = 0, DIG_FOUND = 1, DIG_ALREADY = 2 };
enum mg_err_code {
    ERR_PROTOCOL = 1, ERR_SERVER_FULL = 2, ERR_BAD_REQUEST = 3, ERR_TIMEOUT = 4,
};

#define HELLO_WANT_SNAPSHOT 0x01

typedef struct {
    uint8_t  type;
    uint32_t seq;
    uint32_t len;
    uint8_t  payload[MG_MAX_PAYLOAD];
} mg_msg;

/* Stream reassembler: TCP is a byte stream, so one recv() may return a
 * fragment of a frame or several frames. Bytes are buffered until a full
 * frame is available. */
typedef struct {
    uint8_t buf[MG_HDR_LEN + MG_MAX_PAYLOAD];
    size_t  len;
} mg_reader;

void mg_encode_header(uint8_t out[MG_HDR_LEN], uint8_t type, uint32_t seq, uint32_t len);
/* 0 ok, -1 bad magic/version/length */
int  mg_decode_header(const uint8_t in[MG_HDR_LEN], uint8_t *type, uint32_t *seq, uint32_t *len);

void mg_reader_init(mg_reader *r);
/* Append raw bytes (used by tests and by mg_reader_fill). -1 if overflow. */
int  mg_reader_push(mg_reader *r, const void *data, size_t n);
/* One recv() into the buffer: >0 bytes read, 0 peer closed, -1 error (errno). */
int  mg_reader_fill(mg_reader *r, int fd);
/* 1 = *m holds a full message, 0 = need more bytes, -1 = protocol error. */
int  mg_reader_next(mg_reader *r, mg_msg *m);

const char *mg_type_name(uint8_t type);

static inline void mg_put_u16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
static inline void mg_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v;
}
static inline uint16_t mg_get_u16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t mg_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

#endif
