#include <psp2/kernel/processmgr.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/ime_dialog.h>
#include <psp2/sysmodule.h>
#include <vitaGL.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "defines.h"
#include "chunk.h"
#include "atlas.h"
#include "player.h"
#include "raycast.h"
#include "renderer.h"
#include "network.h"

/* --- FORWARD DECLARATIONS FOR NETWORK FUNCTIONS --- */
void net_send_respawn(void);
void net_send_position_look(Player *p);
void net_send_player_action(int state);
void net_send_block_dig(int status, int x, int y, int z, int face);
void net_send_block_place(int x, int y, int z, int face);
void net_send_use_entity(int entity_id, int action);

#define LOG_MAX_LINES 6
#define LOG_LINE_LEN  128

static int show_menu = 0;
static char server_host[128] = "kreqxs.aternos.me";
static int server_port = 11113;
static int menu_selection = 0; /* 0 = address, 1 = port, 2 = connect */
static int menu_cooldown = 0;
static int prev_lr_held = 0;
static char status_text[128] = "Starting...";
static int render_frame = 0;
/* --- DEFINE HUD TRACKING METRICS --- */
int ping_ms = 0;
int packets_sent = 0;
int packets_recv = 0;

/* Death/Respawn state */
int show_respawn = 0;
static int respawn_selection = 0; /* 0 = respawn, 1 = title screen */
static int respawn_cooldown = 0;

/* Forward declarations */
void draw_string(int x, int y, const char *text, uint32_t color, float scale);
void draw_log_overlay(void);
void draw_hud(const Player *p);

/* FPS/Connection stats */
static int fps_counter = 0;
static int fps = 0;
static SceUInt64 fps_last_time = 0;
static int connection_quality = 0; /* 0=good, 1=warn, 2=bad */
static int chunks_loaded = 0;

/* IME (system keyboard) state */
static int ime_active = 0;
static int ime_target = 0;
static SceWChar16 ime_title[SCE_IME_DIALOG_MAX_TITLE_LENGTH];
static SceWChar16 ime_initial[SCE_IME_DIALOG_MAX_TEXT_LENGTH];
static SceWChar16 ime_buffer[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];

static void utf8_to_ime(const char *src, SceWChar16 *dst, uint32_t max) {
    uint32_t di = 0;
    for (uint32_t i = 0; src[i] && di < max - 1; ) {
        uint32_t cp = (unsigned char)src[i];
        if (cp >= 0xF0) {
            cp = ((src[i] & 0x07) << 18) | ((src[i+1] & 0x3F) << 12) |
                 ((src[i+2] & 0x3F) << 6) | (src[i+3] & 0x3F);
            i += 4;
        } else if (cp >= 0xE0) {
            cp = ((src[i] & 0x0F) << 12) | ((src[i+1] & 0x3F) << 6) | (src[i+2] & 0x3F);
            i += 3;
        } else if (cp >= 0xC0) {
            cp = ((src[i] & 0x1F) << 6) | (src[i+1] & 0x3F);
            i += 2;
        } else {
            i++;
        }
        dst[di++] = (SceWChar16)cp;
    }
    dst[di] = 0;
}

