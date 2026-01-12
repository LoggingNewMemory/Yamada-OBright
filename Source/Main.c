#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <stdbool.h> 

#define LOG_TAG "OplusBrightFix"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* * REMOVED conflicting extern declarations. 
 * The NDK <sys/system_properties.h> already handles this.
 */

#define PATH_BRIGHTNESS "/sys/class/leds/lcd-backlight/brightness"
#define PATH_MAX_BRIGHT "/sys/class/leds/lcd-backlight/max_hw_brightness"
#define PATH_MIN_BRIGHT "/sys/class/leds/lcd-backlight/min_brightness"

#define PROP_BRIGHTNESS_IN "debug.tracing.screen_brightness"
#define PROP_STATE_IN      "debug.tracing.screen_state"
#define PROP_IS_FLOAT      "persist.sys.yamada.brightness.isfloat"
#define PROP_DISPLAY_TYPE  "persist.sys.yamada.display.type" 
#define PROP_RANGE_MIN     "persist.sys.yamada.multibrightness.min"
#define PROP_RANGE_MAX     "persist.sys.yamada.multibrightness.max"

#define FALLBACK_MIN 1
#define FALLBACK_MAX 2047
#define DEFAULT_INPUT_MIN 1
#define DEFAULT_INPUT_MAX 8192

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

struct GlobalState {
    int hw_fd;
    int hw_min;
    int hw_max;
    
    int input_min;
    int input_max;
    
    int current_input_val;
    int current_state;
    
    int last_written_val;
    
    int is_float_mode;
    int is_ips_mode;
} g_state;

int read_file_int(const char* path, int default_val) {
    FILE *f = fopen(path, "r");
    if (!f) return default_val;
    int val = default_val;
    fscanf(f, "%d", &val);
    fclose(f);
    return val;
}

int get_prop_int(const char* key, int default_val) {
    char buf[PROP_VALUE_MAX];
    if (__system_property_get(key, buf) > 0) {
        if (strlen(buf) > 0) return atoi(buf);
    }
    return default_val;
}

int is_panoramic_aod() {
    FILE *fp = popen("settings get secure panoramic_aod_enable", "r");
    if (fp == NULL) return 0;
    
    char output[16] = {0};
    if (fgets(output, sizeof(output), fp) != NULL) {
        pclose(fp);
        return atoi(output) == 1;
    }
    pclose(fp);
    return 0;
}

int calculate_curve(int val) {
    int in_min = g_state.input_min;
    int in_max = g_state.input_max;
    int hw_min = g_state.hw_min;
    int hw_max = g_state.hw_max;

    if (val <= in_min) return hw_min;
    if (val >= in_max) return hw_max;

    long long percent = (long long)(val - in_min) * 100 / (in_max - in_min);
    long long scaled_percent = 0;

    if (percent <= 70) {
        scaled_percent = 1 + (56 * percent / 70);
    } else if (percent <= 90) {
        scaled_percent = 57 + (197 * (percent - 70) / 20);
    } else {
        scaled_percent = 254 + (257 * (percent - 90) / 10);
    }

    long long result = hw_min + (scaled_percent * (hw_max - hw_min) / 511);

    if (result < hw_min) return hw_min;
    if (result > hw_max) return hw_max;
    return (int)result;
}

void write_hw_brightness(int val) {
    if (val == g_state.last_written_val) return;

    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d\n", val);
    
    if (g_state.hw_fd >= 0) {
        write(g_state.hw_fd, buf, len);
    }
    g_state.last_written_val = val;
}

void update_display() {
    pthread_mutex_lock(&lock);

    int raw_bright = g_state.current_input_val;
    int state = g_state.current_state;
    int target = 0;

    if (state == 2) { 
        target = calculate_curve(raw_bright);
    } 
    else if (g_state.is_ips_mode) {
        target = 0;
    } 
    else if (state == 0 || state == 1) {
        target = 0;
    } 
    else if (state == 3 || state == 4) {
        if (is_panoramic_aod()) {
            target = g_state.last_written_val; 
        } else {
            target = 0;
        }
    } 
    else {
        target = g_state.last_written_val;
    }

    write_hw_brightness(target);
    pthread_mutex_unlock(&lock);
}

