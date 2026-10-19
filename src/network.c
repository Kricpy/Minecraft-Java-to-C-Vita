#include "network.h"
#include "defines.h"
#include "chunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <sys/socket.h>
#include <zlib.h>
#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/rtc.h>
#include <psp2/kernel/clib.h>

NetState g_net;
char log_lines[6][128];
int log_count = 0;

extern int ping_ms;
extern int packets_sent;
extern int packets_recv;

static uint8_t net_memory_pool[128 * 1024] __attribute__((aligned(64)));
static uint32_t connect_ticks = 0;
static Player *g_player = NULL;
static uint32_t frame_counter = 0;

static SceUInt64 keepalive_send_time = 0;
SceUInt64 last_position_look_time = 0;
int position_look_received = 0;
int last_confirmed_tid = -1;

/* ------------------------------------------------------------------ */
/* Protocol readers                                                    */
/* ------------------------------------------------------------------ */
static inline int rc_check(RCUR *r, size_t need) {
    return r->p + need <= r->len;
}

uint8_t rc_u8(RCUR *r)  { return rc_check(r, 1) ? r->b[r->p++] : 0; }
uint16_t rc_u16(RCUR *r) { if (!rc_check(r, 2)) return 0; uint16_t v = ((uint16_t)r->b[r->p] << 8 | r->b[r->p+1]); r->p += 2; return v; }
uint32_t rc_u32(RCUR *r) { if (!rc_check(r, 4)) return 0; uint32_t v = ((uint32_t)r->b[r->p] << 24 | (uint32_t)r->b[r->p+1] << 16 | (uint32_t)r->b[r->p+2] << 8 | r->b[r->p+3]); r->p += 4; return v; }
uint64_t rc_u64(RCUR *r) { if (!rc_check(r, 8)) return 0; uint64_t v = ((uint64_t)r->b[r->p] << 56 | (uint64_t)r->b[r->p+1] << 48 | (uint64_t)r->b[r->p+2] << 40 | (uint64_t)r->b[r->p+3] << 32 | (uint64_t)r->b[r->p+4] << 24 | (uint64_t)r->b[r->p+5] << 16 | (uint64_t)r->b[r->p+6] << 8 | r->b[r->p+7]); r->p += 8; return v; }

float rc_f32(RCUR *r) { 
    if (!rc_check(r, 4)) return 0.0f;
    uint32_t u = rc_u32(r); 
    union { uint32_t u; float f; } x; 
    x.u = u; 
    return isfinite(x.f) ? x.f : 0.0f;
}

double rc_f64(RCUR *r) { 
    if (!rc_check(r, 8)) return 0.0;
    uint64_t u = rc_u64(r); 
    union { uint64_t u; double d; } x; 
    x.u = u; 
    return isfinite(x.d) ? x.d : 0.0;
}

int32_t rc_varint(RCUR *r) {
    uint32_t v = 0; 
    int s = 0;
    while (1) {
        if (!rc_check(r, 1)) return 0;
        uint8_t b = r->b[r->p++];
        v |= (uint32_t)(b & 0x7F) << s;
        if (!(b & 0x80)) break;
        s += 7;
        if (s >= 35) return 0;
    }
    return (int32_t)v;
}

static size_t put_varint(uint8_t *buf, int32_t val) {
    uint32_t u = (uint32_t)val; size_t idx = 0;
    while(1) {
        uint8_t b = u & 0x7F;
        u >>= 7;
        if(u > 0) { b |= 0x80; buf[idx++] = b; }
        else { buf[idx++] = b; break; }
    }
    return idx;
}

static void put_string(uint8_t *buf, size_t *p, const char *s) {
    size_t len = strlen(s);
    *p += put_varint(buf + *p, (int32_t)len);
    memcpy(buf + *p, s, len);
    *p += len;
}

