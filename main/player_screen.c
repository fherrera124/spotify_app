#include "player_screen.h"
#include "app_globals.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp_jc3248w535.h"
#include "ui/ui.h"
#include "decode_jpg.h"
#include "jpeg_decoder.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Private macro -------------------------------------------------------------*/
// How often to re-check the queue/refresh the progress display once a track
// is playing. Bounds spotify_wait_event()'s wait so this task actually
// blocks (yields the CPU) between events instead of busy-spinning - see the
// note at ticks_to_wait below.
#define PROGRESS_TICK_MS 1000
// How long to freeze the seek slider/elapsed label after release, waiting
// for a real SAME_TRACK/NEW_TRACK to confirm the seek, before resuming
// periodic extrapolation - otherwise the next PROGRESS_TICK_MS tick yanks
// the slider back to the pre-seek position (ANALYSIS.md 1.18).
#define SEEK_GRACE_MS 2000
// Max resolution `pixels`/pic_img_dsc are ever decoded/rendered at,
// regardless of the real cover size downloaded. Covers are always square
// (Album.cover_size, spotify_client.h), so one side length is enough.
// pick_jpeg_scale() picks a JPEG_IMAGE_SCALE_* that keeps decoded output
// within this bound (64/300/640px source), and lv_image_set_scale()
// stretches it back up to fill the same on-screen box either way.
#define COVER_SIZE_HALF (ALBUM_COVER_PREFERRED_SIZE / 2)
// Buffer for the raw (compressed) downloaded JPEG. Sized above a 300px
// cover's needs so the 640px fallback (ANALYSIS.md 1.10) fits too.
#define ALBUM_COVER_JPEG_BUF_SIZE (128 * 1024)

/* Locally scoped variables --------------------------------------------------*/
static const char *TAG = "PLAYER_SCREEN";

static lv_image_dsc_t pic_img_dsc = {
    .header = {
        .cf = LV_COLOR_FORMAT_RGB565_SWAPPED,
        .w = COVER_SIZE_HALF,
        .h = COVER_SIZE_HALF,
        .stride = COVER_SIZE_HALF * 2,
    },
    .data_size = COVER_SIZE_HALF * COVER_SIZE_HALF * 2,
    .data = NULL,
};

static uint16_t *pixels = NULL;

/* File-scope (was function-local to player_screen_start) so volume_task/
 * seek_task can read the last-known-good volume_percent/progress_ms to
 * resync the sliders after a failed PUT, without reaching into
 * esp_spotify_client's opaque struct (not visible outside the component). */
static TrackInfo track = {.artists.type = STRING_LIST};

/* Private function prototypes -----------------------------------------------*/
static char *join_artist_names(List *artists);
static void format_time(char *buf, size_t buf_size, int64_t ms);
static void volume_task(void *arg);
static void seek_task(void *arg);
static esp_jpeg_image_scale_t pick_jpeg_scale(int source_size, int max_size);
static void reset_cover_to_blank(void);

/* Private functions -----------------------------------------------------------*/
/* Resets pic_img_dsc/pixels back to a blank, fixed-target-resolution square -
 * used both when there's no cover to fetch at all and when fetching/decoding
 * one failed partway through. */
static void reset_cover_to_blank(void)
{
    pic_img_dsc.header.w = COVER_SIZE_HALF;
    pic_img_dsc.header.h = COVER_SIZE_HALF;
    pic_img_dsc.header.stride = COVER_SIZE_HALF * sizeof(uint16_t);
    pic_img_dsc.data_size = COVER_SIZE_HALF * COVER_SIZE_HALF * sizeof(uint16_t);
    lv_image_set_scale(ui_CoverImage, LV_SCALE_NONE);
    memset(pixels, 0, COVER_SIZE_HALF * COVER_SIZE_HALF * sizeof(uint16_t));
}
/* Picks the most detail-preserving JPEG_IMAGE_SCALE_* that still decodes to
 * <= max_size, so the fixed-capacity `pixels` buffer is never exceeded
 * (Album.cover_size, spotify_client.h; ANALYSIS.md 1.10). Spotify's sizes
 * (64/300/640) divide evenly by every ratio here; esp_jpeg_decode() still
 * fails safely (ESP_ERR_NO_MEM) via its own outbuf_size check otherwise. */
static esp_jpeg_image_scale_t pick_jpeg_scale(int source_size, int max_size)
{
    static const esp_jpeg_image_scale_t scales[] = {
        JPEG_IMAGE_SCALE_0, JPEG_IMAGE_SCALE_1_2, JPEG_IMAGE_SCALE_1_4, JPEG_IMAGE_SCALE_1_8
    };
    static const int ratios[] = { 1, 2, 4, 8 };
    for (size_t i = 0; i < sizeof(ratios) / sizeof(ratios[0]); i++)
    {
        if (source_size / ratios[i] <= max_size)
        {
            return scales[i];
        }
    }
    return JPEG_IMAGE_SCALE_1_8;
}

