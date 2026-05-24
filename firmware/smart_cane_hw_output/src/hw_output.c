/**
 * @file hw_output.c
 * @brief SmartCane Hardware Output — Tuya T5AI board.
 *
 * Closes the AIoT loop: the laptop's smart_cane_app.py analyses camera
 * frames with Groq / Llama-4 and publishes results at GET /api/status.
 * This firmware polls that endpoint every 2 s and renders every AI
 * notification on real hardware:
 *
 *   Display   — LVGL full-colour UI:  alert-type chip + notification text
 *   Speaker   — categorised audio tone via the onboard I2S audio codec
 *   Buzzer    — urgent local alerts (fall / very close obstacle)
 *   Vibration — haptic feedback on fall or close obstacle
 *
 * Local sensors run in parallel:
 *   QMI8658 IMU — fall detection  → display + emergency tone + POST /event
 *   HC-SR04     — close obstacle  → display + proximity tone + POST /event
 *
 * Laptop endpoints used (smart_cane_app.py — NO changes required):
 *   GET  /api/status  — {"status":"…","current":{"say":"…","overhead":…,…}}
 *   POST /event       — {"type":"fall_detected","magnitude":…}
 *                       {"type":"obstacle","distance_cm":…}
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * EDIT THESE THREE BLOCKS BEFORE FLASHING
 * ─────────────────────────────────────────────────────────────────────────────
 */

/* ── Network / server ─────────────────────────────────────────────────────── */
#define WIFI_SSID    "JJ Lake"
#define WIFI_PASS    "20220315"
#define LAPTOP_IP    "192.168.34.203"
#define LAPTOP_PORT  5000

/* ── Tuya device licence (from TuyaOpen dashboard) ───────────────────────── */
#define TUYA_DEVICE_UUID    "uuidb744bb895f8e33c2"
#define TUYA_DEVICE_AUTHKEY "P5kB67vHCaz8YBu8kW9lQYLTDxiAmG5E"

/* ─────────────────────────────────────────────────────────────────────────── */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tuya_cloud_types.h"
#include "tal_api.h"
#include "tkl_output.h"
#include "tkl_gpio.h"
#include "tkl_i2c.h"
#include "tkl_pinmux.h"
#include "tkl_memory.h"
#include "tal_system.h"
#include "tal_thread.h"
#include "tal_network.h"
#include "netmgr.h"
#include "netconn_wifi.h"
#include "tal_event.h"
#include "board_com_api.h"
#include "tdl_audio_manage.h"
#include "lvgl.h"
#include "lv_vendor.h"

#ifndef DISPLAY_NAME
#define DISPLAY_NAME "display"
#endif

/* ── Pin assignments ──────────────────────────────────────────────────────── */
#define PIN_BUZZER          47
#define PIN_VIBRATION       17
#define PIN_ULTRASONIC_TRIG 14
#define PIN_ULTRASONIC_ECHO 15
#define PIN_I2C_SDA         20
#define PIN_I2C_SCL         21

/* ── QMI8658 IMU registers ────────────────────────────────────────────────── */
#define QMI_ADDR_HIGH       0x6B
#define QMI_ADDR_LOW        0x6A
#define QMI_WHO_AM_I_VAL    0x05
#define QMI_REG_WHO_AM_I    0x00
#define QMI_REG_CTRL1       0x02
#define QMI_REG_CTRL2       0x03
#define QMI_REG_CTRL7       0x08
#define QMI_REG_STATUS0     0x2E
#define QMI_REG_AX_L        0x35
#define QMI_CTRL2_8G_125HZ  ((0x02 << 4) | 0x06)
#define QMI_ACCEL_LSB_G     4096.0f

/* ── Thresholds & timing ──────────────────────────────────────────────────── */
#define FREE_FALL_G         0.65f   /* g magnitude below which = free-fall       */
#define OBSTACLE_CM         60.0f   /* cm below which = onboard proximity alert  */
#define SAMPLE_MS           25      /* IMU sample interval (ms)                  */
#define ULTRASONIC_MS       120     /* ultrasonic poll interval (ms)             */
#define POLL_STATUS_MS      2000    /* /api/status poll interval (ms)            */
#define STATUS_BUF_KB       16      /* max /api/status response size (KB)        */
#define BUZZ_HOLD_MS        1500
#define BUZZ_FREQ_HZ        2300
#define SAY_MAX             192     /* max chars for a displayed notification    */

/* ── Beken microsecond timer ─────────────────────────────────────────────── */
extern uint64_t bk_aon_rtc_get_us(void);
extern void     bk_timer_delay_us(uint32_t us);

/* ── Alert classification ─────────────────────────────────────────────────── */
typedef enum {
    ALERT_NONE = 0,
    ALERT_GENERAL,
    ALERT_OVERHEAD,
    ALERT_DROPOFF,
    ALERT_TRAFFIC_RED,
    ALERT_TRAFFIC_GREEN,
    ALERT_FALL,
    ALERT_OBSTACLE,
} alert_type_t;