static void ime_to_utf8(const SceWChar16 *src, char *dst, uint32_t max) {
    uint32_t di = 0;
    for (uint32_t i = 0; src[i] && di < max - 1; i++) {
        uint32_t cp = src[i];
        if (cp < 0x80) {
            dst[di++] = (char)cp;
        } else if (cp < 0x800) {
            if (di + 2 >= max) break;
            dst[di++] = (char)(0xC0 | (cp >> 6));
            dst[di++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (di + 3 >= max) break;
            dst[di++] = (char)(0xE0 | (cp >> 12));
            dst[di++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[di++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    dst[di] = 0;
}

static void ime_open(const char *title, const char *initial, uint32_t type, uint32_t maxlen) {
    utf8_to_ime(title, ime_title, SCE_IME_DIALOG_MAX_TITLE_LENGTH);
    utf8_to_ime(initial, ime_initial, SCE_IME_DIALOG_MAX_TEXT_LENGTH);
    memset(ime_buffer, 0, sizeof(ime_buffer));
    memcpy(ime_buffer, ime_initial, SCE_IME_DIALOG_MAX_TEXT_LENGTH * 2);

    SceImeDialogParam param;
    sceImeDialogParamInit(&param);
    param.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH | SCE_IME_LANGUAGE_GERMAN |
                               SCE_IME_LANGUAGE_SPANISH | SCE_IME_LANGUAGE_FRENCH |
                               SCE_IME_LANGUAGE_ITALIAN | SCE_IME_LANGUAGE_PORTUGUESE;
    param.languagesForced = SCE_TRUE;
    param.type = type;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
    param.title = ime_title;
    param.maxTextLength = maxlen;
    param.initialText = ime_initial;
    param.inputTextBuffer = ime_buffer;

    sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
    if (sceImeDialogInit(&param) >= 0) {
        ime_active = 1;
    }
}

/* Returns 1 when the dialog closed; sets *confirmed and fills out on ENTER */
static int ime_poll(char *out, size_t out_cap, int *confirmed) {
    if (!ime_active) return 1;
    SceCommonDialogStatus st = sceImeDialogGetStatus();
    if (st != SCE_COMMON_DIALOG_STATUS_FINISHED)
        return 0; /* NONE / RUNNING — dialog still open */
    SceImeDialogResult res;
    memset(&res, 0, sizeof(res));
    sceImeDialogGetResult(&res);
    sceImeDialogTerm();
    ime_active = 0;
    if (res.button == SCE_IME_DIALOG_BUTTON_ENTER) {
        ime_to_utf8(ime_buffer, out, (uint32_t)out_cap);
        *confirmed = 1;
    } else {
        *confirmed = 0;
    }
    return 1;
}

void update_status_text(void) {
    switch (g_net.phase) {
        case PHASE_CONNECTING:
            snprintf(status_text, sizeof(status_text), "Connecting to %s:%d...", g_net.host, g_net.port);
            break;
        case PHASE_HANDSHAKE:
            snprintf(status_text, sizeof(status_text), "Handshake Sent...");
            break;
        case PHASE_LOGIN:
            snprintf(status_text, sizeof(status_text), "Logging in as %s...", g_net.username);
            break;
        case PHASE_PLAY:
            snprintf(status_text, sizeof(status_text), "Playing (entity=%d)", g_net.entity_id);
            break;
        default:
            snprintf(status_text, sizeof(status_text), "Status: Unknown");
            break;
    }
}

/* Simple 8x8 bitmap font (ASCII 32-126) */
static const uint8_t font8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* ! */
    {0x6C,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, /* # */
    {0x18,0x7E,0xC0,0x7C,0x06,0xFC,0x18,0x00}, /* $ */
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, /* % */
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, /* & */
    {0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00}, /* ' */
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, /* ( */
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, /* ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* * */
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, /* , */
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* . */
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, /* / */
    {0x3C,0x66,0xC6,0xC6,0xC6,0x66,0x3C,0x00}, /* 0 */
    {0x18,0x38,0x58,0x18,0x18,0x18,0x7E,0x00}, /* 1 */
    {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00}, /* 2 */
    {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}, /* 3 */
    {0x0C,0x1C,0x3C,0x6C,0xFE,0x0C,0x0C,0x00}, /* 4 */
    {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, /* 5 */
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00}, /* 6 */
    {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00}, /* 7 */
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, /* 8 */
    {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00}, /* 9 */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, /* : */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, /* ; */
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, /* < */
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, /* = */
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, /* > */
    {0x3C,0x66,0x0C,0x18,0x18,0x00,0x18,0x00}, /* ? */
    {0x3C,0x66,0xDE,0xDE,0xDE,0xC0,0x3C,0x00}, /* @ */
    {0x3C,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, /* A */
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, /* B */
    {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}, /* C */
    {0x7C,0x66,0x66,0x66,0x66,0x66,0x7C,0x00}, /* D */
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00}, /* E */
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}, /* F */
    {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0x00}, /* G */
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, /* H */
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* I */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00}, /* J */
    {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00}, /* K */
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, /* L */
    {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00}, /* M */
    {0x66,0xE6,0xF6,0xDE,0xCE,0xC6,0x66,0x00}, /* N */
    {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, /* O */
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, /* P */
    {0x3C,0x66,0x66,0x66,0x6E,0x3C,0x06,0x00}, /* Q */
    {0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x00}, /* R */
    {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00}, /* S */
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, /* T */
    {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, /* U */
    {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, /* V */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* W */
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, /* X */
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00}, /* Y */
    {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, /* Z */
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, /* [ */
    {0x80,0xC0,0x60,0x30,0x18,0x0C,0x06,0x00}, /* \ */
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, /* ] */
    {0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* _ */
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, /* ` */
    {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3B,0x00}, /* a */
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}, /* b */
    {0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00}, /* c */
    {0x06,0x06,0x3E,0x66,0x66,0x66,0x3B,0x00}, /* d */
    {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00}, /* e */
    {0x1C,0x30,0x60,0x7C,0x60,0x60,0xF0,0x00}, /* f */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x3C}, /* g */
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00}, /* h */
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, /* i */
    {0x0C,0x00,0x1C,0x0C,0x0C,0x0C,0x0C,0x78}, /* j */
    {0x60,0x60,0x6C,0x78,0x70,0x6C,0x66,0x00}, /* k */
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* l */
    {0x00,0x00,0xEC,0xFE,0x73,0x63,0x63,0x00}, /* m */
    {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00}, /* n */
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00}, /* o */
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60}, /* p */
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06}, /* q */
    {0x00,0x00,0x7C,0x60,0x60,0x60,0x60,0x00}, /* r */
    {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00}, /* s */
    {0x18,0x30,0x7C,0x30,0x30,0x30,0x1C,0x00}, /* t */
    {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00}, /* u */
    {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00}, /* v */
    {0x00,0x00,0x63,0x63,0x73,0x7F,0x6B,0x00}, /* w */
    {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00}, /* x */
    {0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C}, /* y */
    {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00}, /* z */
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, /* { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* | */
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, /* } */
    {0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00}, /* ~ */
};

void draw_char(int x, int y, char c, uint32_t color, float scale) {
    if (c < 32 || c > 126) return;
    const uint8_t *glyph = font8x8[c - 32];

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, 960, 544, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glDepthMask(GL_FALSE);

    glColor4ub((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF, (color >> 24) & 0xFF);

    float px = (float)x;
    float py = (float)y;
    float cw = scale;
    float ch = scale;

    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                float cx = px + col * cw;
                float cy = py + row * ch;
                glBegin(GL_QUADS);
                glVertex2f(cx, cy);
                glVertex2f(cx + cw, cy);
                glVertex2f(cx + cw, cy + ch);
                glVertex2f(cx, cy + ch);
                glEnd();
            }
        }
    }

    glDepthMask(GL_TRUE);

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopAttrib();
}

