/* Includes ------------------------------------------------------------------*/
#include "spotify_client.h"
#include "esp_log.h"
#include "handler_callbacks.h"
#include "parse_objects.h"
#include "spotify_client_priv.h"
#include <string.h>
#include <time.h>

/* Private macro -------------------------------------------------------------*/
/* WARNING: undocumented, internal Discord endpoint (Rich Presence/Spotify
 * Connect integration), reached with a raw Discord *user* token
 * (CONFIG_DISCORD_TOKEN), not an official API - can change/rate-limit
 * without notice, may violate Discord's ToS, and a leaked token grants the
 * whole Discord account, not just Spotify. Deliberate, accepted trade-off
 * (ANALYSIS.md 1.8): this token also unlocks the private "dealer" WebSocket
 * (wss://dealer.spotify.com) for real-time playback push events, which
 * Spotify's public OAuth2 flow doesn't grant (would mean polling instead).
 * Don't replace without confirming the OAuth2 replacement can still reach
 * the dealer WebSocket. */
#define ACCESS_TOKEN_URL "https://discord.com/api/v8/users/@me/connections/spotify/" CONFIG_SPOTIFY_UID "/access-token"
#define TOKEN_URL "https://accounts.spotify.com/api/token"

/* Locally scoped variables --------------------------------------------------*/
static const char *TAG = "spotify_client";

/* Exported functions --------------------------------------------------------*/
bool access_token_needs_refresh(esp_spotify_client_handle_t client)
{
    if (strlen(client->access_token.value) == BEARER_PREFIX_LEN)
    {
        return true; // "Bearer " only: never obtained
    }
    // expiresIn == 0 means the last token response didn't carry "expires_in"
    // (see parse_access_token); in that case only reactive (401) refresh applies.
    return client->access_token.expiresIn != 0 && time(NULL) >= client->access_token.expiresIn;
}

/**
 * @brief Same as get_access_token(), but assumes client->http_buf_lock is
 * already held by the caller. Used by the public API functions in
 * player_commands.c so they can retry-after-reauth atomically under a single
 * lock acquisition, instead of releasing and re-acquiring the lock around the
 * refresh (which would let another task's request interleave and corrupt the
 * shared HTTP buffer/ctx in between).
 */
esp_err_t get_access_token_locked(esp_spotify_client_handle_t client)
{
    client->http_client.http_event_cb = json_http_event_cb;
    HttpStatus_Code status_code;
    esp_err_t err = perform_http_request(client, CONFIG_DISCORD_TOKEN, "application/json", ACCESS_TOKEN_URL, HTTP_METHOD_GET, &status_code);
    if (err == ESP_OK)
    {
        if (status_code == HttpStatus_Ok)
        {
            int expires_in = 0;
            err = parse_access_token((char *)(client->http_client.user_data.buffer), client->access_token.value + BEARER_PREFIX_LEN, ACCESS_TOKEN_BUF_SIZE - BEARER_PREFIX_LEN, client->json_tokens, &expires_in);
            if (err == ESP_OK)
            {
                client->access_token.expiresIn = (expires_in > 0) ? (time(NULL) + expires_in) : 0;
                /* Never log the token itself: main.c runs this tag at
                 * ESP_LOG_DEBUG, so printing the raw Bearer token would leak a
                 * live credential to the serial console/log sink. */
                ESP_LOGD(TAG, "Access token obtained (%d chars, expires_in=%d s)", (int)strlen(&client->access_token.value[BEARER_PREFIX_LEN]), expires_in);
            }
            // else: parse_access_token() already logged the raw response;
            // access_token.value is untouched (still whatever it was before -
            // "Bearer " only if never obtained), so access_token_needs_refresh()
            // will correctly ask for another attempt next time instead of us
            // silently proceeding with a stale/empty token.
        }
        else
        {
            ESP_LOGE(TAG, "Error trying to obtain an access token. Status code: %d", status_code);
            err = ESP_FAIL;
        }
    }
    return err;
}

esp_err_t get_access_token(esp_spotify_client_handle_t client)
{
    ACQUIRE_LOCK(client->http_buf_lock);
    esp_err_t err = get_access_token_locked(client);
    RELEASE_LOCK(client->http_buf_lock);
    return err;
}