/* ── Global state ─────────────────────────────────────────────────────────── */
static uint8_t             sg_qmi_addr     = 0;
static volatile bool       sg_wifi_up      = false;
static TDL_AUDIO_HANDLE_T  sg_audio_hdl    = NULL;
static TDL_AUDIO_INFO_T    sg_audio_info   = {0};
static volatile float      sg_magnitude    = 1.0f;
static volatile bool       sg_free_fall    = false;
static volatile float      sg_dist_cm      = -1.0f;
static volatile bool       sg_obstacle     = false;
static volatile uint32_t   sg_buzz_end     = 0;
static volatile uint32_t   sg_vib_end      = 0;
static THREAD_HANDLE       sg_sensor_th    = NULL;
static THREAD_HANDLE       sg_net_th       = NULL;
static bool                sg_display_ready = false;

/* LVGL label handles */
static lv_obj_t *sg_lbl_status  = NULL;   /* "Scanning" / "Analyzing…" / "Alert" */
static lv_obj_t *sg_lbl_type    = NULL;   /* "[ OVERHEAD HAZARD ]" etc.          */
static lv_obj_t *sg_lbl_notice  = NULL;   /* AI-generated say text               */
static lv_obj_t *sg_lbl_sensor  = NULL;   /* distance + accel reading            */

/* Dedup: only re-render when the AI notification text actually changes */
static char sg_last_say[SAY_MAX] = {0};

