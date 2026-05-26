#include "config.h"
#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#define MIN_FRAME_TIME       1000000  // nanoseconds (1 ms)
#define TEXT_UPDATE_INTERVAL 50000000 // nanoseconds (50 ms)
#define WINDOW_WIDTH         640
#define WINDOW_HEIGHT        424
#define LOGICAL_WIDTH        WINDOW_WIDTH
#define LOGICAL_HEIGHT       WINDOW_HEIGHT
#define ORIGIN_X             (LOGICAL_WIDTH * 0.5f)
#define ORIGIN_Y             (LOGICAL_HEIGHT * 0.5f)
#define FONT_WIDTH           SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE
#define FONT_HEIGHT          FONT_WIDTH
#define FONT_LINE_HEIGHT     (FONT_HEIGHT + 2)
#define BORDER_PAD           16
#define TRACKPAD_WIDTH       296.0f
#define TRACKPAD_HEIGHT      TRACKPAD_WIDTH
#define LEFT_ORIGIN_X        (ORIGIN_X - (BORDER_PAD + TRACKPAD_WIDTH) * 0.5f)
#define LEFT_ORIGIN_Y        (ORIGIN_Y - FONT_LINE_HEIGHT * 0.5f)
#define RIGHT_ORIGIN_X       (ORIGIN_X + (BORDER_PAD + TRACKPAD_WIDTH) * 0.5f)
#define RIGHT_ORIGIN_Y       LEFT_ORIGIN_Y
#define MARKER_RADIUS        8.0f
#define NUM_MARKER_SIDES     8
#define NUM_MARKER_POINTS    (NUM_MARKER_SIDES + 1)
#define BUF_LEN              256
#define NUM_FINGERS          2
#define NUM_TRACKPADS        2
#define PRESS_CLICK_THRESH   2000
#define RELEASE_CLICK_THRESH 1400

typedef struct
{
    char buf[BUF_LEN];
    float x;
    float y;
    float x_offset;
    float y_offset;
} debug_text_t;

typedef struct
{
    bool down;
    int16_t x;
    int16_t y;
    uint16_t pressure;
    float draw_x;
    float draw_y;
} finger_t;

typedef struct
{
    SDL_Gamepad *device;
    SDL_JoystickID id;
    const char *name;
    uint16_t vid;
    uint16_t pid;
} gamepad_t;

typedef enum
{
    STATE_CONNECT,
    STATE_TEST
} state_t;

typedef struct
{
    state_t state;
    uint64_t last_time;
    gamepad_t gamepad;
    finger_t fingers[NUM_FINGERS];
    SDL_FPoint markers[NUM_FINGERS][NUM_MARKER_POINTS];
    SDL_FRect trackpads[NUM_TRACKPADS];
    double fps;
    uint64_t fps_counter;
    uint64_t fps_last_time;
    debug_text_t text;
    SDL_Window *window;
    SDL_Renderer *renderer;
} appstate_t;

typedef enum
{
    COLOR_BLACK,
    COLOR_BACKGROUND,
    COLOR_TRACKPAD,
    COLOR_DIM_TEXT,
    COLOR_TEXT,
    COLOR_MARKER,
    COLOR_MARKER_BLUE,
    NUM_DRAW_COLOR
} draw_color_t;

// https://en.wikipedia.org/wiki/Solarized
static const uint8_t draw_color[NUM_DRAW_COLOR][3] = {
    {0x00, 0x00, 0x00},
    {0x00, 0x2b, 0x36},
    {0x07, 0x36, 0x42},
    {0x58, 0x6e, 0x75},
    {0x93, 0xa1, 0xa1},
    {0xfd, 0xf6, 0xe3},
    {0x26, 0x8b, 0xd2},
};

static void SetRenderDrawColor(appstate_t *as, draw_color_t color)
{
    SDL_SetRenderDrawColor(as->renderer, draw_color[color][0],
                           draw_color[color][1], draw_color[color][2],
                           SDL_ALPHA_OPAQUE);
}

