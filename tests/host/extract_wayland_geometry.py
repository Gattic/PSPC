#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract pinned SDL's actual geometry code for a display-independent C test.

Only native configuration input and geometry/commit side effects are substituted. Compiling
these bodies with SDL's real private types avoids a parallel model of the bug.
The narrow source boundaries intentionally fail closed when upstream is updated.
"""
import argparse
from pathlib import Path
import re


def extract(source):
    bodies = []
    for name in ("GetWindowScale", "PointToPixel", "PixelToPoint", "Wayland_HandlePreferredScaleChanged"):
        matches = re.findall(r"^static (?:double|int|void) " + name + r"\([^\n]*\)\n\{\n.*?^\}\n",
                             source, re.MULTILINE | re.DOTALL)
        if len(matches) != 1:
            raise ValueError(f"Expected one pinned SDL function: {name}")
        bodies.append(matches[0])
    create = source.split("bool Wayland_CreateWindow(", 1)[1]
    start = create.index("    data->waylandData = c;\n")
    end = create.index("    // Cache the app_id at creation time", start)
    bodies.append("static void InitGeometry(SDL_VideoDevice *_this, SDL_Window *window)\n{\n"
                  "    SDL_VideoData *c = _this->internal;\n"
                  "    SDL_WindowData *data = window->internal;\n" + create[start:end] + "}\n")
    configure = source.split("static void decoration_frame_configure(", 1)[1]
    start = configure.index("    const bool have_configure_size =")
    end = configure.index("    // Update the window manager capabilities.", start)
    bodies.append("static void ApplyConfigure(SDL_Window *window, bool fullscreen, int w, int h)\n{\n"
                  "    SDL_WindowData *wind = window->internal;\n"
                  "    MockConfiguration storage = {w, h};\n"
                  "    MockConfiguration *configuration = &storage;\n"
                  "    struct libdecor_frame *frame = NULL;\n"
                  "    int width, height;\n"
                  "    bool floating = !fullscreen;\n"
                  "    bool maximized = false, tiled = false, suspended = false, resizing = false, active = true;\n"
                  "    if (fullscreen) window->flags |= SDL_WINDOW_FULLSCREEN;\n"
                  "    else window->flags &= ~SDL_WINDOW_FULLSCREEN;\n" + configure[start:end] +
                  "    (void)started_resize;\n}\n")
    return "/* Generated from pinned SDL_waylandwindow.c; do not edit. */\n" + "\n".join(bodies)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.write_text(extract(args.source.read_text()))


if __name__ == "__main__":
    main()