static inline uint32_t millis(void)
{
    return (uint32_t)(bk_aon_rtc_get_us() / 1000ULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Display helpers
 * ═════════════════════════════════════════════════════════════════════════ */

static void _lbl_set_text(lv_obj_t *lbl, const char *txt)
{
    if (!sg_display_ready || !lbl || !txt) return;
    lv_vendor_disp_lock();
    lv_label_set_text(lbl, txt);
    lv_vendor_disp_unlock();
}

static void _lbl_set_color(lv_obj_t *lbl, lv_color_t col)
{
    if (!sg_display_ready || !lbl) return;
    lv_vendor_disp_lock();
    lv_obj_set_style_text_color(lbl, col, LV_PART_MAIN);
    lv_vendor_disp_unlock();
}

static void display_set_status(const char *txt, lv_color_t col)
{
    _lbl_set_text(sg_lbl_status, txt);
    _lbl_set_color(sg_lbl_status, col);
}

static void display_set_alert_type(const char *txt, lv_color_t col)
{
    _lbl_set_text(sg_lbl_type, txt);
    _lbl_set_color(sg_lbl_type, col);
}

static void display_set_notice(const char *txt)   { _lbl_set_text(sg_lbl_notice, txt); }
static void display_set_sensor(const char *txt)   { _lbl_set_text(sg_lbl_sensor, txt); }

static void display_init(void)
{
    lv_vendor_init(DISPLAY_NAME);
    lv_vendor_disp_lock();

    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), LV_PART_MAIN);

    /* ── Title ──────────────────────────────────────────────────────────── */
    lv_obj_t *title = lv_label_create(lv_screen_active());
    lv_label_set_text(title, "SmartCane AI");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    /* ── Status row  (green = scanning, blue = analyzing, red = alert) ─── */
    sg_lbl_status = lv_label_create(lv_screen_active());
    lv_label_set_text(sg_lbl_status, "Booting...");
    lv_obj_set_style_text_color(sg_lbl_status, lv_palette_main(LV_PALETTE_GREY), LV_PART_MAIN);
    lv_obj_align(sg_lbl_status, LV_ALIGN_TOP_LEFT, 14, 46);

    /* ── Alert-type chip ────────────────────────────────────────────────── */
    sg_lbl_type = lv_label_create(lv_screen_active());
    lv_label_set_text(sg_lbl_type, "\xe2\x80\x94");   /* — (waiting) */
    lv_obj_set_style_text_color(sg_lbl_type, lv_palette_main(LV_PALETTE_GREY), LV_PART_MAIN);
    lv_obj_align(sg_lbl_type, LV_ALIGN_TOP_LEFT, 14, 80);

    /* ── Main AI notification text ──────────────────────────────────────── */
    sg_lbl_notice = lv_label_create(lv_screen_active());
    lv_obj_set_width(sg_lbl_notice, 292);
    lv_label_set_long_mode(sg_lbl_notice, LV_LABEL_LONG_WRAP);
    lv_label_set_text(sg_lbl_notice, "Waiting for AI scene analysis...");
    lv_obj_set_style_text_color(sg_lbl_notice, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(sg_lbl_notice, LV_ALIGN_TOP_LEFT, 14, 112);

    /* ── Sensor row at the bottom ───────────────────────────────────────── */
    sg_lbl_sensor = lv_label_create(lv_screen_active());
    lv_label_set_text(sg_lbl_sensor, "Dist: --  |  Accel: 1.00 g");
    lv_obj_set_style_text_color(sg_lbl_sensor, lv_palette_main(LV_PALETTE_GREY), LV_PART_MAIN);
    lv_obj_align(sg_lbl_sensor, LV_ALIGN_BOTTOM_LEFT, 14, -10);

    lv_vendor_disp_unlock();
    lv_vendor_start(5, 1024 * 8);
    sg_display_ready = true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Buzzer + vibration motor
 * ═════════════════════════════════════════════════════════════════════════ */

static void buzzer_off(void)
{
    tkl_gpio_write(PIN_BUZZER, TUYA_GPIO_LEVEL_LOW);
}

static void buzzer_tone(uint32_t hz, uint32_t dur_ms)
{
    if (hz == 0 || dur_ms == 0) { tal_system_sleep(dur_ms); return; }
    uint32_t half_us = 1000000UL / (hz * 2UL);
    uint32_t cycles  = (dur_ms * 1000UL) / (half_us * 2UL);
    for (uint32_t i = 0; i < cycles; i++) {
        tkl_gpio_write(PIN_BUZZER, TUYA_GPIO_LEVEL_HIGH);
        bk_timer_delay_us(half_us);
        tkl_gpio_write(PIN_BUZZER, TUYA_GPIO_LEVEL_LOW);
        bk_timer_delay_us(half_us);
    }
    buzzer_off();
}

static void vibration_on(void)  { tkl_gpio_write(PIN_VIBRATION, TUYA_GPIO_LEVEL_HIGH); }
static void vibration_off(void) { tkl_gpio_write(PIN_VIBRATION, TUYA_GPIO_LEVEL_LOW);  }

/* ═══════════════════════════════════════════════════════════════════════════
 * Speaker (I2S audio codec) — synthesised sine-wave tones
 * ═════════════════════════════════════════════════════════════════════════ */

static void speaker_tone(uint32_t hz, uint32_t dur_ms)
{
    if (!sg_audio_hdl || hz == 0 || dur_ms == 0) return;

    uint32_t sr  = sg_audio_info.sample_rate ? (uint32_t)sg_audio_info.sample_rate : 16000U;
    uint32_t n   = (sr * dur_ms) / 1000U;
    int16_t *buf = (int16_t *)tkl_system_psram_malloc(n * sizeof(int16_t));
    if (!buf) return;

    for (uint32_t i = 0; i < n; i++) {
        float phase = (2.0f * 3.14159265f * (float)hz * (float)i) / (float)sr;
        /* Taper final 10 % to avoid click */
        float amp = (i > n * 9 / 10) ? (float)(n - i) / (float)(n / 10 + 1) : 1.0f;
        buf[i] = (int16_t)(sinf(phase) * 10000.0f * amp);
    }

    const uint8_t *p   = (const uint8_t *)buf;
    uint32_t       rem = n * sizeof(int16_t);
    while (rem > 0) {
        uint32_t chunk = (rem < sg_audio_info.frame_size) ? rem : sg_audio_info.frame_size;
        tdl_audio_play(sg_audio_hdl, (uint8_t *)p, chunk);
        p   += chunk;
        rem -= chunk;
    }
    tkl_system_psram_free(buf);
}

/**
 * Play a distinct tone pattern for each alert type so the user can identify
 * the hazard category by ear, independent of the display.
 */
static void play_alert_tone(alert_type_t type)
{
    switch (type) {
    case ALERT_OVERHEAD:
        /* Descending two-note — "duck!" urgency */
        speaker_tone(587, 200);   /* D5 */
        tal_system_sleep(40);
        speaker_tone(440, 350);   /* A4 */
        buzzer_tone(BUZZ_FREQ_HZ, 80);
        break;

    case ALERT_DROPOFF:
        /* Triple low pulse — "watch your step" */
        for (int i = 0; i < 3; i++) {
            speaker_tone(220, 110);   /* A3 */
            tal_system_sleep(80);
        }
        buzzer_tone(BUZZ_FREQ_HZ, 60);
        break;

    case ALERT_TRAFFIC_RED:
        /* Three short staccato beeps — stop */
        for (int i = 0; i < 3; i++) {
            speaker_tone(440, 80);   /* A4 */
            tal_system_sleep(70);
        }
        break;

    case ALERT_TRAFFIC_GREEN:
        /* Ascending pleasant chime — go */
        speaker_tone(523, 90);   /* C5 */
        tal_system_sleep(30);
        speaker_tone(659, 90);   /* E5 */
        tal_system_sleep(30);
        speaker_tone(784, 140);  /* G5 */
        break;

    case ALERT_FALL:
        /* Rapid emergency pulse */
        for (int i = 0; i < 5; i++) {
            speaker_tone(494, 50);   /* B4 */
            buzzer_tone(BUZZ_FREQ_HZ, 50);
            tal_system_sleep(30);
        }
        break;

    case ALERT_OBSTACLE:
        /* Single short mid-tone — nearby object */
        speaker_tone(392, 90);   /* G4 */
        break;

    case ALERT_GENERAL:
    default:
        /* Soft two-note notification chime */
        speaker_tone(392, 90);   /* G4 */
        tal_system_sleep(50);
        speaker_tone(494, 120);  /* B4 */
        break;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * QMI8658 IMU helpers
 * ═════════════════════════════════════════════════════════════════════════ */

static OPERATE_RET imu_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return tkl_i2c_master_send(TUYA_I2C_NUM_0, sg_qmi_addr, buf, 2, TRUE);
}

static OPERATE_RET imu_read(uint8_t reg, uint8_t *out, uint16_t len)
{
    OPERATE_RET rt = tkl_i2c_master_send(TUYA_I2C_NUM_0, sg_qmi_addr, &reg, 1, FALSE);
    if (rt != OPRT_OK) return rt;
    return tkl_i2c_master_receive(TUYA_I2C_NUM_0, sg_qmi_addr, out, len, TRUE);
}

static bool qmi_setup(void)
{
    uint8_t who = 0;
    sg_qmi_addr = QMI_ADDR_HIGH;
    if (imu_read(QMI_REG_WHO_AM_I, &who, 1) != OPRT_OK || who != QMI_WHO_AM_I_VAL) {
        sg_qmi_addr = QMI_ADDR_LOW;
        if (imu_read(QMI_REG_WHO_AM_I, &who, 1) != OPRT_OK || who != QMI_WHO_AM_I_VAL) {
            sg_qmi_addr = 0;
            return false;
        }
    }
    imu_write(QMI_REG_CTRL1, 0x60);
    imu_write(QMI_REG_CTRL7, 0x00);
    imu_write(QMI_REG_CTRL2, QMI_CTRL2_8G_125HZ);
    imu_write(QMI_REG_CTRL7, 0x01);
    tal_system_sleep(50);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HC-SR04 ultrasonic
 * ═════════════════════════════════════════════════════════════════════════ */

static float ultrasonic_read_cm(void)
{
    TUYA_GPIO_LEVEL_E lvl;
    uint64_t t;

    tkl_gpio_write(PIN_ULTRASONIC_TRIG, TUYA_GPIO_LEVEL_LOW);
    bk_timer_delay_us(2);
    tkl_gpio_write(PIN_ULTRASONIC_TRIG, TUYA_GPIO_LEVEL_HIGH);
    bk_timer_delay_us(10);
    tkl_gpio_write(PIN_ULTRASONIC_TRIG, TUYA_GPIO_LEVEL_LOW);

    t = bk_aon_rtc_get_us();
    do {
        tkl_gpio_read(PIN_ULTRASONIC_ECHO, &lvl);
        if (bk_aon_rtc_get_us() - t > 30000ULL) return -1.0f;
    } while (lvl == TUYA_GPIO_LEVEL_LOW);

    uint64_t ps = bk_aon_rtc_get_us();
    do {
        tkl_gpio_read(PIN_ULTRASONIC_ECHO, &lvl);
        if (bk_aon_rtc_get_us() - ps > 30000ULL) return -1.0f;
    } while (lvl == TUYA_GPIO_LEVEL_HIGH);

    float d = (float)(bk_aon_rtc_get_us() - ps) * 0.0343f / 2.0f;
    return (d >= 2.0f && d <= 400.0f) ? d : -1.0f;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Minimal JSON field extractor (no external library)
 * ═════════════════════════════════════════════════════════════════════════ */

/**
 * Find `"key":"VALUE"` in `p` and copy VALUE into out[0..out_sz-1].
 * Handles backslash-escaped characters. Returns true on success.
 */
static bool json_str(const char *p, const char *key, char *out, int out_sz)
{
    char tag[72];
    snprintf(tag, sizeof(tag), "\"%s\":", key);
    const char *f = strstr(p, tag);
    if (!f) return false;
    f += strlen(tag);
    while (*f == ' ') f++;
    if (*f != '"') return false;
    f++;
    int i = 0;
    while (*f && *f != '"' && i < out_sz - 1) {
        if (*f == '\\' && *(f + 1)) f++;   /* skip escaped character */
        out[i++] = *f++;
    }
    out[i] = '\0';
    return (i > 0);
}

/**
 * Return true if `"key":true` appears in the first `window` bytes of `p`.
 */
static bool json_bool(const char *p, const char *key, int window)
{
    char tag[72];
    snprintf(tag, sizeof(tag), "\"%s\":true", key);
    const char *f = strstr(p, tag);
    return (f && (int)(f - p) < window);
}

/**
 * Parse GET /api/status JSON.
 *
 * Fills:
 *   say_out[SAY_MAX]   — AI notification text from current.say
 *   type_out           — most specific alert classification
 *   status_out[32]     — server status string ("scanning" / "analyzing" / …)
 *
 * Returns false when current scene is not yet available.
 */
static bool parse_status(const char *json,
                         char        *say_out,
                         alert_type_t *type_out,
                         char        *status_out,
                         int          status_sz)
{
    *type_out = ALERT_NONE;

    /* Top-level "status" field */
    json_str(json, "status", status_out, status_sz);

    /* Narrow search scope to the "current" object so we never pick up
     * matching keys inside the "history" array that follows it. */
    const char *cur = strstr(json, "\"current\":");
    if (!cur) return false;
    cur += strlen("\"current\":");
    while (*cur == ' ') cur++;
    if (*cur != '{') return false;   /* current is null / empty */

    /* Sanity: history starts somewhere after current — find that boundary */
    const char *history_pos = strstr(cur, "\"history\":");
    int scope = history_pos ? (int)(history_pos - cur) : (int)strlen(cur);

    /* Extract say text */
    const char *say_f = strstr(cur, "\"say\":");
    if (!say_f || (int)(say_f - cur) >= scope) return false;
    if (!json_str(cur, "say", say_out, SAY_MAX) || say_out[0] == '\0') return false;

    /* ── Classify alert type (most-specific hazard wins) ─────────────── */

    /* Overhead hazard */
    const char *oh = strstr(cur, "\"overhead\":");
    if (oh && (int)(oh - cur) < scope) {
        /* Only look within the overhead nested object (~150 bytes) */
        int win = (int)((cur + scope) - oh);
        if (win > 150) win = 150;
        if (json_bool(oh, "active", win)) {
            *type_out = ALERT_OVERHEAD;
            return true;
        }
    }

    /* Drop-off hazard */
    const char *dof = strstr(cur, "\"dropoff\":");
    if (dof && (int)(dof - cur) < scope) {
        int win = (int)((cur + scope) - dof);
        if (win > 150) win = 150;
        if (json_bool(dof, "active", win)) {
            *type_out = ALERT_DROPOFF;
            return true;
        }
    }

    /* Traffic light */
    char traffic[16] = {0};
    json_str(cur, "traffic", traffic, sizeof(traffic));
    if (strcmp(traffic, "red")   == 0) { *type_out = ALERT_TRAFFIC_RED;   return true; }
    if (strcmp(traffic, "green") == 0) { *type_out = ALERT_TRAFFIC_GREEN; return true; }

    /* General scene alert (important or any non-empty say) */
    *type_out = ALERT_GENERAL;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HTTP over raw TCP  (no port-number limitations of http_client_interface)
 * ═════════════════════════════════════════════════════════════════════════ */

static int _send_all(int fd, const uint8_t *buf, int len)
{
    int sent = 0;
    while (sent < len) {
        int r = tal_net_send(fd, buf + sent, len - sent);
        if (r <= 0) return -1;
        sent += r;
    }
    return sent;
}

/**
 * HTTP GET, returning the null-terminated body in a PSRAM buffer.
 * Caller must tkl_system_psram_free(*body_out).
 * Returns body length (≥1), 0 for 204 No Content, -1 on error.
 */
static int http_get_body(const char *path, char **body_out)
{
    *body_out = NULL;

    int fd = tal_net_socket_create(PROTOCOL_TCP);
    if (fd < 0) return -1;

    TUYA_IP_ADDR_T addr = tal_net_str2addr(LAPTOP_IP);
    if (addr == 0 || tal_net_connect(fd, addr, LAPTOP_PORT) != 0) {
        tal_net_close(fd);
        return -1;
    }

    char req[256];
    int  rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: " LAPTOP_IP ":%d\r\nAccept: */*\r\n\r\n",
        path, LAPTOP_PORT);
    if (_send_all(fd, (const uint8_t *)req, rlen) < 0) {
        tal_net_close(fd);
        return -1;
    }

    const int BUF = STATUS_BUF_KB * 1024;
    char *resp = (char *)tkl_system_psram_malloc(BUF + 1);
    if (!resp) { tal_net_close(fd); return -1; }

    int total = 0;
    while (total < BUF) {
        int r = tal_net_recv(fd, (uint8_t *)(resp + total), BUF - total);
        if (r <= 0) break;
        total += r;
    }
    tal_net_close(fd);
    resp[total] = '\0';

    if (total < 12) { tkl_system_psram_free(resp); return -1; }

    int status = 0;
    sscanf(resp, "HTTP/%*s %d", &status);
    if (status == 204) { tkl_system_psram_free(resp); return 0; }
    if (status != 200) { tkl_system_psram_free(resp); return -1; }

    /* Find \r\n\r\n header/body separator */
    char *body = NULL;
    for (int i = 0; i < total - 3; i++) {
        if (resp[i] == '\r' && resp[i+1] == '\n' &&
            resp[i+2] == '\r' && resp[i+3] == '\n') {
            body = resp + i + 4;
            break;
        }
    }
    if (!body) { tkl_system_psram_free(resp); return -1; }

    int body_len = total - (int)(body - resp);
    char *out = (char *)tkl_system_psram_malloc(body_len + 1);
    if (out) {
        memcpy(out, body, body_len);
        out[body_len] = '\0';
    }
    tkl_system_psram_free(resp);
    *body_out = out;
    return out ? body_len : -1;
}

static void http_post_json(const char *path, const char *json_body)
{
    int fd = tal_net_socket_create(PROTOCOL_TCP);
    if (fd < 0) return;

    TUYA_IP_ADDR_T addr = tal_net_str2addr(LAPTOP_IP);
    if (addr == 0 || tal_net_connect(fd, addr, LAPTOP_PORT) != 0) {
        tal_net_close(fd);
        return;
    }

    int body_len = (int)strlen(json_body);
    char hdr[256];
    int  hlen = snprintf(hdr, sizeof(hdr),
        "POST %s HTTP/1.0\r\nHost: " LAPTOP_IP ":%d\r\n"
        "Content-Type: application/json\r\nContent-Length: %d\r\n\r\n",
        path, LAPTOP_PORT, body_len);

    _send_all(fd, (const uint8_t *)hdr,       hlen);
    _send_all(fd, (const uint8_t *)json_body, body_len);

    uint8_t drain[64];
    while (tal_net_recv(fd, drain, sizeof(drain)) > 0) {}
    tal_net_close(fd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * WiFi event callback
 * ═════════════════════════════════════════════════════════════════════════ */

static OPERATE_RET net_event_cb(void *data)
{
    netmgr_status_e status = *((netmgr_status_e *)data);
    if (status == NETMGR_LINK_UP) {
        sg_wifi_up = true;
        display_set_status("Connected", lv_palette_main(LV_PALETTE_GREEN));
        display_set_notice("WiFi connected. Waiting for AI analysis...");
        PR_NOTICE("[HWOut] WiFi connected");
    } else {
        sg_wifi_up = false;
        display_set_status("No WiFi", lv_palette_main(LV_PALETTE_RED));
        display_set_notice("Reconnecting...");
        PR_NOTICE("[HWOut] WiFi disconnected");
    }
    return OPRT_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Network thread — polls /api/status, renders AI alerts on hardware
 * ═════════════════════════════════════════════════════════════════════════ */

static void net_thread_fn(void *arg)
{
    (void)arg;
    bool     fall_reported   = false;
    uint32_t last_fall_t     = 0;
    uint32_t last_poll_t     = 0;
    uint32_t last_obstacle_t = 0;

    PR_NOTICE("[HWOut] Net thread started");

    while (1) {
        if (!sg_wifi_up) {
            tal_system_sleep(500);
            continue;
        }

        uint32_t now = millis();

        /* ── Report fall event (once per event, 5 s minimum gap) ─────── */
        if (sg_free_fall && !fall_reported && (now - last_fall_t > 5000)) {
            fall_reported = true;
            last_fall_t   = now;

            char body[72];
            snprintf(body, sizeof(body),
                     "{\"type\":\"fall_detected\",\"magnitude\":%.2f}", sg_magnitude);
            http_post_json("/event", body);

            display_set_status("FALL DETECTED", lv_palette_main(LV_PALETTE_RED));
            display_set_alert_type("[ EMERGENCY ]", lv_palette_main(LV_PALETTE_RED));
            display_set_notice("Fall detected! Alerting system...");
            play_alert_tone(ALERT_FALL);
            PR_NOTICE("[HWOut] Fall event reported to laptop");
        } else if (!sg_free_fall) {
            fall_reported = false;
        }

        /* ── Report close obstacle (3 s throttle) ────────────────────── */
        if (sg_obstacle && (now - last_obstacle_t > 3000)) {
            last_obstacle_t = now;
            char body[72];
            snprintf(body, sizeof(body),
                     "{\"type\":\"obstacle\",\"distance_cm\":%.1f}", sg_dist_cm);
            http_post_json("/event", body);
        }

        /* ── Poll /api/status for AI notifications ───────────────────── */
        if (now - last_poll_t >= POLL_STATUS_MS) {
            last_poll_t = now;

            char *json     = NULL;
            int   json_len = http_get_body("/api/status", &json);

            if (json_len > 0 && json) {
                char        say[SAY_MAX]   = {0};
                char        srv_status[32] = {0};
                alert_type_t type          = ALERT_NONE;

                bool parsed = parse_status(json, say, &type, srv_status, sizeof(srv_status));

                /* Mirror the server scanning/analyzing status on the display */
                if (!sg_free_fall && !sg_obstacle) {
                    if (strcmp(srv_status, "analyzing") == 0) {
                        display_set_status("Analyzing...", lv_palette_main(LV_PALETTE_BLUE));
                    } else if (strcmp(srv_status, "alert") == 0) {
                        display_set_status("Alert", lv_palette_main(LV_PALETTE_RED));
                    } else if (strcmp(srv_status, "scanning") == 0) {
                        display_set_status("Scanning", lv_palette_main(LV_PALETTE_GREEN));
                    } else if (strcmp(srv_status, "error") == 0) {
                        display_set_status("Server Error", lv_palette_main(LV_PALETTE_ORANGE));
                    }
                }

                /* Only re-render + play tone when the notification text changes */
                if (parsed && say[0] != '\0' &&
                    strncmp(say, sg_last_say, SAY_MAX) != 0)
                {
                    strncpy(sg_last_say, say, SAY_MAX - 1);
                    sg_last_say[SAY_MAX - 1] = '\0';

                    display_set_notice(say);

                    /* Colour-coded alert-type chip */
                    switch (type) {
                    case ALERT_OVERHEAD:
                        display_set_alert_type("[ OVERHEAD HAZARD ]",
                            lv_palette_main(LV_PALETTE_ORANGE));
                        break;
                    case ALERT_DROPOFF:
                        display_set_alert_type("[ DROP-OFF AHEAD ]",
                            lv_palette_main(LV_PALETTE_RED));
                        break;
                    case ALERT_TRAFFIC_RED:
                        display_set_alert_type("[ TRAFFIC: RED ]",
                            lv_palette_main(LV_PALETTE_RED));
                        break;
                    case ALERT_TRAFFIC_GREEN:
                        display_set_alert_type("[ TRAFFIC: GREEN ]",
                            lv_palette_main(LV_PALETTE_GREEN));
                        break;
                    case ALERT_GENERAL:
                        display_set_alert_type("[ SCENE ALERT ]",
                            lv_palette_main(LV_PALETTE_CYAN));
                        break;
                    default:
                        display_set_alert_type("\xe2\x80\x94",
                            lv_palette_main(LV_PALETTE_GREY));
                        break;
                    }

                    if (type != ALERT_NONE) {
                        play_alert_tone(type);
                    }

                    PR_NOTICE("[HWOut] [type=%d] %s", (int)type, say);
                }
            }

            if (json) tkl_system_psram_free(json);
        }

        tal_system_sleep(50);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Sensor thread — IMU fall detection + HC-SR04 + buzzer + vibration
 * ═════════════════════════════════════════════════════════════════════════ */

static void sensor_thread_fn(void *arg)
{
    (void)arg;
    PR_NOTICE("[HWOut] Sensor thread started");

    TUYA_GPIO_BASE_CFG_T out_cfg = {
        .mode   = TUYA_GPIO_PUSH_PULL,
        .direct = TUYA_GPIO_OUTPUT,
        .level  = TUYA_GPIO_LEVEL_LOW,
    };
    TUYA_GPIO_BASE_CFG_T in_cfg = {
        .mode   = TUYA_GPIO_FLOATING,
        .direct = TUYA_GPIO_INPUT,
    };
    tkl_gpio_init(PIN_BUZZER,          &out_cfg);
    tkl_gpio_init(PIN_VIBRATION,       &out_cfg);
    tkl_gpio_init(PIN_ULTRASONIC_TRIG, &out_cfg);
    tkl_gpio_init(PIN_ULTRASONIC_ECHO, &in_cfg);
    buzzer_off();
    vibration_off();

    /* Startup chime: C5 → E5 → G5 */
    speaker_tone(523, 100);
    tal_system_sleep(40);
    speaker_tone(659, 100);
    tal_system_sleep(40);
    speaker_tone(784, 160);
    buzzer_tone(BUZZ_FREQ_HZ, 60);

    /* I2C init for QMI8658 */
    TUYA_IIC_BASE_CFG_T i2c_cfg = {
        .role       = TUYA_IIC_MODE_MASTER,
        .speed      = TUYA_IIC_BUS_SPEED_400K,
        .addr_width = TUYA_IIC_ADDRESS_7BIT,
    };
    tkl_io_pinmux_config(PIN_I2C_SDA, TUYA_IIC0_SDA);
    tkl_io_pinmux_config(PIN_I2C_SCL, TUYA_IIC0_SCL);
    tkl_i2c_init(TUYA_I2C_NUM_0, &i2c_cfg);

    if (qmi_setup()) {
        PR_NOTICE("[HWOut] QMI8658 at 0x%02X", sg_qmi_addr);
    } else {
        PR_WARN("[HWOut] QMI8658 not found — fall detection disabled");
    }

    uint32_t last_accel_t  = 0;
    uint32_t last_ultra_t  = 0;
    uint32_t last_sensor_t = 0;
    uint32_t last_buzz_t   = 0;
    uint32_t last_vib_t    = 0;
    uint32_t last_retry_t  = 0;
    bool     vib_state     = false;

    while (1) {
        uint32_t now = millis();

        /* ── IMU sample ─────────────────────────────────────────────── */
        if (sg_qmi_addr && (now - last_accel_t >= SAMPLE_MS)) {
            last_accel_t = now;
            uint8_t sr = 0;
            if (imu_read(QMI_REG_STATUS0, &sr, 1) == OPRT_OK && (sr & 0x01)) {
                uint8_t raw[6];
                if (imu_read(QMI_REG_AX_L, raw, 6) == OPRT_OK) {
                    float ax = (int16_t)((raw[1] << 8) | raw[0]) / QMI_ACCEL_LSB_G;
                    float ay = (int16_t)((raw[3] << 8) | raw[2]) / QMI_ACCEL_LSB_G;
                    float az = (int16_t)((raw[5] << 8) | raw[4]) / QMI_ACCEL_LSB_G;
                    float mag = sqrtf(ax*ax + ay*ay + az*az);
                    sg_magnitude = mag;
                    if (mag < FREE_FALL_G) {
                        sg_free_fall = true;
                        sg_buzz_end  = now + BUZZ_HOLD_MS;
                        sg_vib_end   = now + BUZZ_HOLD_MS;
                    } else {
                        sg_free_fall = false;
                    }
                }
            }
        } else if (!sg_qmi_addr && (now - last_retry_t >= 1000)) {
            last_retry_t = now;
            if (qmi_setup()) PR_NOTICE("[HWOut] QMI8658 reconnected at 0x%02X", sg_qmi_addr);
        }

        /* ── Ultrasonic sample ──────────────────────────────────────── */
        if (now - last_ultra_t >= ULTRASONIC_MS) {
            last_ultra_t = now;
            sg_dist_cm   = ultrasonic_read_cm();
            sg_obstacle  = (sg_dist_cm > 0.0f && sg_dist_cm <= OBSTACLE_CM);
        }

        /* ── Update sensor row on display every 500 ms ──────────────── */
        if (now - last_sensor_t >= 500) {
            last_sensor_t = now;
            char txt[64];
            if (sg_dist_cm > 0.0f)
                snprintf(txt, sizeof(txt), "Dist: %.0f cm  |  Accel: %.2f g",
                         sg_dist_cm, sg_magnitude);
            else
                snprintf(txt, sizeof(txt), "Dist: --  |  Accel: %.2f g", sg_magnitude);
            display_set_sensor(txt);

            /* Show obstacle on the notice area (onboard sensor, not AI) */
            if (sg_obstacle) {
                char notice[64];
                snprintf(notice, sizeof(notice), "Object %.0f cm ahead (sensor)", sg_dist_cm);
                display_set_status("Obstacle", lv_palette_main(LV_PALETTE_ORANGE));
                display_set_alert_type("[ CLOSE OBSTACLE ]", lv_palette_main(LV_PALETTE_ORANGE));
                display_set_notice(notice);
            }
        }

        /* ── Buzzer (fall sustained + obstacle pulsed) ──────────────── */
        bool do_buzz = (now < sg_buzz_end) || sg_obstacle;
        if (do_buzz && (now - last_buzz_t >= 130)) {
            last_buzz_t = now;
            buzzer_tone(BUZZ_FREQ_HZ, 80);
        } else if (!do_buzz) {
            buzzer_off();
        }

        /* ── Vibration motor ────────────────────────────────────────── */
        if (now < sg_vib_end) {
            vibration_on();
        } else if (sg_obstacle) {
            if (now - last_vib_t >= 300) {
                last_vib_t = now;
                vib_state  = !vib_state;
                if (vib_state) vibration_on(); else vibration_off();
            }
        } else {
            vibration_off();
            vib_state = false;
        }

        tal_system_sleep(10);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * user_main — TuyaOpen RTOS entry point
 * ═════════════════════════════════════════════════════════════════════════ */

void user_main(void)
{
    OPERATE_RET rt = OPRT_OK;
    tal_log_init(TAL_LOG_LEVEL_INFO, 1024, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("SmartCane HW Output starting");
    PR_NOTICE("  WiFi:   %s", WIFI_SSID);
    PR_NOTICE("  Server: %s:%d", LAPTOP_IP, LAPTOP_PORT);

    TUYA_CALL_ERR_LOG(board_register_hardware());
    display_init();
    display_set_status("Starting...", lv_palette_main(LV_PALETTE_GREY));

    /* Audio codec */
    if (tdl_audio_find(AUDIO_CODEC_NAME, &sg_audio_hdl) == OPRT_OK) {
        tdl_audio_open(sg_audio_hdl, NULL);
        tdl_audio_get_info(sg_audio_hdl, &sg_audio_info);
        tdl_audio_volume_set(sg_audio_hdl, 80);
        PR_NOTICE("[HWOut] Audio ready — %d Hz, frame %d B",
                  sg_audio_info.sample_rate, sg_audio_info.frame_size);
    } else {
        PR_WARN("[HWOut] Audio codec not found — buzzer-only fallback active");
    }

    /* KV store, timer, and workqueue (required by netmgr) */
    tal_kv_init(&(tal_kv_cfg_t){
        .seed = "vmlkasdh93dlvlcy",
        .key  = "dflfuap134ddlduq",
    });
    tal_kv_set("tuya_uuid",    (const uint8_t *)TUYA_DEVICE_UUID,    strlen(TUYA_DEVICE_UUID));
    tal_kv_set("tuya_authkey", (const uint8_t *)TUYA_DEVICE_AUTHKEY, strlen(TUYA_DEVICE_AUTHKEY));
    tal_sw_timer_init();
    tal_workq_init();

    tal_event_subscribe(EVENT_LINK_STATUS_CHG, "hw_output",
                        net_event_cb, SUBSCRIBE_TYPE_NORMAL);

#if defined(ENABLE_LIBLWIP) && (ENABLE_LIBLWIP == 1)
    TUYA_LwIP_Init();
#endif

    netmgr_init(NETCONN_WIFI);
    netconn_wifi_info_t wifi_info = {0};
    strncpy(wifi_info.ssid, WIFI_SSID, sizeof(wifi_info.ssid) - 1);
    strncpy(wifi_info.pswd, WIFI_PASS, sizeof(wifi_info.pswd) - 1);
    netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_SSID_PSWD, &wifi_info);

    /* Sensor thread (IMU + ultrasonic + haptics) */
    THREAD_CFG_T sensor_cfg = {
        .stackDepth = 1024 * 6,
        .priority   = THREAD_PRIO_2,
        .thrdname   = "hwo_sensor",
    };
    TUYA_CALL_ERR_LOG(tal_thread_create_and_start(
        &sg_sensor_th, NULL, NULL, sensor_thread_fn, NULL, &sensor_cfg));

    /* Network thread (polls /api/status, renders AI alerts) */
    THREAD_CFG_T net_cfg = {
        .stackDepth = 1024 * 12,
        .priority   = THREAD_PRIO_2,
        .thrdname   = "hwo_net",
    };
    TUYA_CALL_ERR_LOG(tal_thread_create_and_start(
        &sg_net_th, NULL, NULL, net_thread_fn, NULL, &net_cfg));

    display_set_notice("Hardware output ready. Waiting for WiFi...");
    display_set_status("Waiting for WiFi", lv_palette_main(LV_PALETTE_YELLOW));
    PR_NOTICE("[HWOut] Init complete");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * RTOS wrapper (non-Linux target)
 * ═════════════════════════════════════════════════════════════════════════ */
#if OPERATING_SYSTEM != SYSTEM_LINUX

static THREAD_HANDLE ty_app_thread = NULL;

static void tuya_app_thread(void *arg)
{
    (void)arg;
    user_main();
    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T cfg = {
        .stackDepth = 1024 * 32,
        .priority   = THREAD_PRIO_1,
        .thrdname   = "tuya_app_main",
    };
    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &cfg);
}

#endif
