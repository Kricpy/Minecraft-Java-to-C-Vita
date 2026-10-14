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
    FILE *f = fopen("ux0:data/minecraft_javac.log", "a");
    if(f) { fprintf(f, "[%02d:%02d:%02d] %s\n", t.hour, t.minute, t.second, buf); fclose(f); }
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
    if (kerr == 11 || kerr == 35 || kerr == 36 || kerr == 57 || kerr == 119 ||
        uerr == 0x80410123u || uerr == 0x80410124u)
        return 1;
    return 0;
}

static int net_send_all(const uint8_t *buf, size_t len) {
    size_t off = 0;
    uint32_t tries = 0;
    while (off < len) {
        int s = sceNetSend(g_net.sock, buf + off, len - off, 0);
        if (s < 0) {
            if (send_err_retry(s)) {
                if (++tries > 200) {
                    log_debug("net_send_all: max retries exceeded");
                    return -1;
                }
                continue;
            }
            log_debug("net_send_all: fatal ret=%d kerr=%d", s, *sceNetErrnoLoc());
            return -1;
        }
        off += (size_t)s;
        tries = 0;
    }
    return 0;
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
    if (g_net.comp_threshold >= 0) {
        wire[w++] = 0; /* uncompressed */
        memcpy(wire + w, pkt, n); w += n;
    } else {
        memcpy(wire, pkt, n); w = n;
    }

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

    log_debug("pkt_send: sending packet id=0x%02X len=%u", id, (unsigned)f);
    /* Special handling for Confirm Teleport (0x00) and Position (0x0E) - retry on failure */
    int max_retries = (id == 0x00 || id == 0x0E) ? 3 : 1;
    int result = -1;
    for (int attempt = 0; attempt < max_retries; attempt++) {
        if (attempt > 0) {
            sceKernelDelayThread(2000); /* 2ms delay between retries */
        }
        if (net_send_all(frame, f) == 0) {
            result = 0;
            break;
        }
    }
    if (result == 0) {
        packets_sent++;
    } else {
        log_debug("pkt_send: packet id=0x%02X failed to send after %d attempts", id, max_retries);
    }
    return result;
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
    int nonblock = 1;
    sceNetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nonblock, sizeof(nonblock));
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
    uint8_t body[8];
    for (int i = 0; i < 8; i++) body[i] = (uint8_t)(id >> (8 * (7 - i)));
    keepalive_send_time = sceKernelGetProcessTimeWide();
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

    /* SEND CONFIRM TELEPORT IMMEDIATELY - before any other processing */
    uint8_t body[8];
    size_t n = put_varint(body, tid);
    log_debug("play: sending Confirm Teleport immediately (tid=%d)", tid);
    int send_result = pkt_send(0x00, body, n);
    if (send_result != 0) {
        log_debug("play: WARNING - Confirm Teleport send failed, retrying once...");
        sceKernelDelayThread(1000); /* 1ms delay */
        send_result = pkt_send(0x00, body, n);
        if (send_result != 0) {
            log_debug("play: CRITICAL - Confirm Teleport send failed permanently!");
        }
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
            
            if (dx != 0 || dz != 0) {
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
    int32_t x = (int32_t)((packed >> 38) & 0x3FFFFFF);
    int32_t y = (int32_t)(packed & 0xFFF);
    int32_t z = (int32_t)((packed >> 12) & 0x3FFFFFF);
    if (x >= (1 << 25)) x -= (1 << 26);
    if (z >= (1 << 25)) z -= (1 << 26);
    
    if (x < -30000000 || x > 30000000 || y < 0 || y > 256 || z < -30000000 || z > 30000000) {
        log_debug("play: invalid spawn position %d %d %d", x, y, z);
        return;
    }
    
    g_net.spawn_x = x; g_net.spawn_y = y; g_net.spawn_z = z;
    /* Always update origin from spawn position - it's authoritative */
    g_net.origin_cx = x >> 4;
    g_net.origin_cz = z >> 4;
    g_net.have_spawn = 1;
    g_net.spawn_position_received = 1;
    log_debug("play: Spawn Position packet (0x45) received: %d %d %d -> origin (%d,%d)", x, y, z, g_net.origin_cx, g_net.origin_cz);
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
        log_debug("chunk data (%d,%d) full=%d mask=%04X vtx=%d",
                  cx, cz, full, mask, world_chunks[ix][iz].vertex_count);
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
    extern int show_respawn;
    show_respawn = 1;
    log_debug("respawn: show_respawn=1");
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
    log_debug("net: parsed play id=0x%02X, remaining=%u", id, (unsigned)(r.len - r.p));
    switch (id) {
        case 0x35: handle_respawn(&r); break;
        case 0x0F: handle_chat(&r); break;
        case 0x1A: handle_disconnect("play", &r); break;
        case 0x1D: handle_unload_chunk(&r); break;
        case 0x1F: handle_keep_alive(&r); break;
        case 0x20: handle_chunk_data(&r); break;
        case 0x23: handle_join_game(&r); break;
        case 0x2F: handle_position_look(&r); break;
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
            rc_f32(&r); // health
            rc_varint(&r); // food
            rc_f32(&r); // food saturation
            log_debug("play: update health");
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
    /* Drain all currently-available data (socket is non-blocking).
       A single recv per tick is too slow for the initial chunk flood. */
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
           responsive during the initial chunk flood. */
        if (packets_this_call >= 3) break;
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
    if (so_error != 0) {
        log_debug("net_tick: connect failed errno=%d", so_error);
        net_disconnect("connect failed");
        return;
    }

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

            log_debug("play: Position & Look timeout (30s), using position (%d,%d,%d), spawn_received=%d, have_spawn=%d",
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
            last_position_look_time = sceKernelGetProcessTimeWide();
            log_debug("play: FALLBACK triggered, origin=(%d,%d), sending position updates", g_net.origin_cx, g_net.origin_cz);
        }
    }

    /* Only send position updates AFTER we've received Position & Look and sent Confirm Teleport */
    if (position_look_received) {
        /* CPacketPlayer.PositionRotation EVERY TICK keeps the server happy */
        uint8_t buf[37];
        size_t n = 0;
        uint64_t ux, uy, uz;
        uint32_t uyaw, upitch;
        double dx = p->x, dy = p->y, dz = p->z;
        float fyaw = p->yaw, fpitch = p->pitch;
        memcpy(&ux, &dx, 8); memcpy(&uy, &dy, 8); memcpy(&uz, &dz, 8);
        memcpy(&uyaw, &fyaw, 4); memcpy(&upitch, &fpitch, 4);
        for (int i = 0; i < 8; i++) buf[n++] = (uint8_t)((ux >> (8 * (7 - i))) & 0xFF);
        for (int i = 0; i < 8; i++) buf[n++] = (uint8_t)((uy >> (8 * (7 - i))) & 0xFF);
        for (int i = 0; i < 8; i++) buf[n++] = (uint8_t)((uz >> (8 * (7 - i))) & 0xFF);
        for (int i = 0; i < 4; i++) buf[n++] = (uint8_t)((uyaw >> (8 * (3 - i))) & 0xFF);
        for (int i = 0; i < 4; i++) buf[n++] = (uint8_t)((upitch >> (8 * (3 - i))) & 0xFF);
        buf[n++] = p->on_ground ? 1 : 0;
        pkt_send(0x0E, buf, n);
    }
}

void net_tick(Player *p) {
    if (g_net.sock < 0) return;
    g_player = p;

    if (g_net.phase == PHASE_CONNECTING || g_net.phase == PHASE_HANDSHAKE) {
        net_tick_connecting();
    }

    net_drain_socket();
    net_process_inbound();

    if (g_net.phase == PHASE_PLAY) {
        net_tick_play(p);
    }
}

void net_send_respawn(void) {
    uint8_t body[1];
    size_t n = put_varint(body, 0); // action=0 (respawn)
    pkt_send(0x03, body, n);
}

void net_send_player_action(int action) {
    uint8_t body[10];
    size_t n = 0;
    n += put_varint(body + n, action);
    pkt_send(0x13, body, n);
}
