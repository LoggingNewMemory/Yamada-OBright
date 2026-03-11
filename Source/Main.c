#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <sys/system_properties.h>
#include <android/log.h>

// ============================================================================
// CONSTANTS
// ============================================================================
#define LOG_DEBUG 3
#define LOG_ERROR 6
#define FALLBACK_MAX 8191
#define FALLBACK_MIN 222
#define BRIGHTNESS_OFF 0
#define OS14_MAX 5118
#define OS14_MIN 22

// ============================================================================
// PATHS & PROPERTY KEYS
// ============================================================================
#define MIN_BRIGHT_PATH "/sys/class/leds/lcd-backlight/min_brightness"
#define MAX_BRIGHT_PATH "/sys/class/leds/lcd-backlight/max_hw_brightness"
#define BRIGHT_PATH "/sys/class/leds/lcd-backlight/brightness"
#define SYS_PROP_MAX "sys.oplus.multibrightness"
#define SYS_PROP_MIN "sys.oplus.multibrightness.min"
#define PERSIST_MAX "persist.sys.rianixia.multibrightness.max"
#define PERSIST_MIN "persist.sys.rianixia.multibrightness.min"
#define LOG_TAG "Xia-DisplayAdaptor"
#define PERSIST_DBG "persist.sys.rianixia.display-debug"
#define OPLUS_BRIGHT_PATH "/data/addon/oplus_display/oplus_brightness"
#define PERSIST_OPLUS_MIN "persist.sys.rianixia-display.min"
#define PERSIST_OPLUS_MAX "persist.sys.rianixia-display.max"
#define IS_OPLUS_PANEL_PROP "persist.sys.rianixia.is-displaypanel.support"
#define PERSIST_CUSTOM_DEVMAX_PROP "persist.sys.rianixia.custom.devmax.brightness"
#define PERSIST_CUSTOM_DEVMIN_PROP "persist.sys.rianixia.custom.devmin.brightness"
#define DISPLAY_TYPE_PROP "persist.sys.rianixia.display.type"
#define PERSIST_HW_MIN "persist.sys.rianixia.hw_min"
#define PERSIST_HW_MAX "persist.sys.rianixia.hw_max"
#define PERSIST_BRIGHT_MODE_PROP "persist.sys.rianixia.brightness.mode"
#define PERSIST_LUX_AOD_PROP "persist.sys.rianixia.oplus.lux_aod"
#define PERSIST_LUX_AOD_BRIGHTNESS_PROP "persist.sys.rianixia.oplus.lux_aod.brightness"

// ============================================================================
// LOGGING
// ============================================================================
#define LOGD(...) __android_log_print(LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ============================================================================
// PROPERTIES
// ============================================================================
bool get_prop(const char* key, char* out_value) {
    return __system_property_get(key, out_value) > 0;
}

int get_prop_int(const char* key, int default_val) {
    char buffer[PROP_VALUE_MAX] = {0};
    if (__system_property_get(key, buffer) > 0) {
        return atoi(buffer);
    }
    return default_val;
}

bool get_prop_bool(const char* key, bool default_val) {
    char buffer[PROP_VALUE_MAX] = {0};
    if (__system_property_get(key, buffer) > 0) {
        return (strcmp(buffer, "true") == 0 || strcmp(buffer, "1") == 0);
    }
    return default_val;
}

bool set_prop(const char* key, const char* val) {
    return __system_property_set(key, val) == 0;
}

// ============================================================================
// RANGE
// ============================================================================
typedef struct BrightnessRange {
    int min;
    int max;
    bool locked;
} BrightnessRange;

BrightnessRange init_brightness_range() {
    BrightnessRange s;
    int pmin = get_prop_int(PERSIST_MIN, -1);
    int pmax = get_prop_int(PERSIST_MAX, -1);

    if (pmin != -1 && pmax != -1 && pmin < pmax) {
        s.min = pmin;
        s.max = pmax;
        s.locked = false;
    } else {
        s.min = FALLBACK_MIN;
        s.max = FALLBACK_MAX;
        s.locked = false;
    }

    if (get_prop_bool(PERSIST_DBG, false)) {
        LOGD("[BrightnessRange] Initialized with range: min=%d, max=%d", s.min, s.max);
    }
    return s;
}

