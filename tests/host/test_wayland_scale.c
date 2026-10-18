// SPDX-License-Identifier: GPL-2.0-or-later
// Test actual pinned SDL initialization/scale code with its real private types.
// No compositor, GPU, sleeps or retry-until-pass loop is involved.
#include "SDL_internal.h"
#include "video/wayland/SDL_waylandwindow.h"
#include "video/wayland/SDL_waylandvideo.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

// The isolated code needs only these math and native-side-effect boundaries.
double SDL_ceil(double x) { return ceil(x); }
long SDL_lround(double x) { return lround(x); }
long SDL_lroundf(float x) { return lroundf(x); }
typedef struct { int width, height; } MockConfiguration;
static bool MockContentSize(MockConfiguration *config, struct libdecor_frame *frame, int *width, int *height)
{
    (void)frame;
    *width = config->width;
    *height = config->height;
    return *width > 0 && *height > 0;
}
#undef libdecor_configuration_get_content_size
#define libdecor_configuration_get_content_size MockContentSize
static void OverrideLibdecorLimits(SDL_Window *window) { (void)window; abort(); }
static int geometry_calls;
static void ConfigureWindowGeometry(SDL_Window *window) { (void)window; ++geometry_calls; }
static void CommitLibdecorFrame(SDL_Window *window) { (void)window; }
static void EnsurePopupPositionIsValid(SDL_Window *window, int *x, int *y) { (void)window; (void)x; (void)y; abort(); }
#include "wayland_geometry.inc"

static int failures;
#define CHECK(condition, message) do { if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); ++failures; } } while (0)

struct Fixture {
    SDL_Window window;
    SDL_WindowData data;
    SDL_VideoData video;
    SDL_VideoDevice device;
    SDL_VideoDisplay display;
    SDL_DisplayData display_data;
    SDL_VideoDisplay *displays[1];
};

static void Init(struct Fixture *f, double scale)
{
    *f = (struct Fixture){0};
    f->window.flags = SDL_WINDOW_RESIZABLE;
    f->window.floating.w = 960;
    f->window.floating.h = 544;
    f->window.internal = &f->data;
    f->video.scale_to_display_enabled = true;
    f->device.internal = &f->video;
    f->device.num_displays = 1;
    f->device.displays = f->displays;
    f->displays[0] = &f->display;
    f->display.internal = &f->display_data;
    f->display_data.scale_factor = scale;
    InitGeometry(&f->device, &f->window);
    // Only presence matters to scale rounding; the side-effect stubs never dereference it.
    f->data.viewport = (struct wp_viewport *)f;
    f->data.requested.pixel_width = 960;
    f->data.requested.pixel_height = 544;
    f->data.requested.logical_width = (int)(960 / scale);
    f->data.requested.logical_height = (int)(544 / scale);
    geometry_calls = 0;
}

int main(void)
{
    struct Fixture f;
    // A compositor's first preferred scale may differ from the guessed display maximum,
    // and arrives before its first toplevel configure. Preserve the application's size.
    Init(&f, 2.0);
    Wayland_HandlePreferredScaleChanged(&f.data, 1.0);
    CHECK(f.data.requested.pixel_width == 960 && f.data.requested.pixel_height == 544,
          "initial 2x->1x scale must not halve 960x544 to 480x272");
    CHECK(f.data.requested.logical_width == 960 && f.data.requested.logical_height == 544,
          "initial preferred scale must recalculate logical size from requested pixels");

    Init(&f, 1.0);
    Wayland_HandlePreferredScaleChanged(&f.data, 2.0);
    CHECK(f.data.requested.pixel_width == 960 && f.data.requested.logical_width == 480,
          "initial 1x->2x scale must not double requested pixels");

    // Execute the real libdecor sizing decisions, not a second implementation of them.
    // Restore substitutes 960x544 for a stale desktop-sized configure, then the scale
    // changes before an activation-only configure repeats that compositor geometry.
    Init(&f, 2.0);
    ApplyConfigure(&f.window, true, 1920, 1080);
    ApplyConfigure(&f.window, false, 1920, 1043);
    Wayland_HandlePreferredScaleChanged(&f.data, 1.0);
    ApplyConfigure(&f.window, false, 1920, 1043);
    CHECK(f.data.requested.pixel_width == 960 && f.data.requested.pixel_height == 544,
          "repeated stale compositor geometry after a scale event must not corrupt restore");
    CHECK(geometry_calls == 1, "scale change must still update native geometry");

    // The next configure can instead echo the old *application* logical size. Both
    // histories are needed: keeping only raw configure history doubles the window here.
    Init(&f, 2.0);
    ApplyConfigure(&f.window, true, 1920, 1080);
    ApplyConfigure(&f.window, false, 1920, 1043);
    Wayland_HandlePreferredScaleChanged(&f.data, 1.0);
    Wayland_HandlePreferredScaleChanged(&f.data, 2.0);
    ApplyConfigure(&f.window, false, 960, 544);
    CHECK(f.data.requested.pixel_width == 960 && f.data.requested.pixel_height == 544,
          "pre-scale application geometry must not be accepted at the new scale");

    // A genuinely different configure remains a user resize, including after restore.
    ApplyConfigure(&f.window, false, 400, 300);
    CHECK(f.data.requested.pixel_width == 800 && f.data.requested.pixel_height == 600,
          "real user resize must not be suppressed by stale-configure filtering");
    Wayland_HandlePreferredScaleChanged(&f.data, 1.0);
    ApplyConfigure(&f.window, false, 400, 300);
    CHECK(f.data.requested.pixel_width == 800 && f.data.requested.logical_width == 800 &&
          f.data.requested.pixel_height == 600 && f.data.requested.logical_height == 600,
          "user-resized 800x600 must survive scale changes and repeated configures");

    // Nonfloating windows are compositor-sized: their logical dimensions stay fixed.
    Init(&f, 1.0);
    f.data.floating = false;
    f.data.requested.logical_width = 1920;
    f.data.requested.logical_height = 1080;
    Wayland_HandlePreferredScaleChanged(&f.data, 2.0);
    CHECK(f.data.requested.pixel_width == 3840 && f.data.requested.pixel_height == 2160 &&
          f.data.requested.logical_width == 1920 && f.data.requested.logical_height == 1080,
          "fullscreen/maximized scale must still resize the backbuffer");
    Wayland_HandlePreferredScaleChanged(&f.data, 2.0);
    CHECK(geometry_calls == 1, "unchanged scale must not commit another resize");

    printf("Wayland scale/restore regression: %d failures\n", failures);
    return failures ? 1 : 0;
}
