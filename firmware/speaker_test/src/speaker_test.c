/**
 * @file speaker_test.c
 * @brief SmartCane hardware bring-up: play a 2-second tone on the speaker.
 *
 * This is a verification app (not a product feature). On boot it:
 *   1. registers the board hardware,
 *   2. opens the audio codec (the speaker path),
 *   3. synthesizes a 1 kHz square-wave tone as 16-bit mono PCM, and
 *   4. plays it for 2 seconds via tdl_audio_play().
 *
 * If you hear a clear ~2 s beep right after flashing/reset, the speaker and
 * the audio DAC path are working.
 *
 * @copyright Copyright (c) 2025 SmartCane / HackStorm-2.0
 */
#include "tuya_cloud_types.h"
#include "tal_api.h"
#include "tkl_output.h"

#include "board_com_api.h"      /* board_register_hardware(), AUDIO_CODEC_NAME */
#include "tdl_audio_manage.h"   /* tdl_audio_* driver-layer API               */

/***********************************************************
************************macro define************************
***********************************************************/
#define TONE_FREQ_HZ     1000   /* pitch of the test tone            */
#define TONE_DURATION_MS 2000   /* how long to play it (2 seconds)   */
#define TONE_AMPLITUDE   6000   /* 16-bit sample amplitude (~ -14 dBFS) */
#define TONE_VOLUME      80     /* codec volume 0..100               */

/***********************************************************
***********************variable define**********************
***********************************************************/
static TDL_AUDIO_HANDLE_T s_audio = NULL;
/* One reusable PCM frame buffer. 2048 bytes covers the codec's frame size
 * (640 bytes at 16 kHz / 16-bit / mono / 20 ms) with margin. */
static int16_t s_frame[1024];

/***********************************************************
***********************function define**********************
***********************************************************/
/* The audio device requires a mic callback even if we only play. We ignore it. */
static void __mic_cb(TDL_AUDIO_FRAME_FORMAT_E type, TDL_AUDIO_STATUS_E status,
                     uint8_t *data, uint32_t len)
{
    (void)type; (void)status; (void)data; (void)len;
}

static void __play_tone(void)
{
    TDL_AUDIO_INFO_T info = {0};
    tdl_audio_get_info(s_audio, &info);

    /* Fall back to the standard T5 codec format if get_info reports zeros. */
    uint16_t frame_ms   = info.sample_tm_ms   ? info.sample_tm_ms   : 20;
    uint16_t frame_size = info.frame_size     ? info.frame_size     : 640;
    uint16_t bits       = info.sample_bits    ? info.sample_bits    : 16;
    uint16_t ch         = info.sample_ch_num  ? info.sample_ch_num  : 1;
    uint32_t rate       = info.sample_rate    ? info.sample_rate    : 16000;

    PR_NOTICE("audio info: rate=%u ch=%u bits=%u frame_ms=%u frame_size=%u",
              rate, ch, bits, frame_ms, frame_size);

    if (frame_size > sizeof(s_frame)) {
        frame_size = sizeof(s_frame);
    }
    uint32_t bytes_per_sample = (bits / 8) * (ch ? ch : 1);
    if (bytes_per_sample == 0) {
        bytes_per_sample = 2;
    }
    uint32_t samples_per_frame = frame_size / bytes_per_sample;

    /* Square wave: flip sign every half-period. half_period in samples. */
    uint32_t half_period = (rate / TONE_FREQ_HZ) / 2;
    if (half_period == 0) {
        half_period = 1;
    }

    uint32_t total_frames = TONE_DURATION_MS / frame_ms;
    uint32_t phase = 0;

    PR_NOTICE(">>> playing %d Hz tone for %d ms (%u frames of %u samples)",
              TONE_FREQ_HZ, TONE_DURATION_MS, total_frames, samples_per_frame);

    for (uint32_t f = 0; f < total_frames; f++) {
        for (uint32_t i = 0; i < samples_per_frame; i++) {
            s_frame[i] = ((phase / half_period) & 1) ? (int16_t)TONE_AMPLITUDE
                                                     : (int16_t)(-TONE_AMPLITUDE);
            phase++;
        }
        tdl_audio_play(s_audio, (uint8_t *)s_frame, samples_per_frame * bytes_per_sample);
    }

    PR_NOTICE(">>> tone finished");
}

int user_main(void)
{
    OPERATE_RET rt = OPRT_OK;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 1024, (TAL_LOG_OUTPUT_CB)tkl_log_output);

    PR_NOTICE("================ SmartCane speaker test ================");
    PR_NOTICE("Compile time:   %s", __DATE__);
    PR_NOTICE("Platform board: %s", PLATFORM_BOARD);

    /* Bring up board peripherals (this wires up the audio codec driver). */
    TUYA_CALL_ERR_LOG(board_register_hardware());

    rt = tdl_audio_find(AUDIO_CODEC_NAME, &s_audio);
    if (OPRT_OK != rt || NULL == s_audio) {
        PR_ERR("tdl_audio_find(%s) failed: %d", AUDIO_CODEC_NAME, rt);
        goto idle;
    }
    TUYA_CALL_ERR_LOG(tdl_audio_open(s_audio, __mic_cb));
    tdl_audio_volume_set(s_audio, TONE_VOLUME);

    __play_tone();

idle:
    while (1) {
        tal_system_sleep(100);
    }
}

/***********************************************************
***************** OS entry / thread wrapper ****************
***********************************************************/
#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[])
{
    user_main();
}
#else
static THREAD_HANDLE ty_app_thread = NULL;

static void tuya_app_thread(void *arg)
{
    user_main();
    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thrd_param = {0};
    thrd_param.stackDepth = 1024 * 4;
    thrd_param.priority   = THREAD_PRIO_1;
    thrd_param.thrdname   = "tuya_app_main";
    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
}
#endif