static SDL_AppResult ErrorBox(const appstate_t *as, const char *fmt, ...)
{
    va_list ap;
    static char text[512];

    va_start(ap, fmt);
    SDL_vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", text,
                             (as && as->window ? as->window : NULL));

    return SDL_APP_FAILURE;
}

static void UpdateMarkers(appstate_t *as)
{
    for (int i = 0; i < NUM_FINGERS; i++)
    {
        for (int j = 0; j < NUM_MARKER_POINTS; j++)
        {
            const float angle = 2.0f * SDL_PI_F * j / NUM_MARKER_SIDES;
            as->markers[i][j].x =
                as->fingers[i].draw_x + MARKER_RADIUS * SDL_cosf(angle);
            as->markers[i][j].y =
                as->fingers[i].draw_y + MARKER_RADIUS * SDL_sinf(angle);
        }
    }
}

static void ResetDrawPoints(appstate_t *as)
{
    as->fingers[0].draw_x = LEFT_ORIGIN_X;
    as->fingers[0].draw_y = LEFT_ORIGIN_Y;
    as->fingers[1].draw_x = RIGHT_ORIGIN_X;
    as->fingers[1].draw_y = RIGHT_ORIGIN_Y;
    UpdateMarkers(as);

    as->trackpads[0].x = LEFT_ORIGIN_X - TRACKPAD_WIDTH * 0.5f;
    as->trackpads[0].y = LEFT_ORIGIN_Y - TRACKPAD_HEIGHT * 0.5f;
    as->trackpads[0].w = TRACKPAD_WIDTH;
    as->trackpads[0].h = TRACKPAD_HEIGHT;

    as->trackpads[1].x = RIGHT_ORIGIN_X - TRACKPAD_WIDTH * 0.5f;
    as->trackpads[1].y = RIGHT_ORIGIN_Y - TRACKPAD_HEIGHT * 0.5f;
    as->trackpads[1].w = TRACKPAD_WIDTH;
    as->trackpads[1].h = TRACKPAD_HEIGHT;
}

static void ResetAppState(appstate_t *as)
{
    as->state = STATE_CONNECT;
    as->last_time = 0;
    SDL_zero(as->gamepad);
    SDL_zero(as->fingers);
    SDL_zero(as->markers);
    SDL_zero(as->trackpads);
    ResetDrawPoints(as);
    SDL_zero(as->text);
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    appstate_t *as = (appstate_t *)SDL_malloc(sizeof(*as));
    if (as)
    {
        *appstate = (void *)as;
    }
    else
    {
        return ErrorBox(as, "Memory allocation failure.");
    }

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        return ErrorBox(as, SDL_GetError());
    }

    const SDL_DisplayID display_id = SDL_GetPrimaryDisplay();
    float content_scale = SDL_GetDisplayContentScale(display_id);
    if (content_scale == 0.0f)
    {
        content_scale = 1.0f;
    }
    const int window_width = SDL_lroundf(WINDOW_WIDTH * content_scale);
    const int window_height = SDL_lroundf(WINDOW_HEIGHT * content_scale);
    as->window = SDL_CreateWindow(PROJECT_STRING, window_width, window_height,
                                  SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!as->window)
    {
        return ErrorBox(as, SDL_GetError());
    }

    as->renderer = SDL_CreateRenderer(as->window, NULL);
    if (!as->renderer)
    {
        return ErrorBox(as, SDL_GetError());
    }

    SetRenderDrawColor(as, COLOR_BLACK);
    SDL_RenderClear(as->renderer);
    SDL_RenderPresent(as->renderer);

    const int pos = (int)SDL_WINDOWPOS_CENTERED_DISPLAY(display_id);
    SDL_SetWindowPosition(as->window, pos, pos);

    SDL_SetRenderLogicalPresentation(as->renderer, LOGICAL_WIDTH,
                                     LOGICAL_HEIGHT,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);

    SDL_SetRenderVSync(as->renderer, 1);

    ResetAppState(as);

    as->fps = 0.0;
    as->fps_counter = 0;
    as->fps_last_time = SDL_GetTicksNS();

    return SDL_APP_CONTINUE;
}

