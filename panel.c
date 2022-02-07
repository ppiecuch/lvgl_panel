#include "lvgl/lvgl.h"
#include "lv_lib_png/lv_png.h"
#ifdef __linux__
#include "lvgl/lv_drivers/display/fbdev.h"
#include "lvgl/lv_drivers/indev/evdev.h"
#else /* __linux__ */
#include "lvgl/lv_drivers/display/monitor.h"
#include "lvgl/lv_drivers/indev/mouse.h"
#include "lvgl/lv_drivers/indev/mousewheel.h"
#include "lvgl/lv_drivers/indev/keyboard.h"
#include <SDL2/SDL.h>
#endif /* __linux__ */
#include <confuse.h>
#include <cJSON.h>
#include <curl/curl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <libgen.h>

#define NORMAL_COLOR "\x1B[0m"
#define GREEN      "\x1B[32m"
#define BLUE       "\x1B[34m"
#define RED        "\x1B[31m"


LV_FONT_DECLARE(digital_clock)

// Config options
static char *openweather_apikey = NULL;
static char *openweather_label = NULL;
static double openweather_coord[2] = { 0, 0 };

// display buffer size - not sure if this size is really needed
#define LV_BUF_SIZE 384000 // 800x480

// A static variable to store the display buffers
static lv_disp_buf_t disp_buf;

// Static buffer(s). The second buffer is optional
static lv_color_t lvbuf1[LV_BUF_SIZE];
static lv_color_t lvbuf2[LV_BUF_SIZE];

