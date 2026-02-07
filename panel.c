#include "lv_lib_png/lv_png.h"
#include "lvgl/lvgl.h"
#ifdef __linux__
#include "lvgl/lv_drivers/display/fbdev.h"
#include "lvgl/lv_drivers/indev/evdev.h"
#else /* __linux__ */
#include "lvgl/lv_drivers/display/monitor.h"
#include "lvgl/lv_drivers/indev/keyboard.h"
#include "lvgl/lv_drivers/indev/mouse.h"
#include "lvgl/lv_drivers/indev/mousewheel.h"
#include <SDL2/SDL.h>
#endif /* __linux__ */
#include <cJSON.h>
#include <confuse.h>
#include <curl/curl.h>
#include <dirent.h>
#include <libgen.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "version.h"

#define NORMAL_COLOR "\x1B[0m"
#define GREEN "\x1B[32m"
#define BLUE "\x1B[34m"
#define RED "\x1B[31m"

LV_FONT_DECLARE(digital_clock)

// Config options
static char *openweather_apikey = NULL;
static char *openweather_label = NULL;
static double openweather_coord[2] = { 0, 0 };
static long scroll_step = 27;

// display buffer size - not sure if this size is really needed
#define LV_BUF_SIZE 384000 // 800x480

// A static variable to store the display buffers
static lv_disp_buf_t disp_buf;

// Static buffer(s). The second buffer is optional
static lv_color_t lvbuf1[LV_BUF_SIZE];
static lv_color_t lvbuf2[LV_BUF_SIZE];

// Display info and controls
static const char *DAY[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *MONTH[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };

static lv_style_t style_large, style_clock, style_gallery;
static const lv_task_t *time_task, *net_task, *weather_task;
static const lv_font_t *font_large, *font_normal;

static lv_obj_t *clock_label[8];
static lv_obj_t *date_label, *weather_label;

static lv_obj_t *led1;
static lv_style_t style_led_green, style_led_red;
static lv_obj_t *version_label, *fps_label;
static lv_obj_t *plot_chart;
static lv_chart_series_t *fps_series, *pxs_series;
#define PLOT_POINTS 60
static lv_obj_t *controls_panel, *gallery_panel;

static char weatherString[64] = { 0 };

// Utilities functions

#define _ssprintf(...) \
	({ int _ss_size = snprintf(0, 0, ##__VA_ARGS__);    \
    char *_ss_ret = (char*)alloca(_ss_size+1);          \
    snprintf(_ss_ret, _ss_size+1, ##__VA_ARGS__);       \
    _ss_ret; })

static void time_timer_cb(lv_task_t *timer) {
	char timeString[16] = { 0 };
	char dateString[128] = { 0 };

	time_t t = time(NULL);
	struct tm *local = localtime(&t);

	snprintf(timeString, 16, "%02d:%02d:%02d", local->tm_hour, local->tm_min, local->tm_sec);
	for (int c = 0; c < 8; c++) {
		const char str[2] = { timeString[c], 0 };
		lv_label_set_text(clock_label[c], str);
	}

	if (strlen(weatherString) > 0)
		snprintf(dateString, 128, "%s | %s %02d %04d | ", DAY[local->tm_wday], MONTH[local->tm_mon], local->tm_mday, local->tm_year + 1900);
	else
		snprintf(dateString, 128, "%s | %s %02d %04d", DAY[local->tm_wday], MONTH[local->tm_mon], local->tm_mday, local->tm_year + 1900);
	lv_label_set_text(date_label, dateString);

	lv_obj_set_x(weather_label, lv_obj_get_width(date_label));
	lv_obj_set_width(weather_label, lv_obj_get_width(controls_panel) - lv_obj_get_width(date_label));
	lv_label_set_text(weather_label, weatherString);
}

static int get_current_network_speed_cb() {
	static unsigned long int kb_sent = 0, kb_sent_prev = 0;
	static bool first_call = true;

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
					ifname, &r_bytes, &r_packets, &t_bytes, &t_packets);
			if (strstr(ifname, "wlan0") != NULL) {
				kb_sent = r_bytes / 1024;
			}
		}

		fclose(fp);

		if (first_call) {
			kb_sent_prev = kb_sent;
			first_call = false;
			return 0;
		}

		unsigned long int net_speed = (kb_sent - kb_sent_prev) * 2;
		kb_sent_prev = kb_sent;

		return net_speed;
	} else
		return -1;
}

