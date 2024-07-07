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

#define DRAW_STR(x, y, font, str)   \
    u8g2_SetFont(&u8g2, font);      \
    u8g2_DrawStr(&u8g2, x, y, str); \
    u8g2_SendBuffer(&u8g2)

#define DRAW_STR_CLR(x, y, font, str) \
    u8g2_ClearBuffer(&u8g2);          \
    DRAW_STR(x, y, font, str)

#define BAR_PADDING 1

/* Private types -------------------------------------------------------------*/

// stores the state of the message scrolling on display
typedef struct {
    u8g2_uint_t width;
    u8g2_uint_t height;
    u8g2_uint_t offset;
    bool        on_an_edge; /* Track is freezed on an edge */
    TickType_t  when_edge_reached; /* Instant when track reached an edge */
} msg_info_t;

/* Private function prototypes -----------------------------------------------*/
static void setup_display();
static void display_task(void* arg);
static void initial_menu_page();
static void now_playing_page();
static void on_update_progress(time_t progress, char* clock, long* prog_bar, uint16_t max_bar_width);
// static void dispatch_command(rotary_encoder_event_t* event);

/* Locally scoped variables --------------------------------------------------*/
static QueueHandle_t encoder;
static const char*   TAG = "DISPLAY";
static u8g2_t        u8g2;

static TrackInfo track = { .artists.type = STRING_LIST };

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
static void display_task(void* args)
{
    assert(track.name = strdup("No device playing..."));
    setup_display();
    while (1) {
        initial_menu_page(&u8g2);
    }
    assert(false);
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

    u8g2_Setup_st7920_s_128x64_f(&u8g2, U8G2_R0, u8g2_esp32_spi_byte_cb,
        u8g2_esp32_gpio_and_delay_cb); // init u8g2 structure

    u8g2_InitDisplay(&u8g2); // send init sequence to the display, display is in sleep mode after this
    u8g2_ClearDisplay(&u8g2);
    u8g2_SetPowerSave(&u8g2, 0); // wake up display
}

static void initial_menu_page()
{
    return now_playing_page();
}

