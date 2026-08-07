// backlight.h — LDR-driven auto-dim for the TFT backlight.
//
// PWM on TFT_BL (GPIO 21). Reads the on-board LDR (GPIO 34) every few
// seconds and maps the raw ADC value to a duty cycle, EMA-smoothed so
// brightness doesn't flicker when someone walks past the desk.
//
// prefs_brightness_mode() can override AUTO with FULL or DIM.

#pragma once

#include <Arduino.h>

void backlight_begin();
void backlight_loop();              // call from a low-rate task or loop()
void backlight_set_duty(uint8_t d); // manual override (0..255)

// Diagnostics. Cheap — just return cached values from the most recent
// backlight_loop tick.
uint16_t backlight_ldr_raw();       // most recent raw ADC reading
uint16_t backlight_ldr_ema();       // smoothed value used by auto-dim
uint8_t  backlight_current_duty();