/* Exported functions --------------------------------------------------------*/
void player_screen_start(void)
{
    if (!pixels)
    {
        // Alocate pixel memory. Each line is an array of IMAGE_W 16-bit pixels; the `*pixels` array itself contains pointers to these lines.
        pixels = heap_caps_calloc((COVER_SIZE_HALF * COVER_SIZE_HALF), sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!pixels)
        {
            ESP_LOGE(TAG, "Failed to alloc buffer");
            return;
        }
    }

    volume_target_queue = xQueueCreate(1, sizeof(int));
    if (!volume_target_queue || xTaskCreate(volume_task, "volume_task", 8192, NULL, 5, NULL) != pdPASS)
    {
        // Non-fatal: the slider just won't actually change anything on
        // Spotify's side if this failed. Rest of the player still works.
        ESP_LOGE(TAG, "Failed to start volume_task, volume slider will have no effect");
    }

    seek_target_queue = xQueueCreate(1, sizeof(int));
    if (!seek_target_queue || xTaskCreate(seek_task, "seek_task", 8192, NULL, 5, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to start seek_task, progress slider will have no effect");
    }

    pic_img_dsc.data = (uint8_t *)pixels;
    bsp_display_lock(0);
    lv_img_set_src(ui_CoverImage, &pic_img_dsc);
    bsp_display_unlock();

    assert(track.name = strdup("No device playing..."));

    SpotifyEvent_t spotify_evt;
    TickType_t event_stamp = 0;

    // enable the player and wait for events
    player_dispatch_event(client, ENABLE_PLAYER_EVENT);

    // in the first iteration we wait forever
    TickType_t ticks_to_wait = portMAX_DELAY;
    time_t progress_ms_now = 0;
    char *artist_str = NULL;
    TickType_t seek_grace_until = 0;
    while (1)
    {
        // Set inside the NEW_TRACK/SAME_TRACK cases below: true exactly for
        // the iteration that processed a real, server-confirmed progress
        // update, as opposed to a periodic timeout recompute extrapolated
        // from the last one we saw.
        bool got_real_update = false;

        /* Wait for track event ------------------------------------------------------*/
        if (pdPASS == spotify_wait_event(client, &spotify_evt, ticks_to_wait))
        {
            // Temporary diagnostic for 1.15 (unconfirmed freeze): if this
            // stops appearing while player_task's own logs keep going, the
            // freeze is downstream of the queue, not player_task. Remove
            // once resolved.
            ESP_LOGI(TAG, "spotify_wait_event -> type=%d", spotify_evt.player_event);
            event_stamp = xTaskGetTickCount();
            // just to be sure...
            if (ticks_to_wait == portMAX_DELAY && spotify_evt.player_event != NEW_TRACK)
            {
                ESP_LOGW(TAG, "Still waiting for the first player event");
                ESP_LOGW(TAG, "Player event: %d", spotify_evt.player_event);
                if (spotify_evt.player_event == NO_PLAYER_ACTIVE)
                {
                    // TODO: get all available devices
                }
                else
                {
                }
                player_dispatch_event(client, DATA_PROCESSED_EVENT);
                continue;
            }
            // Bounded wait from here on (was `0`, a busy-spin: with no
            // timeout, spotify_wait_event()'s xQueueReceive never actually
            // blocks, so this task stays runnable forever and can starve
            // sibling tasks at the same priority - e.g. spotify_client's
            // internal player_task, which is what delivers the NEW_TRACK
            // event after selecting a playlist. A periodic bounded wait
            // still updates the progress bar/elapsed time every tick, but
            // actually yields the CPU in between.
            ticks_to_wait = pdMS_TO_TICKS(PROGRESS_TICK_MS);

            switch (spotify_evt.player_event)
            {
            case NEW_TRACK:
                got_real_update = true;
                spotify_clear_track(&track);
                spotify_clone_track(&track, (TrackInfo *)spotify_evt.payload);
                player_dispatch_event(client, DATA_PROCESSED_EVENT);
                progress_ms_now = track.progress_ms;
                if (progress_ms_now > track.duration_ms)
                    progress_ms_now = track.duration_ms;
                if (artist_str)
                {
                    free(artist_str);
                }
                artist_str = join_artist_names(&track.artists);
                char total_buf[8];
                format_time(total_buf, sizeof(total_buf), track.duration_ms);
                bsp_display_lock(0);
                lv_label_set_text(ui_Track, track.name);
                lv_label_set_text(ui_Artists, artist_str);
                lv_label_set_text(ui_TrackTotalLabel, total_buf);
                lv_label_set_text(ui_PauseUnpauseIcon, track.isPlaying ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
                // Duration is per-track, so the seek slider's range has to
                // be rescaled every time the track changes.
                lv_slider_set_range(ui_ProgressBar, 0, (int32_t)track.duration_ms);
                // Only sync from server data on NEW_TRACK (not on every
                // SAME_TRACK tick) so this doesn't fight the user's own
                // finger mid-drag; -1 means unknown, leave the slider as-is.
                if (track.device.volume_percent >= 0)
                {
                    lv_slider_set_value(ui_VolumeSlider, track.device.volume_percent, LV_ANIM_OFF);
                }
                bsp_display_unlock();
                size_t buf_size = ALBUM_COVER_JPEG_BUF_SIZE;
                uint8_t *buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                uint32_t jpg_size = 0;
                if (!buf)
                {
                    ESP_LOGE(TAG, "Failed to alloc buffer");
                }
                else if (track.album.cover_size == 0)
                {
                    // No usable cover at all - already logged by parse_track().
                }
                else
                {
                    jpg_size = fetch_album_art(client, &track, buf, buf_size);
                }
                if ((int)jpg_size <= 0)
                {
                    ESP_LOGE(TAG, "Failed to fetch album cover");
                    reset_cover_to_blank();
                }
                else
                {
                    esp_jpeg_image_scale_t scale = pick_jpeg_scale(track.album.cover_size, COVER_SIZE_HALF);
                    uint16_t decoded_w, decoded_h;
                    if (decode_image(pixels, buf, jpg_size, COVER_SIZE_HALF, COVER_SIZE_HALF, scale, &decoded_w, &decoded_h) == ESP_OK)
                    {
                        pic_img_dsc.header.w = decoded_w;
                        pic_img_dsc.header.h = decoded_h;
                        pic_img_dsc.header.stride = decoded_w * sizeof(uint16_t);
                        pic_img_dsc.data_size = (size_t)decoded_w * decoded_h * sizeof(uint16_t);
                        // Stretch the real decoded resolution back up to fill
                        // the same on-screen box (256 == 1:1, see lv_image.h).
                        lv_image_set_scale(ui_CoverImage, (uint32_t)(256 * COVER_SIZE_HALF / decoded_w));
                    }
                    else
                    {
                        reset_cover_to_blank();
                    }
                }
                if (buf)
                {
                    free(buf);
                }
                bsp_display_lock(0);
                lv_obj_invalidate(ui_CoverImage);
                bsp_display_unlock();
                break;
            case SAME_TRACK:
                got_real_update = true;
                TrackInfo *t_updated = spotify_evt.payload;
                track.isPlaying = t_updated->isPlaying;
                track.progress_ms = t_updated->progress_ms;
                player_dispatch_event(client, DATA_PROCESSED_EVENT);
                progress_ms_now = track.progress_ms;
                if (progress_ms_now > track.duration_ms)
                    progress_ms_now = track.duration_ms;
                bsp_display_lock(0);
                lv_label_set_text(ui_PauseUnpauseIcon, track.isPlaying ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
                // Only touch the slider if the volume actually changed
                // since we last knew it. When we're the ones who changed
                // it (spotify_set_volume), client->track_info->device.
                // volume_percent (what t_updated points at) was already
                // optimistically updated before this WS echo arrived, so
                // it'll match track.device.volume_percent below and this
                // is a no-op - only a genuine external change (from
                // another Spotify client) shows up as a difference here,
                // so this can't fight the user's own in-progress drag.
                if (t_updated->device.volume_percent >= 0 &&
                    t_updated->device.volume_percent != track.device.volume_percent)
                {
                    track.device.volume_percent = t_updated->device.volume_percent;
                    lv_slider_set_value(ui_VolumeSlider, track.device.volume_percent, LV_ANIM_OFF);
                }
                bsp_display_unlock();
                break;
            case NO_PLAYER_ACTIVE:
                // TODO: get all devices available
                break;
            default:
                player_dispatch_event(client, DATA_PROCESSED_EVENT);
                continue;
            }
        }
        else
        { /* expired */
            if (track.isPlaying)
            {
                progress_ms_now = track.progress_ms + pdTICKS_TO_MS(xTaskGetTickCount() - event_stamp);
                if (progress_ms_now > track.duration_ms)
                    progress_ms_now = track.duration_ms;
            }
        }

        char elapsed_buf[8];
        format_time(elapsed_buf, sizeof(elapsed_buf), progress_ms_now);

        bsp_display_lock(0);
        TickType_t now = xTaskGetTickCount();
        if (lv_obj_has_state(ui_ProgressBar, LV_STATE_PRESSED))
        {
            // Being dragged right now: leave both alone (as before), and
            // keep pushing the grace deadline out so it only starts
            // counting down once the user actually lets go.
            lv_label_set_text(ui_TrackElapsedLabel, elapsed_buf);
            seek_grace_until = now + pdMS_TO_TICKS(SEEK_GRACE_MS);
        }
        else if (!got_real_update && (int32_t)(seek_grace_until - now) > 0)
        {
            // Just released (or recently) and no confirmation arrived yet
            // this iteration: don't touch either widget - a periodic
            // recompute here would still be extrapolating from the
            // pre-seek track.progress_ms/event_stamp, snapping the display
            // back to where it was before the seek. Leave it exactly as
            // the user left it and wait: either a real SAME_TRACK/NEW_TRACK
            // confirms the seek (got_real_update, handled below regardless
            // of this window), or the grace window lapses and we resume
            // showing the truthful (pre-seek, if it failed) extrapolation.
        }
        else
        {
            lv_slider_set_value(ui_ProgressBar, progress_ms_now, LV_ANIM_OFF);
            lv_label_set_text(ui_TrackElapsedLabel, elapsed_buf);
        }
        bsp_display_unlock();
    }
}

/* Private functions ---------------------------------------------------------*/
/* Dedicated task so the (blocking) spotify_set_volume() HTTP call never
 * freezes lvgl_port's own task (which pumps lv_timer_handler() and would
 * otherwise stall rendering/input for that whole time) - same reasoning as
 * playlist_task (playlist_screen.c). Sits idle until
 * volume_commit_timer_cb() (ui_events.c) overwrites volume_target_queue
 * once the user's drag has settled. */
static void volume_task(void *arg)
{
    int target;
    for (;;)
    {
        xQueueReceive(volume_target_queue, &target, portMAX_DELAY);
        HttpStatus_Code status_code;
        esp_err_t err = spotify_set_volume(client, target, &status_code);
        // The slider already shows `target` (LVGL moved it on touch,
        // independent of whether the PUT succeeded). If it didn't, resync
        // to the last confirmed value instead of leaving the UI wrong -
        // Spotify 404s volume/seek fairly often with a briefly-inactive
        // device (ANALYSIS.md 1.17).
        if (err != ESP_OK || (status_code != HttpStatus_Ok && status_code != HTTP_STATUS_NO_CONTENT))
        {
            ESP_LOGW(TAG, "spotify_set_volume(%d) failed (err=%s, http_status=%d), resyncing slider", target, esp_err_to_name(err), status_code);
            if (track.device.volume_percent >= 0)
            {
                bsp_display_lock(0);
                lv_slider_set_value(ui_VolumeSlider, track.device.volume_percent, LV_ANIM_OFF);
                bsp_display_unlock();
            }
        }
    }
}

/* Same reasoning as volume_task above, for the seek slider: the (blocking)
 * spotify_seek_to_position() HTTP call must not run on lvgl_port's task.
 * Sits idle until seekSliderChangedFn() (ui_events.c) overwrites
 * seek_target_queue on release. */
static void seek_task(void *arg)
{
    int target_ms;
    for (;;)
    {
        xQueueReceive(seek_target_queue, &target_ms, portMAX_DELAY);
        HttpStatus_Code status_code;
        esp_err_t err = spotify_seek_to_position(client, target_ms, &status_code);
        // See volume_task's comment above - same silent-failure problem,
        // same fix.
        if (err != ESP_OK || (status_code != HttpStatus_Ok && status_code != HTTP_STATUS_NO_CONTENT))
        {
            ESP_LOGW(TAG, "spotify_seek_to_position(%d) failed (err=%s, http_status=%d), resyncing slider", target_ms, esp_err_to_name(err), status_code);
            bsp_display_lock(0);
            lv_slider_set_value(ui_ProgressBar, track.progress_ms, LV_ANIM_OFF);
            bsp_display_unlock();
        }
    }
}

static void format_time(char *buf, size_t buf_size, int64_t ms)
{
    if (ms < 0)
    {
        ms = 0;
    }
    int total_sec = (int)(ms / 1000);
    snprintf(buf, buf_size, "%d:%02d", total_sec / 60, total_sec % 60);
}

static char *join_artist_names(List *artists)
{
    if (!artists || artists->count == 0) return strdup("");

    size_t total_len = 0;
    Node *node = artists->first;
    while (node) {
        total_len += strlen((char *)node->data);
        if (node->next) total_len += 2; // ", "
        node = node->next;
    }

    // Reservamos la cadena final (+1 para el '\0')
    char *result = malloc(total_len + 1);
    if (!result) return NULL;

    result[0] = '\0';

    node = artists->first;
    while (node) {
        strcat(result, (char *)node->data);
        if (node->next)
            strcat(result, ", ");
        node = node->next;
    }

    return result;
}
