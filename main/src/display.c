/* Includes ------------------------------------------------------------------*/
#include "display.h"
#include "button.h"
#include "esp_log.h"
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

typedef struct
{
    const char*    text;
    u8g2_uint_t    offset;
    TickType_t     edge_stamp;
    TickType_t     max_ticks_on_edge;
    int8_t         pixels_shift; /* pixels on each iteration, must be greater than zero on initialization*/
    const uint8_t* font;
    u8g2_uint_t    x0; /* left corner */
    u8g2_uint_t    x1; /* right corner */
    u8g2_uint_t    y1; /* lower edge */
    u8g2_uint_t    t_width;
    bool           init;
} ScrollData_t;

// stores the state of the message scrolling on display

/* Private function prototypes -----------------------------------------------*/
static void setup_display();
static void display_task(void* arg);
static void initial_menu_page();
static void now_playing_page();
static void on_update_progress(time_t duration, time_t progress, char* clock, long* t_prog_bar, uint16_t t_prog_width);
static void scroll_text(ScrollData_t* scroll_d);
static void print_uptime();
// static void dispatch_command(rotary_encoder_event_t* event);

/* Locally scoped variables --------------------------------------------------*/
static QueueHandle_t encoder;
static const char*   TAG = "DISPLAY";
static u8g2_t        u8g2;

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
    u8g2_esp32_hal.bus.spi.clock_speed_hz = 500000;

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
    static TrackInfo track = { .artists.type = STRING_LIST };
    assert(track.name = strdup("No device playing..."));

    u8g2_SetFont(&u8g2, TRACK_NAME_FONT);
    const u8g2_uint_t t_height = u8g2_GetMaxCharHeight(&u8g2);
    ScrollData_t      s_d = {
             .font = TRACK_NAME_FONT,
             .max_ticks_on_edge = pdMS_TO_TICKS(1500),
             .pixels_shift = 2,
             .x0 = 3,
             .x1 = u8g2.width - 3,
             .y1 = t_height + 3,
    };

    SpotifyEvent_t t_evt;
    TickType_t     t_evt_stamp = 0;
    char           t_time[6] = { "00:00" };
    long           t_prog_bar = 0;
    const uint16_t t_prog_width = u8g2.width - 20;
    TickType_t     notif = 0;
    SendEvent_t    send_evt = 0;

    spotify_dispatch_event(ENABLE_PLAYER_EVENT);

    // in the first iteration we wait forever
    TickType_t ticks_to_wait
        = portMAX_DELAY;
    while (1) {
        /* Wait for track event ------------------------------------------------------*/
        if (pdPASS == spotify_wait_event(&t_evt, ticks_to_wait)) {
            t_evt_stamp = xTaskGetTickCount();
            // just to be sure...
            if (ticks_to_wait != 0 && t_evt.type != NEW_TRACK) {
                ESP_LOGW(TAG, "Still waiting for the first event of a track");
                ESP_LOGW(TAG, "Event: %d", t_evt.type);
                if (t_evt.type == NO_PLAYER_ACTIVE) {
                    // TODO: get all available devices
                    continue;
                } else {
                    continue;
                }
            }
            ticks_to_wait = 0;

            switch (t_evt.type) {
            case NEW_TRACK:
                spotify_clear_track(&track);
                spotify_clone_track(&track, (TrackInfo*)t_evt.payload);
                spotify_dispatch_event(DATA_PROCESSED_EVENT);
                s_d.text = track.name,
                s_d.init = true;
                on_update_progress(track.duration_ms, track.progress_ms, t_time, &t_prog_bar, t_prog_width);
                break;
            case SAME_TRACK:
                TrackInfo* t_updated = t_evt.payload;
                track.isPlaying = t_updated->isPlaying;
                track.progress_ms = t_updated->progress_ms;
                spotify_dispatch_event(DATA_PROCESSED_EVENT);
                on_update_progress(track.duration_ms, track.progress_ms, t_time, &t_prog_bar, t_prog_width);
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
                time_t t_progress_ms = track.progress_ms + pdTICKS_TO_MS(xTaskGetTickCount() - t_evt_stamp);
                on_update_progress(track.duration_ms, t_progress_ms, t_time, &t_prog_bar, t_prog_width);
            }
        }

        /* Encoder -------------------------------------------------------------------*/
        rotary_encoder_event_t queue_event;
        if (pdTRUE == xQueueReceive(encoder, &queue_event, 0)) {
            notif = xTaskGetTickCount();
            if (queue_event.event_type == BUTTON_EVENT) {
                switch (queue_event.btn_event) {
                case SHORT_PRESS:
                    send_evt = track.isPlaying ? DO_PAUSE_EVENT : DO_PLAY_EVENT;
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
                ESP_LOGI(TAG, "Encoder direction: %d", queue_event.re_state.direction);
                ESP_LOGI(TAG, "Encoder position: %li", queue_event.re_state.position);
                send_evt = queue_event.re_state.direction == ROTARY_ENCODER_DIRECTION_CLOCKWISE ? DO_PREVIOUS_EVENT : DO_NEXT_EVENT;
            }
            spotify_dispatch_event(send_evt);
        }
        /* Display track information -------------------------------------------------*/
        u8g2_ClearBuffer(&u8g2);

        /* print Track name */
        u8g2_DrawFrame(&u8g2, 0, s_d.y1 - t_height - 3, u8g2.width, t_height + 4);
        scroll_text(&s_d);

        /* TODO: Track artists */

        /* Time progress */
        u8g2_SetFont(&u8g2, TIME_FONT);
        u8g2_DrawStr(&u8g2, 0, u8g2.height, t_time);

        /* Notifications */
        if ((xTaskGetTickCount() - notif) < pdMS_TO_TICKS(1500)) {
            switch (send_evt) {
            case DO_PLAY_EVENT:
                u8g2_DrawStr(&u8g2, 0, 45, "RESUME");
                break;
            case DO_PAUSE_EVENT:
                u8g2_DrawStr(&u8g2, 0, 45, "PAUSE");
                break;
            case DO_PREVIOUS_EVENT:
                u8g2_DrawStr(&u8g2, 0, 45, "PREVIOUS");
                break;
            case DO_NEXT_EVENT:
                u8g2_DrawStr(&u8g2, 0, 45, "NEXT");
                break;
            default:
                break;
            }
        }

        /* Uptime */
        print_uptime();

        /* Progress bar */
        u8g2_DrawFrame(&u8g2, 20, u8g2.height - 5, t_prog_width, 5);
        u8g2_DrawBox(&u8g2, 20, u8g2.height - 5, (u8g2_uint_t)t_prog_bar, 5);

        u8g2_SendBuffer(&u8g2);
    }
}