static void net_timer_cb(lv_task_t *timer) {
	int net_speed = get_current_network_speed_cb();
	if (net_speed < 0) {
		// Network unavailable
		lv_obj_add_style(led1, LV_LED_PART_MAIN, &style_led_red);
		lv_led_on(led1);
	} else if (net_speed > 0) {
		// Active traffic
		lv_obj_add_style(led1, LV_LED_PART_MAIN, &style_led_green);
		lv_led_on(led1);
	} else {
		// Network present but idle
		lv_obj_add_style(led1, LV_LED_PART_MAIN, &style_led_green);
		lv_led_off(led1);
	}
}

// Gallery cache and width-group index
static intptr_t *g_cache = NULL;   // string offsets into g_strings
static char *g_strings = NULL;     // packed filenames
static int g_count = 0;
static time_t g_last_mtime = 0;

// Width group: images sharing the same pixel width
#define MAX_WIDTH_GROUPS 32
struct width_group {
	int width;
	int count;
	int *indices;  // indices into g_cache
};
static struct width_group g_groups[MAX_WIDTH_GROUPS];
static int g_num_groups = 0;

// Infinite horizontal scroll gallery
#define RESHUFFLE_EVERY_N_LOOPS  3
#define STRIP_MAX_IMGS 256

struct strip_img {
	int cache_idx;   // index into g_cache for the filename
	int base_x;      // logical X position in the full strip
	int width;       // pixel width of this image
	lv_obj_t *obj;   // non-NULL only while visible (materialized)
};
static struct strip_img g_strip[STRIP_MAX_IMGS];
static int g_strip_count = 0;
static int g_total_strip_w = 0;
static int g_scroll_offset = 0;
static int g_loop_count = 0;
static lv_task_t *g_scroll_task = NULL;
static struct timespec g_scroll_start;

static struct width_group *find_width_group(int width) {
	for (int i = 0; i < g_num_groups; i++) {
		if (g_groups[i].width == width)
			return &g_groups[i];
	}
	return NULL;
}

static int pick_random_from_group(struct width_group *grp, int exclude_idx) {
	if (grp->count <= 1)
		return grp->indices[0];
	int pick;
	do {
		pick = grp->indices[rand() % grp->count];
	} while (pick == exclude_idx && grp->count > 1);
	return pick;
}

static void gallery_build_strip(lv_obj_t *panel);
static void gallery_start_scroll(void);

static void gallery_update_positions(int offset) {
	int panel_w = lv_obj_get_width(gallery_panel);
	for (int i = 0; i < g_strip_count; i++) {
		int x = g_strip[i].base_x + offset;
		// Wrap into [0, g_total_strip_w)
		x = ((x % g_total_strip_w) + g_total_strip_w) % g_total_strip_w;
		// Allow negative x so images exiting left stay until right edge is off-screen
		if (x > panel_w)
			x -= g_total_strip_w;
		bool visible = (x + g_strip[i].width > 0) && (x < panel_w);
		if (visible) {
			if (!g_strip[i].obj) {
				g_strip[i].obj = lv_img_create(gallery_panel, NULL);
				if (g_strip[i].obj) {
					lv_obj_set_click(g_strip[i].obj, false);
					lv_img_set_src(g_strip[i].obj,
						_ssprintf("gallery/%s", g_strings + g_cache[g_strip[i].cache_idx]));
				}
			}
			if (g_strip[i].obj)
				lv_obj_set_pos(g_strip[i].obj, x, 0);
		} else {
			if (g_strip[i].obj) {
				lv_obj_del(g_strip[i].obj);
				g_strip[i].obj = NULL;
			}
		}
	}
}

