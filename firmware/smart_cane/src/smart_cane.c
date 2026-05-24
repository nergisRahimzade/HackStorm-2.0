/**
 * @file smart_cane.c
 * @brief SmartCane firmware for the Tuya T5-E1 Touch AMOLED board (BK7258).
 *
 * Connects to WiFi and communicates with the laptop Python app
 * (tools/smart_cane_app.py) running on the same network:
 *
 *   GET  /pending_command  → 200 + raw 16 kHz/16-bit/mono PCM to play, or 204
 *   POST /event            → JSON body reporting fall / obstacle to laptop TTS
 *
 * Hardware wired to the back header  [47][17][16][15][14][3V3][GND][5V]:
 *   IO47 — passive buzzer (piezo)
 *   IO14 — HC-SR04 TRIG
 *   IO15 — HC-SR04 ECHO
 *   IO20 — I2C SDA  (onboard QMI8658 IMU, internal trace)
 *   IO21 — I2C SCL  (onboard QMI8658 IMU, internal trace)
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * BEFORE FLASHING — edit the three #defines below to match your environment:
 * ─────────────────────────────────────────────────────────────────────────────
 */

/* ── User-configurable settings ─────────────────────────────────────────────── */
/* WiFi — UPDATE SSID/PASS/IP when switching networks                            */
#define WIFI_SSID    "hasan adl\u0131 ki\u015fiye ait S21 FE"  /* current 2.4 GHz hotspot        */
#define WIFI_PASS    "ulku2503,"           /* WPA2 passphrase                   */
#define LAPTOP_IP    "10.218.130.50"       /* laptop IP on this network          */
#define LAPTOP_PORT  5000                  /* smart_cane_app.py Flask port       */

/* Tuya device license (uuid + authkey from TuyaOpen dashboard)                  */
#define TUYA_DEVICE_UUID    "uuidb744bb895f8e33c2"
#define TUYA_DEVICE_AUTHKEY "P5kB67vHCaz8YBu8kW9lQYLTDxiAmG5E"
/* ─────────────────────────────────────────────────────────────────────────────── */

#include <string.h>
#include <stdio.h>
#include <math.h>

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

/* WiFi / network manager */
#include "netmgr.h"
#include "netconn_wifi.h"
#include "tal_event.h"

/* Audio */
#include "board_com_api.h"
#include "tdl_audio_manage.h"

/* ── Pin assignments ─────────────────────────────────────────────────────────── */
#define PIN_BUZZER          47
#define PIN_ULTRASONIC_TRIG 14
#define PIN_ULTRASONIC_ECHO 15
#define PIN_I2C_SDA         20
#define PIN_I2C_SCL         21

/* ── QMI8658 IMU registers ───────────────────────────────────────────────────── */
#define QMI_ADDR_HIGH       0x6B
#define QMI_ADDR_LOW        0x6A
#define QMI_WHO_AM_I_VAL    0x05
#define QMI_REG_WHO_AM_I    0x00
#define QMI_REG_CTRL1       0x02
#define QMI_REG_CTRL2       0x03
#define QMI_REG_CTRL7       0x08
#define QMI_REG_STATUS0     0x2E
#define QMI_REG_AX_L        0x35
#define QMI_CTRL2_8G_125HZ  ((0x02 << 4) | 0x06)   /* ±8 g, 125 Hz */
#define QMI_ACCEL_LSB_G     4096.0f

/* ── Thresholds & timing ─────────────────────────────────────────────────────── */
#define FREE_FALL_G         0.65f   /* g magnitude below which = free-fall      */
#define OBSTACLE_CM         10.0f   /* cm below which = obstacle alert           */
#define SAMPLE_MS           25      /* IMU sample interval (ms)                  */
#define ULTRASONIC_MS       100     /* ultrasonic poll interval (ms)             */
#define BUZZ_HOLD_MS        1500    /* buzzer on after fall detected (ms)        */
#define BUZZ_FREQ_HZ        2300    /* buzzer frequency                          */
#define POLL_MS             600     /* HTTP /pending_command poll interval (ms)  */
#define AUDIO_BUF_KB        192     /* max PCM response size (KB)                */

/* ── Beken microsecond timer (same BK7258 chip used by Arduino variant) ────── */
extern uint64_t bk_aon_rtc_get_us(void);
extern void     bk_timer_delay_us(uint32_t us);

