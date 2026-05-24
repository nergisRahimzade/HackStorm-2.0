/**
 * @file smart_cane_t5ai_demo.c
 * @brief Standalone T5AI demo for judges: board display + board speaker + sensors.
 *
 * This firmware does not require the laptop app. It uses the onboard display,
 * onboard audio codec / speaker path, IMU, buzzer, and an HC-SR04 ultrasonic
 * sensor to demonstrate a fully self-contained SmartCane experience.
 */

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

#include "board_com_api.h"
#include "tdl_audio_manage.h"
#include "lvgl.h"
#include "lv_vendor.h"

#ifndef DISPLAY_NAME
#define DISPLAY_NAME "display"
#endif

/* Wiring on the back header */
#define PIN_BUZZER          47
#define PIN_VIBRATION       17      /* ERM coin vibration motor via NPN driver  */
#define PIN_ULTRASONIC_TRIG 14
#define PIN_ULTRASONIC_ECHO 15
#define PIN_I2C_SDA         20
#define PIN_I2C_SCL         21

/* QMI8658 IMU */
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

/* Thresholds */
#define FREE_FALL_G         0.65f
#define OBSTACLE_CM         80.0f
#define SAMPLE_MS           25
#define ULTRASONIC_MS       120

extern uint64_t bk_aon_rtc_get_us(void);
extern void     bk_timer_delay_us(uint32_t us);

static uint8_t            sg_qmi_addr = 0;
static volatile float     sg_magnitude = 1.0f;
static volatile float     sg_dist_cm = -1.0f;
static volatile bool      sg_free_fall = false;
static volatile bool      sg_obstacle = false;
static volatile uint32_t  sg_vib_end  = 0;   /* millis() when vibration should stop */
static THREAD_HANDLE      sg_sensor_th = NULL;
static TDL_AUDIO_HANDLE_T sg_audio_hdl = NULL;
static TDL_AUDIO_INFO_T   sg_audio_info = {0};
static bool               sg_display_ready = false;
static lv_obj_t          *sg_title_label = NULL;
static lv_obj_t          *sg_status_label = NULL;
static lv_obj_t          *sg_detail_label = NULL;
static lv_obj_t          *sg_notice_label = NULL;

static inline uint32_t millis(void)
{
    return (uint32_t)(bk_aon_rtc_get_us() / 1000ULL);
}

static void display_set_text(lv_obj_t *label, const char *text)
{
    if (!sg_display_ready || !label || !text) {
        return;
    }

    lv_vendor_disp_lock();
    lv_label_set_text(label, text);
    lv_vendor_disp_unlock();
}

static void display_set_status(const char *text)
{
    display_set_text(sg_status_label, text);
}

static void display_set_detail(const char *text)
{
    display_set_text(sg_detail_label, text);
}

static void display_set_notice(const char *text)
{
    display_set_text(sg_notice_label, text);
}