// Display info and controls
static const char* DAY[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char* MONTH[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

static lv_style_t style_large, style_clock, style_gallery;
static const lv_task_t *time_task, *net_task, *gallery_task, *weather_task;
static const lv_font_t *font_large, *font_normal;

static lv_obj_t *clock_label[8];
static lv_obj_t *date_label;

static lv_obj_t *led1;
static lv_obj_t *controls_panel, *gallery_panel1, *gallery_panel2;

static char weatherString[1024] = { 0 };

// Utilities functions

# define _ssprintf(...)                                 \
    ({ int _ss_size = snprintf(0, 0, ##__VA_ARGS__);    \
    char *_ss_ret = (char*)alloca(_ss_size+1);          \
    snprintf(_ss_ret, _ss_size+1, ##__VA_ARGS__);       \
    _ss_ret; })

static void time_timer_cb(lv_task_t *timer) {

    char timeString[16] = { 0 };
    char dateString[64] = { 0 };

    time_t t = time(NULL);
    struct tm *local = localtime(&t);

    snprintf(timeString, 16, "%02d:%02d:%02d", local->tm_hour, local->tm_min, local->tm_sec);
    if (strlen(weatherString) > 0)
        snprintf(dateString, 64, "%s | %s %02d %04d | %s", DAY[local->tm_wday], MONTH[local->tm_mon], local->tm_mday, local->tm_year + 1900, weatherString);
    else
        snprintf(dateString, 64, "%s | %s %02d %04d", DAY[local->tm_wday], MONTH[local->tm_mon], local->tm_mday, local->tm_year + 1900);
    for (int c=0; c<8; c++) {
        const char str[2] = { timeString[c], 0 };
        lv_label_set_text(clock_label[c], str);
    }
    lv_label_set_text(date_label, dateString);
}

static int get_current_network_speed_cb() {

    static unsigned long int kb_sent = 0, kb_sent_prev = 0;

    FILE *fp = fopen("/proc/net/dev", "r");
    if (fp) {
        char buf[200], ifname[20];
        unsigned long int r_bytes, t_bytes, r_packets, t_packets;

        // skip first two lines
        for (int i = 0; i < 2; i++) {
            fgets(buf, 200, fp);
        }

        while (fgets(buf, 200, fp)) {
            sscanf(buf, "%[^:]: %lu %lu %*lu %*lu %*lu %*lu %*lu %*lu %lu %lu",
                ifname, &r_bytes, &r_packets, &t_bytes, &t_packets
            );
            if (strstr(ifname, "wlan0") != NULL) {
                kb_sent = r_bytes / 1024;
            }
        }

        unsigned long int net_speed = (kb_sent - kb_sent_prev) * 2;
        kb_sent_prev = kb_sent;

        fclose(fp);
        return net_speed;
    } else
        return -1;
}

static void net_timer_cb(lv_task_t *timer) {
    int net_speed = get_current_network_speed_cb();
    if (net_speed > 0) {
        lv_led_on(led1);
    } else
        lv_led_off(led1);
}

static void gallery_fill(lv_obj_t *gallery_panel) {
    static time_t _last_mtime = 0;
    static char **_cache = NULL;
    static int _index = 0;

    if (!_cache) {
        printf("%s[INFO]%s Reload gallery cache\n", GREEN, NORMAL_COLOR);
        DIR *d = opendir("gallery");
        if (d) {
            int count = 0, index = 0, alloc = 2048, cache_alloc = 512;
            _cache = malloc(cache_alloc * sizeof(char*));
            char *ptr = malloc(alloc);
            struct dirent *dir;
            while ((dir = readdir(d)) != NULL) {
                if (dir->d_type == DT_REG) {
                    if (strstr(dir->d_name, ".png") != NULL) {
                        if (index + strlen(dir->d_name) + 1 >= alloc) {
                            alloc += 2048;
                            ptr = realloc(ptr, alloc);
                        }
                        strcpy(ptr + index, dir->d_name);
                        _cache[count] = ptr + index;
                        index += strlen(dir->d_name) + 1;
                        count++;
                        if (count == cache_alloc) {
                            cache_alloc += 128;
                            _cache = realloc(_cache, cache_alloc * sizeof(char*));
                        }
                    }
                }
            }
            _cache[count] = 0; // last entry
            closedir(d);
            printf("%s[INFO]%s Cached %d entries\n", GREEN, NORMAL_COLOR, count);
        }
    }

    if (_cache) {
        lv_obj_t *img = lv_obj_get_child(gallery_panel, NULL);
        while (img) {
            if (rand()%10 > 6) {
                if (!_cache[_index])
                    _index = 0;
                if (_cache[_index]) {
                    lv_img_set_src(img, _ssprintf("gallery/%s", _cache[_index]));
                    _index++;
                }
                img = lv_obj_get_child(gallery_panel, img);
            }
        }
    }

    struct stat attr;
    if (stat("gallery", &attr) == 0)
        _last_mtime = attr.st_mtime;
}

static void gallery_timer_cb(lv_task_t *timer) {
    static lv_obj_t *gallery_panel = NULL;

    if (gallery_panel == gallery_panel1)
        gallery_panel = gallery_panel2;
    else
        gallery_panel = gallery_panel1;

    gallery_fill(gallery_panel);
}

static size_t round_up(size_t v) {
    if (v == 0)
        return 0;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    /* 64 bit only */
#if SIZE_MAX > 4294967296
        v |= v >> 32;
#endif
    return ++v;
}

struct _mem_chunk {
    char *buf;
    size_t size;
    CURLcode res;
    bool busy;
};

static size_t _curl_write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    const size_t contents_size = size * nmemb;
    struct _mem_chunk *chunk = (struct _mem_chunk*)userp;

    /* realloc can be slow, therefore increase buffer to nearest 2^n */
    chunk->buf = realloc(chunk->buf, round_up(chunk->size + contents_size));
    if (!chunk->buf)
        return 0;
    /* append data and increment size */
    memcpy(chunk->buf + chunk->size, contents, contents_size);
    chunk->size += contents_size;
    chunk->buf[chunk->size] = 0; // zero-termination
    return contents_size;
}

static void *fetch_weather_api(void *thread_data) {
    const char *URL_BASE = "https://api.openweathermap.org/data/2.5/onecall?lat=%g&lon=%g&units=metric&appid=%s";

    struct _mem_chunk *chunk = (struct _mem_chunk*)thread_data;
    chunk->busy = true;
    chunk->size = 0;
    chunk->res = CURLE_OK;
    CURL *curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, _ssprintf(URL_BASE, openweather_apikey, openweather_coord[0], openweather_coord[1], openweather_apikey));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _curl_write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, chunk);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "libcurl-agent/1.0/picture-frame");
        chunk->res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        if (chunk->buf) {
            printf("%s[INFO]%s Downloaded %lu bytes\n", GREEN, NORMAL_COLOR, strlen(chunk->buf));
            cJSON *json = cJSON_Parse(chunk->buf);
            if (json) {
                cJSON *cod = cJSON_GetObjectItemCaseSensitive(json, "cod");
                if (cod) {
                    switch (cod->valueint) {
                        case 401: {
                            cJSON *message = cJSON_GetObjectItemCaseSensitive(json, "message");
                            if (cJSON_IsString(message)) {
                                printf("%s[ERROR]%s Request error: %s\n", RED, NORMAL_COLOR, message->valuestring);
                            }
                        } break;
                        default: {
                            printf("%s[ERROR]%s Unknown JSON data: %s\n", RED, NORMAL_COLOR, chunk->buf);
                        }
                    }
                } else {
                    strcpy(weatherString, "");
                    cJSON *current = cJSON_GetObjectItemCaseSensitive(json, "current");
                    if (current) {
                        cJSON *temp = cJSON_GetObjectItemCaseSensitive(current, "temp");
                        if (cJSON_IsNumber(temp)) {
                            strcat(weatherString, _ssprintf("Temp. %d C", temp->valueint));
                        }
                    } else
                        printf("%s[ERROR]%s Unknown JSON data: %s\n", RED, NORMAL_COLOR, chunk->buf);
                }
            } else
                printf("%s[ERROR]%s Failed to parse JSON data: %s\n", RED, NORMAL_COLOR, chunk->buf);
        }
    }
    chunk->busy = false;
	return NULL;
}

