/* Includes ------------------------------------------------------------------*/
#include "esp_log.h"

#include "display.h"
#include "selection_list.h"
#include "spotify_client.h"
#include "strlib.h"
#include "u8g2_esp32_hal.h"

/* Private macro -------------------------------------------------------------*/
#define MENU_FONT       u8g2_font_6x12_te
#define NOTIF_FONT      MENU_FONT // before: u8g2_font_tom_thumb_4x6_mr
#define TIME_FONT       u8g2_font_tom_thumb_4x6_mr
#define TRACK_NAME_FONT u8g2_font_helvB14_te

#define DRAW_STR(x, y, font, str)     \
    u8g2_SetFont(&s_u8g2, font);      \
    u8g2_DrawStr(&s_u8g2, x, y, str); \
    u8g2_SendBuffer(&s_u8g2)

#define DRAW_STR_CLR(x, y, font, str) \
    u8g2_ClearBuffer(&s_u8g2);        \
    DRAW_STR(x, y, font, str)

#define INIT_MSG_INFO(msg)                           \
    {                                                \
        u8g2_GetUTF8Width(&s_u8g2, msg), 0, false, 0 \
    }

#define BAR_WIDTH   3
#define BAR_PADDING 1

/* Private types -------------------------------------------------------------*/

// stores the state of the message scrolling on display
typedef struct {
    u8g2_uint_t width;
    u8g2_uint_t offset;
    TickType_t  flank_tcount; /* The count of ticks when track reached a flank (left or right) */
    bool        on_right_flank; /* Track is freezed on the right flank */
} msg_info_t;

/* Private function prototypes -----------------------------------------------*/
static void setup_display();
static void display_task(void* arg);
static void initial_menu_page();
static void system_menu_page();
static void now_playing_page();
static void now_playing_context_menu();
static void playlists_page();
static void available_devices_page();
static void change_volume_page();
static void delete_wifi_page();
static void restart_page();
static void draw_volume_bars(uint8_t percent);
static void print_message(const char* msg, uint8_t y, const uint8_t* font, uint8_t times);
static void test_large_msg();

/* Locally scoped variables --------------------------------------------------*/
static QueueHandle_t encoder;
static const char*   TAG = "DISPLAY";
static u8g2_t        s_u8g2;

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

    do {
        selection = userInterfaceSelectionList(&s_u8g2, encoder,
            "Spotify", selection,
            "Available devices\nNow playing\nMy playlists\nSystem\nTest message",
            portMAX_DELAY);
        switch (selection) {
        case 1:
            return available_devices_page();
        case 2:
            return now_playing_page();
        case 3:
            return playlists_page();
        case 4:
            return system_menu_page();
        case 5:
            return test_large_msg();
        default:
            break;
        }
    } while (1);
}

static void playlists_page()
{
    DRAW_STR_CLR(0, 20, NOTIF_FONT, "Retrieving user playlists...");

    uint8_t selection = 1;

    http_user_playlists();
    uint32_t notif;
    xTaskNotifyWait(0, ULONG_MAX, &notif, portMAX_DELAY);

    if (notif == PLAYLISTS_ERROR) {
        DRAW_STR_CLR(0, 20, NOTIF_FONT, "An error ocurred");
        vTaskDelay(pdMS_TO_TICKS(3000));
    } else if (notif == PLAYLISTS_OK) {
        if (!PLAYLISTS.items_string) {
            DRAW_STR_CLR(0, 20, NOTIF_FONT, "User doesn't have playlists");
            vTaskDelay(pdMS_TO_TICKS(3000));
            return now_playing_page();
        } else {
            u8g2_ClearBuffer(&s_u8g2);
            u8g2_SetFont(&s_u8g2, MENU_FONT);
            selection = userInterfaceSelectionList(&s_u8g2, encoder,
                "My Playlists", selection,
                PLAYLISTS.items_string,
                portMAX_DELAY);

            if (selection == 0)
                return initial_menu_page();

            StrListItem* uri = PLAYLISTS.values.first;

            for (uint16_t i = 1; i < selection; i++)
                uri = uri->next;

            ESP_LOGD(TAG, "URI selected: %s", uri->str);

            http_play_context_uri(uri->str);
            vTaskDelay(50);
            UNBLOCK_PLAYER_TASK;
        }
    }
    /* cleanup */
    if (PLAYLISTS.items_string) {
        free(PLAYLISTS.items_string);
        PLAYLISTS.items_string = NULL;
    }
    strListClear(&PLAYLISTS.values);

    return now_playing_page();
}