void refresh_brightness_range(BrightnessRange* self) {
    if (self->locked) return;

    int pmin = get_prop_int(PERSIST_MIN, -1);
    int pmax = get_prop_int(PERSIST_MAX, -1);
    int rmin = get_prop_int(SYS_PROP_MIN, -1);
    int rmax = get_prop_int(SYS_PROP_MAX, -1);

    if (rmin != -1 && rmax != -1) {
        if (rmin < rmax) {
            self->min = rmin;
            self->max = rmax;
            
            if (pmin != rmin) {
                char val_str[16]; snprintf(val_str, sizeof(val_str), "%d", rmin);
                set_prop(PERSIST_MIN, val_str);
            }
            if (pmax != rmax) {
                char val_str[16]; snprintf(val_str, sizeof(val_str), "%d", rmax);
                set_prop(PERSIST_MAX, val_str);
            }
            self->locked = true;
        }
    } else if (pmin != -1 && pmax != -1) {
        if (pmin < pmax) {
            self->min = pmin;
            self->max = pmax;
        }
    } else {
        self->min = FALLBACK_MIN;
        self->max = FALLBACK_MAX;
    }

    if (self->min >= self->max) {
        self->min = FALLBACK_MIN;
        self->max = FALLBACK_MAX;
    }
}

// ============================================================================
// UTILS & FILE READERS
// ============================================================================
int read_file_int(const char* path, int default_val) {
    FILE* f = fopen(path, "r");
    if (!f) return default_val;
    
    int val;
    // fscanf with %d naturally skips leading whitespace and stops at non-digits
    if (fscanf(f, "%d", &val) == 1) {
        fclose(f);
        return val;
    }
    fclose(f);
    return default_val;
}

bool is_panoramic_aod_enabled(bool dbg) {
    FILE *fp = popen("settings get secure panoramic_aod_enable 2>/dev/null", "r");
    if (!fp) {
        if (dbg) LOGE("[DisplayAdaptor] Failed to execute 'settings' command");
        return false;
    }
    char buf[32] = {0};
    if (fgets(buf, sizeof(buf), fp) != NULL) {
        buf[strcspn(buf, "\r\n")] = 0; // trim newline
        if (dbg) LOGD("[DisplayAdaptor] panoramic_aod_enable result: '%s'", buf);
        pclose(fp);
        return strcmp(buf, "1") == 0;
    }
    pclose(fp);
    return false;
}

int get_max_brightness(bool dbg) {
    int custom_max = get_prop_int(PERSIST_CUSTOM_DEVMAX_PROP, -1);
    if (custom_max > 0) {
        if (dbg) LOGD("[DisplayAdaptor] Using custom devmax brightness: %d", custom_max);
        return custom_max;
    }
    
    int cached_max = get_prop_int(PERSIST_HW_MAX, -1);
    if (cached_max > 0) {
        if (dbg) LOGD("[DisplayAdaptor] Using cached hw_max: %d", cached_max);
        return cached_max;
    }

    int file_val = read_file_int(MAX_BRIGHT_PATH, -1);
    if (file_val != -1) {
        if (dbg) LOGD("[DisplayAdaptor] Detected hw_max: %d. Saving to prop.", file_val);
        char val_str[16]; snprintf(val_str, sizeof(val_str), "%d", file_val);
        set_prop(PERSIST_HW_MAX, val_str);
        return file_val;
    }

    if (dbg) LOGD("[DisplayAdaptor] Failed to detect hw_max, using default 511");
    return 511;
}

int get_min_brightness(bool dbg) {
    int custom_min = get_prop_int(PERSIST_CUSTOM_DEVMIN_PROP, -1);
    if (custom_min > 0) {
        if (dbg) LOGD("[DisplayAdaptor] Using custom devmin brightness for calculation: %d", custom_min);
        return custom_min;
    }

    int cached_min = get_prop_int(PERSIST_HW_MIN, -1);
    if (cached_min > 0) {
        if (dbg) LOGD("[DisplayAdaptor] Using cached hw_min: %d", cached_min);
        return cached_min;
    } else if (cached_min == 0) {
        if (dbg) LOGD("[DisplayAdaptor] Cached hw_min invalid (0), forcing to 1.");
        set_prop(PERSIST_HW_MIN, "1");
        return 1;
    }

    int val = read_file_int(MIN_BRIGHT_PATH, -1);
    if (val != -1) {
        if (val <= 0) {
            if (dbg) LOGD("[DisplayAdaptor] Detected hw_min <= 0 (screen off?), falling back to 1.");
            val = 1;
        }
        if (dbg) LOGD("[DisplayAdaptor] Saving hw_min: %d to prop.", val);
        char val_str[16]; snprintf(val_str, sizeof(val_str), "%d", val);
        set_prop(PERSIST_HW_MIN, val_str);
        return val;
    }

    if (dbg) LOGD("[DisplayAdaptor] Failed to detect hw_min, using default 1");
    return 1;
}

