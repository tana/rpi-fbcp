
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <sys/mman.h>

#include <bcm_host.h>

int process(int primary_display_index, char* secondary_display_device, int left, int top, int width, int height) {
    DISPMANX_DISPLAY_HANDLE_T display;
    DISPMANX_MODEINFO_T display_info;
    DISPMANX_RESOURCE_HANDLE_T screen_resource;
    VC_IMAGE_TRANSFORM_T transform;
    uint32_t image_prt;
    VC_RECT_T rect1;
    int ret;
    int fbfd = 0;
    char *fbp = 0;

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;


    bcm_host_init();

    display = vc_dispmanx_display_open(primary_display_index);
    if (!display) {
        syslog(LOG_ERR, "Unable to open primary display");
        return -1;
    }
    ret = vc_dispmanx_display_get_info(display, &display_info);
    if (ret) {
        syslog(LOG_ERR, "Unable to get primary display information");
        return -1;
    }
    syslog(LOG_INFO, "Primary display is %d x %d", display_info.width, display_info.height);


    if (width < 0) {
        width = display_info.width - left;
    }
    if (height < 0) {
        height = display_info.height - top;
    }


    fbfd = open(secondary_display_device, O_RDWR);
    if (fbfd == -1) {
        syslog(LOG_ERR, "Unable to open secondary display");
        return -1;
    }
    if (ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo)) {
        syslog(LOG_ERR, "Unable to get secondary display information");
        return -1;
    }
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo)) {
        syslog(LOG_ERR, "Unable to get secondary display information");
        return -1;
    }

    syslog(LOG_INFO, "Second display is %d x %d %dbps\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);

    screen_resource = vc_dispmanx_resource_create(VC_IMAGE_RGB565, vinfo.xres, vinfo.yres, &image_prt);
    if (!screen_resource) {
        syslog(LOG_ERR, "Unable to create screen buffer");
        close(fbfd);
        vc_dispmanx_display_close(display);
        return -1;
    }

    fbp = (char*) mmap(0, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fbp <= 0) {
        syslog(LOG_ERR, "Unable to create memory mapping");
        close(fbfd);
        ret = vc_dispmanx_resource_delete(screen_resource);
        vc_dispmanx_display_close(display);
        return -1;
    }

    vc_dispmanx_rect_set(&rect1, 0, 0, vinfo.xres, vinfo.yres);

    while (1) {
        ret = vc_dispmanx_snapshot(display, screen_resource, 0);
        vc_dispmanx_resource_read_data(screen_resource, &rect1, fbp, vinfo.xres * vinfo.bits_per_pixel / 8);
        usleep(25 * 1000);
    }

    munmap(fbp, finfo.smem_len);
    close(fbfd);
    ret = vc_dispmanx_resource_delete(screen_resource);
    vc_dispmanx_display_close(display);
}

int main(int argc, char **argv) {
    int primary_display_index = 0;
    char* secondary_display_device = "/dev/fb1";
    int left = 0, top = 0;
    int width = -1, height = -1;    // Negative values mean default
    int opt;

    setlogmask(LOG_UPTO(LOG_DEBUG));
    openlog("fbcp", LOG_NDELAY | LOG_PID, LOG_USER);

    while ((opt = getopt(argc, argv, "p:s:l:t:w:h:")) >= 0) {
        switch (opt) {
            case 'p':
                primary_display_index = atoi(optarg);
                break;
            case 's':
                secondary_display_device = optarg;
                break;
            case 'l':
                left = atoi(optarg);
                break;
            case 't':
                top = atoi(optarg);
                break;
            case 'w':
                width = atoi(optarg);
                break;
            case 'h':
                height = atoi(optarg);
                break;
            default:    // Unknown option
                fprintf(stderr, "Usage: %s [OPTIONS]\n", argv[0]);
                fputs("OPTIONS:\n", stderr);
                fputs("\t-p INDEX: Set primary display index. Default is 0.\n", stderr);
                fputs("\t-s DEV: Set secondary display device. Default is /dev/fb1 .\n", stderr);
                fputs("\t-l LEFT, -t TOP: Set left or top coordinate of rectangle to copy.\n", stderr);
                fputs("\t\tDefault is (0,0).\n", stderr);
                fputs("\t-w WIDTH, -h HEIGHT: Set size of rectangle to copy.\n", stderr);
                fputs("\t\tDefault is (display_width-LEFT,display_height-TOP).\n", stderr);
                exit(-1);
                break;
        }
    }

    return process(primary_display_index, secondary_display_device, left, top, width, height);
}