static int rc_string(RCUR *r, char *out, size_t out_cap) {
    int32_t len = rc_varint(r);
    if (len < 0 || (size_t)len > r->len - r->p) {
        log_debug("rc_string: invalid len=%d, remaining=%u, pos=%u", len, (unsigned)(r->len - r->p), (unsigned)r->p);
        /* Skip invalid string gracefully to avoid stream corruption */
        return -1;
    }
    if (len > 1024) {
        log_debug("rc_string: len=%d too large, truncating", len);
        len = 1024;
    }
    size_t copy = (size_t)len;
    if (out) {
        if (copy >= out_cap) copy = out_cap - 1;
        memcpy(out, r->b + r->p, copy);
        out[copy] = 0;
    }
    r->p += len;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */
void log_init_once(void) {
    FILE *f = fopen("ux0:data/minecraft_javac.log", "w");
    if(f) fclose(f);
}

void log_debug(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[128]; vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    SceDateTime t; sceRtcGetCurrentClockLocalTime(&t);
    printf("[%02d:%02d:%02d] %s\n", t.hour, t.minute, t.second, buf);
    static FILE *log_f = NULL;
    if (!log_f) log_f = fopen("ux0:data/minecraft_javac.log", "a");
    if(log_f) { fprintf(log_f, "[%02d:%02d:%02d] %s\n", t.hour, t.minute, t.second, buf); fflush(log_f); }
    if(log_count < 6) { strncpy(log_lines[log_count++], buf, 128); }
    else { for(int i=0;i<5;i++) strcpy(log_lines[i], log_lines[i+1]); strncpy(log_lines[5], buf, 128); }
}

void log_get_lines(char lines[6][128], int *count) {
    *count = log_count;
    for(int i=0; i<log_count; i++) strcpy(lines[i], log_lines[i]);
}

/* ------------------------------------------------------------------ */
/* Socket helpers                                                      */
/* ------------------------------------------------------------------ */
static int send_err_retry(int ret) {
    if (ret >= 0) return 0;
    int kerr = *sceNetErrnoLoc();
    uint32_t uerr = (uint32_t)(-ret);
    if (kerr == 11 || kerr == 32 || kerr == 35 || kerr == 36 || kerr == 57 || kerr == 119 ||
        uerr == 0x80410123u || uerr == 0x80410124u)
        return 1;
    return 0;
}

static int net_send_all(const uint8_t *buf, size_t len) {
    if (g_net.sock < 0) {
        static int sock_log = 0;
        if ((sock_log++ % 20) == 0) log_debug("net_send_all: sock < 0, cannot send len=%u", (unsigned)len);
        return -1;
    }
    int nonblock = 1;
    sceNetSetsockopt(g_net.sock, SOL_SOCKET, SO_NONBLOCK, &nonblock, sizeof(nonblock));
    uint32_t tries = 0;
    while (tries < 100) {
        int s = sceNetSend(g_net.sock, buf, len, 0);
        if (s >= 0) {
            if ((size_t)s >= len) {
                static int send_ok_log = 0;
                if ((send_ok_log++ % 20) == 0) log_debug("net_send_all: OK sent=%u len=%u", (unsigned)s, (unsigned)len);
                return 0;
            }
            buf += s;
            len -= (size_t)s;
            tries = 0;
            continue;
        }
        int kerr = *sceNetErrnoLoc();
        /* EPIPE(32) and ECONNRESET(35) mean the connection is dead — do not retry.
           Only transient errors (EAGAIN, EINPROGRESS, ENOBUFS, etc.) are retried briefly. */
        if (kerr == 32 || kerr == 35) {
            log_debug("net_send_all: fatal kerr=%d, closing socket", kerr);
            if (g_net.sock >= 0) { sceNetSocketClose(g_net.sock); g_net.sock = -1; }
            return -1;
        }
        if (kerr == 11 || kerr == 36 || kerr == 57 || kerr == 119 ||
            (uint32_t)(-s) == 0x80410123u || (uint32_t)(-s) == 0x80410124u || (uint32_t)(-s) == 0x80410125u ||
            (uint32_t)(-s) == 0x80410136u || (uint32_t)(-s) == 0x80410137u || (uint32_t)(-s) == 0x80410135u) {
            tries++;
            if ((tries % 20) == 0) log_debug("net_send_all: retry kerr=%d tries=%d len=%u", kerr, tries, (unsigned)len);
            sceKernelDelayThread(10000); /* 10ms delay between retries */
            continue;
        }
        static int send_err_log = 0;
        if ((send_err_log++ % 5) == 0) log_debug("net_send_all: fatal ret=%d kerr=%d len=%u sock=%d", s, kerr, (unsigned)len, g_net.sock);
        return -1;
    }
    log_debug("net_send_all: max retries (100) exceeded len=%u, closing socket", (unsigned)len);
    if (g_net.sock >= 0) { sceNetSocketClose(g_net.sock); g_net.sock = -1; }
    return -1;
}

/* Frame + encrypt + send one packet (id + body) */
static int pkt_send(uint8_t id, const uint8_t *body, size_t body_len) {
    if (g_net.sock < 0 || body_len > 8192) return -1;
    uint8_t pkt[8192];
    size_t n = 0;
    pkt[n++] = id;
    if (body_len) memcpy(pkt + n, body, body_len);
    n += body_len;

uint8_t wire[8192];
    size_t w = 0;
    /* Compression wrapper only valid AFTER server sends EnableCompression.
       Before that, send raw [packet_id][body] per vanilla 1.12.2 behavior.
       Sending [data_length=0] prefix before EnableCompression causes the
       server to misparse our login-phase packets (reads 0x00 as packet ID). */
    if (g_net.comp_threshold >= 0) {
        log_debug("pkt_send: adding compression wrapper (comp_threshold=%d)", g_net.comp_threshold);
        w += put_varint(wire + w, 0); /* data_length = 0 (uncompressed) */
    } else {
        log_debug("pkt_send: NO compression wrapper (comp_threshold=%d, pre-EnableCompression)", g_net.comp_threshold);
    }
    memcpy(wire + w, pkt, n); w += n;

    uint8_t frame[8192];
    size_t f = put_varint(frame, (int32_t)w);
    memcpy(frame + f, wire, w); f += w;

    if (g_net.encrypted && g_net.evp_enc) {
        int outl = 0;
        uint8_t encbuf[8192];
        EVP_EncryptUpdate((EVP_CIPHER_CTX *)g_net.evp_enc, encbuf, &outl, frame, (int)f);
        memcpy(frame, encbuf, (size_t)outl);
        f = (size_t)outl;
    }

    log_debug("pkt_send: sending packet id=0x%02X wire_len=%u frame_len=%u enc=%d",
              id, (unsigned)w, (unsigned)f, g_net.encrypted ? 1 : 0);
    if (w > 0) {
        char hex[128]; int h = 0;
        for (size_t i = 0; i < w && i < 32; i++) h += snprintf(hex + h, sizeof(hex) - h, "%02X ", wire[i]);
        log_debug("pkt_send:   wire=%s", hex);
    }
    if (id == 0x12 || id == 0x14) {
        log_debug("pkt_send:   BLOCK_PACKET id=0x%02X body_len=%u", id, (unsigned)w);
    }
    if (net_send_all(frame, f) == 0) {
        packets_sent++;
        return 0;
    }
    log_debug("pkt_send: packet id=0x%02X failed to send", id);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Encryption (online-mode servers)                                    */
/* ------------------------------------------------------------------ */
static void fill_random(uint8_t *out, size_t n) {
    uint64_t seed = sceKernelGetProcessTimeWide();
    for (size_t i = 0; i < n; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        out[i] = (uint8_t)(seed >> 56);
    }
}

static int rsa_encrypt(const uint8_t *der, size_t der_len,
                       const uint8_t *plain, size_t plain_len,
                       uint8_t *out, size_t out_cap) {
    const uint8_t *p = der;
    RSA *rsa = d2i_RSA_PUBKEY(NULL, &p, (long)der_len);
    if (!rsa) { log_debug("rsa: d2i_PUBKEY failed"); return -1; }
    int r = RSA_public_encrypt((int)plain_len, plain, out, rsa, RSA_PKCS1_PADDING);
    RSA_free(rsa);
    if (r < 0 || (size_t)r > out_cap) { log_debug("rsa: encrypt failed r=%d", r); return -1; }
    return r;
}

static void aes_enable(const uint8_t *key16) {
    g_net.evp_enc = EVP_CIPHER_CTX_new();
    g_net.evp_dec = EVP_CIPHER_CTX_new();
    if (!g_net.evp_enc || !g_net.evp_dec) { log_debug("aes: ctx alloc failed"); return; }
    /* MC uses AES/CFB8 with the IV equal to the key itself */
    EVP_EncryptInit_ex((EVP_CIPHER_CTX *)g_net.evp_enc, EVP_aes_128_cfb8(), NULL, key16, key16);
    EVP_DecryptInit_ex((EVP_CIPHER_CTX *)g_net.evp_dec, EVP_aes_128_cfb8(), NULL, key16, key16);
    EVP_CIPHER_CTX_set_padding((EVP_CIPHER_CTX *)g_net.evp_enc, 0);
    EVP_CIPHER_CTX_set_padding((EVP_CIPHER_CTX *)g_net.evp_dec, 0);
    g_net.encrypted = 1;
}

/* ------------------------------------------------------------------ */
/* Initialization                                                      */
/* ------------------------------------------------------------------ */
int net_init(void) {
    memset(&g_net, 0, sizeof(g_net));
    log_init_once();
    log_debug("net_init: loading NET sysmodule");
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    SceNetInitParam net_param;
    net_param.memory = net_memory_pool;
    net_param.size = sizeof(net_memory_pool);
    net_param.flags = 0;
    sceNetInit(&net_param);
    sceNetCtlInit();
    g_net.phase = PHASE_CONNECTING;
    g_net.sock = -1;
    g_net.comp_threshold = -1;
    g_net.origin_cx = 0;
    g_net.origin_cz = 0;
    return 0;
}

static int net_resolve_host(const char *host, SceNetInAddr *out) {
    if (sceNetInetPton(SCE_NET_AF_INET, host, out) > 0) return 0;
    int rid = sceNetResolverCreate("javac_dns", NULL, 0);
    if (rid < 0) return -1;
    int r = sceNetResolverStartNtoa(rid, host, out, 10000000, 3, 0);
    int rerr = 0;
    sceNetResolverGetError(rid, &rerr);
    sceNetResolverDestroy(rid);
    if (r < 0 || rerr != 0) return -1;
    return 0;
}

int net_connect(const char *host, int port, const char *username) {
    log_debug("net_connect: host=%s port=%d user=%s", host, port, username);
    strncpy(g_net.host, host, 128);
    g_net.port = port;
    strncpy(g_net.username, username, 16);
    g_net.username[16] = 0;
    SceNetInAddr resolved;
    memset(&resolved, 0, sizeof(resolved));
    if (net_resolve_host(host, &resolved) < 0) {
        log_debug("net_connect: resolve failed host=%s", host);
        return -1;
    }
    log_debug("net_connect: resolved %s -> %08X", host, resolved.s_addr);
    int sock = sceNetSocket("net_sock", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    SceNetSockaddrIn addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_len = sizeof(addr);
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port = sceNetHtons((uint16_t)port);
    addr.sin_addr = resolved;
    log_debug("net_connect_socket: connecting...");
    int r = sceNetConnect(sock, (SceNetSockaddr *)&addr, sizeof(addr));
    int net_errno = *sceNetErrnoLoc();
    if (r < 0 && net_errno != 36 && net_errno != 119) {
        log_debug("net_connect: socket failed errno=%d", net_errno);
        sceNetSocketClose(sock);
        return -1;
    }
    /* Set TCP_NODELAY to disable Nagle's algorithm for responsive small packets */
    int nodelay = 1;
    sceNetSetsockopt(sock, 6, 1, &nodelay, sizeof(nodelay)); /* IPPROTO_TCP=6, TCP_NODELAY=1 */
    int nonblock = 1;
    sceNetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nonblock, sizeof(nonblock));
    g_net.sock = sock;
    g_net.phase = PHASE_CONNECTING;
    g_net.rx_len = 0;
    g_net.rx_dec = 0;
    /* reset per-connection protocol state so reconnects start clean */
    g_net.comp_threshold = -1;
    g_net.encrypted = 0;
    if (g_net.evp_enc) { EVP_CIPHER_CTX_free((EVP_CIPHER_CTX *)g_net.evp_enc); g_net.evp_enc = NULL; }
    if (g_net.evp_dec) { EVP_CIPHER_CTX_free((EVP_CIPHER_CTX *)g_net.evp_dec); g_net.evp_dec = NULL; }
    g_net.entity_id = 0;
    g_net.have_spawn = 0;
    g_net.spawn_position_received = 0;
    connect_ticks = 0;
    log_debug("net_connect_socket: loop active asynchronously");
    return sock;
}

void net_disconnect(const char *reason) {
    log_debug("net_disconnect: reason=%s", reason);
    if (g_net.evp_enc) { EVP_CIPHER_CTX_free((EVP_CIPHER_CTX *)g_net.evp_enc); g_net.evp_enc = NULL; }
    if (g_net.evp_dec) { EVP_CIPHER_CTX_free((EVP_CIPHER_CTX *)g_net.evp_dec); g_net.evp_dec = NULL; }
    g_net.encrypted = 0;
    if (g_net.sock >= 0) { sceNetSocketClose(g_net.sock); g_net.sock = -1; }
}

/* ------------------------------------------------------------------ */
/* Login packet handlers                                               */
/* ------------------------------------------------------------------ */
static void handle_encryption_request(RCUR *r) {
    char server_id[64];
    if (rc_string(r, server_id, sizeof(server_id)) < 0) return;
    int32_t pk_len = rc_varint(r);
    if (pk_len < 0 || (size_t)pk_len > r->len - r->p) return;
    const uint8_t *pubkey = r->b + r->p; r->p += pk_len;
    int32_t tok_len = rc_varint(r);
    if (tok_len < 0 || (size_t)tok_len > r->len - r->p) return;
    const uint8_t *token = r->b + r->p; r->p += tok_len;

    log_debug("login: encryption request pk=%d tok=%d", pk_len, tok_len);

    uint8_t shared[16];
    fill_random(shared, sizeof(shared));

    uint8_t enc_secret[512], enc_token[512];
    int s_len = rsa_encrypt(pubkey, (size_t)pk_len, shared, sizeof(shared), enc_secret, sizeof(enc_secret));
    int t_len = rsa_encrypt(pubkey, (size_t)pk_len, token, (size_t)tok_len, enc_token, sizeof(enc_token));
    if (s_len < 0 || t_len < 0) { net_disconnect("rsa failed"); return; }

    uint8_t body[1100];
    size_t n = 0;
    n += put_varint(body + n, s_len);
    memcpy(body + n, enc_secret, s_len); n += s_len;
    n += put_varint(body + n, t_len);
    memcpy(body + n, enc_token, t_len); n += t_len;
    pkt_send(0x01, body, n);

    aes_enable(shared);
    log_debug("login: encryption response sent, AES enabled");
}

static void handle_login_success(RCUR *r) {
    char uuid[64], name[20];
    if (rc_string(r, uuid, sizeof(uuid)) < 0) return;
    if (rc_string(r, name, sizeof(name)) < 0) return;
    strncpy(g_net.username, name, 16);
    g_net.username[16] = 0;
    log_debug("login: success uuid=%s name=%s", uuid, g_net.username);
    g_net.phase = PHASE_PLAY;
    /* Client Settings is sent once Join Game arrives (vanilla behavior) */
}

static void handle_enable_compression(RCUR *r) {
    int32_t threshold = rc_varint(r);
    g_net.comp_threshold = (int)threshold;
    log_debug("login: compression enabled threshold=%d", g_net.comp_threshold);
}

/* ------------------------------------------------------------------ */
/* Play packet handlers                                                */
/* ------------------------------------------------------------------ */
static void extract_chat_text(const char *json, char *out, size_t cap) {
    const char *t = strstr(json, "\"text\":\"");
    if (!t) { strncpy(out, json, cap - 1); out[cap-1] = 0; return; }
    t += 8;
    size_t i = 0;
    while (*t && *t != '"' && i < cap - 1) {
        if (*t == '\\' && t[1]) { t++; }
        out[i++] = *t++;
    }
    out[i] = 0;
}

static void handle_keep_alive(RCUR *r) {
    uint64_t id = rc_u64(r);
    static uint64_t seen_ids[8];
    static int seen_count = 0;
    for (int i = 0; i < seen_count; i++) {
        if (seen_ids[i] == id) {
            log_debug("play: keepalive id=%llu already sent, dedup (slot %d)",
                      (unsigned long long)id, i);
            return;
        }
    }
    if (seen_count < 8) seen_ids[seen_count++] = id;
    else {
        /* Ring buffer: shift out oldest, append newest */
        memmove(seen_ids, seen_ids + 1, sizeof(uint64_t) * 7);
        seen_ids[7] = id;
    }
    uint8_t body[8];
    for (int i = 0; i < 8; i++) body[i] = (uint8_t)(id >> (8 * (7 - i)));
    keepalive_send_time = sceKernelGetProcessTimeWide();
    log_debug("play: received KeepAlive (0x1F) id=%llu, responding with 0x0B",
              (unsigned long long)id);
    log_debug("play: keepalive bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
              body[0], body[1], body[2], body[3], body[4], body[5], body[6], body[7]);
    pkt_send(0x0B, body, 8);
}

static void handle_confirm_teleport(RCUR *r) {
    int32_t tid = rc_varint(r);
    uint8_t body[8];
    size_t n = put_varint(body, tid);
    pkt_send(0x00, body, n);
}

static void handle_join_game(RCUR *r) {
    log_debug("play: parsing join game, remaining=%u", (unsigned)(r->len - r->p));
    
    /* If already in PLAY phase with valid position, treat as respawn/update not full reset */
    int already_in_play = (g_net.phase == PHASE_PLAY && position_look_received);
    
    g_net.entity_id = rc_u32(r);
    log_debug("play: join game eid=%d", g_net.entity_id);
    uint8_t gm = rc_u8(r);
    g_net.gamemode = gm & 7;
    int32_t dim = (int32_t)rc_u32(r);
    log_debug("play: raw dim=%d", dim);
    if (dim < -1 || dim > 1) {
        log_debug("play: invalid dimension %d, clamping to 0", dim);
        dim = 0;
    }
    g_net.dimension = dim;
    uint8_t diff = rc_u8(r);
    uint8_t maxp = rc_u8(r);
    char wtype[32];
    if (rc_string(r, wtype, sizeof(wtype)) < 0) {
        log_debug("play: failed to read world type");
        strcpy(wtype, "default");
    }
    uint8_t rdi = rc_u8(r);
    (void)diff; (void)maxp; (void)rdi;
    log_debug("play: join game eid=%d dim=%d gm=%d world=%s (already_in_play=%d)",
              g_net.entity_id, g_net.dimension, g_net.gamemode, wtype, already_in_play);

    /* Only reset fallback state on FIRST join game (phase transition to PLAY), not respawns */
    if (g_net.phase != PHASE_PLAY) {
        position_look_received = 0;
        last_position_look_time = sceKernelGetProcessTimeWide();
    } else {
        /* For respawns, keep position but reset timer */
        last_position_look_time = sceKernelGetProcessTimeWide();
    }
    /* Reset teleport confirm tracking on join/respawn — server starts teleportId at 0 */
    {
        static int last_join_eid = -999;
        if ((int)g_net.entity_id != last_join_eid) {
            last_confirmed_tid = -1;
            last_join_eid = (int)g_net.entity_id;
        }
    }

    /* CPacketClientSettings: lang(<=16), view, chat vis, colors, model parts, main hand */

    /* CPacketClientSettings: lang(<=16), view, chat vis, colors, model parts, main hand */
    uint8_t body[64];
    size_t n = 0;
    put_string(body, &n, "en_US");
    body[n++] = 2;   /* render distance (capped to 2 for performance) */
    body[n++] = 0;   /* chat: full */
    body[n++] = 1;   /* colors */
    body[n++] = 0;   /* skin parts */
    body[n++] = 1;   /* main hand: right */
    pkt_send(0x04, body, n);
}

static void handle_position_look(RCUR *r) {
    log_debug("play: parsing position look, remaining=%u", (unsigned)(r->len - r->p));
    double x = rc_f64(r);
    double y = rc_f64(r);
    double z = rc_f64(r);
    float yaw = rc_f32(r);
    float pitch = rc_f32(r);
    uint8_t flags = rc_u8(r);
    int32_t tid = rc_varint(r);
    int xi = (int)(x * 100);
    int yi = (int)(y * 100);
    int zi = (int)(z * 100);
    int yawi = (int)(yaw * 100);
    int pitchi = (int)(pitch * 100);
    log_debug("play: pos x=%d y=%d z=%d yaw=%d pitch=%d flags=%02X tid=%d (x100)", xi, yi, zi, yawi, pitchi, flags, tid);

    if (!isfinite(x) || !isfinite(y) || !isfinite(z) ||
        !isfinite(yaw) || !isfinite(pitch)) {
        log_debug("play: invalid position/rotation, ignoring");
        return;
    }

    if (flags & 0x01) x += g_player ? g_player->x : 0;
    if (flags & 0x02) y += g_player ? g_player->y : 0;
    if (flags & 0x04) z += g_player ? g_player->z : 0;
    if (flags & 0x08) yaw   += g_player ? g_player->yaw : 0;
    if (flags & 0x10) pitch += g_player ? g_player->pitch : 0;

    if (!isfinite(x) || !isfinite(y) || !isfinite(z) ||
        !isfinite(yaw) || !isfinite(pitch)) {
        log_debug("play: invalid position after flags, ignoring");
        return;
    }

    if ((int)tid <= last_confirmed_tid) {
        log_debug("play: skipping teleport confirm tid=%d (last=%d, not advancing)", (int)tid, last_confirmed_tid);
        return;
    }
    last_confirmed_tid = (int)tid;

    uint8_t body[8];
    size_t n = put_varint(body, tid);
    log_debug("play: sending Confirm Teleport (tid=%d)", tid);
    int send_result = pkt_send(0x00, body, n);
    if (send_result != 0) {
        log_debug("play: Confirm Teleport send failed (tid=%d), no retry", tid);
    }
    log_debug("play: Confirm Teleport sent for tid=%d", tid);

    if (g_player) {
        g_player->x = (float)x; g_player->y = (float)y; g_player->z = (float)z;
        g_player->render_x = g_player->x;
        g_player->render_y = g_player->y;
        g_player->render_z = g_player->z;
        g_player->yaw = yaw; g_player->pitch = pitch;
        g_player->on_ground = 1;
        g_player->vx = g_player->vy = g_player->vz = 0;
    }
    g_net.pos_x = x; g_net.pos_y = y; g_net.pos_z = z;
    g_net.pos_yaw = yaw; g_net.pos_pitch = pitch;
    g_net.teleport_id = (int)tid;

    int cx = (int)floorf((float)x / 16.0f);
    int cz = (int)floorf((float)z / 16.0f);
    if (cx != g_net.origin_cx || cz != g_net.origin_cz || !g_net.have_spawn) {
        int was_in_play = g_net.have_spawn;
        int old_origin_cx = g_net.origin_cx;
        int old_origin_cz = g_net.origin_cz;
        g_net.origin_cx = cx;
        g_net.origin_cz = cz;
        g_net.have_spawn = 1;
        
if (was_in_play) {
            int dx = cx - old_origin_cx;
            int dz = cz - old_origin_cz;

            /* Guard: if the shift is larger than the world window, the old
               origin was garbage (sign extension bug) — just clear all
               chunks and start fresh instead of trying to shift. */
            if (abs(dx) >= WORLD_SIZE_X || abs(dz) >= WORLD_SIZE_Z) {
                log_debug("play: large shift (%d,%d) - clearing all chunks", dx, dz);
                for (int ix = 0; ix < WORLD_SIZE_X; ix++) {
                    for (int iz = 0; iz < WORLD_SIZE_Z; iz++) {
                        Chunk *c = &world_chunks[ix][iz];
                        chunk_free_mesh(c);
                        memset(c->blocks, 0, sizeof(c->blocks));
                        c->loaded = 0;
                        c->vertex_count = 0;
                        c->index_count = 0;
                        c->dirty = 0;
                    }
                }
            } else if (dx != 0 || dz != 0) {
                log_debug("play: shifting window by (%d,%d) from (%d,%d) to (%d,%d)", dx, dz, old_origin_cx, old_origin_cz, cx, cz);
                
                // In-place shift: shift X first, then Z
                // Shift X
                if (dx > 0) {
                    // Moving positive X - shift left (toward 0)
                    for (int ix = 0; ix < WORLD_SIZE_X - dx; ix++) {
                        for (int iz = 0; iz < WORLD_SIZE_Z; iz++) {
                            Chunk *src = &world_chunks[ix + dx][iz];
                            Chunk *dst = &world_chunks[ix][iz];
                            memcpy(dst->blocks, src->blocks, sizeof(dst->blocks));
                            dst->loaded = src->loaded;
                            dst->dirty = src->dirty;
                        }
                    }
                    // Clear right edge
                    for (int ix = WORLD_SIZE_X - dx; ix < WORLD_SIZE_X; ix++) {
                        for (int iz = 0; iz < WORLD_SIZE_Z; iz++) {
                            Chunk *c = &world_chunks[ix][iz];
                            chunk_free_mesh(c);
                            memset(c->blocks, 0, sizeof(c->blocks));
                            c->loaded = 0;
                            c->dirty = 0;
                        }
                    }
                } else if (dx < 0) {
                    // Moving negative X - shift right (toward end)
                    int adx = -dx;
                    for (int ix = WORLD_SIZE_X - 1; ix >= adx; ix--) {
                        for (int iz = 0; iz < WORLD_SIZE_Z; iz++) {
                            Chunk *src = &world_chunks[ix - adx][iz];
                            Chunk *dst = &world_chunks[ix][iz];
                            memcpy(dst->blocks, src->blocks, sizeof(dst->blocks));
                            dst->loaded = src->loaded;
                            dst->dirty = src->dirty;
                        }
                    }
                    // Clear left edge
                    for (int ix = 0; ix < adx; ix++) {
                        for (int iz = 0; iz < WORLD_SIZE_Z; iz++) {
                            Chunk *c = &world_chunks[ix][iz];
                            chunk_free_mesh(c);
                            memset(c->blocks, 0, sizeof(c->blocks));
                            c->loaded = 0;
                            c->dirty = 0;
                        }
                    }
                }
                
                // Shift Z
                if (dz > 0) {
                    // Moving positive Z - shift down (toward 0)
                    for (int ix = 0; ix < WORLD_SIZE_X; ix++) {
                        for (int iz = 0; iz < WORLD_SIZE_Z - dz; iz++) {
                            Chunk *src = &world_chunks[ix][iz + dz];
                            Chunk *dst = &world_chunks[ix][iz];
                            memcpy(dst->blocks, src->blocks, sizeof(dst->blocks));
                            dst->loaded = src->loaded;
                            dst->dirty = src->dirty;
                        }
                    }
                    // Clear bottom edge
                    for (int ix = 0; ix < WORLD_SIZE_X; ix++) {
                        for (int iz = WORLD_SIZE_Z - dz; iz < WORLD_SIZE_Z; iz++) {
                            Chunk *c = &world_chunks[ix][iz];
                            chunk_free_mesh(c);
                            memset(c->blocks, 0, sizeof(c->blocks));
                            c->loaded = 0;
                            c->dirty = 0;
                        }
                    }
                } else if (dz < 0) {
                    // Moving negative Z - shift up (toward end)
                    int adz = -dz;
                    for (int ix = 0; ix < WORLD_SIZE_X; ix++) {
                        for (int iz = WORLD_SIZE_Z - 1; iz >= adz; iz--) {
                            Chunk *src = &world_chunks[ix][iz - adz];
                            Chunk *dst = &world_chunks[ix][iz];
                            memcpy(dst->blocks, src->blocks, sizeof(dst->blocks));
                            dst->loaded = src->loaded;
                            dst->dirty = src->dirty;
                        }
                    }
                    // Clear top edge
                    for (int ix = 0; ix < WORLD_SIZE_X; ix++) {
                        for (int iz = 0; iz < adz; iz++) {
                            Chunk *c = &world_chunks[ix][iz];
                            chunk_free_mesh(c);
                            memset(c->blocks, 0, sizeof(c->blocks));
                            c->loaded = 0;
                            c->dirty = 0;
                        }
                    }
                }
                
                // Update x_off/z_off for all chunks
                for (int ix = 0; ix < WORLD_SIZE_X; ix++) {
                    for (int iz = 0; iz < WORLD_SIZE_Z; iz++) {
                        Chunk *c = &world_chunks[ix][iz];
                        c->x_off = (g_net.origin_cx + ix - WORLD_SIZE_X/2) * 16;
                        c->z_off = (g_net.origin_cz + iz - WORLD_SIZE_Z/2) * 16;
                        if (!c->loaded) {
                            c->vertex_count = 0;
                            c->index_count = 0;
                            c->dirty = 0;
                            c->rebuild_phase = 0;
                        }
                    }
                }
            }
            log_debug("play: window recenter to (%d,%d) - shifted existing chunks", cx, cz);
        } else {
            log_debug("play: initial window center to (%d,%d), clearing chunks", cx, cz);
            for (int ix = 0; ix < WORLD_SIZE_X; ix++) {
                for (int iz = 0; iz < WORLD_SIZE_Z; iz++) {
                    Chunk *c = &world_chunks[ix][iz];
                    chunk_free_mesh(c);
                    memset(c->blocks, 0, sizeof(c->blocks));
                    c->loaded = 0;
                    c->vertex_count = 0;
                    c->index_count = 0;
                    c->dirty = 0;
                    c->x_off = (g_net.origin_cx + ix - WORLD_SIZE_X/2) * 16;
                    c->z_off = (g_net.origin_cz + iz - WORLD_SIZE_Z/2) * 16;
                    c->rebuild_phase = 0;
                }
            }
        }
    }
    int xi2 = (int)(x * 100);
    int yi2 = (int)(y * 100);
    int zi2 = (int)(z * 100);
    log_debug("play: position x=%d y=%d z=%d (x100)", xi2, yi2, zi2);

    position_look_received = 1;
    last_position_look_time = sceKernelGetProcessTimeWide();
    log_debug("play: Confirm Teleport sent for tid=%d", tid);

    /* Trigger mesh rebuild for all loaded chunks after Position & Look */
    for (int cx = 0; cx < WORLD_SIZE_X; cx++) {
        for (int cz = 0; cz < WORLD_SIZE_Z; cz++) {
            if (world_chunks[cx][cz].loaded && world_chunks[cx][cz].dirty) {
                chunk_rebuild_mesh(&world_chunks[cx][cz]);
            }
        }
    }
}

static void handle_spawn_position(RCUR *r) {
    uint64_t packed = rc_u64(r);
    /* 1.12.2 SpawnPosition (0x46) packed long layout:
       x = (packed << 2) >> 52   (26 bits, signed)
       y = (packed << 12) >> 52  (26 bits, signed)
       z = packed >> 43           (21 bits, signed)
       The old code used wrong bit positions and masks, producing
       garbage values like origin_cz = 65536 which caused the
       65522-chunk window shift crash. */
    int32_t x = (int32_t)((int64_t)(packed << 2) >> 52);
    int32_t y = (int32_t)((int64_t)(packed << 12) >> 52);
    int32_t z = (int32_t)((int64_t)packed >> 43);

    if (x < -30000000 || x > 30000000 || y < 0 || y > 256 || z < -30000000 || z > 30000000) {
        log_debug("play: invalid spawn position %d %d %d", x, y, z);
        return;
    }

    g_net.spawn_x = x; g_net.spawn_y = y; g_net.spawn_z = z;
    g_net.origin_cx = x >> 4;
    g_net.origin_cz = z >> 4;
    g_net.have_spawn = 1;
    g_net.spawn_position_received = 1;
    log_debug("play: Spawn Position packet (0x46) received: %d %d %d -> origin (%d,%d)", x, y, z, g_net.origin_cx, g_net.origin_cz);

    /* Send Client Status (0x03) with action=0 (PERFORM_RESPAWN) to tell
       the server we're ready to play.  Without this, some servers keep
       the player in a "loading" state and eventually time out. */
    if (g_net.phase == PHASE_PLAY) {
        uint8_t cs_body[1] = { 0 };  /* action 0 = PERFORM_RESPAWN */
        pkt_send(0x03, cs_body, 1);
    }
}

static void handle_chunk_data(RCUR *r) {
    int32_t cx = (int32_t)rc_u32(r);
    int32_t cz = (int32_t)rc_u32(r);
    uint8_t full = rc_u8(r);
    int32_t mask = rc_varint(r);
    int32_t dlen = rc_varint(r);
    if (dlen < 0 || (size_t)dlen > r->len - r->p) {
        log_debug("handle_chunk_data: invalid dlen=%d", dlen);
        return;
    }
    
    /* If we don't have a valid origin yet (no spawn position received), set it from the first chunk */
    if (!g_net.spawn_position_received && (!g_net.have_spawn || (g_net.origin_cx == 0 && g_net.origin_cz == 0))) {
        g_net.origin_cx = cx;
        g_net.origin_cz = cz;
        g_net.have_spawn = 1;
        log_debug("handle_chunk_data: INITIAL origin from first chunk to (%d,%d)", cx, cz);
    }
    
    /* If chunk is out of bounds and we don't have spawn position yet, 
       shift window ONCE to center on the chunk cluster */
    int ix = cx - g_net.origin_cx + WORLD_SIZE_X / 2;
    int iz = cz - g_net.origin_cz + WORLD_SIZE_Z / 2;
    static bool window_shifted = false;
    if ((ix < 0 || ix >= WORLD_SIZE_X || iz < 0 || iz >= WORLD_SIZE_Z) && !g_net.spawn_position_received && !window_shifted) {
        /* Calculate shift to center this chunk in window */
        int dx = ix - WORLD_SIZE_X / 2;
        int dz = iz - WORLD_SIZE_Z / 2;
        
        log_debug("handle_chunk_data: ONCE shifting window by (%d,%d) to center chunk (%d,%d) at (%d,%d)", dx, dz, cx, cz, ix, iz);
        
        // In-place shift
        if (dx > 0) {
            for (int i = 0; i < WORLD_SIZE_X - dx; i++) for (int z = 0; z < WORLD_SIZE_Z; z++) {
                Chunk *src = &world_chunks[i + dx][z];
                Chunk *dst = &world_chunks[i][z];
                memcpy(dst->blocks, src->blocks, sizeof(dst->blocks));
                dst->loaded = src->loaded; dst->dirty = src->dirty;
            }
            for (int i = WORLD_SIZE_X - dx; i < WORLD_SIZE_X; i++) for (int z = 0; z < WORLD_SIZE_Z; z++) {
                Chunk *c = &world_chunks[i][z]; chunk_free_mesh(c); memset(c->blocks, 0, sizeof(c->blocks)); c->loaded = 0; c->dirty = 0;
            }
        } else if (dx < 0) {
            int adx = -dx;
            for (int i = WORLD_SIZE_X - 1; i >= adx; i--) for (int z = 0; z < WORLD_SIZE_Z; z++) {
                Chunk *src = &world_chunks[i - adx][z];
                Chunk *dst = &world_chunks[i][z];
                memcpy(dst->blocks, src->blocks, sizeof(dst->blocks));
                dst->loaded = src->loaded; dst->dirty = src->dirty;
            }
            for (int i = 0; i < adx; i++) for (int z = 0; z < WORLD_SIZE_Z; z++) {
                Chunk *c = &world_chunks[i][z]; chunk_free_mesh(c); memset(c->blocks, 0, sizeof(c->blocks)); c->loaded = 0; c->dirty = 0;
            }
        }
        if (dz > 0) {
            for (int i = 0; i < WORLD_SIZE_X; i++) for (int z = 0; z < WORLD_SIZE_Z - dz; z++) {
                Chunk *src = &world_chunks[i][z + dz];
                Chunk *dst = &world_chunks[i][z];
                memcpy(dst->blocks, src->blocks, sizeof(dst->blocks));
                dst->loaded = src->loaded; dst->dirty = src->dirty;
            }
            for (int i = 0; i < WORLD_SIZE_X; i++) for (int z = WORLD_SIZE_Z - dz; z < WORLD_SIZE_Z; z++) {
                Chunk *c = &world_chunks[i][z]; chunk_free_mesh(c); memset(c->blocks, 0, sizeof(c->blocks)); c->loaded = 0; c->dirty = 0;
            }
        } else if (dz < 0) {
            int adz = -dz;
            for (int i = 0; i < WORLD_SIZE_X; i++) for (int z = WORLD_SIZE_Z - 1; z >= adz; z--) {
                Chunk *src = &world_chunks[i][z - adz];
                Chunk *dst = &world_chunks[i][z];
                memcpy(dst->blocks, src->blocks, sizeof(dst->blocks));
                dst->loaded = src->loaded; dst->dirty = src->dirty;
            }
            for (int i = 0; i < WORLD_SIZE_X; i++) for (int z = 0; z < adz; z++) {
                Chunk *c = &world_chunks[i][z]; chunk_free_mesh(c); memset(c->blocks, 0, sizeof(c->blocks)); c->loaded = 0; c->dirty = 0;
            }
        }
        
        g_net.origin_cx += dx;
        g_net.origin_cz += dz;
        window_shifted = true;
        
        // Recalculate indices after shift
        ix = cx - g_net.origin_cx + WORLD_SIZE_X / 2;
        iz = cz - g_net.origin_cz + WORLD_SIZE_Z / 2;
        
        log_debug("handle_chunk_data: shifted origin to (%d,%d), new chunk index=(%d,%d)", g_net.origin_cx, g_net.origin_cz, ix, iz);
    }
    
    chunk_decode((int)cx, (int)cz, full ? 1 : 0, (int)mask, r->b + r->p, (size_t)dlen);
    int32_t nte = rc_varint(r);
    (void)nte;
    ix = cx - g_net.origin_cx + WORLD_SIZE_X / 2;
    iz = cz - g_net.origin_cz + WORLD_SIZE_Z / 2;
    if (ix >= 0 && ix < WORLD_SIZE_X && iz >= 0 && iz < WORLD_SIZE_Z) {
        if (world_chunks[ix][iz].loaded && world_chunks[ix][iz].dirty) {
            chunk_rebuild_mesh(&world_chunks[ix][iz]);
        }
    } else {
        log_debug("handle_chunk_data: chunk out of bounds cx=%d cz=%d ix=%d iz=%d origin=(%d,%d)", cx, cz, ix, iz, g_net.origin_cx, g_net.origin_cz);
    }
}

static void handle_unload_chunk(RCUR *r) {
    int32_t cx = (int32_t)rc_u32(r);
    int32_t cz = (int32_t)rc_u32(r);
    int ix = cx - g_net.origin_cx + WORLD_SIZE_X / 2;
    int iz = cz - g_net.origin_cz + WORLD_SIZE_Z / 2;
    if (ix >= 0 && ix < WORLD_SIZE_X && iz >= 0 && iz < WORLD_SIZE_Z) {
        Chunk *c = &world_chunks[ix][iz];
        chunk_free_mesh(c);
        memset(c->blocks, 0, sizeof(c->blocks));
        c->loaded = 0;
        c->vertex_count = 0;
        c->index_count = 0;
        c->dirty = 0;
    }
}

static void handle_chat(RCUR *r) {
    char json[512];
    if (rc_string(r, json, sizeof(json)) < 0) return;
    uint8_t type = rc_u8(r);
    char text[128];
    extract_chat_text(json, text, sizeof(text));
    log_debug("chat[%d]: %s", type, text);
}

static void handle_respawn(RCUR *r) {
    int32_t dim = rc_u32(r);
    uint8_t diff = rc_u8(r);
    uint8_t gm = rc_u8(r);
    char wtype[20];
    rc_string(r, wtype, sizeof(wtype));
    g_net.dimension = dim;
    g_net.gamemode = gm & 7;
    log_debug("play: respawn dim=%d gm=%d world=%s", dim, gm & 7, wtype);
    if (g_player) {
        g_player->x = 8.0f; g_player->y = 64.0f; g_player->z = 8.0f;
        g_player->vx = g_player->vy = g_player->vz = 0.0f;
        g_player->render_x = g_player->x;
        g_player->render_y = g_player->y;
        g_player->render_z = g_player->z;
        g_player->on_ground = 1;
    }
    /* Server's Respawn packet resets position. Only hide the respawn screen
       if the player is actually alive — if health is still 0, keep it
       visible so the player can press O/X to respawn.  The server sends
       Respawn before UpdateHealth, so without this check the respawn
       screen gets hidden before the player knows they're dead. */
    extern int show_respawn;
    if (!g_player || g_player->health > 0) {
        show_respawn = 0;
    }
    log_debug("respawn: server respawn received, position reset (health=%.1f, show_respawn=%d)",
              g_player ? g_player->health : -1.0f, show_respawn);

    /* Clear all chunks so the server's new chunk stream starts fresh.
       Leaving stale chunks loaded causes OOB writes when the server
       sends new chunks at the respawn position with different offsets. */
    extern void clear_all_chunks(void);
    clear_all_chunks();
    log_debug("respawn: cleared all chunks");
}

static void handle_disconnect(const char *stage, RCUR *r) {
    char reason[256];
    rc_string(r, reason, sizeof(reason));
    char text[128];
    extract_chat_text(reason, text, sizeof(text));
    log_debug("%s: disconnected: %s", stage, text);
    net_disconnect("server disconnect");
}

static void net_dispatch_play(const uint8_t *pk, int32_t len) {
    RCUR r = { pk, (size_t)len, 0 };
    int32_t id = rc_varint(&r);
    switch (id) {
            case 0x0B: /* SPacketBlockChange */
                {
                    int32_t bx = (int32_t)rc_u32(&r);
                    int32_t by = (int32_t)rc_u8(&r);
                    int32_t bz = (int32_t)rc_u32(&r);
                    int32_t bid = rc_varint(&r);
                    extern void chunk_set_block(int x, int y, int z, uint16_t packed);
                    chunk_set_block(bx, by, bz, (uint16_t)((bid << 4) | 0));
                }
                break;
            case 0x14: /* SPacketWindowItems */
                {
                    rc_u8(&r); /* window id */
                    int32_t count = rc_varint(&r);
                    for (int i = 0; i < count; i++) {
                        /* Parse Slot: Present(byte) + ItemID(varint) + Count(byte) + Damage(short) + NBT */
                        uint8_t present = rc_u8(&r);
                        if (present) {
                            int32_t item_id = rc_varint(&r);
                            rc_u8(&r); /* count */
                            rc_u16(&r); /* damage */
                            uint8_t nbt = rc_u8(&r);
                            if (nbt) {
                                /* Skip NBT compound: read string name + tags until 0x00 */
                            }
                        }
                    }
                }
                break;
            case 0x16: /* SPacketSetSlot */
                {
                    rc_u8(&r); /* window id */
                    int32_t slot = rc_varint(&r);
                    uint8_t present = rc_u8(&r);
                    if (present) {
                        int32_t item_id = rc_varint(&r);
                        if (slot >= 0 && slot < 9) {
                            g_net.held_item_id = item_id;
                            g_net.have_held_item = 1;
                        }
                        rc_u8(&r); /* count */
                        rc_u16(&r); /* damage */
                        uint8_t nbt = rc_u8(&r);
                        (void)nbt;
                    } else if (slot >= 0 && slot < 9) {
                        g_net.held_item_id = 0;
                        g_net.have_held_item = 1;
                    }
                }
                break;
            case 0x35: handle_respawn(&r); break;
        case 0x0F: handle_chat(&r); break;
        case 0x1A: handle_disconnect("play", &r); break;
        case 0x1D: handle_unload_chunk(&r); break;
        case 0x1F: handle_keep_alive(&r); break;
        case 0x20: handle_chunk_data(&r); break;
        case 0x23: handle_join_game(&r); break;
        case 0x2F: if (!position_look_received) handle_position_look(&r); break;
        case 0x46: handle_spawn_position(&r); break;
        case 0x2C: /* SPacketPlayerAbilities */
            rc_u8(&r); // flags
            rc_f32(&r); // fly speed
            rc_f32(&r); // walk speed
            log_debug("play: player abilities");
            break;
        case 0x4F: /* SPacketEntityEffect */
            rc_varint(&r); // entity id
            rc_u8(&r); // effect id
            rc_u8(&r); // amplifier
            rc_varint(&r); // duration
            rc_u8(&r); // hide particles
            log_debug("play: entity effect");
            break;
        case 0x1B: /* SPacketEntityStatus */
            {
                uint32_t eid = rc_varint(&r); // entity id
                uint8_t status = rc_u8(&r); // status
                log_debug("play: entity status eid=%u status=%d", eid, status);
                if (eid == g_net.entity_id && (status == 2 || status == 3)) {
                    extern int show_respawn;
                    show_respawn = 1;
                }
            }
            break;
        case 0x36: /* SPacketEntityHeadLook */
            rc_varint(&r); // entity id
            rc_u8(&r); // head yaw
            log_debug("play: entity head look");
            break;
        case 0x3C: /* SPacketEntityMetadata */
            rc_varint(&r); // entity id
            for (int i = 0; i < 100 && r.p < r.len; i++) {
                uint8_t b = rc_u8(&r);
                if (b == 0x7F) break;
                if (r.p >= r.len) break;
                rc_u8(&r);
            }
            log_debug("play: entity metadata");
            break;
        case 0x44: /* SPacketTeams */
            { char tmp[256]; rc_string(&r, tmp, sizeof(tmp)); } // team name
            rc_u8(&r); // mode
            if (rc_u8(&r)) { }
            log_debug("play: teams");
            break;
        case 0x4C: /* SPacketEntityTeleport */
            rc_varint(&r); // entity id
            rc_f64(&r); // x
            rc_f64(&r); // y
            rc_f64(&r); // z
            rc_f32(&r); // yaw
            rc_f32(&r); // pitch
            rc_u8(&r); // on ground
            log_debug("play: entity teleport");
            break;
        case 0x3A: /* SPacketHeldItemChange */
            rc_u8(&r); // slot
            log_debug("play: held item change");
            break;
        case 0x3E: /* SPacketEntityVelocity */
            rc_varint(&r); // entity id
            rc_u16(&r); // x velocity (short)
            rc_u16(&r); // y velocity (short)
            rc_u16(&r); // z velocity (short)
            log_debug("play: entity velocity");
            break;
        case 0x3F: /* SPacketEntityEquipment */
            rc_varint(&r); // entity id
            while (r.p < r.len) {
                int32_t slot = rc_varint(&r);
                if (slot < 0) break;
                rc_varint(&r); // item id
                rc_u16(&r); // item damage
                if (rc_u8(&r)) { }
            }
            log_debug("play: entity equipment");
            break;
        case 0x41: /* SPacketUpdateHealth */
            {
                float h = rc_f32(&r); // health
                rc_varint(&r); // food
                rc_f32(&r); // food saturation
                if (g_player) g_player->health = h;
                log_debug("play: update health=%.1f", h);
            }
            break;
        case 0x47: /* SPacketTimeUpdate */
            rc_u64(&r); // world age
            rc_u64(&r); // time of day
            log_debug("play: time update");
            break;
        case 0x1E: /* SPacketChangeGameState */
            rc_u8(&r); // game state
            rc_f32(&r); // value
            log_debug("play: change game state");
            break;
        case 0x34: /* SPacketResourcePackSend */
            { char tmp[256]; rc_string(&r, tmp, sizeof(tmp)); } // url
            { char tmp2[64]; rc_string(&r, tmp2, sizeof(tmp2)); } // hash
            log_debug("play: resource pack send");
            break;
        case 0x0D: /* SPacketServerDifficulty */
            rc_u8(&r); // difficulty
            log_debug("play: server difficulty");
            break;
        case 0x2E: /* SPacketPlayerListItem */
            rc_u8(&r); // action
            rc_varint(&r); // count
            log_debug("play: player list item");
            break;
        case 0x25: /* SPacketEntity */
            rc_varint(&r); // entity id
            log_debug("play: entity");
            break;
        case 0x26: /* SPacketEntity.S15PacketEntityRelMove */
            rc_varint(&r); // entity id
            rc_u16(&r); // dx
            rc_u16(&r); // dy
            rc_u16(&r); // dz
            rc_u8(&r); // on ground
            log_debug("play: entity rel move");
            break;
        case 0x27: /* SPacketEntity.S17PacketEntityLookMove */
            rc_varint(&r); // entity id
            rc_u16(&r); // dx
            rc_u16(&r); // dy
            rc_u16(&r); // dz
            rc_u8(&r); // yaw
            rc_u8(&r); // pitch
            rc_u8(&r); // on ground
            log_debug("play: entity look move");
            break;
        case 0x28: /* SPacketEntity.S16PacketEntityLook */
            rc_varint(&r); // entity id
            rc_u8(&r); // yaw
            rc_u8(&r); // pitch
            rc_u8(&r); // on ground
            log_debug("play: entity look");
            break;
        case 0x32: /* SPacketDestroyEntities */
            { int32_t cnt = rc_varint(&r); for (int i = 0; i < cnt && r.p < r.len; i++) rc_varint(&r); }
            log_debug("play: destroy entities");
            break;
        default:
            log_debug("play: unknown packet 0x%02X len=%d", id, len);
            break;
    }
}

static void net_dispatch_login(const uint8_t *pk, int32_t len) {
    RCUR r = { pk, (size_t)len, 0 };
    int32_t id = rc_varint(&r);
    switch (id) {
        case 0x00: handle_disconnect("login", &r); break;
        case 0x01: handle_encryption_request(&r); break;
        case 0x02: handle_login_success(&r); break;
        case 0x03: handle_enable_compression(&r); break;
        default: break;
    }
}

/* ------------------------------------------------------------------ */
/* Receive path                                                        */
/* ------------------------------------------------------------------ */
static void net_drain_socket(void) {
    /* Drain all currently-available data (socket is non-blocking). */
    while (g_net.rx_len < NET_RX_CAP) {
        int r = sceNetRecv(g_net.sock, g_net.rx + g_net.rx_len, NET_RX_CAP - g_net.rx_len, 0);
        if (r > 0) {
            g_net.rx_len += (size_t)r;
        } else if (r == 0) {
            net_disconnect("connection closed");
            return;
        } else {
            if (send_err_retry(r)) break; /* EAGAIN - no more data right now */
            log_debug("net_recv: error ret=%d kerr=%d", r, *sceNetErrnoLoc());
            net_disconnect("recv error");
            return;
        }
    }
}

static void net_process_inbound(void) {
    if (g_net.rx_len == g_net.rx_dec) return;

    /* Decrypt the newly arrived tail (CFB8 stream state persists) */
    if (g_net.encrypted && g_net.evp_dec) {
        size_t pending = g_net.rx_len - g_net.rx_dec;
        int outl = 0;
        EVP_DecryptUpdate((EVP_CIPHER_CTX *)g_net.evp_dec,
                          g_net.crypt, &outl,
                          g_net.rx + g_net.rx_dec, (int)pending);
        memcpy(g_net.rx + g_net.rx_dec, g_net.crypt, (size_t)outl);
    }
    g_net.rx_dec = g_net.rx_len;

    /* First pass: quickly scan for keepalive packets to prevent server
       timeout during the initial chunk flood.  Keepalive packets are
       small (< 256 bytes) and always sent uncompressed (data_length=0),
       so we can identify them by reading just 2 bytes per frame without
       doing expensive decompression of chunk data. */
    if (g_net.phase == PHASE_PLAY && g_net.comp_threshold >= 0) {
        size_t kp = 0;
        while (kp < g_net.rx_len) {
            RCUR kr = { g_net.rx, g_net.rx_len, kp };
            int32_t kflen = rc_varint(&kr);
            if (kflen <= 0 || (size_t)kflen > NET_INFLATE_CAP) break;
            if (kr.p + (size_t)kflen > g_net.rx_len) break; /* partial frame */
            /* Uncompressed packet: data_length=0, then packet ID varint */
            if (kr.p + 2 <= g_net.rx_len && g_net.rx[kr.p] == 0x00) {
                uint8_t pid = g_net.rx[kr.p + 1];
                if (pid == 0x1F || pid == 0x2F) {
                    uint8_t *kk = g_net.rx + kr.p + 2; /* skip data_length + packet_id */
                    RCUR kcur = { kk, (size_t)(kflen - 2), 0 };
                    if (pid == 0x1F) {
                        handle_keep_alive(&kcur);
                        packets_recv++;
                        keepalive_send_time = 0;
                        log_debug("play: keepalive processed in first-pass scan");
                    } else if (pid == 0x2F) {
                        handle_position_look(&kcur);
                        log_debug("play: position look processed in first-pass scan");
                    }
                }
            }
            kp = kr.p + (size_t)kflen;
        }
    }

    size_t p = 0;
    int packets_this_call = 0;
    while (p < g_net.rx_len) {
        RCUR r = { g_net.rx, g_net.rx_len, p };
        int32_t flen = rc_varint(&r);
        if (flen <= 0 || (size_t)flen > NET_INFLATE_CAP) { net_disconnect("bad frame len"); return; }
        if (r.p + (size_t)flen > g_net.rx_len) break; /* partial frame */

        uint8_t *payload = g_net.rx + r.p;
        int32_t plen = flen;

        if (g_net.comp_threshold >= 0) {
            RCUR cr = { payload, (size_t)plen, 0 };
            int32_t dlen = rc_varint(&cr);
            if (dlen < 0 || (size_t)dlen > NET_INFLATE_CAP) { net_disconnect("bad dlen"); return; }
            if (dlen == 0) {
                payload += cr.p;
                plen -= (int32_t)cr.p;
            }             else {
                uLongf destLen = NET_INFLATE_CAP;
                int zr = uncompress(g_net.inflate_buf, &destLen,
                               payload + cr.p, (uLong)(plen - (int32_t)cr.p));
                if (zr != Z_OK) {
                    log_debug("net: inflate failed zr=%d dlen=%d srclen=%d "
                              "head=%02X%02X%02X%02X",
                              zr, dlen, plen - (int32_t)cr.p,
                              payload[cr.p], payload[cr.p+1],
                              payload[cr.p+2], payload[cr.p+3]);
                    net_disconnect("inflate failed");
                    return;
                }
                payload = g_net.inflate_buf;
                plen = (int32_t)destLen;
            }
        }

        if (g_net.phase == PHASE_LOGIN) net_dispatch_login(payload, plen);
        else if (g_net.phase == PHASE_PLAY) net_dispatch_play(payload, plen);

        packets_recv++;
        packets_this_call++;
        if (keepalive_send_time) {
            SceUInt64 now = sceKernelGetProcessTimeWide();
            ping_ms = (int)((now - keepalive_send_time) / 1000);
            keepalive_send_time = 0;
        }

        p = r.p + (size_t)flen;
        if (g_net.sock < 0) break;
        /* Process a few packets per frame so input/camera stays
           responsive during the initial chunk flood.  Keep this low
           so that net_process_inbound returns quickly and the main
           loop can send heartbeats and render frames. */
        if (packets_this_call >= 4) break;
    }

    /* Send a periodic Player (0x0C) heartbeat during the chunk flood.
       Without this, net_process_inbound blocks for seconds while
       decoding chunks, preventing net_tick from sending heartbeats
       and causing the server to read-timeout-kick us. */
    if (g_net.phase == PHASE_PLAY && g_player) {
        static SceUInt64 last_hb_inbound = 0;
        SceUInt64 now_hb = sceKernelGetProcessTimeWide();
        if (now_hb - last_hb_inbound >= 1000000) {  /* 1 second */
            last_hb_inbound = now_hb;
            uint8_t hb_body[1] = { g_player->on_ground ? 1 : 0 };
            pkt_send(0x0C, hb_body, 1);
        }
    }

    if (p > 0) {
        memmove(g_net.rx, g_net.rx + p, g_net.rx_len - p);
        g_net.rx_len -= p;
        g_net.rx_dec = g_net.rx_len;
    }
}

/* ------------------------------------------------------------------ */
/* Connection establishment + login                                    */
/* ------------------------------------------------------------------ */
static int pkt_login_start(const char *name) {
    if (g_net.sock < 0 || name == NULL) return -1;
    uint8_t pk[64]; size_t p = 0;
    pk[p++] = 0x00;
    size_t len = strlen(name);
    p += put_varint(pk + p, (int32_t)len);
    memcpy(pk + p, name, len); p += len;
    uint8_t fr[128]; size_t fn = put_varint(fr, (int32_t)p);
    memcpy(fr + fn, pk, p); fn += p;
    return net_send_all(fr, fn) == 0 ? (int)fn : -1;
}

static void net_tick_connecting(void) {
    int so_error = 0; unsigned int len = sizeof(so_error);
    sceNetGetsockopt(g_net.sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
    if (so_error != 0 && so_error != 36 && so_error != 119) {
        log_debug("net_tick: connect failed errno=%d", so_error);
        net_disconnect("connect failed");
        return;
    }
    if (so_error == 36 || so_error == 119) {
        /* Non-blocking connect still in progress, wait for next tick */
        if (++connect_ticks > 600) {
            log_debug("net_tick: connect timeout");
            net_disconnect("connect timeout");
            return;
        }
        return;
    }
    /* so_error == 0: connection established */
    if (g_net.phase == PHASE_CONNECTING) {
        if (++connect_ticks > 600) {
            log_debug("net_tick: connect timeout");
            net_disconnect("connect timeout");
            return;
        }
        /* C00Handshake: protocol 340, server address (the Aternos proxy
           drops handshakes with an empty address), port, next state LOGIN */
        size_t hlen = strlen(g_net.host);
        uint8_t hs[256]; size_t n = 0;
        hs[n++] = 0x00;
        n += put_varint(hs + n, 340);
        n += put_varint(hs + n, (int32_t)hlen);
        memcpy(hs + n, g_net.host, hlen); n += hlen;
        hs[n++] = (uint8_t)(g_net.port >> 8);
        hs[n++] = (uint8_t)(g_net.port & 0xFF);
        n += put_varint(hs + n, 2);
        uint8_t frame[288]; size_t fn = put_varint(frame, (int32_t)n);
        memcpy(frame + fn, hs, n); fn += n;
        if (net_send_all(frame, fn) != 0) return;
        log_debug("net_tick: TCP established, handshake sent (%d bytes)", (int)fn);
        g_net.phase = PHASE_HANDSHAKE;
    }

    if (pkt_login_start(g_net.username) < 0) return;
    log_debug("net_tick: login start sent");
    g_net.phase = PHASE_LOGIN;
}

static void net_tick_play(Player *p) {
    frame_counter++;
    if (!p) return;

    /* FALLBACK: if Position & Look (0x2F) hasn't arrived within a few
       seconds, initialize from spawn/origin and start sending position
       updates so the server does not time us out (it kicks at ~15s). */
    if (!position_look_received) {
        SceUInt64 now = sceKernelGetProcessTimeWide();
        if (now - last_position_look_time > 3000000) { /* 3 seconds */
            int spawn_x, spawn_y, spawn_z;
            if (g_net.spawn_position_received) {
                spawn_x = (int)g_net.spawn_x;
                spawn_y = (int)g_net.spawn_y;
                spawn_z = (int)g_net.spawn_z;
            } else if (g_net.have_spawn) {
                /* Use current origin (set from first chunk) as fallback position */
                spawn_x = g_net.origin_cx * 16 + 8;
                spawn_y = 64;
                spawn_z = g_net.origin_cz * 16 + 8;
            } else {
                spawn_x = 8; spawn_y = 64; spawn_z = 8;
            }

            log_debug("play: Position & Look timeout (3s), using position (%d,%d,%d), spawn_received=%d, have_spawn=%d",
                      spawn_x, spawn_y, spawn_z, g_net.spawn_position_received, g_net.have_spawn);
            p->x = (float)spawn_x;
            p->y = (float)spawn_y;
            p->z = (float)spawn_z;
            p->render_x = p->x;
            p->render_y = p->y;
            p->render_z = p->z;
            p->on_ground = 1;
            p->vx = p->vy = p->vz = 0;

            /* Only update origin from authoritative spawn position - do NOT
               override the chunk-derived origin (which keeps decoded chunks valid) */
            if (g_net.spawn_position_received) {
                g_net.origin_cx = spawn_x >> 4;
                g_net.origin_cz = spawn_z >> 4;
                g_net.have_spawn = 1;
            }
            position_look_received = 1;
            last_confirmed_tid = -1;
            last_position_look_time = sceKernelGetProcessTimeWide();
            log_debug("play: FALLBACK triggered, origin=(%d,%d), sending position updates", g_net.origin_cx, g_net.origin_cz);
        }
    }

    /* Proactive KeepAlive removed — the server only accepts responses to
       its own KeepAlive packets, not client-initiated ones.  Sending our
       own ID causes "sent an invalid keepalive" kicks.  The server sends
       KeepAlive (0x1F) every ~15s; handle_keep_alive() echoes it back. */
    /* (proactive keepalive removed) */

    /* CPacketPlayer.PositionRotation (0x0E) - send on meaningful change,
       plus a 1-second heartbeat so the server doesn't idle-kick us.
       Vanilla sends Player Position once every 20 ticks (1s) even when
       stationary, and on every movement tick. */
    net_send_position_look(p);
}

void net_tick(Player *p) {
    if (g_net.sock < 0) return;
    g_player = p;

    if (g_net.phase == PHASE_CONNECTING || g_net.phase == PHASE_HANDSHAKE) {
        net_tick_connecting();
    }

    /* Send periodic Player (0x0C) heartbeat BEFORE processing inbound
       data.  During the initial chunk flood, net_process_inbound can
       block for several seconds, delaying net_tick_play (and thus the
       heartbeat).  The server's read timeout kicks in before we get
       a chance to send any outgoing packet. */
    if (g_net.phase == PHASE_PLAY && p) {
        static SceUInt64 last_heartbeat = 0;
        SceUInt64 now = sceKernelGetProcessTimeWide();
        if (now - last_heartbeat >= 1000000) {
            last_heartbeat = now;
            uint8_t hb_body[1] = { p->on_ground ? 1 : 0 };
            pkt_send(0x0C, hb_body, 1);
        }
    }

    net_drain_socket();
    net_process_inbound();

    if (g_net.phase == PHASE_PLAY) {
        static SceUInt64 last_net_log = 0;
        SceUInt64 now_log = sceKernelGetProcessTimeWide();
        if (now_log - last_net_log >= 2000000) {
            log_debug("net_tick: phase=PLAY pos_recv=%d eid=%d comp_thresh=%d",
                      position_look_received, g_net.entity_id, g_net.comp_threshold);
            last_net_log = now_log;
        }
        net_tick_play(p);
    }
}

static int net_socket_writable(void) {
    if (g_net.sock < 0) return 0;
    int eid = sceNetEpollCreate("javac_sock_check", 0);
    if (eid < 0) return 1;
    SceNetEpollEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = SCE_NET_EPOLLOUT | SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP;
    int ret = sceNetEpollControl(eid, SCE_NET_EPOLL_CTL_ADD, g_net.sock, &ev);
    if (ret < 0) { sceNetEpollDestroy(eid); return 1; }
    ret = sceNetEpollWait(eid, &ev, 1, 50); /* 50ms timeout */
    sceNetEpollControl(eid, SCE_NET_EPOLL_CTL_DEL, g_net.sock, NULL);
    sceNetEpollDestroy(eid);
    if (ret < 0) return 1;
    if (ret == 0) return 0; /* timeout — socket not writable */
    if (ev.events & (SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP)) return -1;
    return 1;
}

void net_send_player_heartbeat(Player *p) {
    /* Send Player (0x0C) — on_ground only. No position data.
       This resets the server's idle timeout without sending invalid
       positions before we've received the server's PositionAndLook (0x2F). */
    static SceUInt64 last_hb = 0;
    SceUInt64 now = sceKernelGetProcessTimeWide();
    if (now - last_hb < 1000000) return; /* 1 second */
    last_hb = now;

    uint8_t body[1];
    body[0] = p->on_ground ? 1 : 0;
    static int hb_log = 0;
    if ((hb_log++ % 10) == 0) log_debug("play: heartbeat Player (0x0C) on_ground=%d", p->on_ground ? 1 : 0);
    int ret = pkt_send(0x0C, body, 1);
    if (ret != 0) {
        log_debug("play: Player heartbeat send FAILED");
    }
}

void net_send_position_look(Player *p) {
    /* Vanilla behavior:
       - Before receiving server PositionAndLook (0x2F): send Player (0x0C)
         heartbeat (just on_ground) to keep idle timeout alive.
       - After receiving 0x2F: send PositionAndLook (0x0E) on significant
         movement (delta > 0.01 blocks, > 1° rotation) or 500ms heartbeat.
    */
    static double last_x = 0.0, last_y = 0.0, last_z = 0.0;
    static float last_yaw = 0.0f, last_pitch = 0.0f;
    static int last_on_ground = -1;
    static SceUInt64 last_send = 0;

    SceUInt64 now = sceKernelGetProcessTimeWide();

    if (!position_look_received) {
        /* Haven't received server's PositionAndLook yet — send Player (0x0C) */
        net_send_player_heartbeat(p);
        return;
    }

    /* After receiving server's PositionAndLook — send PositionAndLook (0x0E)
       only on significant change or periodic heartbeat */
    double dx = fabs(p->x - last_x);
    double dy = fabs(p->y - last_y);
    double dz = fabs(p->z - last_z);
    float dyaw = fabsf(p->yaw - last_yaw);
    float dpitch = fabsf(p->pitch - last_pitch);
    int dground = (p->on_ground ? 1 : 0) != last_on_ground;

    int moved = (dx > 0.01 || dy > 0.01 || dz > 0.01 ||
                 dyaw > 1.0f || dpitch > 1.0f || dground);

    int send_interval = (now - last_send >= 500000); /* 500ms min interval */

    if (!moved && !send_interval) return;

    /* Check socket writability before sending */
    int writable = net_socket_writable();
    if (writable <= 0) {
        if (writable < 0) {
            log_debug("play: socket not writable (error), closing");
            if (g_net.sock >= 0) { sceNetSocketClose(g_net.sock); g_net.sock = -1; }
        }
        return;
    }

    uint8_t buf[41];
    size_t n = 0;
    uint64_t ux, uy, uz;
    uint32_t uyaw, upitch;
    double px = p->x, py = p->y, pz = p->z;
    memcpy(&ux, &px, 8); memcpy(&uy, &py, 8); memcpy(&uz, &pz, 8);
    for (int i = 0; i < 8; i++) buf[n++] = (uint8_t)((ux >> (8 * (7 - i))) & 0xFF);
    for (int i = 0; i < 8; i++) buf[n++] = (uint8_t)((uy >> (8 * (7 - i))) & 0xFF);
    for (int i = 0; i < 8; i++) buf[n++] = (uint8_t)((uz >> (8 * (7 - i))) & 0xFF);
    memcpy(&uyaw, &p->yaw, 4); memcpy(&upitch, &p->pitch, 4);
    for (int i = 0; i < 4; i++) buf[n++] = (uint8_t)((uyaw >> (8 * (3 - i))) & 0xFF);
    for (int i = 0; i < 4; i++) buf[n++] = (uint8_t)((upitch >> (8 * (3 - i))) & 0xFF);
    buf[n++] = p->on_ground ? 1 : 0;

    last_x = p->x; last_y = p->y; last_z = p->z;
    last_yaw = p->yaw; last_pitch = p->pitch;
    last_on_ground = p->on_ground ? 1 : 0;
    last_send = now;

    static int pos_log = 0;
    if ((pos_log++ % 10) == 0) log_debug("play: sending PositionAndLook (0x0E) body_len=%u yaw=%.1f pitch=%.1f x=%.1f y=%.1f z=%.1f on_ground=%d moved=%d",
               (unsigned)n, p->yaw, p->pitch, p->x, p->y, p->z, p->on_ground ? 1 : 0, moved);
    int send_result = pkt_send(0x0E, buf, n);
    if (send_result != 0) {
        log_debug("play: PositionAndLook send FAILED");
    }
}

void net_send_respawn(void) {
    /* 1.12.2 ClientStatus (0x03), NOT 0x0B.
       0x0B is KeepAlive — the server reads our action byte as the
       keepalive ID, then tries to read a varint payload and crashes. */
    uint8_t body[1];
    size_t n = put_varint(body, 0); // action=0 (PERFORM_RESPAWN)
    pkt_send(0x03, body, n);
}

void net_send_chat(const char *msg) {
    /* 1.12.2 ChatMessage (0x0F):
       message (string, max 256 chars), server-bound.
       Format: string(varint length + data). No type byte in 1.12.2. */
    uint8_t body[512];
    size_t n = 0;
    size_t mlen = strlen(msg);
    if (mlen > 255) mlen = 255;
    n += put_varint(body + n, (int32_t)mlen);
    memcpy(body + n, msg, mlen);
    n += mlen;
    log_debug("chat: sending msg='%s' body_len=%u", msg, (unsigned)n);
    pkt_send(0x02, body, n);
}

void net_send_player_action(int action) {
    /* 1.12.2 EntityAction (0x15), NOT 0x14.
       0x14 is PlayerDigging — the server reads our entity ID as the
       digging status varint (e.g. 671), then tries to read a Position
       from the action bytes and crashes with ArrayIndexOutOfBounds.
       0x15 requires: entity ID (varint) + action (varint) + auxData (varint). */
    uint8_t body[16];
    size_t n = 0;
    n += put_varint(body + n, g_net.entity_id);  /* entity ID */
    n += put_varint(body + n, action);            /* action */
    n += put_varint(body + n, 0);                 /* auxData = jump boost */
    pkt_send(0x15, body, n);
}

void net_send_animation(int hand) {
    /* 1.12.2 Animation (0x1D): hand (VarInt Enum) - 0=main hand, 1=off hand */
    uint8_t body[4];
    body[0] = hand & 0x7F;
    pkt_send(0x1D, body, 1);
}

void net_send_block_dig(int status, int x, int y, int z, int face) {
    /* 1.12.2 PlayerDigging (0x14):
       action (varint): 0=START_DESTROY_BLOCK, 1=ABORT_DESTROY_BLOCK, 2=STOP_DESTROY_BLOCK,
                       3=DROP_ALL_ITEMS, 4=DROP_ITEM, 5=RELEASE_USE_ITEM, 6=SWAP_HELD_ITEMS
       position (BlockPos): packed long (8 bytes BE)
       facing (byte): EnumFacing index 0=DOWN,1=UP,2=NORTH,3=SOUTH,4=WEST,5=EAST
       Convert FaceDir (client internal) to EnumFacing (server) */
    static const int face_map[6] = {3, 2, 4, 5, 1, 0};
    int server_face = (face >= 0 && face <= 5) ? face_map[face] : 1;
    uint8_t body[16];
    size_t n = 0;
    n += put_varint(body + n, status);
    /* BlockPos packed long: 8 bytes big-endian */
    uint64_t packed = (((uint64_t)(uint32_t)x & 0x3FFFFFFULL) << 38) |
                      (((uint64_t)(uint32_t)y & 0xFFFULL) << 26) |
                      (((uint64_t)(uint32_t)z & 0x3FFFFFFULL));
    for (int i = 7; i >= 0; i--)
        body[n++] = (uint8_t)((packed >> (8 * i)) & 0xFF);
    body[n++] = (uint8_t)server_face;
    log_debug("play: block dig status=%d at (%d,%d,%d) face=%d->%d", status, x, y, z, face, server_face);
    pkt_send(0x14, body, n);
}

void net_send_block_place(int x, int y, int z, int face) {
    /* 1.12.2 PlayerTryUseItemOnBlock (0x1F):
       position (BlockPos): packed long (8 bytes BE)
       direction (EnumFacing): varint
       hand (EnumHand): varint = 0 (main hand)
       facingX/Y/Z (float BE): cursor hit coords
       Convert FaceDir (client internal) to EnumFacing (server) */
    static const int face_map[6] = {3, 2, 4, 5, 1, 0};
    int server_face = (face >= 0 && face <= 5) ? face_map[face] : 1;
    uint8_t body[32];
    size_t n = 0;
    uint64_t packed = (((uint64_t)(uint32_t)x & 0x3FFFFFFULL) << 38) |
                      (((uint64_t)(uint32_t)y & 0xFFFULL) << 26) |
                      (((uint64_t)(uint32_t)z & 0x3FFFFFFULL));
    for (int i = 7; i >= 0; i--)
        body[n++] = (uint8_t)((packed >> (8 * i)) & 0xFF);
    n += put_varint(body + n, server_face);
    n += put_varint(body + n, 0);                /* hand: main hand */
    float cx = 0.5f, cy = 0.5f, cz = 0.5f;
    uint32_t ucx, ucy, ucz;
    memcpy(&ucx, &cx, 4); memcpy(&ucy, &cy, 4); memcpy(&ucz, &cz, 4);
    for (int i = 0; i < 4; i++) body[n++] = (uint8_t)((ucx >> (8 * (3 - i))) & 0xFF);
    for (int i = 0; i < 4; i++) body[n++] = (uint8_t)((ucy >> (8 * (3 - i))) & 0xFF);
    for (int i = 0; i < 4; i++) body[n++] = (uint8_t)((ucz >> (8 * (3 - i))) & 0xFF);
    log_debug("play: block place at (%d,%d,%d) face=%d->%d", x, y, z, face, server_face);
    pkt_send(0x1F, body, n);
}
