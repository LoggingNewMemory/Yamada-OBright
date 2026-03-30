#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <time.h>
#include <sys/system_properties.h>
#include <android/log.h>

#define PROP_NAME "debug.tracing.screen_brightness"
#define STATE_PROP "debug.tracing.screen_state"
#define BACKLIGHT_PATH "/sys/class/leds/lcd-backlight/brightness"
#define MAX_BRIGHT_PATH "/sys/class/leds/lcd-backlight/max_hw_brightness"
#define MIN_BRIGHT_PATH "/sys/class/leds/lcd-backlight/min_brightness"
#define LOG_TAG "YamadaOBright"

// --- Hardcoded Input Bounds ---
#define INPUT_MAX 8191
#define INPUT_MIN 222

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

void write_backlight_fd(int fd, int brightness_val) {
    if (fd < 0) return;
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", brightness_val);
    pwrite(fd, buf, len, 0);
}

// --- Property Reading Helpers ---
float get_float_prop(const char* prop_name, float default_val) {
    char value[PROP_VALUE_MAX];
    if (__system_property_get(prop_name, value) > 0) {
        return atof(value);
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

    if (prop_val > 0.0f && prop_val <= 1.0f) {
        prop_val = f_input_min + (prop_val * (f_input_max - f_input_min));
    }

    if (prop_val <= f_input_min) return hw_min;
    if (prop_val >= f_input_max) return hw_max;
    
    float linear_percentage = cbrtf(prop_val / f_input_max);
    int result = (int)roundf(linear_percentage * (float)hw_max);
    
    if (result < hw_min) return hw_min;
    if (result > hw_max) return hw_max;
    return result;
}

// --- Main Daemon ---
int main() {
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Starting Yamada OPlus Display Adaptor (Anti-Cache Edition)...");

    int hw_max = read_int_from_file(MAX_BRIGHT_PATH, 4095);
    int hw_min = read_int_from_file(MIN_BRIGHT_PATH, 1);    
    
    // [BUG FIX]: Hard-cap the physical minimum brightness to 100.
    // This prevents the screen from becoming unreadably dim and stops the panel 
    // from fully shutting off the backlight (black screen) at minimum values.
    if (hw_min < 100) {
        hw_min = 100;
    }
    
    int backlight_fd = open(BACKLIGHT_PATH, O_WRONLY);
    if (backlight_fd < 0) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "Failed to open backlight path!");
    }

    uint32_t global_serial = 0;
    struct timespec no_wait = {0, 0};
    __system_property_wait(NULL, 0, &global_serial, &no_wait); 

    float current_prop_val = get_float_prop(PROP_NAME, 0.0f);
    int prev_state = get_screen_state();
    
    // Calculate initial brightness using the hardcoded INPUT_MIN and INPUT_MAX
    int raw_initial = (current_prop_val == 0.0f) ? -1 : calculate_brightness(current_prop_val, hw_min, hw_max, INPUT_MIN, INPUT_MAX);
    int prev_bright = (raw_initial == -1) ? hw_min : raw_initial;
    
    // Safety net: ensure even initialization respects the new 100 floor
    if (prev_bright < hw_min) prev_bright = hw_min; 
    
    int last_written_val = -1;
    int wake_ticks = 0;

    if (prev_state != 2) {
        last_written_val = 0;
        write_backlight_fd(backlight_fd, 0);
    } else {
        last_written_val = prev_bright;
        write_backlight_fd(backlight_fd, prev_bright);
    }

    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Entering Smart Timeout Event Loop.");

    for (;;) {
        uint32_t old_serial = global_serial;

        // 1. Smart Waiting
        if (wake_ticks > 0) {
            struct timespec timeout = {0, 50000000}; // 50ms
            __system_property_wait(NULL, old_serial, &global_serial, &timeout);
        } else {
            __system_property_wait(NULL, old_serial, &global_serial, NULL);
        }

        bool props_changed = (global_serial != old_serial);
        
        float new_prop_val = current_prop_val;
        int cur_state = prev_state;
        int cur_bright = prev_bright;

        if (props_changed) {
            // 2. Read Fresh Data
            new_prop_val = get_float_prop(PROP_NAME, current_prop_val);
            cur_state = get_screen_state();

            // Calculate new brightness using the hardcoded bounds
            int raw_bright = (new_prop_val == 0.0f) ? -1 : calculate_brightness(new_prop_val, hw_min, hw_max, INPUT_MIN, INPUT_MAX);
            cur_bright = (raw_bright == -1) ? prev_bright : raw_bright;
            
            // [BUG FIX]: Absolute safety check. Nothing evaluates below hw_min (100) when ON.
            if (cur_bright < hw_min) cur_bright = hw_min; 
        }

        // 3. State Change Detection
        if (cur_state != prev_state) {
            if (cur_state == 2) {
                wake_ticks = 15; 
            } else {
                wake_ticks = 0;  
            }
        }

        // 4. Calculate Final Value
        // Note: It is still safe (and necessary) to write 0 when cur_state != 2 (Screen OFF)
        int val_to_write = (cur_state != 2) ? 0 : cur_bright;

        // 5. Execution Logic
        if (val_to_write != last_written_val) {
            write_backlight_fd(backlight_fd, val_to_write);
            last_written_val = val_to_write;
            
        } else if (wake_ticks > 0 && cur_state == 2 && !props_changed) {
            // The Anti-Cache Wobble
            wake_ticks--;
            int wobble = (val_to_write > hw_min) ? val_to_write - 1 : val_to_write + 1;
            
            write_backlight_fd(backlight_fd, wobble);
            write_backlight_fd(backlight_fd, val_to_write);
        }

        prev_bright = cur_bright;
        prev_state = cur_state;
        current_prop_val = new_prop_val;
    }

    if (backlight_fd >= 0) close(backlight_fd); 
    return 0;
}