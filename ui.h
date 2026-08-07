// ui.h — grid render loop + LovyanGFX owner.
//
// The UI never does I/O. It reads from g_devices atomics and renders.
// The dashboard is display-only — there is no on-device input.

#pragma once

#include <Arduino.h>
#include <LovyanGFX.hpp>

void ui_begin();
void ui_loop();   // call every frame from loop()

// Set/clear a transient status line shown over the grid view.
// Used during boot ("Connecting to WiFi...") and on errors.
void ui_set_status(const char* msg);

// The grid view draws via this accessor. The concrete LGFX subclass
// (panel-specific config) lives in ui.cpp; consumers see the
// LGFX_Device base which carries all the drawing primitives.
lgfx::LGFX_Device& ui_gfx();