static void now_playing_page()
{
    ENABLE_PLAYER_TASK;
    DRAW_STR_CLR(0, 20, NOTIF_FONT, "Retrieving player state...");

    /* wait for the current track */

    uint32_t notif;
    xTaskNotifyWait(0, ULONG_MAX, &notif, portMAX_DELAY);

    if (notif == LAST_DEVICE_FAILED) {
        DISABLE_PLAYER_TASK;
        ESP_LOGD(TAG, "No device playing");
        DRAW_STR_CLR(0, 20, NOTIF_FONT, "No device playing");
        vTaskDelay(pdMS_TO_TICKS(3000));
        return available_devices_page();
    }
    // else...
    u8g2_SetFont(&s_u8g2, TRACK_NAME_FONT);
    msg_info_t trk = INIT_MSG_INFO(TRACK->name);
    TickType_t start = xTaskGetTickCount();
    time_t     progress_base = TRACK->progress_ms;
    time_t     last_progress = 0, progress_ms = 0;
    char       mins[3], secs[3];
    strcpy(mins, u8x8_u8toa(progress_base / 60000, 2));
    strcpy(secs, u8x8_u8toa((progress_base / 1000) % 60, 2));
    enum {
        paused,
        playing,
        toBePaused,
        toBeUnpaused,
    } track_state
        = TRACK->isPlaying ? playing : paused;

    while (1) {

        /* Intercept any encoder event -----------------------------------------------*/

        rotary_encoder_event_t queue_event;
        if (pdTRUE == xQueueReceive(encoder, &queue_event, 0)) {
            if (queue_event.event_type == BUTTON_EVENT) {
                switch (queue_event.btn_event) {
                case SHORT_PRESS:
                    track_state = TRACK->isPlaying ? toBePaused : toBeUnpaused;
                    player_cmd(&queue_event);
                    break;
                case MEDIUM_PRESS:
                    DISABLE_PLAYER_TASK;
                    return now_playing_context_menu();
                    break;
                case LONG_PRESS:
                    DISABLE_PLAYER_TASK;
                    return initial_menu_page();
                    break;
                }
            } else { /* ROTARY_ENCODER_EVENT intercepted */
                player_cmd(&queue_event);
                /* now block the task to ignore the values the ISR is storing
                 * in the queue while the rotary encoder is still moving */
                vTaskDelay(pdMS_TO_TICKS(500));
                /* The task is active again. Reset the queue to discard
                 * the last move of the rotary encoder */
                xQueueReset(encoder);
            }
        }

        /* Wait for track event ------------------------------------------------------*/

        if (pdPASS == xTaskNotifyWait(0, ULONG_MAX, &notif, pdMS_TO_TICKS(50))) {
            start = xTaskGetTickCount();
            progress_base = TRACK->progress_ms;

            if (notif == VOLUME_CHANGED) {
                ESP_LOGD(TAG, "Volume changed");
                uint8_t percent = atoi(TRACK->device.volume_percent);
                draw_volume_bars(percent);
                vTaskDelay(pdMS_TO_TICKS(2000));
            }

            if (notif == SAME_TRACK) {
                ESP_LOGD(TAG, "Same track event");
            } else if (notif == NEW_TRACK) {
                ESP_LOGD(TAG, "New track event");
                last_progress = trk.offset = 0;
                u8g2_SetFont(&s_u8g2, TRACK_NAME_FONT);
                trk.width = u8g2_GetUTF8Width(&s_u8g2, TRACK->name);
            } else if (notif == LAST_DEVICE_FAILED) {
                DISABLE_PLAYER_TASK;
                ESP_LOGW(TAG, "Last device failed");
                DRAW_STR_CLR(0, 20, NOTIF_FONT, "Device disconected...");
                vTaskDelay(pdMS_TO_TICKS(3000));
                return available_devices_page();
            }

            track_state = TRACK->isPlaying ? playing : paused;

        } else { /* expired */
            TickType_t finish = xTaskGetTickCount();
            switch (track_state) {
            case playing:;
                time_t prg = progress_base + pdTICKS_TO_MS(finish - start);
                /* track finished, early unblock of PLAYER_TASK */
                if (prg > TRACK->duration_ms) {
                    /* only notify once */
                    if (progress_ms != TRACK->duration_ms) {
                        progress_ms = TRACK->duration_ms;
                        vTaskDelay(50);
                        ESP_LOGW(TAG, "End of track, unblock playing task");
                        UNBLOCK_PLAYER_TASK;
                    }
                } else {
                    progress_ms = prg;
                }
                break;
            case paused:
                progress_ms = progress_base;
                break;
            case toBePaused:
                track_state = paused;
                progress_base = progress_ms;
                break;
            case toBeUnpaused:
                track_state = playing;
                start = xTaskGetTickCount();
                break;
            default:
                break;
            }
            strcpy(mins, u8x8_u8toa(progress_ms / 60000, 2));
            /* if there's an increment of one second */
            if ((progress_ms / 1000) != (last_progress / 1000)) {
                last_progress = progress_ms;
                strcpy(secs, u8x8_u8toa((progress_ms / 1000) % 60, 2));
                ESP_LOGD(TAG, "Time: %s:%s", mins, secs);
            }
        }

        /* Display track information -------------------------------------------------*/

        u8g2_SetFont(&s_u8g2, TRACK_NAME_FONT);
        u8g2_ClearBuffer(&s_u8g2);

        /* print Track name */
        u8g2_DrawUTF8(&s_u8g2, trk.offset, 35, TRACK->name);

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
                    trk.offset -= 1; // scroll by one pixel
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
        u8g2_DrawStr(&s_u8g2, 0, s_u8g2.height, mins);
        u8g2_DrawStr(&s_u8g2, u8g2_GetStrWidth(&s_u8g2, mins) - 1, s_u8g2.height, ":");
        u8g2_DrawStr(&s_u8g2, u8g2_GetStrWidth(&s_u8g2, mins) + 3, s_u8g2.height, secs);

        /* Progress bar */
        const uint16_t max_bar_width = s_u8g2.width - 20;
        u8g2_DrawFrame(&s_u8g2, 20, s_u8g2.height - 5, max_bar_width, 5);
        float progress_percent = ((float)(progress_ms)) / TRACK->duration_ms;
        long  bar_width = progress_percent * max_bar_width;
        u8g2_DrawBox(&s_u8g2, 20, s_u8g2.height - 5, (u8g2_uint_t)bar_width, 5);

        u8g2_SendBuffer(&s_u8g2);
    }
}