// ============================================================================
// STATE
// ============================================================================
int get_prop_brightness(BrightnessRange* range, bool is_float) {
    char val_str[PROP_VALUE_MAX] = {0};
    if (!get_prop("debug.tracing.screen_brightness", val_str)) {
        return FALLBACK_MIN;
    }

    if (is_float) {
        float f = strtof(val_str, NULL);
        if (f == 0.0f) return -1; // skip
        
        // clamp 0.0 to 1.0
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        
        return (int)roundf((float)range->min + f * (float)(range->max - range->min));
    } else {
        // atoi stops at non-digits, nicely handling string splits like "255.0" -> 255
        int val = atoi(val_str);
        if (val == 0) return -1; // skip
        return val;
    }
}

int get_screen_state(void) {
    // 0: OFF, 1: OFF (AOD), 2: ON, 3: DOZE (AOD), 4: DOZE_SUSPEND (AOD DIMMED)
    return get_prop_int("debug.tracing.screen_state", 2);
}

// ============================================================================
// WRITER
// ============================================================================
void write_brightness(int fd, int val, int* last_val, bool dbg) {
    if (*last_val == val) {
        return;
    }
    
    if (dbg) LOGD("[DisplayAdaptor] Writing brightness: %d -> %d", *last_val, val);

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", val);
    
    ssize_t result = write(fd, buf, len);
    if (result < 0) {
        if (dbg) LOGE("[DisplayAdaptor] Write failed for value %d", val);
    } else {
        *last_val = val;
    }
}

// ============================================================================
// SCALING
// ============================================================================
int scale_brightness_linear(int val, int hw_min, int hw_max, int input_min, int input_max) {
    if (val <= input_min) return hw_min;
    if (val >= input_max) return hw_max;

    float range_input = (float)(input_max - input_min);
    float range_hw = (float)(hw_max - hw_min);
    float ratio = (float)(val - input_min) / range_input;

    return (int)roundf((float)hw_min + ratio * range_hw);
}

int scale_brightness_curved(int val, int hw_min, int hw_max, int input_min, int input_max) {
    if (val <= input_min) return hw_min;
    if (val >= input_max) return hw_max;

    float range_input = (float)(input_max - input_min);
    float range_hw = (float)(hw_max - hw_min);
    float ratio = (float)(val - input_min) / range_input;

    float gamma = 2.2f;
    float curve = powf(ratio, gamma);

    return (int)roundf((float)hw_min + curve * range_hw);
}

int scale_brightness_custom(int val, int hw_min, int hw_max, int input_min, int input_max) {
    if (val <= input_min) return hw_min;
    if (val >= input_max) return hw_max;

    float range_input = (float)(input_max - input_min);
    float normalized = (float)(val - input_min) / range_input;

    float mid_in = 0.75f;
    float mid_out = 255.0f;

    if (normalized <= mid_in) {
        float ratio = normalized / mid_in;
        return (int)roundf((float)hw_min + ratio * (mid_out - (float)hw_min));
    } else {
        float ratio = (normalized - mid_in) / (1.0f - mid_in);
        return (int)roundf(mid_out + ratio * ((float)hw_max - mid_out));
    }
}

// ============================================================================
// MODES
// ============================================================================
bool dbg_on() {
    return get_prop_bool(PERSIST_DBG, false);
}
bool is_oplus_panel_mode() {
    return get_prop_bool(IS_OPLUS_PANEL_PROP, false);
}
bool is_float_mode() {
    return get_prop_bool("persist.sys.rianixia.brightness.isfloat", false);
}
bool is_ips_mode() {
    char type[PROP_VALUE_MAX] = {0};
    if (get_prop(DISPLAY_TYPE_PROP, type)) {
        return strcmp(type, "IPS") == 0;
    }
    return false;
}
bool is_lux_aod_mode() {
    return get_prop_bool(PERSIST_LUX_AOD_PROP, false);
}
int get_brightness_mode() {
    return get_prop_int(PERSIST_BRIGHT_MODE_PROP, 0);
}

