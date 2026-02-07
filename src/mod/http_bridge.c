#include "modding.h"
#include "global.h"
#include "recomputils.h"
#include "recompconfig.h"
#include "recompui.h"
#include "libc64/sprintf.h"

#include <stddef.h>
#include <string.h>

RECOMP_IMPORT(".", int http_server_start(const char* bind_address, int port, const char* api_key));
RECOMP_IMPORT(".", void http_server_stop());
RECOMP_IMPORT(".", void http_server_set_snapshot(const char* json));
RECOMP_IMPORT(".", int http_server_pop_message(char* out_buf, int max_len));

#define HTTP_SNAPSHOT_BUFFER_SIZE 1024
#define HTTP_MAX_MESSAGE_SIZE 513

static int sServerEnabled = 0;
static int sPort = 6464;
static int sSnapshotRate = 5;
static int sSnapshotCounter = 0;
static int sMaxMessageLength = 200;

static char sBindAddress[64] = "127.0.0.1";
static char sApiKey[64] = "changeme";

static char sSnapshotBuffer[HTTP_SNAPSHOT_BUFFER_SIZE];
static char sMessageBuffer[HTTP_MAX_MESSAGE_SIZE];

static RecompuiContext sUiContext = RECOMPUI_NULL_CONTEXT;
static RecompuiResource sUiRoot = RECOMPUI_NULL_RESOURCE;
static RecompuiResource sUiPanel = RECOMPUI_NULL_RESOURCE;
static RecompuiResource sUiLabel = RECOMPUI_NULL_RESOURCE;
static int sUiDuration = 0;

static RecompuiColor sUiPanelColor = { 24, 24, 24, 220 };
static RecompuiColor sUiBorderColor = { 70, 70, 70, 255 };
static RecompuiColor sUiTextColor = { 240, 240, 240, 255 };

static void CopyConfigString(const char* key, char* dest, size_t dest_len, const char* fallback) {
    char* value = recomp_get_config_string(key);
    const char* source = (value != NULL) ? value : fallback;
    size_t len = strlen(source);

    if (len >= dest_len) {
        len = dest_len - 1;
    }

    memcpy(dest, source, len);
    dest[len] = '\0';

    if (value != NULL) {
        recomp_free_config_string(value);
    }
}

static void LoadConfig(void) {
    sServerEnabled = recomp_get_config_u32("server_enabled") != 0;
    sPort = (int)recomp_get_config_u32("port");
    sSnapshotRate = (int)recomp_get_config_u32("snapshot_rate");
    sMaxMessageLength = (int)recomp_get_config_u32("max_message_length");

    if (sPort < 1 || sPort > 65535) {
        sPort = 6464;
    }
    if (sSnapshotRate < 1) {
        sSnapshotRate = 1;
    }
    if (sMaxMessageLength < 1) {
        sMaxMessageLength = 1;
    }
    if (sMaxMessageLength > (HTTP_MAX_MESSAGE_SIZE - 1)) {
        sMaxMessageLength = HTTP_MAX_MESSAGE_SIZE - 1;
    }

    CopyConfigString("bind_address", sBindAddress, sizeof(sBindAddress), "127.0.0.1");
    CopyConfigString("api_key", sApiKey, sizeof(sApiKey), "changeme");
}

static void InitUi(void) {
    if (sUiContext != RECOMPUI_NULL_CONTEXT) {
        return;
    }

    sUiContext = recompui_create_context();
    recompui_open_context(sUiContext);
    recompui_set_context_captures_input(sUiContext, 0);
    recompui_set_context_captures_mouse(sUiContext, 0);

    sUiRoot = recompui_context_root(sUiContext);
    recompui_set_position(sUiRoot, POSITION_ABSOLUTE);
    recompui_set_top(sUiRoot, 0, UNIT_DP);
    recompui_set_left(sUiRoot, 0, UNIT_DP);
    recompui_set_right(sUiRoot, 0, UNIT_DP);
    recompui_set_bottom(sUiRoot, 0, UNIT_DP);
    recompui_set_display(sUiRoot, DISPLAY_FLEX);
    recompui_set_flex_direction(sUiRoot, FLEX_DIRECTION_COLUMN);
    recompui_set_justify_content(sUiRoot, JUSTIFY_CONTENT_FLEX_START);
    recompui_set_align_items(sUiRoot, ALIGN_ITEMS_CENTER);
    recompui_set_padding(sUiRoot, 24.0f, UNIT_DP);

    sUiPanel = recompui_create_element(sUiContext, sUiRoot);
    recompui_set_display(sUiPanel, DISPLAY_FLEX);
    recompui_set_padding(sUiPanel, 16.0f, UNIT_DP);
    recompui_set_border_radius(sUiPanel, 12.0f, UNIT_DP);
    recompui_set_border_width(sUiPanel, 2.0f, UNIT_DP);
    recompui_set_background_color(sUiPanel, &sUiPanelColor);
    recompui_set_border_color(sUiPanel, &sUiBorderColor);

    sUiLabel = recompui_create_label(sUiContext, sUiPanel, "", LABELSTYLE_NORMAL);
    recompui_set_color(sUiLabel, &sUiTextColor);
    recompui_set_text_align(sUiLabel, TEXT_ALIGN_CENTER);

    recompui_set_visibility(sUiPanel, VISIBILITY_HIDDEN);

    recompui_close_context(sUiContext);
    recompui_show_context(sUiContext);
}