struct _mem_chunk weather_info = { NULL, 0, 0, false };

static void weather_timer_cb(lv_task_t *timer) {
    if (weather_info.busy) {
        printf("%s[INFO]%s Download already in progress\n", GREEN, NORMAL_COLOR);
        return;
    }
    pthread_t thread;
    if (pthread_create(&thread, NULL, fetch_weather_api, &weather_info))
        printf("%s[ERROR]%s Couldn't create a thread.\n", RED, NORMAL_COLOR);
    pthread_detach(thread);
}

//  Main entry

static void panel_init(char *prog_name) {

    font_large =  &lv_font_montserrat_24;
    font_normal =  &lv_font_montserrat_16;

#if LV_USE_THEME_MATERIAL
    if(LV_THEME_DEFAULT_INIT == lv_theme_material_init) {
        LV_THEME_DEFAULT_INIT(lv_theme_get_color_primary(), lv_theme_get_color_primary(),
            LV_THEME_MATERIAL_FLAG_LIGHT,
            lv_theme_get_font_small(), lv_theme_get_font_normal(), lv_theme_get_font_subtitle(), lv_theme_get_font_title());
    }
#endif

    lv_style_init(&style_large);
    lv_style_set_text_font(&style_large, LV_STATE_DEFAULT, font_large);

    lv_style_init(&style_clock);
    lv_style_set_text_font(&style_clock, LV_STATE_DEFAULT, &digital_clock);

    lv_obj_t *scr = lv_scr_act();

    // Open configuration file

    cfg_opt_t opts[] = {
        CFG_SIMPLE_STR("openweather_apikey", &openweather_apikey),
        CFG_SIMPLE_STR("openweather_label", &openweather_label),
        CFG_FLOAT_LIST("openweather_coord", "{0, 0}", CFGF_NONE),
        CFG_END()
    };
    cfg_t *cfg = cfg_init(opts, 0);
    if(cfg_parse(cfg, _ssprintf("%s.ini", basename(prog_name))) == CFG_PARSE_ERROR)
        printf("%s[ERROR]%s Couldn't open configuration file.\n", RED, NORMAL_COLOR);
    openweather_coord[0] = cfg_getnfloat(cfg, "openweather_coord", 0);
    openweather_coord[1] = cfg_getnfloat(cfg, "openweather_coord", 1);
    cfg_free(cfg);

    // Time/date controls

    controls_panel = lv_cont_create(scr, NULL);
    lv_obj_set_pos(controls_panel, 0, 0);
    lv_obj_set_size(controls_panel, lv_obj_get_width(scr), 150);

    const int gl_h = 118, gl_w = 71;
    int x_off = (lv_obj_get_width(scr) - 8 * gl_w) / 2;
    for (int c=0; c<8; c++,x_off+=gl_w) {
        clock_label[c] = lv_label_create(controls_panel, NULL);
        lv_obj_set_pos(clock_label[c], x_off, 0);
        lv_obj_set_size(clock_label[c], gl_w, gl_h);
        lv_obj_add_style(clock_label[c], LV_LABEL_PART_MAIN, &style_clock);
        lv_label_set_text(clock_label[c], "");
        lv_label_set_long_mode(clock_label[c], LV_LABEL_LONG_EXPAND);
    }

    date_label = lv_label_create(controls_panel, NULL);
    lv_obj_set_pos(date_label, 0, gl_h + 2);
    lv_obj_set_size(date_label, lv_obj_get_width(controls_panel), 25);
    lv_label_set_text(date_label, "");
    lv_obj_add_style(date_label, LV_LABEL_PART_MAIN, &style_large);
    lv_label_set_long_mode(date_label, LV_LABEL_LONG_EXPAND);

    led1 = lv_led_create(controls_panel, NULL);
    lv_obj_set_pos(led1, 785, 1);
    lv_obj_set_size(led1, 14, 14);
    lv_led_off(led1);

    // Gallery panel

    const int gallery_height = (lv_obj_get_height(scr) - lv_obj_get_height(controls_panel)) / 2;

    gallery_panel1 = lv_cont_create(scr, NULL);
    lv_obj_set_pos(gallery_panel1, 0, lv_obj_get_height(controls_panel));
    lv_obj_set_size(gallery_panel1, lv_obj_get_width(scr), gallery_height);
    lv_obj_set_auto_realign(gallery_panel1, true);                    /*Auto realign when the size changes*/
    lv_cont_set_layout(gallery_panel1, LV_LAYOUT_ROW_TOP);

    gallery_panel2 = lv_cont_create(scr, NULL);
    lv_obj_set_pos(gallery_panel2, 0, lv_obj_get_height(controls_panel) + gallery_height);
    lv_obj_set_size(gallery_panel2, lv_obj_get_width(scr), gallery_height);
    lv_obj_set_auto_realign(gallery_panel2, true);                    /*Auto realign when the size changes*/
    lv_cont_set_layout(gallery_panel2, LV_LAYOUT_ROW_TOP);

    lv_style_init(&style_gallery);
    lv_style_set_border_width(&style_gallery, LV_STATE_DEFAULT , 0);
    lv_style_set_pad_top(&style_gallery, LV_STATE_DEFAULT, 0);
    lv_style_set_pad_bottom(&style_gallery, LV_STATE_DEFAULT, 0);
    lv_style_set_pad_left(&style_gallery, LV_STATE_DEFAULT, 0);
    lv_style_set_pad_right(&style_gallery, LV_STATE_DEFAULT, 0);
    lv_style_set_pad_inner(&style_gallery, LV_STATE_DEFAULT, 0);

    lv_obj_add_style(gallery_panel1, LV_CONT_PART_MAIN, &style_gallery);
    lv_obj_add_style(gallery_panel2, LV_CONT_PART_MAIN, &style_gallery);

    printf("%s[INFO]%s Gallery panel is: %d x %d\n",
        GREEN, NORMAL_COLOR,
        lv_obj_get_width(gallery_panel1), lv_obj_get_height(gallery_panel1));

    for (int i=0; i<10; i++) {
        // image placeholders
        lv_img_create(gallery_panel1, NULL);
        lv_img_create(gallery_panel2, NULL);
    }

    gallery_fill(gallery_panel1);
    gallery_fill(gallery_panel2);

    weather_timer_cb(NULL);

    time_task = lv_task_create(time_timer_cb, 1000, LV_TASK_PRIO_MID, NULL);
    net_task = lv_task_create(net_timer_cb, 3000, LV_TASK_PRIO_LOW, NULL);
    gallery_task = lv_task_create(gallery_timer_cb, 10000, LV_TASK_PRIO_LOW, NULL);
    weather_task = lv_task_create(weather_timer_cb, 5*60000, LV_TASK_PRIO_LOW, NULL);
}