static void now_playing_context_menu()
{
    uint8_t selection = 1;

    const char* sl = "change volume\nartist\nqueue\nBack\nMain Menu";

    u8g2_SetFont(&s_u8g2, MENU_FONT);

    do {
        selection = userInterfaceSelectionList(&s_u8g2, encoder,
            "Track options", selection,
            sl, portMAX_DELAY);
        switch (selection) {
        case 1:
            return change_volume_page();
        case 2:
            /* code */
            break;
        case 3:
            /* code */
            break;
        case 4:
            return now_playing_page();
        case 5:
            return initial_menu_page();
        default:
            break;
        }

    } while (1);
}

static void available_devices_page()
{
    DRAW_STR_CLR(0, 20, NOTIF_FONT, "Retrieving available devices...");
    uint8_t selection;
update_list:
    selection = 1;

    http_available_devices();
    uint32_t notif;
    xTaskNotifyWait(0, ULONG_MAX, &notif, portMAX_DELAY);

    if (notif == ACTIVE_DEVICES_FOUND) {
        u8g2_SetFont(&s_u8g2, MENU_FONT);
        selection = userInterfaceSelectionList(&s_u8g2, encoder,
            "Select a device", selection,
            DEVICES.items_string,
            pdMS_TO_TICKS(10000));

        if (selection == MENU_EVENT_TIMEOUT)
            goto cleanup;

        StrListItem* device = DEVICES.values.first;

        for (uint16_t i = 1; i < selection; i++) {
            device = device->next;
        }

        ESP_LOGI(TAG, "DEVICE ID: %s", device->str);

        http_set_device(device->str);
        u8g2_SetFont(&s_u8g2, NOTIF_FONT);
        xTaskNotifyWait(0, ULONG_MAX, &notif, portMAX_DELAY);
        u8g2_ClearBuffer(&s_u8g2);

        if (notif == PLAYBACK_TRANSFERRED_OK) {
            u8g2_DrawStr(&s_u8g2, 0, 20, "Playback transferred to device");
        } else if (notif == PLAYBACK_TRANSFERRED_FAIL) {
            u8g2_DrawStr(&s_u8g2, 0, 20, "Device failed");
        }
        u8g2_SendBuffer(&s_u8g2);
        vTaskDelay(pdMS_TO_TICKS(3000));

    } else if (notif == NO_ACTIVE_DEVICES) {
        DRAW_STR_CLR(0, 20, NOTIF_FONT, "No devices found :c");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

cleanup:
    if (DEVICES.items_string) {
        free(DEVICES.items_string);
        DEVICES.items_string = NULL;
    }
    strListClear(&DEVICES.values);

    if (selection == MENU_EVENT_TIMEOUT)
        goto update_list;
    return now_playing_page(); // TODO: make dynamic
}

static void change_volume_page()
{
    ENABLE_PLAYER_TASK;
    int steps = 0;
    while (1) {
        uint8_t percent = atoi(TRACK->device.volume_percent);
        draw_volume_bars(percent);
        /* Intercept any encoder event -----------------------------------------------*/
        rotary_encoder_event_t queue_event;
        if (pdTRUE == xQueueReceive(encoder, &queue_event, 0)) {
            if (queue_event.event_type == ROTARY_ENCODER_EVENT) {
                if (queue_event.re_state.direction == ROTARY_ENCODER_DIRECTION_CLOCKWISE) {
                    steps -= 3;
                } else {
                    steps += 3;
                }
            } else { /* BUTTON_EVENT intercepted */
                switch (queue_event.btn_event) {
                case SHORT_PRESS:
                    player_cmd(&queue_event);
                    break;
                case MEDIUM_PRESS:
                case LONG_PRESS:
                    return now_playing_page();
                    break;
                }
            }
        } else {
            if (steps != 0) {
                steps += percent;
                if (steps > 100) {
                    steps = 100;
                } else if (steps < 0) {
                    steps = 0;
                }
                http_update_volume(steps);
                steps = 0;
            }
        }
    }
}

static void print_message(const char* msg, uint8_t y, const uint8_t* font, uint8_t times)
{
    u8g2_SetFont(&s_u8g2, font);
    msg_info_t msg_info = INIT_MSG_INFO(msg);
    do {
        u8g2_ClearBuffer(&s_u8g2);
        u8g2_DrawUTF8(&s_u8g2, msg_info.offset, y, msg);

        /* When the message doesn't fit on display, the offset is
         * updated on each iteration to make it scroll */
        if (msg_info.width > s_u8g2.width) {
            /* ticks transcurred since reached a flank */
            TickType_t ticks_on_flank = xTaskGetTickCount() - msg_info.flank_tcount;
            /* Track can scroll if it exceeds the threshold of being frozen on one edge */
            if (pdMS_TO_TICKS(1000) < ticks_on_flank) { /* threshold: 1000 ms */
                if (msg_info.on_right_flank) { /* go to the left flank */
                    msg_info.on_right_flank = false;
                    msg_info.offset = 0;
                    msg_info.flank_tcount = xTaskGetTickCount();
                } else {
                    msg_info.offset -= 1; // scroll by one pixel
                    if ((u8g2_uint_t)msg_info.offset < (u8g2_uint_t)(s_u8g2.width - msg_info.width)) { /* right flank reached */
                        msg_info.on_right_flank = true;
                        times--;
                        msg_info.flank_tcount = xTaskGetTickCount();
                    }
                }
            }
        }
        u8g2_SendBuffer(&s_u8g2);
        vTaskDelay(pdMS_TO_TICKS(50));
    } while (times > 0);
}

static void system_menu_page()
{
    uint8_t selection = 1;

    u8g2_SetFont(&s_u8g2, MENU_FONT);

    do {
        selection = userInterfaceSelectionList(&s_u8g2, encoder,
            "System", selection,
            "Delete wifi\nRestart\nBack",
            portMAX_DELAY);
        switch (selection) {
        case 1:
            return delete_wifi_page();
        case 2:
            return restart_page();
        case 3:
            return initial_menu_page(&s_u8g2);
        default:
            break;
        }
    } while (1);
}

static void delete_wifi_page()
{
    /* TODO: add confirmation button */
    int err = wifi_config_delete();
    if (err == ESP_OK) {
        print_message("Wifi credentials successfully deleted", 35, NOTIF_FONT, 1);

    } else {
        print_message("Error deleting wifi credentials", 35, NOTIF_FONT, 1);
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    restart_page();
}

static void restart_page()
{
    /* TODO: add confirmation button */
    DRAW_STR_CLR(15, 20, NOTIF_FONT, "Restarting...");
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}

static void test_large_msg()
{
    const char* msg = "Hola gente como andan eiii, ajjajaj. Esto mira que puede ser largo";
    print_message(msg, 35, NOTIF_FONT, 1);
}