static void CloseGamepad(appstate_t *as, SDL_JoystickID id)
{
    if (id)
    {
        SDL_Gamepad *device = SDL_GetGamepadFromID(id);
        if (device)
        {
            SDL_CloseGamepad(device);
        }

        if (id == as->gamepad.id)
        {
            ResetAppState(as);
        }
    }
}

static void OpenGamepad(appstate_t *as, SDL_JoystickID id)
{
    if (!id)
    {
        return;
    }

    SDL_Gamepad *device = SDL_OpenGamepad(id);
    if (!device)
    {
        return;
    }

    if (SDL_GetGamepadType(device) != SDL_GAMEPAD_TYPE_STEAM)
    {
        CloseGamepad(as, id);
        return;
    }

    int num_trackpads = SDL_GetNumGamepadTouchpads(device);
    if (num_trackpads != NUM_TRACKPADS)
    {
        CloseGamepad(as, id);
        return;
    }

    for (int i = 0; i < num_trackpads; i++)
    {
        if (SDL_GetNumGamepadTouchpadFingers(device, i) != 1)
        {
            CloseGamepad(as, id);
            return;
        }
    }

    as->gamepad.device = device;
    as->gamepad.id = id;
    as->gamepad.name = SDL_GetGamepadName(device);
    if (as->gamepad.name == NULL)
    {
        as->gamepad.name = "Unknown Controller";
    }
    as->gamepad.vid = SDL_GetGamepadVendor(device);
    as->gamepad.pid = SDL_GetGamepadProduct(device);
}

static void OpenNextGamepad(appstate_t *as)
{
    SDL_JoystickID *ids = SDL_GetGamepads(NULL);
    if (ids)
    {
        OpenGamepad(as, ids[0]);
        SDL_free(ids);
    }
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    appstate_t *as = (appstate_t *)appstate;

    switch (event->type)
    {
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!as->gamepad.device)
            {
                OpenGamepad(as, event->gdevice.which);
            }
            break;

        case SDL_EVENT_GAMEPAD_REMOVED:
            CloseGamepad(as, event->gdevice.which);
            if (!as->gamepad.device)
            {
                OpenNextGamepad(as);
            }
            break;

        case SDL_EVENT_QUIT:
            return SDL_APP_SUCCESS;

        default:
            break;
    }

    return SDL_APP_CONTINUE;
}

typedef enum
{
    H_LEFT,
    H_CENTER,
    H_RIGHT
} draw_halign_t;

typedef enum
{
    V_TOP,
    V_MIDDLE,
    V_BOTTOM
} draw_valign_t;

static void AlignTextX(appstate_t *as, draw_halign_t halign, int count)
{
    as->text.x = as->text.x_offset;
    switch (halign)
    {
        case H_LEFT:
            as->text.x += BORDER_PAD;
            break;
        case H_CENTER:
            as->text.x += (LOGICAL_WIDTH - count * FONT_WIDTH) * 0.5f;
            break;
        case H_RIGHT:
            as->text.x += LOGICAL_WIDTH - BORDER_PAD - count * FONT_WIDTH;
            break;
    }
}

static void AlignTextY(appstate_t *as, draw_valign_t valign)
{
    as->text.y = as->text.y_offset;
    switch (valign)
    {
        case V_TOP:
            as->text.y += BORDER_PAD;
            break;
        case V_MIDDLE:
            as->text.y += (LOGICAL_HEIGHT - FONT_LINE_HEIGHT) * 0.5f;
            break;
        case V_BOTTOM:
            as->text.y += LOGICAL_HEIGHT - BORDER_PAD - FONT_LINE_HEIGHT + 3.0f;
            break;
    }
}

static void DrawDebugTextFormat(appstate_t *as, draw_halign_t halign,
                                draw_valign_t valign, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int num_bytes = SDL_vsnprintf(as->text.buf, BUF_LEN, fmt, ap);
    va_end(ap);

    if (num_bytes > 0)
    {
        const int count = SDL_utf8strlen(as->text.buf);
        AlignTextX(as, halign, count);
        AlignTextY(as, valign);
        SDL_RenderDebugText(as->renderer, as->text.x, as->text.y, as->text.buf);
    }
}