void draw_string(int x, int y, const char *text, uint32_t color, float scale) {
    int cx = x;
    for (const char *p = text; *p; p++) {
        draw_char(cx, y, *p, color, scale);
        cx += (int)(8 * scale);
    }
}

void draw_log_overlay(void) {
    char lines[LOG_MAX_LINES][LOG_LINE_LEN];
    int count = 0;
    log_get_lines(lines, &count);
    
    draw_string(10, 10, status_text, 0xFFFFFFFF, 1.5f);
    
    for (int i = 0; i < count; i++) {
        int y = 30 + i * 16;
        draw_string(10, y, lines[i], 0xFFCCCCCC, 1.0f);
    }
}

void draw_hud(const Player *p) {
    char buf[64];
    int right_x = 760; /* Right side of screen */
    
    /* FPS */
    snprintf(buf, sizeof(buf), "FPS: %d", fps);
    draw_string(right_x, 10, buf, 0xFFFFFFFF, 0.8f);
    
    /* Ping */
    snprintf(buf, sizeof(buf), "Ping: %dms", ping_ms);
    draw_string(right_x, 30, buf, ping_ms < 100 ? 0xFF00FF00 : (ping_ms < 200 ? 0xFFFFFF00 : 0xFFFF0000), 0.8f);
    
    /* Connection quality */
    const char *qual_str[] = {"GOOD", "WARN", "BAD"};
    uint32_t qual_color[] = {0xFF00FF00, 0xFFFFFF00, 0xFFFF0000};
    snprintf(buf, sizeof(buf), "Conn: %s", qual_str[connection_quality]);
    draw_string(right_x, 50, buf, qual_color[connection_quality], 0.8f);
    
    /* Packets */
    snprintf(buf, sizeof(buf), "TX: %d RX: %d", packets_sent, packets_recv);
    draw_string(right_x, 70, buf, 0xFFFFFFFF, 0.8f);
    
    /* Position */
    int pxi = (int)(p->x * 10);
    int pyi = (int)(p->y * 10);
    int pzi = (int)(p->z * 10);
    snprintf(buf, sizeof(buf), "Pos: %d %d %d (x10)", pxi, pyi, pzi);
    draw_string(right_x, 490, buf, 0xFFFFFFFF, 0.7f);
    
    /* Chunk */
    int cx = (int)floorf(p->x / 16.0f);
    int cz = (int)floorf(p->z / 16.0f);
    snprintf(buf, sizeof(buf), "Chunk: %d %d", cx, cz);
    draw_string(right_x, 510, buf, 0xFFFFFFFF, 0.7f);
    
    /* Chunks loaded */
    int loaded = 0;
    for (int cx = 0; cx < WORLD_SIZE_X; cx++)
        for (int cz = 0; cz < WORLD_SIZE_Z; cz++)
            if (world_chunks[cx][cz].loaded) loaded++;
    snprintf(buf, sizeof(buf), "Chunks: %d/256", loaded);
    draw_string(right_x, 530, buf, 0xFFFFFFFF, 0.7f);
}