static void ShowMessage(const char* text) {
    if (sUiContext == RECOMPUI_NULL_CONTEXT) {
        return;
    }

    recompui_open_context(sUiContext);
    recompui_set_text(sUiLabel, text);
    recompui_set_opacity(sUiPanel, 1.0f);
    recompui_set_visibility(sUiPanel, VISIBILITY_VISIBLE);
    recompui_close_context(sUiContext);

    sUiDuration = 20 * 5;
}

static void UpdateUi(void) {
    if (sUiDuration <= 0 || sUiContext == RECOMPUI_NULL_CONTEXT) {
        return;
    }

    sUiDuration--;

    if (sUiDuration == 0) {
        recompui_open_context(sUiContext);
        recompui_set_visibility(sUiPanel, VISIBILITY_HIDDEN);
        recompui_close_context(sUiContext);
        return;
    }

    if (sUiDuration < 40) {
        float alpha = (float)sUiDuration / 40.0f;
        recompui_open_context(sUiContext);
        recompui_set_opacity(sUiPanel, alpha);
        recompui_close_context(sUiContext);
    }
}

static void UpdateSnapshot(PlayState* play) {
    Player* player = GET_PLAYER(play);
    if (player == NULL) {
        return;
    }

    s16 health = gSaveContext.save.saveInfo.playerData.health;
    s16 rupees = gSaveContext.save.saveInfo.playerData.rupees;
    s16 sceneId = play->sceneId;
    s8 roomId = play->roomCtx.curRoom.num;
    s32 time = gSaveContext.save.time;
    s32 day = gSaveContext.save.day;
    s32 form = gSaveContext.save.playerForm;

    sprintf(sSnapshotBuffer,
            "{\"sceneId\":%d,\"room\":%d,\"time\":%ld,\"day\":%ld,\"playerForm\":%ld,\"health\":%d,\"rupees\":%d,"
            "\"pos\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}}",
            sceneId, roomId, (long)time, (long)day, (long)form, health, rupees,
            player->actor.world.pos.x, player->actor.world.pos.y, player->actor.world.pos.z);

    http_server_set_snapshot(sSnapshotBuffer);
}

static void PollMessageQueue(void) {
    int max_len = sMaxMessageLength + 1;
    if (max_len > HTTP_MAX_MESSAGE_SIZE) {
        max_len = HTTP_MAX_MESSAGE_SIZE;
    }

    int received = http_server_pop_message(sMessageBuffer, max_len);
    if (received > 0) {
        sMessageBuffer[received] = '\0';
        ShowMessage(sMessageBuffer);
    }
}

RECOMP_CALLBACK("*", recomp_on_init)
void HttpBridge_OnInit(void) {
    LoadConfig();
    InitUi();

    if (!sServerEnabled) {
        recomp_printf("[http-bridge] server disabled\n");
        return;
    }

    if (sApiKey[0] == '\0') {
        recomp_printf("[http-bridge] api_key is empty, server not started\n");
        return;
    }

    int result = http_server_start(sBindAddress, sPort, sApiKey);
    recomp_printf("[http-bridge] start %s:%d result=%d\n", sBindAddress, sPort, result);
}

RECOMP_CALLBACK("*", recomp_on_play_main)
void HttpBridge_OnPlayMain(PlayState* play) {
    if (play == NULL) {
        return;
    }

    UpdateUi();

    if (!sServerEnabled) {
        return;
    }

    sSnapshotCounter++;
    if (sSnapshotCounter >= sSnapshotRate) {
        sSnapshotCounter = 0;
        UpdateSnapshot(play);
    }

    PollMessageQueue();
}
