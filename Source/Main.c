#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <stdbool.h>
#include <time.h>
#include <sys/system_properties.h>
#include <android/log.h>

#define PROP_NAME "debug.tracing.screen_brightness"
#define STATE_PROP "debug.tracing.screen_state"
#define SYS_PROP_MAX "sys.oplus.multibrightness"
#define SYS_PROP_MIN "sys.oplus.multibrightness.min"
#define BACKLIGHT_PATH "/sys/class/leds/lcd-backlight/brightness"
#define MAX_BRIGHT_PATH "/sys/class/leds/lcd-backlight/max_hw_brightness"
#define MIN_BRIGHT_PATH "/sys/class/leds/lcd-backlight/min_brightness"
#define LOG_TAG "YamadaOBright"

// --- File Reading Helpers ---
int read_int_from_file(const char* path, int default_val) {
    FILE* f = fopen(path, "r");
    if (!f) return default_val;
    int val = default_val;
    if (fscanf(f, "%d", &val) != 1) {
        val = default_val;
    }
    fclose(f);
    return val;
}

void write_backlight(int brightness_val) {
    FILE *f = fopen(BACKLIGHT_PATH, "w");
    if (f != NULL) {
        fprintf(f, "%d\n", brightness_val);
        fclose(f);
    }
}

// --- Property Reading Helpers ---
float get_float_prop(const char* prop_name, float default_val) {
    char value[PROP_VALUE_MAX];
    if (__system_property_get(prop_name, value) > 0) {
        return atof(value);
    }
    return default_val;
}

int get_int_prop(const char* prop_name, int default_val) {
    char value[PROP_VALUE_MAX];
    if (__system_property_get(prop_name, value) > 0) {
        return atoi(value);
    }
    return default_val;
}

int get_screen_state() {
    char value[PROP_VALUE_MAX];
    if (__system_property_get(STATE_PROP, value) > 0) {
        return atoi(value);
    }
    return 2; // Default to ON (2)
}

// --- Math Translation ---
int calculate_brightness(float prop_val, int hw_min, int hw_max, int input_min, int input_max) {
    float f_input_min = (float)input_min;
    float f_input_max = (float)input_max;

    // Handle float percentages (0.0 to 1.0) scaling to the dynamic input range
    if (prop_val > 0.0f && prop_val <= 1.0f) {
        prop_val = f_input_min + (prop_val * (f_input_max - f_input_min));
    }

    if (prop_val <= f_input_min) return hw_min;
    if (prop_val >= f_input_max) return hw_max;
    
    float normalized = prop_val / f_input_max;
    float linear_percentage = cbrtf(normalized);
    float mapped = linear_percentage * (float)hw_max;
    
    int result = (int)roundf(mapped);
    if (result < hw_min) return hw_min;
    if (result > hw_max) return hw_max;
    return result;
}

// --- Main Daemon ---
int main() {
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Starting Yamada OPlus Display Adaptor (Anti-Cache Edition)...");

    int hw_max = read_int_from_file(MAX_BRIGHT_PATH, 4095);
    int hw_min = read_int_from_file(MIN_BRIGHT_PATH, 1);    
    
    uint32_t global_serial = 0;
    struct timespec no_wait = {0, 0};
    __system_property_wait(NULL, 0, &global_serial, &no_wait); 

    float current_prop_val = get_float_prop(PROP_NAME, 0.0f);
    int prev_state = get_screen_state();
    
    // Initial fetch of input properties
    int input_max = get_int_prop(SYS_PROP_MAX, 8191);
    int input_min = get_int_prop(SYS_PROP_MIN, 222);

    int raw_initial = (current_prop_val == 0.0f) ? -1 : calculate_brightness(current_prop_val, hw_min, hw_max, input_min, input_max);
    int prev_bright = (raw_initial == -1) ? hw_min : raw_initial;
    int last_written_val = -1;
    
    int wake_ticks = 0; // Tracks our wake verification window

    if (prev_state != 2) {
        last_written_val = 0;
        write_backlight(0);
    } else {
        last_written_val = prev_bright;
        write_backlight(prev_bright);
    }

    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Entering Smart Timeout Event Loop.");

    for (;;) {
        uint32_t old_serial = global_serial;

        // 1. Smart Waiting
        if (wake_ticks > 0) {
            // Screen is waking up. Wait up to 50ms for a property change.
            struct timespec timeout = {0, 50000000}; // 50ms
            __system_property_wait(NULL, old_serial, &global_serial, &timeout);
        } else {
            // Screen is stable. Block at 0% CPU until a property changes.
            __system_property_wait(NULL, old_serial, &global_serial, NULL);
        }

        // 2. Read Fresh Data
        float new_prop_val = get_float_prop(PROP_NAME, current_prop_val);
        int cur_state = get_screen_state();
        
        // Refresh input properties dynamically
        input_max = get_int_prop(SYS_PROP_MAX, 8191);
        input_min = get_int_prop(SYS_PROP_MIN, 222);

        int raw_bright = (new_prop_val == 0.0f) ? -1 : calculate_brightness(new_prop_val, hw_min, hw_max, input_min, input_max);
        int cur_bright = (raw_bright == -1) ? prev_bright : raw_bright;
        if (cur_bright == -1) cur_bright = hw_min;

        // 3. State Change Detection
        if (cur_state != prev_state) {
            if (cur_state == 2) {
                // Start a 750ms verification window (15 ticks * 50ms) for slow CPUs
                wake_ticks = 15; 
            } else {
                // Instantly cancel verification if the user rapidly turns the screen off
                wake_ticks = 0;  
            }
        }

        // 4. Calculate Final Value
        int val_to_write = (cur_state != 2) ? 0 : cur_bright;

        // 5. Execution Logic
        if (val_to_write != last_written_val) {
            // Standard write when target changes
            write_backlight(val_to_write);
            last_written_val = val_to_write;
            
        } else if (wake_ticks > 0 && cur_state == 2 && global_serial == old_serial) {
            // The Anti-Cache Wobble:
            // We are in the wake window, the target hasn't changed, and the 50ms timeout hit.
            // Force the kernel to bypass its cache and update the physical hardware.
            wake_ticks--;
            int wobble = (val_to_write > hw_min) ? val_to_write - 1 : val_to_write + 1;
            
            write_backlight(wobble);       // Hardware receives modified value
            write_backlight(val_to_write); // Hardware instantly receives correct value
        }

        prev_bright = cur_bright;
        prev_state = cur_state;
        current_prop_val = new_prop_val;
    }

    return 0;
}