static void gallery_scroll_tick(lv_task_t *task) {
	(void)task;
	if (g_total_strip_w == 0)
		return;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	// Compute offset from real elapsed time
	int64_t elapsed_ms = (now.tv_sec - g_scroll_start.tv_sec) * 1000LL
		+ (now.tv_nsec - g_scroll_start.tv_nsec) / 1000000LL;

	// FPS & pixel speed counter
	static int frame_count = 0;
	static int prev_scroll_px = 0;
	static int px_moved_accum = 0;
	static struct timespec fps_prev = {0, 0};
	frame_count++;

	int raw_px = (int)((elapsed_ms * scroll_step) / 1000);
	if (raw_px >= prev_scroll_px)
		px_moved_accum += raw_px - prev_scroll_px;
	prev_scroll_px = raw_px;

	int64_t fps_elapsed = (now.tv_sec - fps_prev.tv_sec) * 1000LL
		+ (now.tv_nsec - fps_prev.tv_nsec) / 1000000LL;
	if (fps_elapsed >= 1000) {
		int measured_fps = frame_count;
		int measured_pxs = px_moved_accum;
		if (fps_label)
			lv_label_set_text(fps_label,
				_ssprintf("#00BBCC -# %d fps  #CC8800 -# %d px/s", measured_fps, measured_pxs));
		if (plot_chart) {
			lv_chart_set_next(plot_chart, fps_series, measured_fps);
			lv_chart_set_next(plot_chart, pxs_series, measured_pxs);
		}
		frame_count = 0;
		px_moved_accum = 0;
		fps_prev = now;
	}
	// offset in pixels (negative = scrolling left)
	int offset = -(int)((elapsed_ms * scroll_step) / 1000);
	// Normalize to one loop range
	int loop_offset = offset % g_total_strip_w;
	g_scroll_offset = loop_offset;
	gallery_update_positions(loop_offset);

	// Detect completed loops
	int loops = -(offset / g_total_strip_w);
	if (loops > g_loop_count) {
		g_loop_count = loops;
		printf("%s[INFO]%s Gallery scroll loop %d completed\n", GREEN, NORMAL_COLOR, g_loop_count);

		// Check for gallery directory changes
		struct stat attr;
		if (stat("gallery", &attr) == 0) {
			if (g_last_mtime && g_last_mtime < attr.st_mtime) {
				printf("%s[INFO]%s Gallery changed\n", GREEN, NORMAL_COLOR);
				exit(1);
			}
		}

		if (g_loop_count % RESHUFFLE_EVERY_N_LOOPS == 0) {
			printf("%s[INFO]%s Reshuffling gallery strip\n", GREEN, NORMAL_COLOR);
			gallery_build_strip(gallery_panel);
			gallery_start_scroll();
		}
	}
}

static void gallery_build_strip(lv_obj_t *panel) {
	if (g_count == 0)
		return;

	// Dispose any existing materialized objects
	for (int i = 0; i < g_strip_count; i++) {
		if (g_strip[i].obj) {
			lv_obj_del(g_strip[i].obj);
			g_strip[i].obj = NULL;
		}
	}
	g_strip_count = 0;
	g_total_strip_w = 0;

	// Shuffle indices
	int n = g_count < STRIP_MAX_IMGS ? g_count : STRIP_MAX_IMGS;
	int *indices = alloca(g_count * sizeof(int));
	for (int i = 0; i < g_count; i++)
		indices[i] = i;
	for (int i = g_count - 1; i > 0; i--) {
		int j = rand() % (i + 1);
		int tmp = indices[i];
		indices[i] = indices[j];
		indices[j] = tmp;
	}

	// Probe image widths with a temporary object
	lv_obj_t *probe = lv_img_create(panel, NULL);
	lv_obj_set_hidden(probe, true);

	int x = 0;
	for (int i = 0; i < n; i++) {
		int idx = indices[i];
		lv_img_set_src(probe, _ssprintf("gallery/%s", g_strings + g_cache[idx]));
		int w = lv_obj_get_width(probe);

		g_strip[i].cache_idx = idx;
		g_strip[i].base_x = x;
		g_strip[i].width = w;
		g_strip[i].obj = NULL;  // not materialized yet
		x += w;
	}
	lv_obj_del(probe);

	g_strip_count = n;
	g_total_strip_w = x;

	printf("%s[INFO]%s Built scroll strip: %d images, total width %dpx\n",
		GREEN, NORMAL_COLOR, g_strip_count, g_total_strip_w);

	// Materialize initially visible images
	gallery_update_positions(0);
}