static void display_init(void)
{
    lv_vendor_init(DISPLAY_NAME);

    lv_vendor_disp_lock();
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), LV_PART_MAIN);

    sg_title_label = lv_label_create(lv_screen_active());
    lv_label_set_text(sg_title_label, "SmartCane T5AI Demo");
    lv_obj_set_style_text_color(sg_title_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(sg_title_label, LV_ALIGN_TOP_MID, 0, 16);

    sg_status_label = lv_label_create(lv_screen_active());
    lv_label_set_text(sg_status_label, "Booting...");
    lv_obj_set_style_text_color(sg_status_label, lv_palette_main(LV_PALETTE_GREEN), LV_PART_MAIN);
    lv_obj_align(sg_status_label, LV_ALIGN_TOP_LEFT, 14, 58);

    sg_detail_label = lv_label_create(lv_screen_active());
    lv_obj_set_width(sg_detail_label, 292);
    lv_label_set_long_mode(sg_detail_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(sg_detail_label, "Distance: -- cm\nAccel: 1.00 g");
    lv_obj_set_style_text_color(sg_detail_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(sg_detail_label, LV_ALIGN_TOP_LEFT, 14, 100);

    sg_notice_label = lv_label_create(lv_screen_active());
    lv_obj_set_width(sg_notice_label, 292);
    lv_label_set_long_mode(sg_notice_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(sg_notice_label, "Standalone mode. No laptop required.");
    lv_obj_set_style_text_color(sg_notice_label, lv_palette_main(LV_PALETTE_ORANGE), LV_PART_MAIN);
    lv_obj_align(sg_notice_label, LV_ALIGN_TOP_LEFT, 14, 180);

    lv_vendor_disp_unlock();
    lv_vendor_start(5, 1024 * 8);
    sg_display_ready = true;
}

static void buzzer_off(void)
{
    tkl_gpio_write(PIN_BUZZER, TUYA_GPIO_LEVEL_LOW);
}

static void buzzer_tone(uint32_t freq_hz, uint32_t dur_ms)
{
    if (freq_hz == 0 || dur_ms == 0) {
        return;
    }

    uint32_t half_us = 1000000UL / (freq_hz * 2UL);
    uint32_t cycles = (dur_ms * 1000UL) / (half_us * 2UL);
    for (uint32_t i = 0; i < cycles; ++i) {
        tkl_gpio_write(PIN_BUZZER, TUYA_GPIO_LEVEL_HIGH);
        bk_timer_delay_us(half_us);
        tkl_gpio_write(PIN_BUZZER, TUYA_GPIO_LEVEL_LOW);
        bk_timer_delay_us(half_us);
    }
    buzzer_off();
}

static void vibration_on(void)
{
    tkl_gpio_write(PIN_VIBRATION, TUYA_GPIO_LEVEL_HIGH);
}

static void vibration_off(void)
{
    tkl_gpio_write(PIN_VIBRATION, TUYA_GPIO_LEVEL_LOW);
}

static void audio_play_pcm(const uint8_t *pcm, uint32_t len)
{
    if (!sg_audio_hdl || sg_audio_info.frame_size == 0 || !pcm || len == 0) {
        return;
    }

    const uint8_t *cursor = pcm;
    uint32_t remaining = len;

    while (remaining > 0) {
        uint32_t chunk = (remaining < sg_audio_info.frame_size) ? remaining : sg_audio_info.frame_size;
        tdl_audio_play(sg_audio_hdl, (uint8_t *)cursor, chunk);
        cursor += chunk;
        remaining -= chunk;
    }
}

static void speaker_tone(uint32_t freq_hz, uint32_t dur_ms)
{
    if (!sg_audio_hdl || freq_hz == 0 || dur_ms == 0) {
        return;
    }

    uint32_t sample_rate = sg_audio_info.sample_rate ? (uint32_t)sg_audio_info.sample_rate : 16000U;
    uint32_t sample_count = (sample_rate * dur_ms) / 1000U;
    uint32_t byte_count = sample_count * sizeof(int16_t);
    int16_t *samples = (int16_t *)tkl_system_psram_malloc(byte_count);

    if (!samples) {
        PR_WARN("[Demo] speaker tone alloc failed");
        return;
    }

    for (uint32_t i = 0; i < sample_count; ++i) {
        float phase = (2.0f * 3.1415926f * (float)freq_hz * (float)i) / (float)sample_rate;
        samples[i] = (int16_t)(sinf(phase) * 11000.0f);
    }

    audio_play_pcm((const uint8_t *)samples, byte_count);
    tkl_system_psram_free(samples);
}

static OPERATE_RET imu_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return tkl_i2c_master_send(TUYA_I2C_NUM_0, sg_qmi_addr, buf, 2, TRUE);
}

static OPERATE_RET imu_read(uint8_t reg, uint8_t *out, uint16_t len)
{
    OPERATE_RET rt = tkl_i2c_master_send(TUYA_I2C_NUM_0, sg_qmi_addr, &reg, 1, FALSE);
    if (rt != OPRT_OK) {
        return rt;
    }
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

static float ultrasonic_read_cm(void)
{
    TUYA_GPIO_LEVEL_E level;
    uint64_t started;

    tkl_gpio_write(PIN_ULTRASONIC_TRIG, TUYA_GPIO_LEVEL_LOW);
    bk_timer_delay_us(2);
    tkl_gpio_write(PIN_ULTRASONIC_TRIG, TUYA_GPIO_LEVEL_HIGH);
    bk_timer_delay_us(10);
    tkl_gpio_write(PIN_ULTRASONIC_TRIG, TUYA_GPIO_LEVEL_LOW);

    started = bk_aon_rtc_get_us();
    do {
        tkl_gpio_read(PIN_ULTRASONIC_ECHO, &level);
        if (bk_aon_rtc_get_us() - started > 30000ULL) {
            return -1.0f;
        }
    } while (level == TUYA_GPIO_LEVEL_LOW);

    uint64_t pulse_start = bk_aon_rtc_get_us();
    do {
        tkl_gpio_read(PIN_ULTRASONIC_ECHO, &level);
        if (bk_aon_rtc_get_us() - pulse_start > 30000ULL) {
            return -1.0f;
        }
    } while (level == TUYA_GPIO_LEVEL_HIGH);

    float dist = (float)(bk_aon_rtc_get_us() - pulse_start) * 0.0343f / 2.0f;
    return (dist >= 2.0f && dist <= 400.0f) ? dist : -1.0f;
}

static void refresh_sensor_detail(void)
{
    char detail[96];
    if (sg_dist_cm > 0.0f) {
        snprintf(detail, sizeof(detail), "Distance: %.0f cm\nAccel: %.2f g", sg_dist_cm, sg_magnitude);
    } else {
        snprintf(detail, sizeof(detail), "Distance: -- cm\nAccel: %.2f g", sg_magnitude);
    }
    display_set_detail(detail);
}

static void sensor_thread_fn(void *arg)
{
    (void)arg;

    TUYA_GPIO_BASE_CFG_T out_cfg = {
        .mode = TUYA_GPIO_PUSH_PULL,
        .direct = TUYA_GPIO_OUTPUT,
        .level = TUYA_GPIO_LEVEL_LOW,
    };
    TUYA_GPIO_BASE_CFG_T in_cfg = {
        .mode = TUYA_GPIO_FLOATING,
        .direct = TUYA_GPIO_INPUT,
    };
    TUYA_IIC_BASE_CFG_T i2c_cfg = {
        .role = TUYA_IIC_MODE_MASTER,
        .speed = TUYA_IIC_BUS_SPEED_400K,
        .addr_width = TUYA_IIC_ADDRESS_7BIT,
    };

    tkl_gpio_init(PIN_BUZZER,          &out_cfg);
    tkl_gpio_init(PIN_VIBRATION,       &out_cfg);
    tkl_gpio_init(PIN_ULTRASONIC_TRIG, &out_cfg);
    tkl_gpio_init(PIN_ULTRASONIC_ECHO, &in_cfg);
    buzzer_off();
    vibration_off();

    tkl_io_pinmux_config(PIN_I2C_SDA, TUYA_IIC0_SDA);
    tkl_io_pinmux_config(PIN_I2C_SCL, TUYA_IIC0_SCL);
    tkl_i2c_init(TUYA_I2C_NUM_0, &i2c_cfg);

    display_set_notice("Standalone mode ready. Move an object near the sensor.");
    display_set_status("Self-test");
    speaker_tone(880, 120);
    tal_system_sleep(60);
    speaker_tone(1175, 120);
    buzzer_tone(2200, 80);

    if (qmi_setup()) {
        PR_NOTICE("[Demo] QMI8658 ready at 0x%02X", sg_qmi_addr);
    } else {
        display_set_notice("IMU not found. Distance demo still works.");
        PR_WARN("[Demo] QMI8658 not found");
    }

    bool prev_fall = false;
    bool prev_obstacle = false;
    uint32_t last_accel_t = 0;
    uint32_t last_ultra_t = 0;
    uint32_t last_detail_t = 0;
    uint32_t last_obstacle_tone_t = 0;
    uint32_t last_vib_t = 0;
    bool     vib_state  = false;

    while (1) {
        uint32_t now = millis();

        if (sg_qmi_addr && (now - last_accel_t >= SAMPLE_MS)) {
            last_accel_t = now;
            uint8_t status_reg = 0;
            if (imu_read(QMI_REG_STATUS0, &status_reg, 1) == OPRT_OK && (status_reg & 0x01)) {
                uint8_t raw[6];
                if (imu_read(QMI_REG_AX_L, raw, 6) == OPRT_OK) {
                    float ax = (int16_t)((raw[1] << 8) | raw[0]) / QMI_ACCEL_LSB_G;
                    float ay = (int16_t)((raw[3] << 8) | raw[2]) / QMI_ACCEL_LSB_G;
                    float az = (int16_t)((raw[5] << 8) | raw[4]) / QMI_ACCEL_LSB_G;
                    sg_magnitude = sqrtf(ax * ax + ay * ay + az * az);
                    sg_free_fall = (sg_magnitude < FREE_FALL_G);
                }
            }
        }

        if (now - last_ultra_t >= ULTRASONIC_MS) {
            last_ultra_t = now;
            sg_dist_cm = ultrasonic_read_cm();
            sg_obstacle = (sg_dist_cm > 0.0f && sg_dist_cm <= OBSTACLE_CM);
        }

        if (sg_free_fall && !prev_fall) {
            display_set_status("FALL DETECTED");
            display_set_notice("IMU detected a free-fall event.");
            sg_vib_end = now + 1500;
            speaker_tone(740, 180);
            tal_system_sleep(40);
            speaker_tone(440, 260);
            buzzer_tone(2300, 120);
        } else if (!sg_free_fall && prev_fall) {
            display_set_status("Recovered");
            display_set_notice("Motion stabilized.");
        }
        prev_fall = sg_free_fall;

        if (sg_obstacle) {
            if (!prev_obstacle) {
                char notice[96];
                snprintf(notice, sizeof(notice), "Obstacle %.0f cm ahead.", sg_dist_cm);
                display_set_status("Obstacle ahead");
                display_set_notice(notice);
                speaker_tone(1320, 140);
                last_obstacle_tone_t = now;
            } else if (now - last_obstacle_tone_t >= 1200) {
                speaker_tone(1320, 120);
                last_obstacle_tone_t = now;
            }
        } else if (!sg_obstacle && prev_obstacle) {
            display_set_status("Path clear");
            display_set_notice("No close obstacle detected.");
        }
        prev_obstacle = sg_obstacle;

        if (!sg_obstacle && !sg_free_fall && (now - last_detail_t >= 500)) {
            last_detail_t = now;
            display_set_status("Monitoring");
            display_set_notice("Standalone board demo running.");
        }

        /* ── Vibration motor ────────────────────────────────────────────────── */
        /* Fall → sustained vibration; obstacle → short pulse every 300 ms      */
        bool vib_fall = (now < sg_vib_end);
        if (vib_fall) {
            vibration_on();
        } else if (sg_obstacle) {
            if (now - last_vib_t >= 300) {
                last_vib_t = now;
                vib_state  = !vib_state;
                if (vib_state) vibration_on();
                else           vibration_off();
            }
        } else {
            vibration_off();
            vib_state = false;
        }

        refresh_sensor_detail();

        tal_system_sleep(10);
    }
}

void user_main(void)
{
    OPERATE_RET rt = OPRT_OK;

    tal_log_init(TAL_LOG_LEVEL_INFO, 1024, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("SmartCane T5AI standalone demo starting");

    TUYA_CALL_ERR_LOG(board_register_hardware());
    display_init();

    if (tdl_audio_find(AUDIO_CODEC_NAME, &sg_audio_hdl) == OPRT_OK) {
        TUYA_CALL_ERR_LOG(tdl_audio_open(sg_audio_hdl, NULL));
        TUYA_CALL_ERR_LOG(tdl_audio_get_info(sg_audio_hdl, &sg_audio_info));
        TUYA_CALL_ERR_LOG(tdl_audio_volume_set(sg_audio_hdl, 75));
        display_set_status("Speaker ready");
    } else {
        display_set_status("Speaker missing");
        display_set_notice("Audio codec not found. Buzzer-only fallback active.");
    }

    THREAD_CFG_T sensor_cfg = {
        .stackDepth = 1024 * 8,
        .priority = THREAD_PRIO_2,
        .thrdname = "sc_demo",
    };

    TUYA_CALL_ERR_LOG(tal_thread_create_and_start(
        &sg_sensor_th, NULL, NULL, sensor_thread_fn, NULL, &sensor_cfg));

    display_set_notice("T5AI standalone demo ready for judges.");
}

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
        .stackDepth = 1024 * 4,
        .priority = THREAD_PRIO_1,
        .thrdname = "tuya_app_main",
    };

    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &cfg);
}

#endif