// Hand-written (redesigned "cover-forward" layout, no longer matches the
// original SquareLine Studio export). Solid dark background, matching
// ui_PlaylistScreen.c's palette: 0x1A1A1A bg, white/gray text, a single
// green accent (Spotify brand color) for the progress fill and play/pause
// icon.
//
// Layout (480x320 landscape), top to bottom:
//   y=14..164  cover art (150x150)
//   y=172..198 track title (1 line, ellipsis)
//   y=200..220 artist(s) (1 line, ellipsis)
//   y=236..254 elapsed / progress bar / total time
//   y=268..312 transport buttons (icon-only, no background pill)
// ui_PlaylistsBtn: top-right corner (mirrors ui_PlaylistBackBtn's top-left
// placement on ui_PlaylistScreen). ui_VolumeSlider: top-left corner,
// debounced in ui_events.c (volumeSliderChangedFn) before calling
// spotify_set_volume() (player_commands.c). ui_DeviceBtn continues the
// transport row's -70/0/+70 spacing to +140, opens ui_DeviceModal (a
// backdrop+panel child of this screen) listing spotify_available_devices()
// to transfer playback via spotify_transfer_playback() (player_commands.c)
// and device_screen.c.

#include "../ui.h"

void ui_PlayerScreen_screen_init(void)
{
    ui_PlayerScreen = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_PlayerScreen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_PlayerScreen, lv_color_hex(0x1A1A1A), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_PlayerScreen, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_CoverImage = lv_img_create(ui_PlayerScreen);
    lv_obj_set_width(ui_CoverImage, 150);
    lv_obj_set_height(ui_CoverImage, 150);
    lv_obj_set_x(ui_CoverImage, 0);
    lv_obj_set_y(ui_CoverImage, 14);
    lv_obj_set_align(ui_CoverImage, LV_ALIGN_TOP_MID);
    lv_obj_add_flag(ui_CoverImage, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ui_CoverImage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_CoverImage, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_clip_corner(ui_CoverImage, true, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Track = lv_label_create(ui_PlayerScreen);
    lv_obj_set_width(ui_Track, 420);
    lv_obj_set_height(ui_Track, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_Track, 0);
    lv_obj_set_y(ui_Track, 172);
    lv_obj_set_align(ui_Track, LV_ALIGN_TOP_MID);
    lv_label_set_long_mode(ui_Track, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(ui_Track, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Track, &lv_font_es_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_Track, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(ui_Track, "");

    ui_Artists = lv_label_create(ui_PlayerScreen);
    lv_obj_set_width(ui_Artists, 420);
    lv_obj_set_height(ui_Artists, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_Artists, 0);
    lv_obj_set_y(ui_Artists, 200);
    lv_obj_set_align(ui_Artists, LV_ALIGN_TOP_MID);
    lv_label_set_long_mode(ui_Artists, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(ui_Artists, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_Artists, lv_color_hex(0xCCCCCC), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(ui_Artists, "");

    ui_TrackElapsedLabel = lv_label_create(ui_PlayerScreen);
    lv_obj_set_width(ui_TrackElapsedLabel, 40);
    lv_obj_set_height(ui_TrackElapsedLabel, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_TrackElapsedLabel, 20);
    lv_obj_set_y(ui_TrackElapsedLabel, 236);
    lv_obj_set_align(ui_TrackElapsedLabel, LV_ALIGN_TOP_LEFT);
    lv_obj_set_style_text_color(ui_TrackElapsedLabel, lv_color_hex(0x999999), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(ui_TrackElapsedLabel, "0:00");

    ui_TrackTotalLabel = lv_label_create(ui_PlayerScreen);
    lv_obj_set_width(ui_TrackTotalLabel, 40);
    lv_obj_set_height(ui_TrackTotalLabel, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_TrackTotalLabel, -20);
    lv_obj_set_y(ui_TrackTotalLabel, 236);
    lv_obj_set_align(ui_TrackTotalLabel, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_style_text_align(ui_TrackTotalLabel, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_TrackTotalLabel, lv_color_hex(0x999999), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(ui_TrackTotalLabel, "0:00");

    // Hand-changed from lv_bar to lv_slider: was a passive progress
    // indicator, now a touch-seekable scrubber (see seekSliderChangedFn,
    // ui_events.c, and spotify_seek_to_position, player_commands.c). Range
    // is in track-position milliseconds (not percent), rescaled to each
    // track's duration on every NEW_TRACK - see player_screen.c.
    ui_ProgressBar = lv_slider_create(ui_PlayerScreen);
    lv_slider_set_orientation(ui_ProgressBar, LV_SLIDER_ORIENTATION_HORIZONTAL);
    lv_slider_set_range(ui_ProgressBar, 0, 100);
    lv_slider_set_value(ui_ProgressBar, 0, LV_ANIM_OFF);
    lv_obj_set_width(ui_ProgressBar, 340);
    lv_obj_set_height(ui_ProgressBar, 6);
    lv_obj_set_x(ui_ProgressBar, 0);
    lv_obj_set_y(ui_ProgressBar, 242);
    lv_obj_set_align(ui_ProgressBar, LV_ALIGN_TOP_MID);
    lv_obj_set_style_radius(ui_ProgressBar, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_ProgressBar, lv_color_hex(0x333333), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_ProgressBar, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_ProgressBar, 3, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_ProgressBar, lv_color_hex(0x1DB954), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_ProgressBar, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_ProgressBar, lv_color_hex(0x1DB954), LV_PART_KNOB | LV_STATE_DEFAULT);
    // Small explicit knob pad (not the ~6px theme default, see the volume
    // slider's y=20 fix note) - here it's the x-extremes that matter: with
    // 70px of horizontal margin on each side (screen is 480 wide, bar is
    // 340) even the theme default would've been safe, but staying explicit
    // keeps both sliders visually consistent.
    lv_obj_set_style_pad_all(ui_ProgressBar, 3, LV_PART_KNOB | LV_STATE_DEFAULT);

    ui_PrevBtn = lv_btn_create(ui_PlayerScreen);
    lv_obj_set_width(ui_PrevBtn, 44);
    lv_obj_set_height(ui_PrevBtn, 44);
    lv_obj_set_x(ui_PrevBtn, -70);
    lv_obj_set_y(ui_PrevBtn, 268);
    lv_obj_set_align(ui_PrevBtn, LV_ALIGN_TOP_MID);
    lv_obj_clear_flag(ui_PrevBtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ui_PrevBtn, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_PrevBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_PrevBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * prev_icon = lv_label_create(ui_PrevBtn);
    lv_label_set_text(prev_icon, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(prev_icon, &lv_font_es_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(prev_icon, lv_color_hex(0xE0E0E0), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(prev_icon);

    ui_PauseUnpauseBtn = lv_btn_create(ui_PlayerScreen);
    lv_obj_set_width(ui_PauseUnpauseBtn, 44);
    lv_obj_set_height(ui_PauseUnpauseBtn, 44);
    lv_obj_set_x(ui_PauseUnpauseBtn, 0);
    lv_obj_set_y(ui_PauseUnpauseBtn, 268);
    lv_obj_set_align(ui_PauseUnpauseBtn, LV_ALIGN_TOP_MID);
    lv_obj_clear_flag(ui_PauseUnpauseBtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ui_PauseUnpauseBtn, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_PauseUnpauseBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_PauseUnpauseBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_PauseUnpauseIcon = lv_label_create(ui_PauseUnpauseBtn);
    lv_label_set_text(ui_PauseUnpauseIcon, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(ui_PauseUnpauseIcon, &lv_font_es_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_PauseUnpauseIcon, lv_color_hex(0x1DB954), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(ui_PauseUnpauseIcon);

    ui_NextBtn = lv_btn_create(ui_PlayerScreen);
    lv_obj_set_width(ui_NextBtn, 44);
    lv_obj_set_height(ui_NextBtn, 44);
    lv_obj_set_x(ui_NextBtn, 70);
    lv_obj_set_y(ui_NextBtn, 268);
    lv_obj_set_align(ui_NextBtn, LV_ALIGN_TOP_MID);
    lv_obj_clear_flag(ui_NextBtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ui_NextBtn, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_NextBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_NextBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * next_icon = lv_label_create(ui_NextBtn);
    lv_label_set_text(next_icon, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(next_icon, &lv_font_es_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(next_icon, lv_color_hex(0xE0E0E0), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(next_icon);

    // Hand-added: opens the device picker modal. Same row as the transport
    // buttons, continuing their -70/0/+70 spacing to +140 so the gap looks
    // consistent, rather than a new corner (screen real estate is tight).
    ui_DeviceBtn = lv_btn_create(ui_PlayerScreen);
    lv_obj_set_width(ui_DeviceBtn, 44);
    lv_obj_set_height(ui_DeviceBtn, 44);
    lv_obj_set_x(ui_DeviceBtn, 140);
    lv_obj_set_y(ui_DeviceBtn, 268);
    lv_obj_set_align(ui_DeviceBtn, LV_ALIGN_TOP_MID);
    lv_obj_clear_flag(ui_DeviceBtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ui_DeviceBtn, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_DeviceBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_DeviceBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * device_icon = lv_label_create(ui_DeviceBtn);
    lv_label_set_text(device_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(device_icon, &lv_font_es_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(device_icon, lv_color_hex(0xE0E0E0), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(device_icon);

    // Hand-added: opens ui_PlaylistScreen. Top-right corner mirrors
    // ui_PlaylistBackBtn's top-left placement on ui_PlaylistScreen -
    // navigational actions live in the corners, transport stays in its own
    // row, so neither clashes with the other (see ANALYSIS/plan notes on
    // the previous bottom-bar overlap bug).
    ui_PlaylistsBtn = lv_btn_create(ui_PlayerScreen);
    lv_obj_set_width(ui_PlaylistsBtn, 36);
    lv_obj_set_height(ui_PlaylistsBtn, 36);
    lv_obj_set_x(ui_PlaylistsBtn, -8);
    lv_obj_set_y(ui_PlaylistsBtn, 8);
    lv_obj_set_align(ui_PlaylistsBtn, LV_ALIGN_TOP_RIGHT);
    lv_obj_clear_flag(ui_PlaylistsBtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ui_PlaylistsBtn, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_PlaylistsBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_PlaylistsBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * playlists_icon = lv_label_create(ui_PlaylistsBtn);
    lv_label_set_text(playlists_icon, LV_SYMBOL_LIST);
    lv_obj_set_style_text_color(playlists_icon, lv_color_hex(0xAAAAAA), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(playlists_icon);

    // Hand-added: opens ui_SearchScreen. Grouped with ui_PlaylistsBtn in the
    // top-right corner (search is content-discovery, not playback) rather
    // than extending the transport row (ANALYSIS.md 3.7).
    ui_SearchBtn = lv_btn_create(ui_PlayerScreen);
    lv_obj_set_width(ui_SearchBtn, 36);
    lv_obj_set_height(ui_SearchBtn, 36);
    lv_obj_set_x(ui_SearchBtn, -8);
    lv_obj_set_y(ui_SearchBtn, 52);
    lv_obj_set_align(ui_SearchBtn, LV_ALIGN_TOP_RIGHT);
    lv_obj_clear_flag(ui_SearchBtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ui_SearchBtn, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_SearchBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ui_SearchBtn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Hand-drawn magnifying glass (circle + diagonal handle) instead of a
    // label glyph: LVGL's symbol font has no search icon (ANALYSIS.md 3.7).
    lv_obj_t * search_icon_circle = lv_obj_create(ui_SearchBtn);
    lv_obj_set_size(search_icon_circle, 12, 12);
    lv_obj_set_style_radius(search_icon_circle, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(search_icon_circle, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(search_icon_circle, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(search_icon_circle, lv_color_hex(0xAAAAAA), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(search_icon_circle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(search_icon_circle, LV_ALIGN_CENTER, -3, -3);

    static lv_point_precise_t search_icon_handle_pts[] = { { 0, 0 }, { 6, 6 } };
    lv_obj_t * search_icon_handle = lv_line_create(ui_SearchBtn);
    lv_line_set_points(search_icon_handle, search_icon_handle_pts, 2);
    lv_obj_set_style_line_width(search_icon_handle, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(search_icon_handle, lv_color_hex(0xAAAAAA), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_line_rounded(search_icon_handle, true, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(search_icon_handle, LV_ALIGN_CENTER, 4, 4);

    // Hand-added: vertical volume slider, top-left corner mirrors
    // ui_PlaylistsBtn's top-right placement, same reasoning (out of the way
    // of cover/transport). Taller than it is wide -> LVGL renders it
    // vertical (see lv_slider_set_orientation below, set explicitly rather
    // than relying on the width<height auto-detection).
    ui_VolumeSlider = lv_slider_create(ui_PlayerScreen);
    lv_slider_set_orientation(ui_VolumeSlider, LV_SLIDER_ORIENTATION_VERTICAL);
    lv_slider_set_range(ui_VolumeSlider, 0, 100);
    lv_slider_set_value(ui_VolumeSlider, 50, LV_ANIM_OFF);
    lv_obj_set_width(ui_VolumeSlider, 24);
    lv_obj_set_height(ui_VolumeSlider, 140);
    lv_obj_set_x(ui_VolumeSlider, 8);
    // y=20, not 8: the default theme's LV_PART_KNOB pad (~6px each side,
    // see lv_theme_default.c) makes the knob wider than the bar, and at
    // value=100 the knob is centered right at the track's top edge - with
    // only 8px of clearance the knob poked past y=0 (off the top of the
    // screen). Pairing a smaller explicit knob pad (below) with more
    // clearance here keeps it fully on-screen at every value.
    lv_obj_set_y(ui_VolumeSlider, 20);
    lv_obj_set_align(ui_VolumeSlider, LV_ALIGN_TOP_LEFT);
    lv_obj_set_style_bg_color(ui_VolumeSlider, lv_color_hex(0x333333), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_VolumeSlider, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_VolumeSlider, lv_color_hex(0x1DB954), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_VolumeSlider, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_VolumeSlider, lv_color_hex(0x1DB954), LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_VolumeSlider, 3, LV_PART_KNOB | LV_STATE_DEFAULT);

    // Hand-added: device picker modal. A dimmed backdrop covering the whole
    // screen (child of ui_PlayerScreen, not a separate SquareLine-style
    // screen - opening/closing it is just toggling LV_OBJ_FLAG_HIDDEN, no
    // lv_disp_load_scr) with a centered panel on top. Hidden by default;
    // shown by openDevicesFn (ui_events.c), populated/hidden again by
    // device_task (device_screen.c). Clickable, and closes on its own
    // LV_EVENT_CLICKED (ui_event_DeviceModal below) - LVGL doesn't bubble
    // events from the panel/list to the backdrop by default, so a tap
    // lands directly on ui_DeviceModal only when it's genuinely outside
    // the panel (row taps inside the list target the list/its buttons,
    // never this backdrop).
    ui_DeviceModal = lv_obj_create(ui_PlayerScreen);
    lv_obj_set_size(ui_DeviceModal, 480, 320);
    lv_obj_set_pos(ui_DeviceModal, 0, 0);
    lv_obj_clear_flag(ui_DeviceModal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui_DeviceModal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ui_DeviceModal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(ui_DeviceModal, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_DeviceModal, 150, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_DeviceModal, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_DeviceModal, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * device_panel = lv_obj_create(ui_DeviceModal);
    lv_obj_set_size(device_panel, 360, 220);
    lv_obj_center(device_panel);
    lv_obj_clear_flag(device_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(device_panel, lv_color_hex(0x1A1A1A), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(device_panel, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(device_panel, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(device_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t * device_title = lv_label_create(device_panel);
    lv_obj_set_width(device_title, LV_SIZE_CONTENT);
    lv_obj_set_height(device_title, LV_SIZE_CONTENT);
    lv_obj_set_x(device_title, 0);
    lv_obj_set_y(device_title, 10);
    lv_obj_set_align(device_title, LV_ALIGN_TOP_MID);
    lv_label_set_text(device_title, "Dispositivos");
    lv_obj_set_style_text_font(device_title, &lv_font_es_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(device_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_DeviceStatusLabel = lv_label_create(device_panel);
    lv_obj_set_width(ui_DeviceStatusLabel, 320);
    lv_obj_set_height(ui_DeviceStatusLabel, LV_SIZE_CONTENT);
    lv_obj_set_align(ui_DeviceStatusLabel, LV_ALIGN_CENTER);
    lv_label_set_long_mode(ui_DeviceStatusLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(ui_DeviceStatusLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_DeviceStatusLabel, lv_color_hex(0xCCCCCC), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(ui_DeviceStatusLabel, "");
    lv_obj_add_flag(ui_DeviceStatusLabel, LV_OBJ_FLAG_HIDDEN);

    ui_DeviceList = lv_list_create(device_panel);
    lv_obj_set_width(ui_DeviceList, 340);
    lv_obj_set_height(ui_DeviceList, 165);
    lv_obj_set_x(ui_DeviceList, 0);
    lv_obj_set_y(ui_DeviceList, -10);
    lv_obj_set_align(ui_DeviceList, LV_ALIGN_BOTTOM_MID);

    lv_obj_add_event_cb(ui_PrevBtn, ui_event_PrevBtn, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_PauseUnpauseBtn, ui_event_PauseUnpauseBtn, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_NextBtn, ui_event_NextBtn, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_ProgressBar, ui_event_ProgressBar, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_PlaylistsBtn, ui_event_PlaylistsBtn, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_VolumeSlider, ui_event_VolumeSlider, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_DeviceBtn, ui_event_DeviceBtn, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_DeviceModal, ui_event_DeviceModal, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_SearchBtn, ui_event_SearchBtn, LV_EVENT_ALL, NULL);
}
