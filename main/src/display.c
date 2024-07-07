/* Includes ------------------------------------------------------------------*/
#include "esp_log.h"

#include "button.h"
#include "display.h"
#include "rotary_encoder.h"
#include "selection_list.h"
#include "spotify_client.h"
#include "u8g2_esp32_hal.h"

/* Private macro -------------------------------------------------------------*/
#define MENU_FONT       u8g2_font_6x12_te
#define NOTIF_FONT      MENU_FONT // before: u8g2_font_tom_thumb_4x6_mr
#define TIME_FONT       u8g2_font_tom_thumb_4x6_mr
#define TRACK_NAME_FONT u8g2_font_helvB14_tr

#define DRAW_STR(x, y, font, str)     \
    u8g2_SetFont(&s_u8g2, font);      \
    u8g2_DrawStr(&s_u8g2, x, y, str); \
    u8g2_SendBuffer(&s_u8g2)

#define DRAW_STR_CLR(x, y, font, str) \
    u8g2_ClearBuffer(&s_u8g2);        \
    DRAW_STR(x, y, font, str)

#define BAR_WIDTH   3
#define BAR_PADDING 1

/* Private types -------------------------------------------------------------*/

// stores the state of the message scrolling on display
typedef struct {
    u8g2_uint_t width;
    u8g2_uint_t height;
    u8g2_uint_t offset;
    TickType_t  flank_tcount; /* The count of ticks when track reached a flank (left or right) */
    bool        on_right_flank; /* Track is freezed on the right flank */
} msg_info_t;

/* Private function prototypes -----------------------------------------------*/
static void setup_display();
static void display_task(void* arg);
static void initial_menu_page();
static void now_playing_page();
static void on_update_progress(const time_t* new_progress, char* clock, long* bar_width, uint16_t max_bar_width);
// static void dispatch_command(rotary_encoder_event_t* event);

/* Locally scoped variables --------------------------------------------------*/
static QueueHandle_t encoder;
static const char*   TAG = "DISPLAY";
static u8g2_t        s_u8g2;

static TrackInfo track = { .name = { "No device playing..." }, .artists.type = STRING_LIST };

/* Globally scoped variables definitions -------------------------------------*/
TaskHandle_t DISPLAY_TASK = NULL;

/* Exported functions --------------------------------------------------------*/
void display_init(UBaseType_t priority, QueueHandle_t encoder_queue_hlr)
{
    encoder = encoder_queue_hlr;
    int res = xTaskCreate(display_task, "display_task", 4096, NULL, priority, &DISPLAY_TASK);
    assert((res == pdPASS) && "Error creating task");
}

void send_err(const char* msg)
{
    DRAW_STR_CLR(0, 35, NOTIF_FONT, msg);
}

/* Private functions ---------------------------------------------------------*/
static void draw_volume_bars(uint8_t percent)
{
    uint8_t max_height = s_u8g2.height / 2;
    uint8_t width_percent = (percent * s_u8g2.width) / 100;

    u8g2_ClearBuffer(&s_u8g2);
    for (uint8_t x = 0; x < width_percent; x += (BAR_WIDTH + BAR_PADDING)) {
        uint8_t bar_height = (x * max_height) / s_u8g2.width;
        uint8_t y = s_u8g2.height - bar_height;
        u8g2_DrawBox(&s_u8g2, x, y, BAR_WIDTH, bar_height);
    }
    u8g2_SendBuffer(&s_u8g2);
}

static void display_task(void* args)
{
    assert(track.name = calloc(1, 1));
    setup_display();

    while (1) {
        initial_menu_page(&s_u8g2);
    }
    assert(false && "Unexpected exit of infinite task loop");
}

static void setup_display()
{
    u8g2_esp32_hal_t u8g2_esp32_hal = U8G2_ESP32_HAL_DEFAULT();
    u8g2_esp32_hal.bus.spi.clk = GPIO_NUM_14;
    u8g2_esp32_hal.bus.spi.mosi = GPIO_NUM_13;
    u8g2_esp32_hal.bus.spi.cs = GPIO_NUM_15;
    u8g2_esp32_hal.bus.spi.flags = SPI_DEVICE_POSITIVE_CS; // https://www.esp32.com/viewtopic.php?p=88613#p88613
    u8g2_esp32_hal.bus.spi.clock_speed_hz = 100000;

    u8g2_esp32_hal_init(u8g2_esp32_hal);

    u8g2_Setup_st7920_s_128x64_f(&s_u8g2, U8G2_R0, u8g2_esp32_spi_byte_cb,
        u8g2_esp32_gpio_and_delay_cb); // init u8g2 structure

    u8g2_InitDisplay(&s_u8g2); // send init sequence to the display, display is in sleep mode after this
    u8g2_ClearDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0); // wake up display
}

static void initial_menu_page()
{
    uint8_t selection = 1;

    u8g2_SetFont(&s_u8g2, MENU_FONT);

    return now_playing_page();
}

