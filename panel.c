#include "lvgl/lvgl.h"
#include "lvgl/lv_drivers/display/fbdev.h"
#include "lvgl/lv_drivers/indev/evdev.h"
#include <time.h>
#include <unistd.h>
#include <stdio.h>

// display buffer size - not sure if this size is really needed
#define LV_BUF_SIZE 384000		// 800x480

// A static variable to store the display buffers
static lv_disp_buf_t disp_buf;

// Static buffer(s). The second buffer is optional
static lv_color_t lvbuf1[LV_BUF_SIZE];
static lv_color_t lvbuf2[LV_BUF_SIZE];

// Display info and controls
unsigned long int kb_sent, kb_sent_prev = 0;
const char* DAY[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* MONTH[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

char timeString[9];
char dateString[16];

static lv_obj_t *clock_label;
static lv_obj_t *date_label;


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
    FILE *fp = fopen("/proc/net/dev", "r");
    char buf[200], ifname[20];
    unsigned long int r_bytes, t_bytes, r_packets, t_packets;

    // skip first two lines
    for (int i = 0; i < 2; i++) {
        fgets(buf, 200, fp);
    }

    while (fgets(buf, 200, fp)) {
        sscanf(buf, "%[^:]: %lu %lu %*lu %*lu %*lu %*lu %*lu %*lu %lu %lu", ifname, &r_bytes, &r_packets, &t_bytes, &t_packets);
        if(strstr(ifname, "eth0") != NULL) {
	  kb_sent = r_bytes / 1024;
        }
    }

    unsigned long int eth0_speed = (kb_sent - kb_sent_prev) * 2;
    kb_sent_prev = kb_sent;

    fclose(fp);
    return eth0_speed;
}


//  Main loop

int main(void)
{

	// LittlevGL init
	lv_init();

	//Linux frame buffer device init
	fbdev_init();

	// Touch pointer device init
	evdev_init();

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

	// Handle LitlevGL tasks (tickless mode)
	while(1) {
		lv_tick_inc(5);
		lv_task_handler();
		usleep(5000);
	}
	return 0;
}