static void gallery_start_scroll(void) {
	if (g_total_strip_w == 0)
		return;

	g_loop_count = 0;
	clock_gettime(CLOCK_MONOTONIC, &g_scroll_start);

	if (!g_scroll_task)
		g_scroll_task = lv_task_create(gallery_scroll_tick, 16, LV_TASK_PRIO_HIGH, NULL);

	printf("%s[INFO]%s Scroll started: speed %dpx/s, strip %dpx\n",
		GREEN, NORMAL_COLOR, (int)scroll_step, g_total_strip_w);
}

static void gallery_cache_load(void) {
	printf("%s[INFO]%s Reload gallery cache\n", GREEN, NORMAL_COLOR);
	DIR *d = opendir("gallery");
	if (!d)
		return;

	int count = 0, index = 0, alloc = 1024 * 10, cache_alloc = 1024;
	g_cache = calloc(cache_alloc, sizeof(intptr_t));
	g_strings = calloc(1, alloc);
	struct dirent *dir;
	while ((dir = readdir(d)) != NULL) {
		if (dir->d_type == DT_REG) {
			if (strstr(dir->d_name, ".png") != NULL) {
				if (index + (int)strlen(dir->d_name) + 2 >= alloc) {
					alloc += LV_MATH_MAX((int)strlen(dir->d_name) + 2, 2048);
					g_strings = realloc(g_strings, alloc);
				}
				strcpy(g_strings + index, dir->d_name);
				g_cache[count] = index;
				index += strlen(dir->d_name) + 1;
				count++;
				if (count == cache_alloc) {
					cache_alloc += 128;
					g_cache = realloc(g_cache, cache_alloc * sizeof(intptr_t));
				}
			}
		}
	}
	g_strings[index] = 0;
	g_count = count;
	closedir(d);

	struct stat attr;
	if (stat("gallery", &attr) == 0)
		g_last_mtime = attr.st_mtime;

	if (count == 0) {
		printf("%s[WARN]%s Gallery is missing\n", RED, NORMAL_COLOR);
		return;
	}
	printf("%s[INFO]%s Cached %d entries\n", GREEN, NORMAL_COLOR, count);
}

static void gallery_build_width_groups(lv_obj_t *panel) {
	// Temporarily load each image to discover its width
	lv_obj_t *probe = lv_img_create(panel, NULL);
	lv_obj_set_hidden(probe, true);

	// First pass: discover widths and assign group indices per image
	int *img_widths = calloc(g_count, sizeof(int));
	for (int i = 0; i < g_count; i++) {
		lv_img_set_src(probe, _ssprintf("gallery/%s", g_strings + g_cache[i]));
		img_widths[i] = lv_obj_get_width(probe);
	}
	lv_obj_del(probe);

	// Build groups
	g_num_groups = 0;
	for (int i = 0; i < g_count; i++) {
		int w = img_widths[i];
		struct width_group *grp = find_width_group(w);
		if (!grp) {
			if (g_num_groups >= MAX_WIDTH_GROUPS) continue;
			grp = &g_groups[g_num_groups++];
			grp->width = w;
			grp->count = 0;
			grp->indices = malloc(g_count * sizeof(int));
		}
		grp->indices[grp->count++] = i;
	}
	free(img_widths);

	printf("%s[INFO]%s Built %d width groups:", GREEN, NORMAL_COLOR, g_num_groups);
	for (int i = 0; i < g_num_groups; i++)
		printf(" %dpx(%d)", g_groups[i].width, g_groups[i].count);
	printf("\n");
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
	struct _mem_chunk *chunk = (struct _mem_chunk *)userp;

	/* realloc can be slow, therefore increase buffer to nearest 2^n */
	chunk->buf = realloc(chunk->buf, round_up(chunk->size + contents_size + 1));
	if (!chunk->buf)
		return 0;
	/* append data and increment size */
	memcpy(chunk->buf + chunk->size, contents, contents_size);
	chunk->size += contents_size;
	chunk->buf[chunk->size] = 0; // zero-termination
	return contents_size;
}