#ifdef __linux__
static void hal_init() {
    fbdev_init(); //Linux frame buffer device init
    evdev_init(); // Touch pointer device init

    // Initialize `disp_buf` with the display buffer(s)
    lv_disp_buf_init(&disp_buf, lvbuf1, lvbuf2, LV_BUF_SIZE);

    // Initialize and register a display driver
    lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = fbdev_flush;	// flushes the internal graphical buffer to the frame buffer
    disp_drv.buffer = &disp_buf;		// set teh display buffere reference in the driver
    lv_disp_drv_register(&disp_drv);

    // Initialize and register a pointer device driver
    lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = evdev_read;    // defined in lv_drivers/indev/evdev.h
    lv_indev_drv_register(&indev_drv);
}

#else /* __linux__ */

// A task to measure the elapsed time for LVGL
static int tick_thread(void *data) {
    (void)data;
    while(1) {
        SDL_Delay(5);
        lv_tick_inc(5); /* Tell LittelvGL that 5 milliseconds were elapsed */
    }
    return 0;
}

static void hal_init() {
    /* Use the 'monitor' driver which creates window on PC's monitor to simulate a display*/
    monitor_init();
    /* Tick init.
    * You have to call 'lv_tick_inc()' in periodically to inform LittelvGL about
    * how much time were elapsed Create an SDL thread to do this */
    SDL_CreateThread(tick_thread, "tick", NULL);

    /*Create a display buffer*/
    lv_disp_buf_init(&disp_buf, lvbuf1, lvbuf2, LV_BUF_SIZE);

    /*Create a display*/
    lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = monitor_flush;	// flushes the internal graphical buffer to the frame buffer
    disp_drv.buffer = &disp_buf;		// set teh display buffere reference in the driver
    lv_disp_drv_register(&disp_drv);
    disp_drv.antialiasing = 1;

    lv_group_t * g = lv_group_create();

    /* Add the mouse as input device
    * Use the 'mouse' driver which reads the PC's mouse*/
    mouse_init();
    static lv_indev_drv_t indev_drv_1;
    lv_indev_drv_init(&indev_drv_1); /*Basic initialization*/
    indev_drv_1.type = LV_INDEV_TYPE_POINTER;

    /*This function will be called periodically (by the library) to get the mouse position and state*/
    indev_drv_1.read_cb = mouse_read;
    lv_indev_drv_register(&indev_drv_1);

    keyboard_init();
    static lv_indev_drv_t indev_drv_2;
    lv_indev_drv_init(&indev_drv_2); /*Basic initialization*/
    indev_drv_2.type = LV_INDEV_TYPE_KEYPAD;
    indev_drv_2.read_cb = keyboard_read;
    lv_indev_t *kb_indev = lv_indev_drv_register(&indev_drv_2);
    lv_indev_set_group(kb_indev, g);
    mousewheel_init();
    static lv_indev_drv_t indev_drv_3;
    lv_indev_drv_init(&indev_drv_3); /*Basic initialization*/
    indev_drv_3.type = LV_INDEV_TYPE_ENCODER;
    indev_drv_3.read_cb = mousewheel_read;

    lv_indev_t * enc_indev = lv_indev_drv_register(&indev_drv_3);
    lv_indev_set_group(enc_indev, g);
}
#endif /* __linux__ */

int main(int argc, char *argv[]) {

    lv_init(); // LittlevGL init
    lv_png_init(); // Png file support

    hal_init();

    // Panel initialization
    panel_init(argv[0]);

    // Handle LitlevGL tasks (tickless mode)
    while(1) {
        lv_tick_inc(5);
        lv_task_handler();
        usleep(5000);
    }
    return 0;
}