void run_oplus_panel_mode() {
    bool dbg = dbg_on();
    if (dbg) LOGD("[DisplayAdaptor] Starting in DisplayPanel Mode...");

    if (access(OPLUS_BRIGHT_PATH, F_OK) != 0) {
        if (dbg) LOGD("[DisplayPanel Mode] File %s not found, attempting to create it.", OPLUS_BRIGHT_PATH);
        while (true) {
            int fd_create = open(OPLUS_BRIGHT_PATH, O_CREAT | O_WRONLY, 0644);
            if (fd_create >= 0) {
                close(fd_create);
                if (dbg) LOGD("[DisplayPanel Mode] Successfully created %s.", OPLUS_BRIGHT_PATH);
                break;
            } else {
                LOGE("[DisplayPanel Mode] Failed to create %s, retrying in 1s", OPLUS_BRIGHT_PATH);
                sleep(1);
            }
        }
    }

    int hw_min = get_min_brightness(dbg);
    int hw_max = get_max_brightness(dbg);

    int input_min = get_prop_int(PERSIST_OPLUS_MIN, OS14_MIN);
    int input_max = get_prop_int(PERSIST_OPLUS_MAX, OS14_MAX);
    if (dbg) LOGD("[DisplayPanel Mode] Scaling range: %d-%d -> %d-%d", input_min, input_max, hw_min, hw_max);

    int fd = open(BRIGHT_PATH, O_WRONLY);
    if (fd < 0) {
        LOGE("[DisplayPanel Mode] Could not open brightness file");
        return;
    }

    int last_val = -1;
    int current_val = read_file_int(BRIGHT_PATH, hw_min);
    write_brightness(fd, current_val, &last_val, dbg);

    while (true) {
        current_val = read_file_int(BRIGHT_PATH, current_val);
        int oplus_bright = read_file_int(OPLUS_BRIGHT_PATH, -1);
        
        if (oplus_bright != -1) {
            if (oplus_bright == 0) {
                if (current_val != BRIGHTNESS_OFF) {
                    current_val = BRIGHTNESS_OFF;
                    write_brightness(fd, current_val, &last_val, dbg);
                }
            } else {
                int mode = get_brightness_mode();
                int target_val;
                switch(mode) {
                    case 1: target_val = scale_brightness_linear(oplus_bright, hw_min, hw_max, input_min, input_max); break;
                    case 2: target_val = scale_brightness_custom(oplus_bright, hw_min, hw_max, input_min, input_max); break;
                    default: target_val = scale_brightness_curved(oplus_bright, hw_min, hw_max, input_min, input_max); break;
                }

                if (current_val != target_val) {
                    int diff = target_val - current_val;
                    int step = diff / 4;
                    if (diff != 0 && step == 0) {
                        step = (diff > 0) ? 1 : -1;
                    }
                    
                    current_val += step;
                    
                    if ((step > 0 && current_val > target_val) || (step < 0 && current_val < target_val)) {
                        current_val = target_val;
                    }

                    write_brightness(fd, current_val, &last_val, dbg);
                }
            }
        } else {
            if (dbg) LOGE("[DisplayPanel Mode] Failed to read from %s", OPLUS_BRIGHT_PATH);
        }
        
        usleep(33000); // 33ms
    }
}