static void *fetch_weather_api(void *thread_data) {
	// https://openweathermap.org/one-call-transfer
	const char *URL_BASE = "https://api.openweathermap.org/data/3.0/onecall?lat=%g&lon=%g&units=metric&appid=%s";

	struct _mem_chunk *chunk = (struct _mem_chunk *)thread_data;
	chunk->busy = true;
	chunk->size = 0;
	chunk->res = CURLE_OK;
	CURL *curl = curl_easy_init();
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_URL, _ssprintf(URL_BASE, openweather_coord[0], openweather_coord[1], openweather_apikey));
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
					weatherString[0] = '\0';
					int ws_off = 0;
					cJSON *current = cJSON_GetObjectItemCaseSensitive(json, "current");
					if (current) {
						cJSON *temp = cJSON_GetObjectItemCaseSensitive(current, "temp");
						if (temp && cJSON_IsNumber(temp))
							ws_off += snprintf(weatherString + ws_off, sizeof(weatherString) - ws_off,
									"Temp. %d\x7f"
									"C",
									temp->valueint);
						cJSON *feels = cJSON_GetObjectItemCaseSensitive(current, "feels_like");
						if (feels && cJSON_IsNumber(feels) && ws_off < (int)sizeof(weatherString))
							ws_off += snprintf(weatherString + ws_off, sizeof(weatherString) - ws_off,
									" / Feels %d\x7f"
									"C",
									feels->valueint);
						cJSON *clouds = cJSON_GetObjectItemCaseSensitive(current, "clouds");
						if (clouds && cJSON_IsNumber(clouds) && ws_off < (int)sizeof(weatherString))
							ws_off += snprintf(weatherString + ws_off, sizeof(weatherString) - ws_off,
									" / Clouds %d%%", clouds->valueint);
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
	static int _lock_count = 0;
	if (weather_info.busy) {
		printf("%s[INFO]%s Download already in progress\n", GREEN, NORMAL_COLOR);
		_lock_count++;
		if (_lock_count > 3) {
			printf("%s[INFO]%s Download locked. Restarting.\n", GREEN, NORMAL_COLOR);
			exit(1);
		}
		return;
	}
	static pthread_t thread;
	if (pthread_create(&thread, NULL, fetch_weather_api, &weather_info))
		printf("%s[ERROR]%s Couldn't create a thread.\n", RED, NORMAL_COLOR);
	pthread_detach(thread);

	_lock_count = 0;
}

//  Main entry