static void now_playing_page()
{
    spotify_dispatch_event(ENABLE_PLAYER_EVENT);
    DRAW_STR_CLR(0, 20, NOTIF_FONT, "Retrieving player state...");

    /* wait for the current track */

    SpotifyClientEvent_t event;

    u8g2_SetFont(&u8g2, TRACK_NAME_FONT);
    msg_info_t trk;
    TickType_t last_evt = xTaskGetTickCount();
    time_t     progress_ms;
    char       time[6] = { "00:00" };
    trk.height = u8g2_GetMaxCharHeight(&u8g2);

    /* Progress bar */
    long           prog_bar;
    const uint16_t max_bar_width = u8g2.width - 20;

    // in the first iteration we wait forever
    TickType_t ticks_waiting = portMAX_DELAY;
    while (1) {

        /* Wait for track event ------------------------------------------------------*/

        if (pdPASS == spotify_wait_event(&event, ticks_waiting)) {
            // just to be sure...
            if (ticks_waiting != 0 && event.type != NEW_TRACK) {
                ESP_LOGW(TAG, "Still waiting for the first event of a track");
                ESP_LOGW(TAG, "Event: %d", event.type);
                if (event.type = NO_PLAYER_ACTIVE) {
                    // TODO: get all available devices
                    continue;
                } else {
                    continue;
                }
            }
            ticks_waiting = 0;
            last_evt = xTaskGetTickCount();

            switch (event.type) {
            case NEW_TRACK:
                spotify_clear_track(&track);
                spotify_clone_track(&track, (TrackInfo*)event.payload);
                spotify_dispatch_event(DATA_PROCESSED_EVENT);
                u8g2_SetFont(&u8g2, TRACK_NAME_FONT);
                trk.width = u8g2_GetUTF8Width(&u8g2, track.name);
                trk.on_an_edge = false;
                trk.when_edge_reached = 0;
                trk.offset = 0;
                progress_ms = track.progress_ms;
                on_update_progress(progress_ms, time, &prog_bar, max_bar_width);
                break;
            case SAME_TRACK:
                TrackInfo* track_updated = event.payload;
                track.isPlaying = track_updated->isPlaying;
                track.progress_ms = track_updated->progress_ms;
                spotify_dispatch_event(DATA_PROCESSED_EVENT);
                progress_ms = track.progress_ms;
                on_update_progress(progress_ms, time, &prog_bar, max_bar_width);
                break;
            case NO_PLAYER_ACTIVE:
                // TODO: get all devices available
                break;
            default:
                spotify_dispatch_event(DATA_PROCESSED_EVENT);
                continue;
            }
        } else { /* expired */
            if (track.isPlaying) {
                progress_ms = track.progress_ms + pdTICKS_TO_MS(xTaskGetTickCount() - last_evt);
                on_update_progress(progress_ms, time, &prog_bar, max_bar_width);
            }
        }

        /* Encoder -------------------------------------------------------------------*/
        rotary_encoder_event_t queue_event;
        if (pdTRUE == xQueueReceive(encoder, &queue_event, 0)) {
            if (queue_event.event_type == BUTTON_EVENT) {
                switch (queue_event.btn_event) {
                case SHORT_PRESS:
                    SendEvent_t evt = track.isPlaying ? DO_PAUSE : DO_PLAY;
                    spotify_dispatch_event(evt);
                    break;
                case MEDIUM_PRESS:
                    /* DISABLE_PLAYER_TASK;
                    return now_playing_context_menu(); */
                    break;
                case LONG_PRESS:
                    /* DISABLE_PLAYER_TASK;
                    return initial_menu_page(); */
                    break;
                }
            } else { /* ROTARY_ENCODER_EVENT intercepted */
                SendEvent_t evt = queue_event.re_state.direction == ROTARY_ENCODER_DIRECTION_CLOCKWISE ? DO_PREVIOUS : DO_NEXT;
                spotify_dispatch_event(evt);
                /* now block the task to ignore the values the ISR is storing
                 * in the queue while the rotary encoder is still moving */
                vTaskDelay(pdMS_TO_TICKS(500));
                /* The task is active again. Reset the queue to discard
                 * the last move of the rotary encoder */
                xQueueReset(encoder);
            }
        }
        /* Display track information -------------------------------------------------*/
        u8g2_ClearBuffer(&u8g2);

        /* print Track name */
        u8g2_SetFont(&u8g2, TRACK_NAME_FONT);
        u8g2_DrawFrame(&u8g2, 0, trk.height, u8g2.width, trk.height + 4);
        u8g2_DrawUTF8(&u8g2, trk.offset + 2, 35, track.name);

        /* When the track doesn't fit on display, the offset is
         * updated on each iteration to make it scroll */
        if (trk.width > u8g2.width) {
            /* ticks on an edge */
            TickType_t ticks_on_edge = xTaskGetTickCount() - trk.when_edge_reached;
            /* Track can scroll if it exceeds the threshold of being frozen on one edge */
            if (pdMS_TO_TICKS(1000) < ticks_on_edge) { /* threshold: 1000 ms */
                if (trk.on_an_edge) { /* go to the left flank */
                    trk.on_an_edge = false;
                    trk.offset = 0;
                    trk.when_edge_reached = xTaskGetTickCount();
                } else {
                    trk.offset -= 2; // scroll by two pixels
                    if ((u8g2_uint_t)trk.offset < (u8g2_uint_t)(u8g2.width - trk.width)) { /* right flank reached */
                        trk.on_an_edge = true;
                        trk.when_edge_reached = xTaskGetTickCount();
                    }
                }
            }
        }
        /* Track artists */
        /* TODO: implement */

        /* Time progress */
        u8g2_SetFont(&u8g2, TIME_FONT);
        u8g2_DrawStr(&u8g2, 0, u8g2.height, time);

        /* Progress bar */
        u8g2_DrawFrame(&u8g2, 20, u8g2.height - 5, max_bar_width, 5);
        u8g2_DrawBox(&u8g2, 20, u8g2.height - 5, (u8g2_uint_t)prog_bar, 5);

        u8g2_SendBuffer(&u8g2);
    }
}

static inline void on_update_progress(const time_t progress, char* time, long* prog_bar, uint16_t max_bar_width)
{
    memcpy(time, u8x8_u8toa(progress / 60000, 2), 2);
    memcpy(time + 3, u8x8_u8toa((progress / 1000) % 60, 2), 2);
    float progress_percent = ((float)progress) / track.duration_ms;
    *prog_bar = progress_percent * max_bar_width;
}
