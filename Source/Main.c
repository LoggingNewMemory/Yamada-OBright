#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <stdbool.h>
#include <sys/system_properties.h>
#include <android/log.h>

#define PROP_NAME "debug.tracing.screen_brightness"
#define BACKLIGHT_PATH "/sys/class/leds/lcd-backlight/brightness"
#define LOG_TAG "YamadaOBright"

// Translation Logic based on Yamada Blueprint
int calculate_brightness(float prop_val) {
    // Hardware constraints
    if (prop_val <= 222.0f) return 1;
    if (prop_val >= 8191.0f) return 4095;
    
    // Linear interpolation: y = y_min + ((x - x_min) * (y_max - y_min)) / (x_max - x_min)
    float mapped = 1.0f + ((prop_val - 222.0f) * 4094.0f) / 7969.0f;
    
    // Using roundf from <math.h> (linked via -lm) for better precision
    return (int)roundf(mapped);
}

// Hardware Write Function
void write_backlight(int brightness_val) {
    FILE *f = fopen(BACKLIGHT_PATH, "w");
    if (f != NULL) {
        fprintf(f, "%d\n", brightness_val);
        fclose(f);
    } else {
        // Log errors to logcat (linked via -llog)
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "Failed to write to %s", BACKLIGHT_PATH);
    }
}

// Callback to extract the float value from the property
void read_prop_callback(void* cookie, const char* name, const char* value, uint32_t serial) {
    float* out_val = (float*)cookie;
    if (value != NULL) {
        *out_val = atof(value);
    }
}

int main() {
    const prop_info *pi;
    uint32_t serial = 0;
    uint32_t new_serial = 0;
    float current_prop_val = 0.0f;

    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Starting Yamada OPlus Display Adaptor...");

    // 1. Initialization: Wait for the target property to exist
    while ((pi = __system_property_find(PROP_NAME)) == NULL) {
        usleep(500000); // 500ms
    }

    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Found target property. Entering event listener mode.");

    // 2. Event Listener (NO WHILE(TRUE) POLLING)
    for (;;) {
        // Blocks thread with 0% CPU usage. Passing NULL for timeout means wait forever until changed.
        if (__system_property_wait(pi, serial, &new_serial, NULL)) {
            // Update the serial tracker so we don't trigger on the same change twice
            serial = new_serial;

            // Safely read the new value
            __system_property_read_callback(pi, read_prop_callback, &current_prop_val);

            // Translate and push to hardware
            int new_brightness = calculate_brightness(current_prop_val);
            write_backlight(new_brightness);
            
            // Optional debugging
            // __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "Input: %.1f -> Output: %d", current_prop_val, new_brightness);
        }
    }

    return 0;
}