static void panel_init(char *prog_name) {
	font_large = &lv_font_montserrat_24;
	font_normal = &lv_font_montserrat_16;

#if LV_USE_THEME_MATERIAL
	if (LV_THEME_DEFAULT_INIT == lv_theme_material_init) {
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
		CFG_SIMPLE_INT("scroll_step", &scroll_step),
		CFG_END()
	};
	cfg_t *cfg = cfg_init(opts, 0);
	if (cfg_parse(cfg, _ssprintf("%s.ini", basename(prog_name))) == CFG_PARSE_ERROR)
		printf("%s[ERROR]%s Couldn't open configuration file.\n", RED, NORMAL_COLOR);
	openweather_coord[0] = cfg_getnfloat(cfg, "openweather_coord", 0);
	openweather_coord[1] = cfg_getnfloat(cfg, "openweather_coord", 1);
	cfg_free(cfg);

	// Gallery panel

	gallery_panel = lv_cont_create(scr, NULL);
	lv_obj_set_pos(gallery_panel, 0, 0);
	lv_obj_set_size(gallery_panel, lv_obj_get_width(scr), lv_obj_get_height(scr) - 150);
	lv_obj_set_auto_realign(gallery_panel, true); /*Auto realign when the size changes*/
	lv_cont_set_layout(gallery_panel, LV_LAYOUT_OFF);

	lv_style_init(&style_gallery);
	lv_style_set_border_width(&style_gallery, LV_STATE_DEFAULT, 0);
	lv_style_set_pad_top(&style_gallery, LV_STATE_DEFAULT, 0);
	lv_style_set_pad_bottom(&style_gallery, LV_STATE_DEFAULT, 0);
	lv_style_set_pad_left(&style_gallery, LV_STATE_DEFAULT, 0);
	lv_style_set_pad_right(&style_gallery, LV_STATE_DEFAULT, 0);
	lv_style_set_pad_inner(&style_gallery, LV_STATE_DEFAULT, 0);
	lv_style_set_clip_corner(&style_gallery, LV_STATE_DEFAULT, true);

	lv_obj_add_style(gallery_panel, LV_CONT_PART_MAIN, &style_gallery);

	printf("%s[INFO]%s Gallery panel is: %d x %d\n",
			GREEN, NORMAL_COLOR,
			lv_obj_get_width(gallery_panel), lv_obj_get_height(gallery_panel));

	gallery_cache_load();
	gallery_build_width_groups(gallery_panel);
	gallery_build_strip(gallery_panel);
	gallery_start_scroll();
	weather_timer_cb(NULL);

	// Time/date controls

	controls_panel = lv_cont_create(scr, NULL);
	lv_obj_set_pos(controls_panel, 0, lv_obj_get_height(gallery_panel));
	lv_obj_set_size(controls_panel, lv_obj_get_width(scr), 150);

	// Version label (top-left of controls panel)
	static lv_style_t style_version;
	lv_style_init(&style_version);
	lv_style_set_text_font(&style_version, LV_STATE_DEFAULT, &lv_font_unscii_8);
	version_label = lv_label_create(controls_panel, NULL);
	lv_obj_set_pos(version_label, 4, 3);
	lv_obj_add_style(version_label, LV_LABEL_PART_MAIN, &style_version);
	lv_label_set_text(version_label, "v" PANEL_VERSION "\n" PANEL_BUILD_DATE "\n" PANEL_BUILD_HASH);

	fps_label = lv_label_create(controls_panel, NULL);
	lv_obj_set_pos(fps_label, 4, 30);
	lv_obj_add_style(fps_label, LV_LABEL_PART_MAIN, &style_version);
	lv_label_set_recolor(fps_label, true);
	lv_label_set_text(fps_label, "#00BBCC -# -- fps  #CC8800 -# -- px/s");

	// Performance plotter
	plot_chart = lv_chart_create(controls_panel, NULL);
	lv_obj_set_pos(plot_chart, 4, 42);
	lv_obj_set_size(plot_chart, 190, 75);
	lv_chart_set_type(plot_chart, LV_CHART_TYPE_LINE);
	lv_chart_set_point_count(plot_chart, PLOT_POINTS);
	lv_chart_set_range(plot_chart, 0, 80);
	lv_chart_set_div_line_count(plot_chart, 3, 0);
	lv_chart_set_update_mode(plot_chart, LV_CHART_UPDATE_MODE_SHIFT);

	static lv_style_t style_chart_bg, style_chart_series_bg, style_chart_series;
	lv_style_init(&style_chart_bg);
	lv_style_set_bg_opa(&style_chart_bg, LV_STATE_DEFAULT, LV_OPA_COVER);
	lv_style_set_bg_color(&style_chart_bg, LV_STATE_DEFAULT, lv_color_hex(0xF0F0F0));
	lv_style_set_border_width(&style_chart_bg, LV_STATE_DEFAULT, 1);
	lv_style_set_border_color(&style_chart_bg, LV_STATE_DEFAULT, lv_color_hex(0xC0C0C0));
	lv_style_set_pad_top(&style_chart_bg, LV_STATE_DEFAULT, 2);
	lv_style_set_pad_bottom(&style_chart_bg, LV_STATE_DEFAULT, 2);
	lv_style_set_pad_left(&style_chart_bg, LV_STATE_DEFAULT, 2);
	lv_style_set_pad_right(&style_chart_bg, LV_STATE_DEFAULT, 2);
	lv_obj_add_style(plot_chart, LV_CHART_PART_BG, &style_chart_bg);

	lv_style_init(&style_chart_series_bg);
	lv_style_set_bg_opa(&style_chart_series_bg, LV_STATE_DEFAULT, LV_OPA_TRANSP);
	lv_style_set_line_color(&style_chart_series_bg, LV_STATE_DEFAULT, lv_color_hex(0x404040));
	lv_style_set_line_width(&style_chart_series_bg, LV_STATE_DEFAULT, 1);
	lv_style_set_line_opa(&style_chart_series_bg, LV_STATE_DEFAULT, LV_OPA_30);
	lv_obj_add_style(plot_chart, LV_CHART_PART_SERIES_BG, &style_chart_series_bg);

	lv_style_init(&style_chart_series);
	lv_style_set_line_width(&style_chart_series, LV_STATE_DEFAULT, 2);
	lv_style_set_size(&style_chart_series, LV_STATE_DEFAULT, 0);
	lv_obj_add_style(plot_chart, LV_CHART_PART_SERIES, &style_chart_series);

	fps_series = lv_chart_add_series(plot_chart, lv_color_hex(0x00BBCC));
	pxs_series = lv_chart_add_series(plot_chart, lv_color_hex(0xCC8800));
	lv_chart_init_points(plot_chart, fps_series, 0);
	lv_chart_init_points(plot_chart, pxs_series, 0);

	const int gl_h = 118, gl_w = 71;
	int x_off = 800 - 8 * gl_w - 30;
	for (int c = 0; c < 8; c++, x_off += gl_w) {
		clock_label[c] = lv_label_create(controls_panel, NULL);
		lv_obj_set_pos(clock_label[c], x_off, 3);
		lv_obj_set_size(clock_label[c], gl_w, gl_h);
		lv_obj_add_style(clock_label[c], LV_LABEL_PART_MAIN, &style_clock);
		lv_label_set_text(clock_label[c], "");
		lv_label_set_long_mode(clock_label[c], LV_LABEL_LONG_EXPAND);
	}

	date_label = lv_label_create(controls_panel, NULL);
	lv_obj_set_y(date_label, gl_h + 4);
	lv_obj_set_size(date_label, lv_obj_get_width(controls_panel), 25);
	lv_label_set_text(date_label, "");
	lv_obj_add_style(date_label, LV_LABEL_PART_MAIN, &style_large);
	lv_label_set_long_mode(date_label, LV_LABEL_LONG_EXPAND);

	weather_label = lv_label_create(controls_panel, NULL);
	lv_obj_set_y(weather_label, gl_h + 4);
	lv_label_set_text(weather_label, "");
	lv_obj_add_style(weather_label, LV_LABEL_PART_MAIN, &style_large);
	lv_label_set_long_mode(weather_label, LV_LABEL_LONG_SROLL);

	lv_style_init(&style_led_green);
	lv_style_set_bg_color(&style_led_green, LV_STATE_DEFAULT, lv_color_hex(0x00CC00));
	lv_style_set_border_color(&style_led_green, LV_STATE_DEFAULT, lv_color_hex(0x009900));
	lv_style_set_shadow_color(&style_led_green, LV_STATE_DEFAULT, lv_color_hex(0x00CC00));

	lv_style_init(&style_led_red);
	lv_style_set_bg_color(&style_led_red, LV_STATE_DEFAULT, lv_color_hex(0xCC0000));
	lv_style_set_border_color(&style_led_red, LV_STATE_DEFAULT, lv_color_hex(0x990000));
	lv_style_set_shadow_color(&style_led_red, LV_STATE_DEFAULT, lv_color_hex(0xCC0000));

	led1 = lv_led_create(controls_panel, NULL);
	lv_obj_set_pos(led1, 785, 1);
	lv_obj_set_size(led1, 14, 14);
	lv_obj_add_style(led1, LV_LED_PART_MAIN, &style_led_green);
	lv_led_off(led1);

	// Start processing ..

	time_task = lv_task_create(time_timer_cb, 1000, LV_TASK_PRIO_MID, NULL);
	net_task = lv_task_create(net_timer_cb, 3000, LV_TASK_PRIO_LOW, NULL);
	weather_task = lv_task_create(weather_timer_cb, 10 * 60000, LV_TASK_PRIO_LOW, NULL);
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
	disp_drv.flush_cb = fbdev_flush; // flushes the internal graphical buffer to the frame buffer
	disp_drv.buffer = &disp_buf; // set teh display buffere reference in the driver
	lv_disp_drv_register(&disp_drv);

	// Initialize and register a pointer device driver
	lv_indev_drv_t indev_drv;
	lv_indev_drv_init(&indev_drv);
	indev_drv.type = LV_INDEV_TYPE_POINTER;
	indev_drv.read_cb = evdev_read; // defined in lv_drivers/indev/evdev.h
	lv_indev_drv_register(&indev_drv);
}