static void now_playing_page()
{
    spotify_dispatch_event(ENABLE_PLAYER_EVENT);
    DRAW_STR_CLR(0, 20, NOTIF_FONT, "Retrieving player state...");

    /* wait for the current track */

    SpotifyClientEvent_t event;

    u8g2_SetFont(&s_u8g2, TRACK_NAME_FONT);
    msg_info_t trk;
    TickType_t last_evt;
    time_t     progress_ms;
    char       time[6] = { "00:00" };
    trk.height = u8g2_GetMaxCharHeight(&s_u8g2);

    /* Progress bar */
    long           bar_width;
    const uint16_t max_bar_width = s_u8g2.width - 20;

    // la primera vez esperamos indefinidamente
    spotify_wait_event(&event, portMAX_DELAY);
    switch (event.type) {
    case NEW_TRACK:
        spotify_clear_track(&track);
        spotify_clone_track(&track, (TrackInfo*)event.payload);
        spotify_dispatch_event(DATA_PROCESSED_EVENT);
        ESP_LOGD(TAG, "New track event");
        trk.width = u8g2_GetUTF8Width(&s_u8g2, track.name);
        trk.on_right_flank = false;
        trk.flank_tcount = 0;
        trk.offset = 0;

        last_evt = xTaskGetTickCount();
        progress_ms = track.progress_ms;
        on_update_progress(&progress_ms, time, &bar_width, max_bar_width);
        break;
    case SAME_TRACK:
        spotify_dispatch_event(DATA_PROCESSED_EVENT);
        break;
    default:
        spotify_dispatch_event(DATA_PROCESSED_EVENT);
        break;
    }

    while (1) {

        /* Wait for track event ------------------------------------------------------*/

        if (pdPASS == spotify_wait_event(&event, 0)) {
            last_evt = xTaskGetTickCount();

            switch (event.type) {
            case NEW_TRACK:
                spotify_clear_track(&track);
                spotify_clone_track(&track, (TrackInfo*)event.payload);
                spotify_dispatch_event(DATA_PROCESSED_EVENT);
                ESP_LOGD(TAG, "New track event");
                u8g2_SetFont(&s_u8g2, TRACK_NAME_FONT);
                trk.width = u8g2_GetUTF8Width(&s_u8g2, track.name);
                trk.on_right_flank = false;
                trk.flank_tcount = 0;
                trk.offset = 0;

                progress_ms = track.progress_ms;
                on_update_progress(&progress_ms, time, &bar_width, max_bar_width);
                break;
            case SAME_TRACK:
                ESP_LOGD(TAG, "Same track event");
                TrackInfo* track_updated = event.payload;

                track.isPlaying = track_updated->isPlaying;
                track.progress_ms = track_updated->progress_ms;
                spotify_dispatch_event(DATA_PROCESSED_EVENT);
                progress_ms = track.progress_ms;
                on_update_progress(&progress_ms, time, &bar_width, max_bar_width);
                break;
            default:
                spotify_dispatch_event(DATA_PROCESSED_EVENT);
                continue;
            }
        } else { /* expired */
            if (track.isPlaying) {
                progress_ms = track.progress_ms + pdTICKS_TO_MS(xTaskGetTickCount() - last_evt);
                on_update_progress(&progress_ms, time, &bar_width, max_bar_width);
            }
        }

        /* Display track information -------------------------------------------------*/
        u8g2_ClearBuffer(&s_u8g2);

        /* print Track name */
        u8g2_SetFont(&s_u8g2, TRACK_NAME_FONT);
        u8g2_DrawFrame(&s_u8g2, 0, trk.height, s_u8g2.width, trk.height + 4);
        u8g2_DrawUTF8(&s_u8g2, trk.offset + 2, 35, track.name);

        /* When the track doesn't fit on display, the offset is
         * updated on each iteration to make it scroll */
        if (trk.width > s_u8g2.width) {
            /* ticks transcurred since reached a flank */
            TickType_t ticks_on_flank = xTaskGetTickCount() - trk.flank_tcount;
            /* Track can scroll if it exceeds the threshold of being frozen on one edge */
            if (pdMS_TO_TICKS(1000) < ticks_on_flank) { /* threshold: 1000 ms */
                if (trk.on_right_flank) { /* go to the left flank */
                    trk.on_right_flank = false;
                    trk.offset = 0;
                    trk.flank_tcount = xTaskGetTickCount();
                } else {
                    trk.offset -= 3; // scroll by three pixels
                    if ((u8g2_uint_t)trk.offset < (u8g2_uint_t)(s_u8g2.width - trk.width)) { /* right flank reached */
                        trk.on_right_flank = true;
                        trk.flank_tcount = xTaskGetTickCount();
                    }
                }
            }
        }
        /* Track artists */
        /* IMPLEMENT */

        /* Time progress */
        u8g2_SetFont(&s_u8g2, TIME_FONT);
        u8g2_DrawStr(&s_u8g2, 0, s_u8g2.height, time);

        /* Progress bar */
        u8g2_DrawFrame(&s_u8g2, 20, s_u8g2.height - 5, max_bar_width, 5);
        u8g2_DrawBox(&s_u8g2, 20, s_u8g2.height - 5, (u8g2_uint_t)bar_width, 5);

        u8g2_SendBuffer(&s_u8g2);
    }
}

static inline void on_update_progress(const time_t* new_progress, char* time, long* bar_width, uint16_t max_bar_width)
{
    memcpy(time, u8x8_u8toa(*new_progress / 60000, 2), 2);
    memcpy(time + 3, u8x8_u8toa((*new_progress / 1000) % 60, 2), 2);
    float progress_percent = ((float)(*new_progress)) / track.duration_ms;
    *bar_width = progress_percent * max_bar_width;
}

/* static void dispatch_command(rotary_encoder_event_t* event)
{
    PlayerCommand_t cmd;

    if (event->event_type == BUTTON_EVENT) {
        cmd = TOGGLE;
    } else {
        cmd = event->re_state.direction == ROTARY_ENCODER_DIRECTION_CLOCKWISE ? PREVIOUS : NEXT;
    }
    player_cmd(cmd); // TODO: maybe we should return de status code
} */