#ifndef NETWORK_H
#define NETWORK_H

#include <stdint.h>
#include <stddef.h>

#include "player.h"

typedef enum {
    PHASE_CONNECTING,
    PHASE_HANDSHAKE,
    PHASE_LOGIN,
    PHASE_PLAY
} NetPhase;

#define NET_RX_CAP       (256 * 1024)
#define NET_CRYPT_CAP    (256 * 1024)
#define NET_INFLATE_CAP  (1024 * 1024)
#define NET_TX_CAP       (16 * 1024)

typedef struct {
    int sock;
    NetPhase phase;
    char error[128];
    char host[128];
    int port;
    int entity_id;
    char username[17];
    int dimension;
    int gamemode;
    double spawn_x, spawn_y, spawn_z;
    float spawn_yaw, spawn_pitch;
    double pos_x, pos_y, pos_z;
    float pos_yaw, pos_pitch;
    int have_spawn;
    int origin_cx, origin_cz;
    uint8_t rx[NET_RX_CAP];
    size_t rx_len;
    size_t rx_dec;
    uint8_t crypt[NET_CRYPT_CAP];
    uint8_t inflate_buf[NET_INFLATE_CAP];
    uint8_t tx[NET_TX_CAP];
    int encrypted;
    int comp_threshold;
    void *evp_enc;
    void *evp_dec;
    int teleport_id;
        uint64_t last_pos_send;
            int spawn_position_received;
} NetState;

extern NetState g_net;
extern char log_lines[6][128];
extern int log_count;

typedef struct { const uint8_t *b; size_t len; size_t p; } RCUR;

uint8_t  rc_u8(RCUR *r);
uint16_t rc_u16(RCUR *r);
uint32_t rc_u32(RCUR *r);
uint64_t rc_u64(RCUR *r);
float    rc_f32(RCUR *r);
double   rc_f64(RCUR *r);
int32_t  rc_varint(RCUR *r);

void log_init_once(void);
    void log_debug(const char *fmt, ...);
    void log_get_lines(char lines[6][128], int *count);
    int  net_init(void);
    int  net_connect(const char *host, int port, const char *username);
    void net_tick(Player *p);
    void net_disconnect(const char *reason);
void net_send_chat(const char *msg);
void net_send_respawn(void);
void net_send_position_look(Player *p);
void net_send_player_action(int action);
    void net_send_animation(int hand);
    void net_send_block_dig(int status, int x, int y, int z, int face);
    void net_send_block_place(int x, int y, int z, int face);

#endif