void run_default_mode() {
    bool dbg = dbg_on();
    if (dbg) LOGD("[DisplayAdaptor] Starting in Default Mode...");
    
    bool is_float = is_float_mode();
    int mode = get_brightness_mode();
    bool is_lux_aod = is_lux_aod_mode();
    
    if (dbg) { 
        const char* mode_str = (mode == 1) ? "Linear" : ((mode == 2) ? "Custom" : "Curved");
        LOGD("[Default Mode] Mode: %s, Lux AOD: %d", mode_str, is_lux_aod); 
    }

    int hw_min = get_min_brightness(dbg);
    int hw_max = get_max_brightness(dbg);

    BrightnessRange range = init_brightness_range();
    refresh_brightness_range(&range);
    
    if (dbg) LOGD("[Default Mode] IR locked: min=%d, max=%d", range.min, range.max);

    int fd = open(BRIGHT_PATH, O_WRONLY);
    if (fd < 0) {
        LOGE("[Default Mode] Could not open brightness file");
        return;
    }

    int last_val = -1;
    int prev_state = get_screen_state();
    int prev_bright = get_prop_brightness(&range, is_float);
    
    if (prev_bright == -1) { 
        if (dbg) LOGD("[DisplayAdaptor] Initial brightness is 0, using fallback.");
        prev_bright = FALLBACK_MIN; 
    }
    
    int initial;
    switch(mode) {
        case 1: initial = scale_brightness_linear(prev_bright, hw_min, hw_max, range.min, range.max); break;
        case 2: initial = scale_brightness_custom(prev_bright, hw_min, hw_max, range.min, range.max); break;
        default: initial = scale_brightness_curved(prev_bright, hw_min, hw_max, range.min, range.max); break;
    }
    
    write_brightness(fd, initial, &last_val, dbg);

    bool is_ips = is_ips_mode();
    if (dbg) LOGD("[Default Mode] IPS Mode: %d", is_ips);

    while (true) {
        int cur_state = get_screen_state();
        int raw_bright = get_prop_brightness(&range, is_float);
        
        int cur_bright = (raw_bright == -1) ? prev_bright : raw_bright;

        int current_mode = get_brightness_mode();

        if (cur_bright != prev_bright || cur_state != prev_state) {
            int val_to_write = last_val;

            if (cur_state == 2) {
                if (prev_state != 2) usleep(100000); // 100ms
                switch(current_mode) {
                    case 1: val_to_write = scale_brightness_linear(cur_bright, hw_min, hw_max, range.min, range.max); break;
                    case 2: val_to_write = scale_brightness_custom(cur_bright, hw_min, hw_max, range.min, range.max); break;
                    default: val_to_write = scale_brightness_curved(cur_bright, hw_min, hw_max, range.min, range.max); break;
                }
            } else if (is_ips) {
                if (dbg) LOGD("[DisplayAdaptor] IPS Mode: State is %d (OFF), setting brightness 0", cur_state);
                val_to_write = BRIGHTNESS_OFF;
            } else {
                if (cur_state == 0 || cur_state == 1) {
                    if (dbg) LOGD("[DisplayAdaptor] State is %d (OFF), setting brightness 0", cur_state);
                    val_to_write = BRIGHTNESS_OFF;
                } else if (cur_state == 3 || cur_state == 4) {
                    bool is_panoramic = is_panoramic_aod_enabled(dbg);
                    
                    if (is_lux_aod && is_panoramic) {
                         int target_lux = get_prop_int(PERSIST_LUX_AOD_BRIGHTNESS_PROP, -1);
                         if (target_lux > 0) {
                             if (dbg) LOGD("[DisplayAdaptor] Lux+Panoramic AOD active. Forcing brightness: %d", target_lux);
                             val_to_write = target_lux;
                         } else {
                             if (dbg) LOGD("[DisplayAdaptor] Lux+Panoramic AOD active but prop empty/0. Maintaining last value.");
                             val_to_write = last_val;
                         }
                    } else if (cur_state == 3 && is_lux_aod) {
                        char raw_prop[PROP_VALUE_MAX] = {0};
                        get_prop("debug.tracing.screen_brightness", raw_prop);
                        
                        if (strncmp(raw_prop, "2937.773", 8) == 0) {
                            int lux_val = get_prop_int(PERSIST_LUX_AOD_BRIGHTNESS_PROP, 1);
                            if (dbg) LOGD("[DisplayAdaptor] Lux AOD: Detected, forcing brightness to %d", lux_val);
                            val_to_write = lux_val;
                        } else {
                            if (dbg) LOGD("[DisplayAdaptor] State is 3 (Doze) & Lux AOD ON: Updating brightness: %d", cur_bright);
                            switch(current_mode) {
                                case 1: val_to_write = scale_brightness_linear(cur_bright, hw_min, hw_max, range.min, range.max); break;
                                case 2: val_to_write = scale_brightness_custom(cur_bright, hw_min, hw_max, range.min, range.max); break;
                                default: val_to_write = scale_brightness_curved(cur_bright, hw_min, hw_max, range.min, range.max); break;
                            }
                        }
                    } else if (is_panoramic) {
                        if (dbg) LOGD("[DisplayAdaptor] State is %d Panoramic AOD is ON, skipping brightness write", cur_state);
                        val_to_write = last_val;
                    } else {
                        if (dbg) LOGD("[DisplayAdaptor] State is %d Panoramic AOD is OFF, setting brightness 0", cur_state);
                        val_to_write = BRIGHTNESS_OFF;
                    }
                } else if (prev_state == 2) {
                    if (is_panoramic_aod_enabled(dbg)) {
                        if (dbg) LOGD("[DisplayAdaptor] Transitioned from ON with Panoramic AOD, deferring brightness 0");
                        val_to_write = last_val;
                    } else {
                        if (dbg) LOGD("[DisplayAdaptor] Transitioned from ON without Panoramic AOD, setting brightness 0");
                        val_to_write = BRIGHTNESS_OFF;
                    }
                } else {
                    val_to_write = last_val;
                }
            }

            if (val_to_write != last_val) {
                write_brightness(fd, val_to_write, &last_val, dbg);
            }
        }

        prev_bright = cur_bright;
        prev_state = cur_state;
        usleep(100000); // 100ms
    }
}

// ============================================================================
// MAIN ENTRY
// ============================================================================
int main(int argc, char **argv) {
    if (is_oplus_panel_mode()) {
        run_oplus_panel_mode();
    } else {
        run_default_mode();
    }
    return 0;
}