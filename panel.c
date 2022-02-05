#include "lvgl/lvgl.h"
#include "lvgl/lv_drivers/display/fbdev.h"
#include "lvgl/lv_drivers/indev/evdev.h"
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdbool.h>

LV_FONT_DECLARE(digital_clock)

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

char timeString[9];
char dateString[16];

static lv_style_t style_large;
static lv_style_t style_clock;

static lv_obj_t *clock_label;
static lv_obj_t *date_label;

static const lv_font_t *font_large;
static const lv_font_t *font_normal;

static lv_obj_t *controls_panel;
static lv_obj_t *gallery_panel;

// Utilities functions

static void time_timer_cb(lv_task_t *timer) {

    time_t t = time(NULL);
    struct tm *local = localtime(&t);

    sprintf(timeString, "%02d:%02d:%02d", local->tm_hour, local->tm_min, local->tm_sec);
    sprintf(dateString, "%s\n%s %02d %04d", DAY[local->tm_wday], MONTH[local->tm_mon], local->tm_mday, local->tm_year + 1900);

    lv_label_set_text(clock_label, timeString);
    lv_label_set_text(date_label, dateString);
}

static int get_current_network_speed() {

    static unsigned long int kb_sent = 0, kb_sent_prev = 0;

    FILE *fp = fopen("/proc/net/dev", "r");
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
        if (strstr(ifname, "eth0") != NULL) {
            kb_sent = r_bytes / 1024;
        }
    }

    unsigned long int eth0_speed = (kb_sent - kb_sent_prev) * 2;
    kb_sent_prev = kb_sent;

    fclose(fp);
    return eth0_speed;
}


//  Main entry

static void panel_init(void) {

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

    controls_panel = lv_cont_create(lv_scr_act(), NULL);
    lv_obj_set_auto_realign(controls_panel, true);                    /*Auto realign when the size changes*/
    lv_obj_align_origo(controls_panel, NULL, LV_ALIGN_CENTER, 0, 0);
    lv_cont_set_fit(controls_panel, LV_FIT_TIGHT);
    lv_cont_set_layout(controls_panel, LV_LAYOUT_COLUMN_MID);

    clock_label = lv_label_create(controls_panel, NULL);
    lv_obj_add_style(clock_label, LV_LABEL_PART_MAIN, &style_clock);
    lv_label_set_text(clock_label, timeString);
    lv_label_set_long_mode(clock_label, LV_LABEL_LONG_EXPAND);

    date_label = lv_label_create(controls_panel, NULL);
    lv_label_set_text(date_label, dateString);
    lv_obj_add_style(date_label, LV_LABEL_PART_MAIN, &style_large);
    lv_label_set_long_mode(date_label, LV_LABEL_LONG_SROLL_CIRC);
}

int main(void) {

	lv_init(); // LittlevGL init
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

	// Create a "Hello world!" label
	lv_obj_t * label = lv_label_create(lv_scr_act(), NULL);
	lv_label_set_text(label, "Hello world!");
	lv_obj_align(label, NULL, LV_ALIGN_CENTER, 0, 0);

    // Panel initialization
    panel_init();

	// Handle LitlevGL tasks (tickless mode)
	while(1) {
		lv_tick_inc(5);
		lv_task_handler();
		usleep(5000);
	}
	return 0;
}