static inline void on_update_progress(time_t duration_ms, time_t progress, char* time, long* t_prog_bar, uint16_t t_prog_width)
{
    memcpy(time, u8x8_u8toa(progress / 60000, 2), 2);
    memcpy(time + 3, u8x8_u8toa((progress / 1000) % 60, 2), 2);
    float progress_percent = ((float)progress) / duration_ms;
    *t_prog_bar = progress_percent * t_prog_width;
}

static void scroll_text(ScrollData_t* s_d)
{
    u8g2_SetFont(&u8g2, s_d->font);
    if (s_d->init) {
        s_d->t_width = u8g2_GetUTF8Width(&u8g2, s_d->text);
        s_d->offset = 0;
        s_d->edge_stamp = 0;
        s_d->init = false;
        if (s_d->pixels_shift < 0) {
            s_d->pixels_shift = -s_d->pixels_shift;
        }
    }
    u8g2_SetClipWindow(&u8g2, s_d->x0, 0, s_d->x1, s_d->y1);
    u8g2_DrawUTF8(&u8g2, s_d->x0 + s_d->offset, s_d->y1 - 4, s_d->text);
    if (s_d->t_width > s_d->x1 - s_d->x0) {
        if (!s_d->edge_stamp) {
            s_d->offset -= s_d->pixels_shift;
            if ((u8g2_uint_t)s_d->offset < (u8g2_uint_t)(s_d->x1 - s_d->x0 - s_d->t_width)) {
                s_d->edge_stamp = xTaskGetTickCount();
                s_d->pixels_shift = -s_d->pixels_shift;
            }
        } else {
            TickType_t ticks_on_edge = xTaskGetTickCount() - s_d->edge_stamp;
            if (s_d->max_ticks_on_edge < ticks_on_edge) {
                s_d->edge_stamp = 0;
            }
        }
    }
    u8g2_SetMaxClipWindow(&u8g2);
}

static inline void print_uptime()
{
    char     buffer[21] = { "Uptime: 000h 00m 00s" };
    uint32_t uptime_ms = pdTICKS_TO_MS(xTaskGetTickCount());

    memcpy(buffer + 8, u8x8_u8toa(uptime_ms / (60 * 60 * 1000), 3), 3);
    memcpy(buffer + 13, u8x8_u8toa(uptime_ms / (60 * 1000), 2), 2);
    memcpy(buffer + 17, u8x8_u8toa((uptime_ms / 1000) % 60, 2), 2);

    u8g2_SetFont(&u8g2, TIME_FONT);
    u8g2_DrawStr(&u8g2, 0, 35, buffer);
}