#else /* __linux__ */

// Tick thread no longer needed — main loop uses clock_gettime for accurate ticks

static void hal_init() {
	/* Use the 'monitor' driver which creates window on PC's monitor to simulate a display*/
	monitor_init();
	/* Tick handling moved to main loop using clock_gettime */

	/*Create a display buffer*/
	lv_disp_buf_init(&disp_buf, lvbuf1, lvbuf2, LV_BUF_SIZE);

	/*Create a display*/
	lv_disp_drv_t disp_drv;
	lv_disp_drv_init(&disp_drv);
	disp_drv.flush_cb = monitor_flush; // flushes the internal graphical buffer to the frame buffer
	disp_drv.buffer = &disp_buf; // set teh display buffere reference in the driver
	lv_disp_drv_register(&disp_drv);
	disp_drv.antialiasing = 1;

	lv_group_t *g = lv_group_create();

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

	lv_indev_t *enc_indev = lv_indev_drv_register(&indev_drv_3);
	lv_indev_set_group(enc_indev, g);
}
#endif /* __linux__ */

int main(int argc, char *argv[]) {
	srand(time(NULL));
	lv_init(); // LVGL init
	lv_png_init(); // png file support

	hal_init();

	// Panel initialization
	panel_init(argv[0]);

	// Handle LVGL tasks — use real elapsed time for accurate animation
	struct timespec ts_prev, ts_now;
	clock_gettime(CLOCK_MONOTONIC, &ts_prev);
	while (1) {
		clock_gettime(CLOCK_MONOTONIC, &ts_now);
		uint32_t elapsed_ms = (ts_now.tv_sec - ts_prev.tv_sec) * 1000
			+ (ts_now.tv_nsec - ts_prev.tv_nsec) / 1000000;
		if (elapsed_ms > 0) {
			lv_tick_inc(elapsed_ms);
			ts_prev = ts_now;
		}
		lv_task_handler();
		usleep(1000); // ~1ms for smooth animation
	}
	return 0;
}