void* thread_brightness_monitor(void* arg) {
    const prop_info *pi = NULL;
    
    while (!pi) {
        pi = __system_property_find(PROP_BRIGHTNESS_IN);
        if (!pi) sleep(1);
    }

    unsigned int serial = 0;
    char value[PROP_VALUE_MAX];

    while (1) {
        __system_property_wait(pi, serial, &serial, NULL);
        
        // __system_property_read returns int length in NDK
        if (__system_property_read(pi, NULL, value) > 0 && strlen(value) > 0) {
            int new_val = -1;
            
            pthread_mutex_lock(&lock);
            if (g_state.is_float_mode) {
                float f = strtof(value, NULL);
                if (f > 0.0) {
                    new_val = g_state.input_min + (int)(f * (g_state.input_max - g_state.input_min));
                }
            } else {
                new_val = atoi(value);
            }
            
            if (new_val >= 0) {
                g_state.current_input_val = new_val;
            }
            pthread_mutex_unlock(&lock);

            update_display();
        }
    }
    return NULL;
}

void* thread_state_monitor(void* arg) {
    const prop_info *pi = NULL;

    while (!pi) {
        pi = __system_property_find(PROP_STATE_IN);
        if (!pi) sleep(1);
    }

    unsigned int serial = 0;
    char value[PROP_VALUE_MAX];

    while (1) {
        __system_property_wait(pi, serial, &serial, NULL);
        
        if (__system_property_read(pi, NULL, value) > 0 && strlen(value) > 0) {
            int new_state = atoi(value);
            
            pthread_mutex_lock(&lock);
            g_state.current_state = new_state;
            pthread_mutex_unlock(&lock);

            update_display();
        }
    }
    return NULL;
}

int main() {
    LOGD("Service Starting...");

    g_state.hw_max = read_file_int(PATH_MAX_BRIGHT, FALLBACK_MAX);
    g_state.hw_min = read_file_int(PATH_MIN_BRIGHT, FALLBACK_MIN);
    g_state.input_max = get_prop_int(PROP_RANGE_MAX, DEFAULT_INPUT_MAX);
    g_state.input_min = get_prop_int(PROP_RANGE_MIN, DEFAULT_INPUT_MIN);
    
    char prop_buf[PROP_VALUE_MAX];
    
    if (__system_property_get(PROP_IS_FLOAT, prop_buf) > 0) {
        g_state.is_float_mode = (strcmp(prop_buf, "true") == 0);
    } else {
        g_state.is_float_mode = 0;
    }

    if (__system_property_get(PROP_DISPLAY_TYPE, prop_buf) > 0) {
        g_state.is_ips_mode = (strcmp(prop_buf, "IPS") == 0);
    } else {
        g_state.is_ips_mode = 0;
    }

    g_state.current_state = 2; 
    g_state.current_input_val = g_state.input_max / 2;
    g_state.last_written_val = -1;

    LOGD("Config: HW=[%d-%d] Input=[%d-%d] Float=%d IPS=%d", 
         g_state.hw_min, g_state.hw_max, g_state.input_min, g_state.input_max, 
         g_state.is_float_mode, g_state.is_ips_mode);

    g_state.hw_fd = open(PATH_BRIGHTNESS, O_WRONLY);
    if (g_state.hw_fd < 0) {
        LOGE("Failed to open brightness path: %s", PATH_BRIGHTNESS);
        return 1;
    }

    pthread_t t_bright, t_state;
    pthread_create(&t_bright, NULL, thread_brightness_monitor, NULL);
    pthread_create(&t_state, NULL, thread_state_monitor, NULL);

    pthread_join(t_bright, NULL);
    pthread_join(t_state, NULL);

    close(g_state.hw_fd);
    return 0;
}