void draw_menu_rect(float x, float y, float w, float h, uint32_t color) {
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, 960, 544, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    
    glColor4ub((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF, (color >> 24) & 0xFF);
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
    
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
}

void draw_config_menu(void) {
    draw_menu_rect(0, 0, 960, 544, 0xC0000000);
    draw_menu_rect(180, 70, 600, 470, 0xFF181818);
    draw_menu_rect(180, 70, 600, 3, 0xFF00FF00);
    draw_menu_rect(180, 537, 600, 3, 0xFF00FF00);
    draw_menu_rect(180, 70, 3, 470, 0xFF00FF00);
    draw_menu_rect(777, 70, 3, 470, 0xFF00FF00);

    draw_string(220, 95, "=== CONFIG MENU (L+R to close) ===", 0xFF00FF00, 1.0f);

    /* Server address field */
    int sel0 = (menu_selection == 0);
    draw_string(220, 150, "Server Address", sel0 ? 0xFF00FF00 : 0xFFAAAAAA, 0.9f);
    draw_menu_rect(217, 168, 546, 34, sel0 ? 0xFF004400 : 0xFF2A2A2A);
    draw_string(228, 176, server_host, 0xFFFFFFFF, 0.9f);
    draw_string(220, 212, sel0 ? "> Press X to edit (keyboard)" : "  Press X to edit (keyboard)",
                sel0 ? 0xFF666666 : 0xFF444444, 0.6f);

    /* Port field */
    int sel1 = (menu_selection == 1);
    draw_string(220, 245, "Port", sel1 ? 0xFF00FF00 : 0xFFAAAAAA, 0.9f);
    draw_menu_rect(217, 263, 200, 34, sel1 ? 0xFF004400 : 0xFF2A2A2A);
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", server_port);
    draw_string(228, 271, port_str, 0xFFFFFFFF, 0.9f);
    draw_string(220, 307, sel1 ? "> D-pad L/R adjusts, X for keyboard" : "  D-pad L/R adjusts, X for keyboard",
                sel1 ? 0xFF666666 : 0xFF444444, 0.6f);

    /* Connect button */
    int sel2 = (menu_selection == 2);
    draw_menu_rect(217, 345, 200, 40, sel2 ? 0xFF006600 : 0xFF2A2A2A);
    draw_string(255, 358, "CONNECT", sel2 ? 0xFFFFFF00 : 0xFFAAAAAA, 1.0f);
    if (sel2) draw_string(217, 395, "> Press X to connect", 0xFF666666, 0.6f);

    /* Respawn button (for testing) */
    int sel3 = (menu_selection == 3);
    draw_menu_rect(217, 410, 200, 40, sel3 ? 0xFFFF0000 : 0xFF2A2A2A);
    draw_string(255, 423, "RESPAWN", sel3 ? 0xFFFFFF00 : 0xFFAAAAAA, 1.0f);
    if (sel3) draw_string(217, 460, "> Press X to respawn", 0xFF666666, 0.6f);

    draw_string(220, 500, "Status:", 0xFFAAAAAA, 0.7f);
    draw_string(290, 500, status_text, 0xFFCCCCCC, 0.7f);

    char build_ver[64];
    snprintf(build_ver, sizeof(build_ver), "Build: %s %s", __DATE__, __TIME__);
    draw_string(600, 527, build_ver, 0xFF666666, 0.6f);
}

void draw_respawn_screen(void) {
    draw_menu_rect(0, 0, 960, 544, 0xC0000000);
    draw_menu_rect(180, 70, 600, 400, 0xFF181818);
    draw_menu_rect(180, 70, 600, 3, 0xFFFF0000);
    draw_menu_rect(180, 467, 600, 3, 0xFFFF0000);
    draw_menu_rect(180, 70, 3, 400, 0xFFFF0000);
    draw_menu_rect(777, 70, 3, 400, 0xFFFF0000);

    draw_string(220, 95, "=== YOU DIED ===", 0xFFFF0000, 1.2f);

    int sel0 = (respawn_selection == 0);
    draw_menu_rect(217, 180, 546, 50, sel0 ? 0xFF004400 : 0xFF2A2A2A);
    draw_string(300, 198, "RESPAWN", sel0 ? 0xFFFFFF00 : 0xFFAAAAAA, 1.2f);
    if (sel0) draw_string(217, 240, "> Press X to respawn", 0xFF666666, 0.7f);

    int sel1 = (respawn_selection == 1);
    draw_menu_rect(217, 270, 546, 50, sel1 ? 0xFF004400 : 0xFF2A2A2A);
    draw_string(270, 288, "TITLE SCREEN", sel1 ? 0xFFFFFF00 : 0xFFAAAAAA, 1.2f);
    if (sel1) draw_string(217, 330, "> Press X for title screen", 0xFF666666, 0.7f);

    draw_string(220, 425, "Status:", 0xFFAAAAAA, 0.7f);
    draw_string(290, 425, status_text, 0xFFCCCCCC, 0.7f);
}

void handle_respawn_input(const SceCtrlData *ctrl) {
    if (respawn_cooldown > 0) {
        respawn_cooldown--;
        return;
    }
    if (ctrl->buttons & SCE_CTRL_UP) {
        respawn_selection = 0;
        respawn_cooldown = 10;
    }
    if (ctrl->buttons & SCE_CTRL_DOWN) {
        respawn_selection = 1;
        respawn_cooldown = 10;
    }
    if (ctrl->buttons & SCE_CTRL_CIRCLE) { /* O button = respawn */
        log_debug("respawn: O button pressed, respawning");
        show_respawn = 0;
        respawn_cooldown = 30;
        net_send_respawn();
    }
    if (ctrl->buttons & SCE_CTRL_CROSS) {
        if (respawn_selection == 0) {
            log_debug("respawn: player respawned via X");
            show_respawn = 0;
            respawn_cooldown = 30;
            net_send_respawn();
        } else {
            show_respawn = 0;
            show_menu = 1;
            respawn_cooldown = 30;
            net_disconnect("return to menu");
        }
    }
}

void handle_menu_input(const SceCtrlData *ctrl) {
    if (ime_active) return;
    if (menu_cooldown > 0) return;

    static int prev_buttons = 0;
    int pressed = ctrl->buttons & ~prev_buttons;
    prev_buttons = ctrl->buttons;

    if (pressed & SCE_CTRL_UP) {
        menu_selection = (menu_selection + 3) % 4;
        menu_cooldown = 10;
    }
    if (pressed & SCE_CTRL_DOWN) {
        menu_selection = (menu_selection + 1) % 4;
        menu_cooldown = 10;
    }
    if (pressed & SCE_CTRL_LEFT && menu_selection == 1) {
        server_port--;
        if (server_port < 1) server_port = 65535;
        menu_cooldown = 8;
    }
    if (pressed & SCE_CTRL_RIGHT && menu_selection == 1) {
        server_port++;
        if (server_port > 65535) server_port = 1;
        menu_cooldown = 8;
    }
    if (pressed & SCE_CTRL_CROSS) {
        if (menu_selection == 0) {
            ime_target = 0;
            ime_open("Server Address", server_host, SCE_IME_TYPE_URL, 64);
        } else if (menu_selection == 1) {
            ime_target = 1;
            char ps[16];
            snprintf(ps, sizeof(ps), "%d", server_port);
            ime_open("Port", ps, SCE_IME_TYPE_NUMBER, 5);
        } else if (menu_selection == 2) {
            log_debug("menu: connecting to %s:%d", server_host, server_port);
            net_connect(server_host, server_port, "VitaPlayer");
            show_menu = 0;
            menu_cooldown = 30;
        } else if (menu_selection == 3) {
            /* Respawn button - for testing when stuck */
            log_debug("menu: respawn button pressed");
            net_send_respawn();
            menu_cooldown = 30;
        }
    }
}

int main() {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    vglInit(0x800000);
    renderer_init();

    /* Init logging FIRST so atlas/texture load diagnostics survive. */
    extern void log_init_once(void);
    log_init_once();

    GLuint tex = compile_dynamic_atlas();
    g_atlas_tex = tex;

    net_init();
    
    static int auto_connect_attempts = 0;
    int conn = net_connect("kreqxs.aternos.me", 11113, "VitaPlayer");
    if (conn < 0) {
        log_debug("main: initial connect failed, will retry automatically");
    } else {
        log_debug("main: initial connect started (sock=%d)", conn);
    }
    static SceUInt64 last_auto_connect_attempt = 0;

Player player;
    player_init(&player);

    SceUInt64 last_time = sceKernelGetProcessTimeWide();
    SceUInt64 last_net_tick = last_time;
    const SceUInt64 NET_TICK_INTERVAL = 50000; /* 50ms = 20Hz */
    int frame_counter = 0;
    int render_frame = 0;
    int chunks_loaded = 0;
    static int rebuilds_this_frame = 0;
    static int frame_counter2 = 0;

    while (1) {
        SceUInt64 now = sceKernelGetProcessTimeWide();
        
        /* HEARTBEAT - log once per second to avoid file I/O bottleneck */
        if (++frame_counter % 60 == 0) {
            extern int position_look_received;
            int pxi = (int)(player.x * 10);
            int pyi = (int)(player.y * 10);
            int pzi = (int)(player.z * 10);
            log_debug("main: frame=%d phase=%d pos=(%d,%d,%d) fallback=%d chunks=%d", 
                frame_counter, g_net.phase, pxi, pyi, pzi, position_look_received, chunks_loaded);
        }
        
        /* NETWORK TICK: run at most once per frame so a slow tick (chunk
         * decode under the packet cap) can never wedge the main loop */
        if (now - last_net_tick >= NET_TICK_INTERVAL) {
            net_tick(&player);
            last_net_tick = now;
        }

        /* AUTO-CONNECT RETRY: if socket is dead and we're not in PLAY,
           retry connecting every 5 seconds so the user doesn't have to
           manually open the config menu after a failed initial connect. */
        static SceUInt64 last_reconnect_attempt = 0;
        if (g_net.sock < 0 && g_net.phase != PHASE_PLAY &&
            now - last_reconnect_attempt > 5000000) {
            last_reconnect_attempt = now;
            auto_connect_attempts++;
            log_debug("main: auto-connect retry #%d to %s:%d",
                      auto_connect_attempts, g_net.host[0] ? g_net.host : "kreqxs.aternos.me", g_net.port ? g_net.port : 11113);
            net_connect(g_net.host[0] ? g_net.host : "kreqxs.aternos.me",
                        g_net.port ? g_net.port : 11113, "VitaPlayer");
        }

        SceCtrlData ctrl;
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        
if (ctrl.buttons & SCE_CTRL_START) break;

        /* SELECT button opens chat input */
        if (ctrl.buttons & SCE_CTRL_SELECT) {
            if (!ime_active && menu_cooldown == 0) {
                ime_target = 2;
                ime_open("Chat", "", SCE_IME_TYPE_DEFAULT, 256);
                ime_active = 1;
                log_debug("main: chat opened via SELECT");
                menu_cooldown = 30;
            }
        }

        /* L + R shoulder buttons toggle the config menu. */
        int lr_held = (ctrl.buttons & SCE_CTRL_LTRIGGER) && (ctrl.buttons & SCE_CTRL_RTRIGGER);
        if (lr_held && !prev_lr_held) {
            log_debug("main: L+R detected, toggling menu (was %d)", show_menu);
            if (menu_cooldown == 0 && !ime_active) {
                show_menu = !show_menu;
                menu_cooldown = 30;
                if (show_menu) menu_selection = 0;
            }
        }
        prev_lr_held = lr_held;

        if (menu_cooldown > 0) menu_cooldown--;

        if (ime_active) {
            int confirmed = 0;
            char result[256];
            if (ime_poll(result, sizeof(result), &confirmed) && confirmed) {
                if (ime_target == 0) {
                    strncpy(server_host, result, sizeof(server_host) - 1);
                    server_host[sizeof(server_host) - 1] = 0;
                    log_debug("menu: server address set to %s", server_host);
                } else if (ime_target == 1) {
                    int p = atoi(result);
                    if (p >= 1 && p <= 65535) {
                        server_port = p;
                        log_debug("menu: port set to %d", server_port);
                    }
                } else if (ime_target == 2) {
                    /* Chat message — send raw text, not /say command.
                       /say may not be registered on all servers, and
                       wrapping in /say adds an extra token that the
                       server rejects.  Send the message directly. */
                    if (result[0]) {
                        net_send_chat(result);
                        log_debug("chat: sent '%s'", result);
                    }
                }
            }
        }

        if (show_menu) {
            handle_menu_input(&ctrl);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            draw_config_menu();
            vglSwapBuffers(GL_TRUE);
            continue;
        }

/* Frame time delta for player physics */
        static SceUInt64 last_frame_time = 0;
        SceUInt64 now_frame = sceKernelGetProcessTimeWide();
        float frame_dt = (float)((now_frame - last_frame_time) / 1000000.0);
        if (frame_dt <= 0.0f || frame_dt > 0.25f) frame_dt = 1.0f/60.0f;
        last_frame_time = now_frame;

        /* Detect death BEFORE the respawn screen check so the screen
           appears on the same frame death is detected.  Placing this
           after the check meant the respawn screen was skipped on the
           death frame, and the player never saw the menu. */
        if (!show_respawn && (player.y < -64.0f || player.health <= 0)) {
            show_respawn = 1;
            respawn_selection = 0;
            respawn_cooldown = 0;
            log_debug("main: detected death (y=%.1f health=%d), showing respawn screen",
                      player.y, player.health);
        }

        if (show_respawn) {
            handle_respawn_input(&ctrl);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            draw_respawn_screen();
            vglSwapBuffers(GL_TRUE);
            continue;
        }

        /* Update player physics, camera, and input.
           player_update() was never called — the function existed but
           the main loop skipped it, so the player couldn't move, look
           around, jump, or crouch. */
        player_update(&player, frame_dt);

        /* Send sneaking/sprint state changes to the server.
           1.12.2 EntityAction (0x15):
           action 0 = START_SNEAKING, 1 = STOP_SNEAKING,
           action 3 = START_SPRINTING, 4 = STOP_SPRINTING.
           Aux data = jump boost (0 for player). */
        static int last_sneak = -1, last_sprint = -1;
        int cur_sneak = player.is_sneaking ? 1 : 0;
        int cur_sprint = player.is_sprinting ? 1 : 0;
        if (cur_sneak != last_sneak) {
            net_send_player_action(cur_sneak ? 0 : 1);
            log_debug("main: EntityAction sneak=%d (0=START_SNEAKING, 1=STOP_SNEAKING)", cur_sneak ? 0 : 1);
            last_sneak = cur_sneak;
        }
        if (cur_sprint != last_sprint) {
            net_send_player_action(cur_sprint ? 3 : 4);
            log_debug("main: EntityAction sprint=%d (3=START_SPRINTING, 4=STOP_SPRINTING)", cur_sprint ? 3 : 4);
            last_sprint = cur_sprint;
        }

        /* Block interaction: L = place/interact, R = break/dig */
        static int last_l = 0, last_r = 0;
        static int dig_hx=0, dig_hy=0, dig_hz=0, dig_fd=0, dig_active=0;
        static SceUInt64 dig_start_time = 0;
        static int anim_counter = 0;
        int cur_l = (ctrl.buttons & SCE_CTRL_LTRIGGER) ? 1 : 0;
        int cur_r = (ctrl.buttons & SCE_CTRL_RTRIGGER) ? 1 : 0;
        float cur_eye = player.is_sneaking ? CROUCH_EYE_LEVEL : STANDING_EYE_LEVEL;

        /* Block breaking: send periodic Animation while holding R for visual feedback */
        if (dig_active && cur_r) {
            anim_counter++;
            if (anim_counter >= 20) { /* every ~0.33s at 60fps, ~0.66s at 30fps */
                anim_counter = 0;
                extern void net_send_animation(int hand);
                net_send_animation(0);
            }
            /* Auto-FINISH after break threshold */
            SceUInt64 now = sceKernelGetProcessTimeWide();
            if (dig_start_time > 0 && now - dig_start_time >= 333333) { /* 0.33s min */
                net_send_block_dig(2, dig_hx, dig_hy, dig_hz, dig_fd);  /* STOP_DESTROY_BLOCK */
                dig_active = 0;
                log_debug("main: R auto-finish dig at (%d,%d,%d)", dig_hx, dig_hy, dig_hz);
            }
        }

        if (cur_l && !last_l) {
            /* Raycast to find target block for placing */
            float rad_yaw   = (player.yaw) * DEG_TO_RAD;
            float rad_pitch = player.pitch * DEG_TO_RAD;
            float ldx = -sinf(rad_yaw) * cosf(rad_pitch);
            float ldy = -sinf(rad_pitch);
            float ldz =  cosf(rad_yaw) * cosf(rad_pitch);
            int hx, hy, hz, fd;
            if (perform_raycast(player.render_x, player.render_y + cur_eye, player.render_z,
                                ldx, ldy, ldz, &hx, &hy, &hz, &fd)) {
                /* Only send place packet + animation if holding a block (item ID > 0) */
                if (g_net.have_held_item && g_net.held_item_id > 0) {
                    /* Send Animation (0x1D) to swing arm */
                    extern void net_send_animation(int hand);
                    net_send_animation(0);
                    /* Place adjacent block on hit face */
                    int px = hx, py = hy, pz = hz;
                    if      (fd == FACE_TOP)    py += 1;
                    else if (fd == FACE_BOTTOM) py -= 1;
                    else if (fd == FACE_FRONT)  pz += 1;
                    else if (fd == FACE_BACK)   pz -= 1;
                    else if (fd == FACE_LEFT)   px -= 1;
                    else if (fd == FACE_RIGHT)  px += 1;
                    net_send_block_place(px, py, pz, fd);
                    log_debug("main: L place block at (%d,%d,%d) face=%d", px, py, pz, fd);
                }
            }
        }

        if (cur_r && !last_r) {
            /* Start digging on press */
            float rad_yaw   = (player.yaw) * DEG_TO_RAD;
            float rad_pitch = player.pitch * DEG_TO_RAD;
            float ldx = -sinf(rad_yaw) * cosf(rad_pitch);
            float ldy = -sinf(rad_pitch);
            float ldz =  cosf(rad_yaw) * cosf(rad_pitch);
            int hx, hy, hz, fd;
            if (perform_raycast(player.render_x, player.render_y + cur_eye, player.render_z,
                                ldx, ldy, ldz, &hx, &hy, &hz, &fd)) {
                /* Send Animation (0x1D) to swing arm */
                extern void net_send_animation(int hand);
                net_send_animation(0);
                net_send_block_dig(0, hx, hy, hz, fd);  /* START_DESTROY_BLOCK */
                dig_hx = hx; dig_hy = hy; dig_hz = hz; dig_fd = fd;
                dig_active = 1;
                dig_start_time = sceKernelGetProcessTimeWide();
                anim_counter = 0;
                log_debug("main: R start dig block at (%d,%d,%d) face=%d", hx, hy, hz, fd);
            } else {
                /* No block hit — attack nearest entity or punch air */
                extern void net_send_animation(int hand);
                net_send_animation(0);
                log_debug("main: R punch (no block target)");
            }
        }

        if (!cur_r && last_r && dig_active) {
            /* Cancel digging on early release (before auto-finish timer).
               This prevents quick-tap from breaking blocks.
               If timer already fired FINISH, dig_active=0 so this won't run. */
            net_send_block_dig(1, dig_hx, dig_hy, dig_hz, dig_fd);  /* ABORT_DESTROY_BLOCK */
            dig_active = 0;
            log_debug("main: R cancel dig at (%d,%d,%d)", dig_hx, dig_hy, dig_hz);
        }

        last_l = cur_l;
        last_r = cur_r;

        /* Incremental chunk rebuild - process multiple chunks per frame */
        static int rebuild_index = 0;
        int rebuilds_this_frame = 0;
        for (int i = 0; i < MAX_CHUNKS && rebuilds_this_frame < 4; i++) {
            int idx = (rebuild_index + i) % MAX_CHUNKS;
            int cx = idx / WORLD_SIZE_Z;
            int cz = idx % WORLD_SIZE_Z;
            if (world_chunks[cx][cz].dirty && world_chunks[cx][cz].loaded) {
                chunk_rebuild_mesh(&world_chunks[cx][cz]);
                rebuilds_this_frame++;
            }
        }
        rebuild_index = (rebuild_index + 1) % MAX_CHUNKS;

        /* Count loaded chunks */
        chunks_loaded = 0;
        for (int cx = 0; cx < WORLD_SIZE_X; cx++)
            for (int cz = 0; cz < WORLD_SIZE_Z; cz++)
                if (world_chunks[cx][cz].loaded) chunks_loaded++;

        /* RENDER EVERY FRAME - even if no chunks loaded (to clear vitaGL boot screen) */
        render_frame++;
        float current_eye = player.is_sneaking ? CROUCH_EYE_LEVEL : STANDING_EYE_LEVEL;
        float current_h   = player.is_sneaking ? CROUCH_HEIGHT : STANDING_HEIGHT;
        float rad_yaw     = player.yaw * DEG_TO_RAD;
        float rad_pitch   = player.pitch * DEG_TO_RAD;
        /* Minecraft look: yaw=0 faces +Z(south), pitch>0 = look down */
        float dir_x = -sinf(rad_yaw) * cosf(rad_pitch);
        float dir_y = -sinf(rad_pitch);
        float dir_z =  cosf(rad_yaw) * cosf(rad_pitch);

        float eyex = player.render_x, eyey = player.render_y + current_eye, eyez = player.render_z;

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        float fov = player.is_sprinting ? 77.0f : 70.0f;
        set_perspective(fov, 960.0f/544.0f, 0.05f, 100.0f);

/* GL camera.  OpenGL applies transforms in REVERSE order to vertices:
           glRotatef(pitch) → glRotatef(yaw+180) → glTranslatef(-pos)
           means: T(-pos) * Ry(yaw+180) * Rx(pitch) * v
           i.e. translate world so camera is at origin, then rotate.
           This is the correct look-at matrix. */
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glRotatef(player.pitch, 1,0,0);
    glRotatef(player.yaw + 180.0f, 0,1,0);
    glTranslatef(-player.render_x, -(player.render_y+current_eye), -player.render_z);

        glBindTexture(GL_TEXTURE_2D, g_atlas_tex);
        if (render_frame % 60 == 0) log_debug("render: frame %d draw_all_chunks start", render_frame);
        draw_all_chunks(player.render_x, player.render_z);
        if (render_frame % 60 == 0) log_debug("render: frame %d draw_all_chunks done", render_frame);
        render_frame++;

/* Block highlight from a raycast along the look direction */
        {
            /* Raycast direction MUST match the camera's forward vector exactly.
               Camera matrix: glRotatef(pitch,1,0,0) * glRotatef(yaw+180,0,1,0)
               Forward = (-sin(yaw)*cos(pitch), -sin(pitch), cos(yaw)*cos(pitch))
               The old formula had X sign wrong and misplaced cos(yaw) on Y/Z,
               causing the block outline to appear to the side of the crosshair. */
            float rad_yaw   = (player.yaw) * DEG_TO_RAD;
            float rad_pitch = player.pitch * DEG_TO_RAD;
            float ldx = -sinf(rad_yaw) * cosf(rad_pitch);
            float ldy = -sinf(rad_pitch);
            float ldz =  cosf(rad_yaw) * cosf(rad_pitch);
            int hx, hy, hz, fd;
            if (perform_raycast(player.render_x, player.render_y + current_eye, player.render_z,
                                ldx, ldy, ldz, &hx, &hy, &hz, &fd)) {
                draw_selection(hx, hy, hz);
            }
        }

        draw_crosshair();
        draw_log_overlay();

        /* FPS counter */
        SceUInt64 now_fps = sceKernelGetProcessTimeWide();
        static int fps_counter = 0;
        static int fps = 0;
        static SceUInt64 fps_last_time = 0;
        fps_counter++;
        if (now - fps_last_time >= 1000000) {
            fps = fps_counter;
            fps_counter = 0;
            fps_last_time = now;
            
            extern int ping_ms;
            extern int packets_sent, packets_recv;
            if (ping_ms < 100) connection_quality = 0;
            else if (ping_ms < 250) connection_quality = 1;
            else connection_quality = 2;
        }
        
        draw_hud(&player);
        
        vglSwapBuffers(GL_TRUE);
    }

    net_disconnect("quit");
    sceKernelExitProcess(0);
    return 0;
}