#include "backlight.h"
#include "config.h"
#include "prefs.h"
#include <Arduino.h>

static uint8_t  s_duty       = BL_MAX_DUTY;
static uint32_t s_last_check = 0;
static float    s_ema        = 300;   // mid-range under the inverted CYD wiring; converges in a few samples
static uint16_t s_last_raw   = 0;

void backlight_begin() {
  // ESP32 Arduino Core 3.x LedC API: pin-based, channels are managed
  // internally. ledcAttach binds the pin to a freshly-allocated channel
  // at the requested frequency/resolution; subsequent ledcWrite calls
  // address the pin directly.
  ledcAttach(TFT_BL, TFT_BL_PWM_FREQ, TFT_BL_PWM_BITS);
  ledcWrite(TFT_BL, BL_MAX_DUTY);
  pinMode(LDR_PIN, INPUT);

  // Be explicit about ADC config so we get a usable range from the LDR.
  // Default resolution on ESP32 is already 12-bit (0..4095), but the
  // arduino-esp32 3.x core changed some defaults around per-pin
  // attenuation; setting it here keeps the BL_LDR_DARK/BRIGHT values
  // in config.h meaningful regardless of core defaults.
  //
  // 6 dB attenuation is empirically the sweet spot on this board: at
  // 0 dB the indoor-lit reading is already at half-scale (no headroom
  // toward dark); at 11 dB the bright end falls into the ADC's lower
  // dead-zone (~150 mV) and reads zero. 6 dB keeps the full lit→dark
  // sweep within (~50, ~3000) raw with clean stable samples.
  analogReadResolution(12);
  analogSetPinAttenuation(LDR_PIN, ADC_6db);
}

void backlight_set_duty(uint8_t d) {
  s_duty = d;
  ledcWrite(TFT_BL, d);
}

static uint8_t duty_for_ldr(uint16_t raw) {
  // CYD wiring is: 3V3 — R10 (1MΩ) — GPIO 34 — LDR — GND. Bright light
  // drops the LDR's resistance, pulls the tap toward GND, gives a LOW
  // raw value. Dark gives a HIGH raw value. So map low raw → max duty,
  // high raw → min duty (i.e. the comparison polarity is inverted vs.
  // a naive divider assumption).
  if (raw <= BL_LDR_BRIGHT) return BL_MAX_DUTY;
  if (raw >= BL_LDR_DARK)   return BL_MIN_DUTY;
  uint32_t span = BL_LDR_DARK - BL_LDR_BRIGHT;
  uint32_t pos  = raw - BL_LDR_BRIGHT;
  return BL_MAX_DUTY - (uint8_t)((BL_MAX_DUTY - BL_MIN_DUTY) * pos / span);
}

void backlight_loop() {
  uint32_t now = millis();
  if (now - s_last_check < 500) return;
  s_last_check = now;

  switch (prefs_brightness_mode()) {
    case BRIGHTNESS_FULL: backlight_set_duty(BL_MAX_DUTY); return;
    case BRIGHTNESS_DIM:  backlight_set_duty(BL_MIN_DUTY); return;
    case BRIGHTNESS_AUTO: break;
  }

  uint16_t raw = analogRead(LDR_PIN);
  s_last_raw = raw;
  s_ema = s_ema * 0.85f + (float)raw * 0.15f;
  uint8_t target = duty_for_ldr((uint16_t)s_ema);
  if (target != s_duty) backlight_set_duty(target);
}

uint16_t backlight_ldr_raw()      { return s_last_raw; }
uint16_t backlight_ldr_ema()      { return (uint16_t)s_ema; }
uint8_t  backlight_current_duty() { return s_duty; }