static void DrawDebugText(appstate_t *as, const char *text)
{
    SetRenderDrawColor(as, COLOR_TEXT);
    DrawDebugTextFormat(as, H_CENTER, V_MIDDLE, text);
}

static void ResetTextPosition(appstate_t *as)
{
    as->text.x = 0.0f;
    as->text.y = 0.0f;
    as->text.x_offset = 0.0f;
    as->text.y_offset = 0.0f;
}

static void AddTextOffsetLines(appstate_t *as, float font_lines)
{
    as->text.y_offset += font_lines * FONT_LINE_HEIGHT;
}

static void DrawTrackpadData(appstate_t *as, uint64_t now)
{
    static int16_t x[NUM_FINGERS];
    static int16_t y[NUM_FINGERS];
    static uint16_t pressure[NUM_FINGERS];
    static uint64_t last_time;
    if (now - last_time >= TEXT_UPDATE_INTERVAL)
    {
        for (int i = 0; i < NUM_FINGERS; i++)
        {
            x[i] = as->fingers[i].x;
            y[i] = as->fingers[i].y;
            pressure[i] = as->fingers[i].pressure;
        }
        last_time = now;
    }

    const draw_halign_t halign[NUM_FINGERS] = {H_LEFT, H_RIGHT};
    for (int i = 0; i < NUM_FINGERS; i++)
    {
        ResetTextPosition(as);
        AddTextOffsetLines(as, -3.0f);
        DrawDebugTextFormat(as, halign[i], V_BOTTOM, "%5s : %6s", "Touch",
                            as->fingers[i].down ? "Yes" : "No");
        AddTextOffsetLines(as, 1.0f);
        DrawDebugTextFormat(as, halign[i], V_BOTTOM, "%5s : %6d", "X", x[i]);
        AddTextOffsetLines(as, 1.0f);
        DrawDebugTextFormat(as, halign[i], V_BOTTOM, "%5s : %6d", "Y", y[i]);
        AddTextOffsetLines(as, 1.0f);
        DrawDebugTextFormat(as, halign[i], V_BOTTOM, "%5s : %6d", "Press",
                            pressure[i]);
    }
}

static void DrawFPS(appstate_t *as)
{
    ResetTextPosition(as);
    SetRenderDrawColor(as, COLOR_DIM_TEXT);
    DrawDebugTextFormat(as, H_RIGHT, V_TOP, "%6.1f FPS", as->fps);
}

static void DrawControllerInfo(appstate_t *as)
{
    ResetTextPosition(as);
    SetRenderDrawColor(as, COLOR_TEXT);
    DrawDebugTextFormat(as, H_LEFT, V_TOP, as->gamepad.name);
    SetRenderDrawColor(as, COLOR_DIM_TEXT);
    AddTextOffsetLines(as, 1.0f);
    DrawDebugTextFormat(as, H_LEFT, V_TOP, "VID: 0x%04x", as->gamepad.vid);
    AddTextOffsetLines(as, 1.0f);
    DrawDebugTextFormat(as, H_LEFT, V_TOP, "PID: 0x%04x", as->gamepad.pid);
}

static void DrawMarkers(appstate_t *as)
{
    static draw_color_t draw_color[NUM_FINGERS] = {COLOR_MARKER, COLOR_MARKER};

    for (int i = 0; i < NUM_FINGERS; i++)
    {
        if (as->fingers[i].down)
        {
            if (as->fingers[i].pressure > PRESS_CLICK_THRESH)
            {
                draw_color[i] = COLOR_MARKER_BLUE;
            }
            else if (draw_color[i] == COLOR_MARKER_BLUE
                     && as->fingers[i].pressure < RELEASE_CLICK_THRESH)
            {
                draw_color[i] = COLOR_MARKER;
            }

            SetRenderDrawColor(as, draw_color[i]);
            SDL_RenderLines(as->renderer, (const SDL_FPoint *)&as->markers[i],
                            NUM_MARKER_POINTS);
        }
    }
}

static void DrawTrackpads(appstate_t *as)
{
    SetRenderDrawColor(as, COLOR_TRACKPAD);
    for (int i = 0; i < NUM_TRACKPADS; i++)
    {
        SDL_RenderFillRect(as->renderer, (const SDL_FRect *)&as->trackpads[i]);
    }
}

