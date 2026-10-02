#include "jar_progress.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <switch.h>
#include <switch/display/parcel.h>

#include "config.h"
#include "jar_import.h"

extern void debugPrintf(const char *fmt, ...);

/*
 * libnx creates its default VI window before main() via __nx_win_init().
 * Reopening "Default" through the global vi service therefore fails with
 * VI's operation-failed result. The import overlay uses its own vi:u service
 * sessions instead, leaving the engine's default window untouched.
 */
static Service g_vi_root;
static Service g_vi_app;
static Service g_vi_relay;
static u64 g_display_id;
static u64 g_layer_id;
static NWindow g_window;
static Framebuffer g_fb;
static int g_vi_open;
static int g_display_open;
static int g_layer_open;
static int g_ready;
static uint64_t g_last_tick;

#define OVERLAY_W 1280u
#define OVERLAY_H 720u
#define UPDATE_NS 50000000ULL

/*
 * Compact 5x7 font. Each row uses the low five bits, left-to-right.
 * Character set is intentionally ASCII-only; an unsupported byte becomes '?'.
 */
static const char g_charset[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789%:/. -_?";
static const uint8_t g_font[][7] = {
    /* A */ {0x0e,0x11,0x11,0x1f,0x11,0x11,0x11},
    /* B */ {0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e},
    /* C */ {0x0f,0x10,0x10,0x10,0x10,0x10,0x0f},
    /* D */ {0x1e,0x11,0x11,0x11,0x11,0x11,0x1e},
    /* E */ {0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f},
    /* F */ {0x1f,0x10,0x10,0x1e,0x10,0x10,0x10},
    /* G */ {0x0f,0x10,0x10,0x17,0x11,0x11,0x0f},
    /* H */ {0x11,0x11,0x11,0x1f,0x11,0x11,0x11},
    /* I */ {0x1f,0x04,0x04,0x04,0x04,0x04,0x1f},
    /* J */ {0x07,0x02,0x02,0x02,0x12,0x12,0x0c},
    /* K */ {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    /* L */ {0x10,0x10,0x10,0x10,0x10,0x10,0x1f},
    /* M */ {0x11,0x1b,0x15,0x15,0x11,0x11,0x11},
    /* N */ {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    /* O */ {0x0e,0x11,0x11,0x11,0x11,0x11,0x0e},
    /* P */ {0x1e,0x11,0x11,0x1e,0x10,0x10,0x10},
    /* Q */ {0x0e,0x11,0x11,0x11,0x15,0x12,0x0d},
    /* R */ {0x1e,0x11,0x11,0x1e,0x14,0x12,0x11},
    /* S */ {0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e},
    /* T */ {0x1f,0x04,0x04,0x04,0x04,0x04,0x04},
    /* U */ {0x11,0x11,0x11,0x11,0x11,0x11,0x0e},
    /* V */ {0x11,0x11,0x11,0x11,0x11,0x0a,0x04},
    /* W */ {0x11,0x11,0x11,0x15,0x15,0x15,0x0a},
    /* X */ {0x11,0x11,0x0a,0x04,0x0a,0x11,0x11},
    /* Y */ {0x11,0x11,0x0a,0x04,0x04,0x04,0x04},
    /* Z */ {0x1f,0x01,0x02,0x04,0x08,0x10,0x1f},
    /* 0 */ {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e},
    /* 1 */ {0x04,0x0c,0x04,0x04,0x04,0x04,0x0e},
    /* 2 */ {0x0e,0x11,0x01,0x02,0x04,0x08,0x1f},
    /* 3 */ {0x1e,0x01,0x01,0x0e,0x01,0x01,0x1e},
    /* 4 */ {0x02,0x06,0x0a,0x12,0x1f,0x02,0x02},
    /* 5 */ {0x1f,0x10,0x10,0x1e,0x01,0x01,0x1e},
    /* 6 */ {0x0e,0x10,0x10,0x1e,0x11,0x11,0x0e},
    /* 7 */ {0x1f,0x01,0x02,0x04,0x08,0x08,0x08},
    /* 8 */ {0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e},
    /* 9 */ {0x0e,0x11,0x11,0x0f,0x01,0x01,0x0e},
    /* % */ {0x19,0x19,0x02,0x04,0x08,0x13,0x13},
    /* : */ {0x00,0x04,0x04,0x00,0x00,0x04,0x04},
    /* / */ {0x01,0x02,0x02,0x04,0x08,0x08,0x10},
    /* . */ {0x00,0x00,0x00,0x00,0x00,0x0c,0x0c},
    /*   */ {0,0,0,0,0,0,0},
    /* - */ {0,0,0,0x1f,0,0,0},
    /* _ */ {0,0,0,0,0,0,0x1f},
    /* ? */ {0x0e,0x11,0x01,0x02,0x04,0x00,0x04},
};

static const uint8_t *glyph(char ch) {
    if (ch >= 'a' && ch <= 'z')
        ch = (char)toupper((unsigned char)ch);
    for (size_t i = 0; i + 1 < sizeof(g_charset); ++i)
        if (g_charset[i] == ch)
            return g_font[i];
    return g_font[sizeof(g_charset) - 2];
}

static void fill(u32 *pixels, u32 stride, u32 color) {
    for (u32 y = 0; y < OVERLAY_H; ++y) {
        u32 *row = (u32 *)((uint8_t *)pixels + (size_t)y * stride);
        for (u32 x = 0; x < OVERLAY_W; ++x)
            row[x] = color;
    }
}

static void rect(u32 *pixels, u32 stride, int x, int y, int w, int h, u32 color) {
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)OVERLAY_W) w = (int)OVERLAY_W - x;
    if (y + h > (int)OVERLAY_H) h = (int)OVERLAY_H - y;
    if (w <= 0 || h <= 0) return;

    for (int yy = 0; yy < h; ++yy) {
        u32 *row = (u32 *)((uint8_t *)pixels + (size_t)(y + yy) * stride);
        for (int xx = 0; xx < w; ++xx)
            row[x + xx] = color;
    }
}

static void text(u32 *pixels, u32 stride, int x, int y, int scale,
                 const char *src, u32 color, int max_chars) {
    if (!src || scale <= 0) return;
    int cursor = x;
    int count = 0;

    for (const unsigned char *p = (const unsigned char *)src;
         *p && (max_chars <= 0 || count < max_chars); ++p, ++count) {
        const uint8_t *g = glyph((char)*p);
        for (int gy = 0; gy < 7; ++gy) {
            for (int gx = 0; gx < 5; ++gx) {
                if (!(g[gy] & (1u << (4 - gx)))) continue;
                rect(pixels, stride, cursor + gx * scale, y + gy * scale,
                     scale, scale, color);
            }
        }
        cursor += 6 * scale;
    }
}

static void overlay_close_partial(void) {
    if (g_ready) {
        framebufferClose(&g_fb);
        nwindowClose(&g_window);
        memset(&g_fb, 0, sizeof(g_fb));
        memset(&g_window, 0, sizeof(g_window));
        g_ready = 0;
    }

    if (g_layer_open) {
        Result rc = serviceDispatchIn(&g_vi_app, 2031, g_layer_id);
        if (R_FAILED(rc))
            debugPrintf("[jar-ui] close layer failed: 0x%x\n", rc);
        g_layer_open = 0;
        g_layer_id = 0;
    }

    if (g_display_open) {
        Result rc = serviceDispatchIn(&g_vi_app, 1020, g_display_id);
        if (R_FAILED(rc))
            debugPrintf("[jar-ui] close display failed: 0x%x\n", rc);
        g_display_open = 0;
        g_display_id = 0;
    }

    if (serviceIsActive(&g_vi_relay))
        serviceClose(&g_vi_relay);
    if (serviceIsActive(&g_vi_app))
        serviceClose(&g_vi_app);
    if (serviceIsActive(&g_vi_root))
        serviceClose(&g_vi_root);

    memset(&g_vi_relay, 0, sizeof(g_vi_relay));
    memset(&g_vi_app, 0, sizeof(g_vi_app));
    memset(&g_vi_root, 0, sizeof(g_vi_root));
    g_vi_open = 0;
}

static int overlay_init(void) {
    if (g_ready) return 1;

    if (!g_vi_open) {
        /*
         * Do not call viInitialize()/viOpenDefaultDisplay() here. libnx has
         * already initialized its process-global VI session and opened the
         * Default display for nwindowGetDefault(). Use an independent
         * application VI session (vi:u) for this transient overlay.
         */
        Result rc = smGetService(&g_vi_root, "vi:u");
        if (R_FAILED(rc)) {
            debugPrintf("[jar-ui] smGetService(vi:u) failed: 0x%x\n", rc);
            return 0;
        }

        rc = serviceDispatchIn(&g_vi_root, 0, 0,
                               .out_num_objects = 1,
                               .out_objects = &g_vi_app);
        if (R_FAILED(rc)) {
            debugPrintf("[jar-ui] vi:u application session failed: 0x%x\n", rc);
            overlay_close_partial();
            return 0;
        }

        rc = serviceDispatch(&g_vi_app, 100,
                             .out_num_objects = 1,
                             .out_objects = &g_vi_relay);
        if (R_FAILED(rc)) {
            debugPrintf("[jar-ui] vi:u binder relay failed: 0x%x\n", rc);
            overlay_close_partial();
            return 0;
        }

        g_vi_open = 1;
    }

    Result rc;
    struct {
        u64 display_id;
    } display_out = {0};

    ViDisplayName display_name = {};
    strncpy(display_name.data, "Default", sizeof(display_name.data) - 1);
    rc = serviceDispatchInOut(&g_vi_app, 1010, display_name, display_out);
    if (R_FAILED(rc)) {
        debugPrintf("[jar-ui] vi:u OpenDisplay(Default) failed: 0x%x\n", rc);
        overlay_close_partial();
        return 0;
    }
    g_display_id = display_out.display_id;
    g_display_open = 1;

    /*
     * CreateStrayLayer is the same operation libnx uses for a non-managed
     * window. We only need the IGraphicBufferProducer binder object from the
     * returned native-window parcel; no System/Manager VI session is needed
     * because we deliberately leave size/position/z-order at compositor
     * defaults and configure the NWindow itself to 1280x720.
     */
    alignas(8) u8 native_window_raw[0x100];
    u64 native_window_size = 0;
    const struct {
        u32 layer_flags;
        u32 pad;
        u64 display_id;
    } layer_in = { ViLayerFlags_Default, 0, g_display_id };
    struct {
        u64 layer_id;
        u64 native_window_size;
    } layer_out = {0};

    rc = serviceDispatchInOut(&g_vi_app, 2030, layer_in, layer_out,
                              .buffer_attrs = { {
                                  SfBufferAttr_Out | SfBufferAttr_HipcMapAlias
                              } },
                              .buffers = { { native_window_raw, sizeof(native_window_raw) } });
    if (R_FAILED(rc)) {
        debugPrintf("[jar-ui] vi:u CreateStrayLayer failed: 0x%x\n", rc);
        overlay_close_partial();
        return 0;
    }

    if (layer_out.native_window_size > sizeof(native_window_raw) ||
        layer_out.native_window_size < sizeof(ParcelHeader)) {
        debugPrintf("[jar-ui] vi:u layer parcel size invalid: 0x%llx\n",
                    (unsigned long long)layer_out.native_window_size);
        overlay_close_partial();
        return 0;
    }

    ParcelHeader *hdr = (ParcelHeader *)native_window_raw;
    if (hdr->payload_off > layer_out.native_window_size ||
        hdr->payload_size > layer_out.native_window_size - hdr->payload_off ||
        hdr->payload_size < 3 * sizeof(u32)) {
        debugPrintf("[jar-ui] vi:u layer parcel invalid off=0x%x size=0x%x total=0x%llx\n",
                    hdr->payload_off, hdr->payload_size,
                    (unsigned long long)layer_out.native_window_size);
        overlay_close_partial();
        return 0;
    }

    u32 *payload = (u32 *)&native_window_raw[hdr->payload_off];
    const u32 binder_id = payload[2];
    if (!binder_id) {
        debugPrintf("[jar-ui] vi:u layer returned empty IGBP binder id\n");
        overlay_close_partial();
        return 0;
    }

    g_layer_id = layer_out.layer_id;
    g_layer_open = 1;

    rc = serviceDispatchIn(&g_vi_app, 2101,
                           ((const struct {
                               u32 scaling_mode;
                               u32 pad;
                               u64 layer_id;
                           }){ ViScalingMode_FitToLayer, 0, g_layer_id }));
    if (R_FAILED(rc))
        debugPrintf("[jar-ui] vi:u SetLayerScalingMode failed: 0x%x\n", rc);

    rc = nwindowCreate(&g_window, &g_vi_relay, (s32)binder_id, false);
    if (R_FAILED(rc)) {
        debugPrintf("[jar-ui] nwindowCreate overlay failed: 0x%x\n", rc);
        overlay_close_partial();
        return 0;
    }

    rc = nwindowSetDimensions(&g_window, OVERLAY_W, OVERLAY_H);
    if (R_FAILED(rc))
        debugPrintf("[jar-ui] nwindowSetDimensions overlay failed: 0x%x\n", rc);

    rc = framebufferCreate(&g_fb, &g_window, OVERLAY_W, OVERLAY_H,
                           PIXEL_FORMAT_RGBA_8888, 1);
    if (R_FAILED(rc)) {
        debugPrintf("[jar-ui] framebufferCreate failed: 0x%x\n", rc);
        overlay_close_partial();
        return 0;
    }

    rc = framebufferMakeLinear(&g_fb);
    if (R_FAILED(rc)) {
        debugPrintf("[jar-ui] framebufferMakeLinear failed: 0x%x\n", rc);
        overlay_close_partial();
        return 0;
    }

    g_ready = 1;
    debugPrintf("[jar-ui] progress layer ready (vi:u layer=%llu)\n",
                (unsigned long long)g_layer_id);
    return 1;
}

void jar_progress_overlay_update(void) {
    char stage[96] = {0};
    char detail[256] = {0};
    unsigned percent = 0, done = 0, total = 0;
    const int active = jar_import_progress_read(
        stage, sizeof(stage), detail, sizeof(detail),
        &percent, &done, &total);

    if (!active) {
        if (g_ready)
            jar_progress_overlay_shutdown();
        return;
    }

    if (!overlay_init())
        return;

    const u64 now = armGetSystemTick();
    if (g_last_tick != 0 &&
        armTicksToNs(now - g_last_tick) < UPDATE_NS &&
        percent == 0)
        return;
    g_last_tick = now;

    u32 stride = 0;
    u32 *pixels = (u32 *)framebufferBegin(&g_fb, &stride);
    if (!pixels) {
        debugPrintf("[jar-ui] framebufferBegin returned NULL\n");
        return;
    }

    fill(pixels, stride, RGBA8_MAXALPHA(4, 13, 19));

    text(pixels, stride, 155, 92, 6, "JAR IMPORT", RGBA8_MAXALPHA(139, 214, 238), 32);
    text(pixels, stride, 155, 210, 4, stage, RGBA8_MAXALPHA(214, 232, 238), 42);

    char pct[16];
    snprintf(pct, sizeof(pct), "%u%%", percent > 100 ? 100 : percent);
    text(pixels, stride, 155, 292, 8, pct, RGBA8_MAXALPHA(255, 255, 255), 8);

    const int bx = 155;
    const int by = 410;
    const int bw = 970;
    const int bh = 48;
    rect(pixels, stride, bx, by, bw, bh, RGBA8_MAXALPHA(21, 35, 42));
    int filled = (int)(((uint64_t)(percent > 100 ? 100 : percent) * (bw - 8)) / 100);
    if (filled > 0)
        rect(pixels, stride, bx + 4, by + 4, filled, bh - 8,
             RGBA8_MAXALPHA(88, 174, 201));

    if (total) {
        char resources[64];
        snprintf(resources, sizeof(resources), "RESOURCES %u / %u", done, total);
        text(pixels, stride, 155, 500, 3, resources, RGBA8_MAXALPHA(161, 195, 208), 42);
    }

    text(pixels, stride, 155, 560, 2, detail, RGBA8_MAXALPHA(161, 195, 208), 70);
    text(pixels, stride, 155, 650, 2, "PLEASE WAIT", RGBA8_MAXALPHA(120, 150, 160), 30);

    framebufferEnd(&g_fb);
}

void jar_progress_overlay_shutdown(void) {
    overlay_close_partial();
    g_last_tick = 0;
}