/* ── Global state ────────────────────────────────────────────────────────────── */
static uint8_t             sg_qmi_addr   = 0;
static volatile bool       sg_wifi_up    = false;
static TDL_AUDIO_HANDLE_T  sg_audio_hdl  = NULL;
static TDL_AUDIO_INFO_T    sg_audio_info = {0};
static volatile float      sg_magnitude  = 1.0f;
static volatile bool       sg_free_fall  = false;
static volatile uint32_t   sg_buzz_end   = 0;   /* millis() when buzzer should stop */
static volatile float      sg_dist_cm    = -1.0f;
static volatile bool       sg_obstacle   = false;
static THREAD_HANDLE       sg_sensor_th  = NULL;
static THREAD_HANDLE       sg_net_th     = NULL;

/* ── Millisecond helper ──────────────────────────────────────────────────────── */
static inline uint32_t millis(void)
{
    return (uint32_t)(bk_aon_rtc_get_us() / 1000ULL);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Buzzer
 * ═════════════════════════════════════════════════════════════════════════════ */
static void buzzer_off(void)
{
    tkl_gpio_write(PIN_BUZZER, TUYA_GPIO_LEVEL_LOW);
}

static void buzzer_tone(uint32_t freq_hz, uint32_t dur_ms)
{
    if (freq_hz == 0 || dur_ms == 0) {
        tal_system_sleep(dur_ms);
        return;
    }
    uint32_t half_us = 1000000UL / (freq_hz * 2UL);
    uint32_t cycles  = (dur_ms * 1000UL) / (half_us * 2UL);
    for (uint32_t i = 0; i < cycles; ++i) {
        tkl_gpio_write(PIN_BUZZER, TUYA_GPIO_LEVEL_HIGH);
        bk_timer_delay_us(half_us);
        tkl_gpio_write(PIN_BUZZER, TUYA_GPIO_LEVEL_LOW);
        bk_timer_delay_us(half_us);
    }
    buzzer_off();
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * I2C helpers for QMI8658
 * ═════════════════════════════════════════════════════════════════════════════ */
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

/* ── Probe + configure QMI8658 ──────────────────────────────────────────────── */
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
    imu_write(QMI_REG_CTRL1, 0x60);              /* enable address auto-increment */
    imu_write(QMI_REG_CTRL7, 0x00);              /* disable all sensors first     */
    imu_write(QMI_REG_CTRL2, QMI_CTRL2_8G_125HZ);
    imu_write(QMI_REG_CTRL7, 0x01);              /* enable accelerometer           */
    tal_system_sleep(50);
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * HC-SR04 ultrasonic distance
 * ═════════════════════════════════════════════════════════════════════════════ */
static float ultrasonic_read_cm(void)
{
    TUYA_GPIO_LEVEL_E lvl;
    uint64_t t;

    /* 10 µs trigger pulse */
    tkl_gpio_write(PIN_ULTRASONIC_TRIG, TUYA_GPIO_LEVEL_LOW);
    bk_timer_delay_us(2);
    tkl_gpio_write(PIN_ULTRASONIC_TRIG, TUYA_GPIO_LEVEL_HIGH);
    bk_timer_delay_us(10);
    tkl_gpio_write(PIN_ULTRASONIC_TRIG, TUYA_GPIO_LEVEL_LOW);

    /* wait for echo to go HIGH (30 ms timeout) */
    t = bk_aon_rtc_get_us();
    do {
        tkl_gpio_read(PIN_ULTRASONIC_ECHO, &lvl);
        if (bk_aon_rtc_get_us() - t > 30000ULL) return -1.0f;
    } while (lvl == TUYA_GPIO_LEVEL_LOW);

    /* measure HIGH pulse duration */
    uint64_t pulse_start = bk_aon_rtc_get_us();
    do {
        tkl_gpio_read(PIN_ULTRASONIC_ECHO, &lvl);
        if (bk_aon_rtc_get_us() - pulse_start > 30000ULL) return -1.0f;
    } while (lvl == TUYA_GPIO_LEVEL_HIGH);

    float dist = (float)(bk_aon_rtc_get_us() - pulse_start) * 0.0343f / 2.0f;
    return (dist >= 2.0f && dist <= 400.0f) ? dist : -1.0f;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Audio playback (16 kHz / 16-bit / mono PCM)
 * ═════════════════════════════════════════════════════════════════════════════ */
static void audio_play_pcm(const uint8_t *pcm, uint32_t len)
{
    if (!sg_audio_hdl || sg_audio_info.frame_size == 0 || !pcm || len == 0) return;
    const uint8_t *p   = pcm;
    uint32_t       rem = len;
    while (rem > 0) {
        uint32_t chunk = (rem < sg_audio_info.frame_size) ? rem : sg_audio_info.frame_size;
        tdl_audio_play(sg_audio_hdl, (uint8_t *)p, chunk);
        p   += chunk;
        rem -= chunk;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Minimal HTTP over raw TCP (avoids port-number limitations of http_client_interface)
 * ═════════════════════════════════════════════════════════════════════════════ */

/** Send all bytes, retrying on short writes. */
static int send_all(int fd, const uint8_t *buf, int len)
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
 * HTTP GET /path on LAPTOP_IP:LAPTOP_PORT.
 * Allocates a PSRAM buffer, writes the response body into it, returns body length.
 * Caller must tkl_system_psram_free(*body_out).
 * Returns 0 on 204 No Content, -1 on error.
 */
static int http_get_body(const char *path, uint8_t **body_out)
{
    *body_out = NULL;

    int fd = tal_net_socket_create(PROTOCOL_TCP);
    if (fd < 0) { PR_ERR("[Net] socket create failed"); return -1; }

    TUYA_IP_ADDR_T addr = tal_net_str2addr(LAPTOP_IP);
    if (addr == 0) { tal_net_close(fd); PR_ERR("[Net] bad IP"); return -1; }

    if (tal_net_connect(fd, addr, LAPTOP_PORT) != 0) {
        tal_net_close(fd);
        return -1;  /* server not up yet — silent fail */
    }

    char req[256];
    int  rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: " LAPTOP_IP ":%d\r\nAccept: */*\r\n\r\n",
        path, LAPTOP_PORT);
    if (send_all(fd, (const uint8_t *)req, rlen) < 0) {
        tal_net_close(fd);
        return -1;
    }

    /* Read entire response into a PSRAM buffer */
    const int BUF = AUDIO_BUF_KB * 1024;
    uint8_t  *resp = (uint8_t *)tkl_system_psram_malloc(BUF);
    if (!resp) { tal_net_close(fd); PR_ERR("[Net] psram alloc failed"); return -1; }

    int total = 0;
    while (total < BUF) {
        int r = tal_net_recv(fd, resp + total, BUF - total);
        if (r <= 0) break;
        total += r;
    }
    tal_net_close(fd);

    if (total < 12) { tkl_system_psram_free(resp); return -1; }

    /* Parse status code from first line "HTTP/1.x NNN ..." */
    int status = 0;
    sscanf((char *)resp, "HTTP/%*s %d", &status);
    if (status == 204) { tkl_system_psram_free(resp); return 0; }
    if (status != 200) { tkl_system_psram_free(resp); return -1; }

    /* Find header/body separator \r\n\r\n */
    uint8_t *body = NULL;
    for (int i = 0; i < total - 3; i++) {
        if (resp[i] == '\r' && resp[i+1] == '\n' &&
            resp[i+2] == '\r' && resp[i+3] == '\n') {
            body = resp + i + 4;
            break;
        }
    }
    if (!body || body >= resp + total) { tkl_system_psram_free(resp); return -1; }

    int body_len = (int)(total - (body - resp));

    /* Move body to a fresh compact buffer */
    uint8_t *out = (uint8_t *)tkl_system_psram_malloc(body_len);
    if (out) memcpy(out, body, body_len);
    tkl_system_psram_free(resp);

    *body_out = out;
    return out ? body_len : -1;
}

/**
 * HTTP POST /path with a JSON body.
 */
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

    send_all(fd, (const uint8_t *)hdr,       hlen);
    send_all(fd, (const uint8_t *)json_body, body_len);

    /* drain response */
    uint8_t drain[128];
    while (tal_net_recv(fd, drain, sizeof(drain)) > 0) {}
    tal_net_close(fd);
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Network event callback
 * ═════════════════════════════════════════════════════════════════════════════ */
static OPERATE_RET net_event_cb(void *data)
{
    netmgr_status_e status = *((netmgr_status_e *)data);
    if (status == NETMGR_LINK_UP) {
        sg_wifi_up = true;
        PR_NOTICE("[SmartCane] WiFi connected");
    } else {
        sg_wifi_up = false;
        PR_NOTICE("[SmartCane] WiFi disconnected");
    }
    return OPRT_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Network / HTTP thread — polls for TTS audio, reports events
 * ═════════════════════════════════════════════════════════════════════════════ */
static void net_thread_fn(void *arg)
{
    PR_NOTICE("[SmartCane] Net thread started");

    bool     fall_reported      = false;
    uint32_t last_fall_report_t = 0;

    while (1) {
        if (!sg_wifi_up) {
            tal_system_sleep(500);
            continue;
        }

        uint32_t now = millis();

        /* ── Report fall event to laptop (once per fall, 5-second minimum gap) ── */
        if (sg_free_fall && !fall_reported && (now - last_fall_report_t > 5000)) {
            fall_reported      = true;
            last_fall_report_t = now;
            char body[72];
            snprintf(body, sizeof(body),
                     "{\"type\":\"fall_detected\",\"magnitude\":%.2f}", sg_magnitude);
            http_post_json("/event", body);
            PR_NOTICE("[SmartCane] Fall event sent to laptop");
        } else if (!sg_free_fall) {
            fall_reported = false;
        }

        /* ── Poll laptop for pending TTS audio ─────────────────────────────────── */
        uint8_t *pcm     = NULL;
        int      pcm_len = http_get_body("/pending_command", &pcm);

        if (pcm_len > 0 && pcm) {
            PR_NOTICE("[SmartCane] Playing audio (%d bytes)", pcm_len);
            audio_play_pcm(pcm, (uint32_t)pcm_len);
        }
        if (pcm) tkl_system_psram_free(pcm);

        tal_system_sleep(POLL_MS);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * Sensor thread — IMU fall detection + ultrasonic + buzzer
 * ═════════════════════════════════════════════════════════════════════════════ */
static void sensor_thread_fn(void *arg)
{
    PR_NOTICE("[SmartCane] Sensor thread started");

    /* GPIO init */
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
    tkl_gpio_init(PIN_ULTRASONIC_TRIG, &out_cfg);
    tkl_gpio_init(PIN_ULTRASONIC_ECHO, &in_cfg);
    buzzer_off();

    /* Startup beep (3 × 80 ms) */
    for (int i = 0; i < 3; ++i) {
        buzzer_tone(BUZZ_FREQ_HZ, 80);
        tal_system_sleep(90);
    }

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
        PR_NOTICE("[SmartCane] QMI8658 at 0x%02X", sg_qmi_addr);
    } else {
        PR_WARN("[SmartCane] QMI8658 not found — fall detection disabled");
    }

    uint32_t last_accel_t     = 0;
    uint32_t last_ultrasonic_t = 0;
    uint32_t last_buzzer_t    = 0;
    uint32_t last_retry_t     = 0;

    while (1) {
        uint32_t now = millis();

        /* ── IMU sample ─────────────────────────────────────────────────────── */
        if (sg_qmi_addr && (now - last_accel_t >= SAMPLE_MS)) {
            last_accel_t = now;
            uint8_t status_reg = 0;
            if (imu_read(QMI_REG_STATUS0, &status_reg, 1) == OPRT_OK &&
                (status_reg & 0x01)) {
                uint8_t raw[6];
                if (imu_read(QMI_REG_AX_L, raw, 6) == OPRT_OK) {
                    float ax = (int16_t)((raw[1] << 8) | raw[0]) / QMI_ACCEL_LSB_G;
                    float ay = (int16_t)((raw[3] << 8) | raw[2]) / QMI_ACCEL_LSB_G;
                    float az = (int16_t)((raw[5] << 8) | raw[4]) / QMI_ACCEL_LSB_G;
                    float mag = sqrtf(ax*ax + ay*ay + az*az);
                    sg_magnitude = mag;
                    if (mag < FREE_FALL_G) {
                        if (!sg_free_fall) {
                            PR_NOTICE("[SmartCane] FREE FALL mag=%.2fg", mag);
                        }
                        sg_free_fall = true;
                        sg_buzz_end  = now + BUZZ_HOLD_MS;
                    } else {
                        sg_free_fall = false;
                    }
                } else {
                    sg_qmi_addr = 0; /* re-probe on next loop */
                }
            }
        } else if (!sg_qmi_addr && (now - last_retry_t >= 1000)) {
            last_retry_t = now;
            if (qmi_setup()) {
                PR_NOTICE("[SmartCane] QMI8658 reconnected at 0x%02X", sg_qmi_addr);
            }
        }

        /* ── Ultrasonic sample ──────────────────────────────────────────────── */
        if (now - last_ultrasonic_t >= ULTRASONIC_MS) {
            last_ultrasonic_t = now;
            sg_dist_cm  = ultrasonic_read_cm();
            sg_obstacle = (sg_dist_cm > 0.0f && sg_dist_cm <= OBSTACLE_CM);
        }

        /* ── Buzzer ─────────────────────────────────────────────────────────── */
        bool buzz = (now < sg_buzz_end) || sg_obstacle;
        if (buzz && (now - last_buzzer_t >= 130)) {
            last_buzzer_t = now;
            buzzer_tone(BUZZ_FREQ_HZ, 80);
        } else if (!buzz) {
            buzzer_off();
        }

        tal_system_sleep(10);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * user_main — called by TuyaOpen RTOS framework
 * ═════════════════════════════════════════════════════════════════════════════ */
void user_main(void)
{
    OPERATE_RET rt = OPRT_OK;
    tal_log_init(TAL_LOG_LEVEL_INFO, 1024, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("SmartCane firmware starting");
    PR_NOTICE("  WiFi:   %s", WIFI_SSID);
    PR_NOTICE("  Laptop: %s:%d", LAPTOP_IP, LAPTOP_PORT);

    /* ── Audio subsystem ──────────────────────────────────────────────────── */
    TUYA_CALL_ERR_LOG(board_register_hardware());
    if (tdl_audio_find(AUDIO_CODEC_NAME, &sg_audio_hdl) == OPRT_OK) {
        tdl_audio_open(sg_audio_hdl, NULL);
        tdl_audio_get_info(sg_audio_hdl, &sg_audio_info);
        tdl_audio_volume_set(sg_audio_hdl, 75);
        PR_NOTICE("[SmartCane] Audio ready — frame %d B @ %d Hz",
                  sg_audio_info.frame_size, sg_audio_info.sample_rate);
    } else {
        PR_WARN("[SmartCane] Audio codec not found — speaker will be silent");
    }

    /* ── KV / timer / workq (required by netmgr) ─────────────────────────── */
    tal_kv_init(&(tal_kv_cfg_t){
        .seed = "vmlkasdh93dlvlcy",
        .key  = "dflfuap134ddlduq",
    });
    /* Store Tuya device credentials so the framework can access them */
    tal_kv_set("tuya_uuid",    (const uint8_t *)TUYA_DEVICE_UUID,    strlen(TUYA_DEVICE_UUID));
    tal_kv_set("tuya_authkey", (const uint8_t *)TUYA_DEVICE_AUTHKEY, strlen(TUYA_DEVICE_AUTHKEY));
    tal_sw_timer_init();
    tal_workq_init();

    /* Subscribe to link-status changes */
    tal_event_subscribe(EVENT_LINK_STATUS_CHG, "smart_cane",
                        net_event_cb, SUBSCRIBE_TYPE_NORMAL);

#if defined(ENABLE_LIBLWIP) && (ENABLE_LIBLWIP == 1)
    TUYA_LwIP_Init();
#endif

    /* ── WiFi connect (netmgr handles reconnection automatically) ─────────── */
    netmgr_init(NETCONN_WIFI);
    netconn_wifi_info_t wifi_info = {0};
    strncpy(wifi_info.ssid, WIFI_SSID, sizeof(wifi_info.ssid) - 1);
    strncpy(wifi_info.pswd, WIFI_PASS, sizeof(wifi_info.pswd) - 1);
    netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_SSID_PSWD, &wifi_info);

    /* ── Launch sensor thread ────────────────────────────────────────────── */
    THREAD_CFG_T sensor_cfg = {
        .stackDepth = 1024 * 6,
        .priority   = THREAD_PRIO_2,
        .thrdname   = "sc_sensor",
    };
    TUYA_CALL_ERR_LOG(tal_thread_create_and_start(
        &sg_sensor_th, NULL, NULL, sensor_thread_fn, NULL, &sensor_cfg));

    /* ── Launch network/HTTP thread ──────────────────────────────────────── */
    THREAD_CFG_T net_cfg = {
        .stackDepth = 1024 * 10,
        .priority   = THREAD_PRIO_2,
        .thrdname   = "sc_net",
    };
    TUYA_CALL_ERR_LOG(tal_thread_create_and_start(
        &sg_net_th, NULL, NULL, net_thread_fn, NULL, &net_cfg));

    PR_NOTICE("[SmartCane] Init complete — waiting for WiFi...");
}

/* ═══════════════════════════════════════════════════════════════════════════════
 * RTOS entry point (non-Linux target)
 * ═════════════════════════════════════════════════════════════════════════════ */
#if OPERATING_SYSTEM != SYSTEM_LINUX

static THREAD_HANDLE ty_app_thread = NULL;

static void tuya_app_thread(void *arg)
{
    user_main();
    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T cfg = {
        .stackDepth = 1024 * 4,
        .priority   = THREAD_PRIO_1,
        .thrdname   = "tuya_app_main",
    };
    tal_thread_create_and_start(&ty_app_thread, NULL, NULL,
                                tuya_app_thread, NULL, &cfg);
}

#endif /* OPERATING_SYSTEM != SYSTEM_LINUX */