static void DrawBackground(appstate_t *as)
{
    SetRenderDrawColor(as, COLOR_BACKGROUND);
    SDL_RenderClear(as->renderer);
}

static void UpdateMarkerData(appstate_t *as)
{
    const float origin_x[NUM_FINGERS] = {LEFT_ORIGIN_X, RIGHT_ORIGIN_X};
    const float origin_y[NUM_FINGERS] = {LEFT_ORIGIN_Y, RIGHT_ORIGIN_Y};

    for (int i = 0; i < NUM_FINGERS; i++)
    {
        bool down = false;
        float x = 0.5f;
        float y = 0.5f;
        float pressure = 0.0f;

        if (SDL_GetGamepadTouchpadFinger(as->gamepad.device, i, 0, &down, &x,
                                         &y, &pressure))
        {
            // Undo SDL normalization.
            float finger_x = ((x - 0.5f) * 65536.0f);
            float finger_y = ((y - 0.5f) * 65536.0f);
            float finger_pressure = pressure * 32768.0f;

            // Clamp ranges.
            finger_x = SDL_clamp(finger_x, (float)INT16_MIN, (float)INT16_MAX);
            finger_y = SDL_clamp(finger_y, (float)INT16_MIN, (float)INT16_MAX);
            finger_pressure =
                SDL_clamp(finger_pressure, 0.0f, (float)UINT16_MAX);

            // Update data.
            as->fingers[i].down = down;
            as->fingers[i].x = (int16_t)SDL_lroundf(finger_x);
            as->fingers[i].y = (int16_t)SDL_lroundf(finger_y);
            as->fingers[i].pressure = (uint16_t)SDL_lroundf(finger_pressure);
        }
        else
        {
            // Reset temporary values, just in case.
            down = false;
            x = 0.5f;
            y = 0.5f;
            pressure = 0.0f;

            // Dummy data.
            as->fingers[i].down = false;
            as->fingers[i].x = 0;
            as->fingers[i].y = 0;
            as->fingers[i].pressure = 0;
        }

        // Update finger position for drawing markers.
        as->fingers[i].draw_x = origin_x[i] + (x - 0.5f) * TRACKPAD_WIDTH;
        as->fingers[i].draw_y = origin_y[i] + (y - 0.5f) * TRACKPAD_HEIGHT;
    }
}

static void UpdateFPS(appstate_t *as, uint64_t now)
{
    as->fps_counter++;
    if (now - as->fps_last_time >= SDL_NS_PER_SECOND)
    {
        as->fps = (double)as->fps_counter * SDL_NS_PER_SECOND
                  / (now - as->fps_last_time);
        as->fps = SDL_clamp(as->fps, 0.0, 9999.9);
        as->fps_counter = 0;
        as->fps_last_time = now;
    }
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    const uint64_t now = SDL_GetTicksNS();
    appstate_t *as = (appstate_t *)appstate;

    if (now - as->last_time >= MIN_FRAME_TIME)
    {
        UpdateFPS(as, now);
        DrawBackground(as);

        switch (as->state)
        {
            case STATE_CONNECT:
                if (!as->gamepad.device)
                {
                    ResetTextPosition(as);
                    AddTextOffsetLines(as, -2.5f);
                    DrawDebugText(as, "Waiting for a Steam Controller...");
                    break;
                }
                as->state = STATE_TEST;
                // Fall through.

            case STATE_TEST:
                UpdateMarkerData(as);
                UpdateMarkers(as);
                DrawTrackpads(as);
                DrawMarkers(as);
                DrawControllerInfo(as);
                DrawFPS(as);
                DrawTrackpadData(as, now);
                break;
        }

        SDL_RenderPresent(as->renderer);
        as->last_time = now;
    }

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    appstate_t *as = (appstate_t *)appstate;

    if (as)
    {
        if (as->gamepad.device)
        {
            SDL_CloseGamepad(as->gamepad.device);
        }

        SDL_DestroyRenderer(as->renderer);
        SDL_DestroyWindow(as->window);
        SDL_free(as);
    }

    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
}
