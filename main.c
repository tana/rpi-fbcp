
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <sys/mman.h>

#include <bcm_host.h>

int process(int primary_display_index, char* secondary_display_device, int left, int top, int width, int height, DISPMANX_TRANSFORM_T transform) {
    DISPMANX_DISPLAY_HANDLE_T display, offscreen;
    DISPMANX_MODEINFO_T display_info;
    DISPMANX_RESOURCE_HANDLE_T screen_resource, capture_resource;
    DISPMANX_UPDATE_HANDLE_T update;
    DISPMANX_ELEMENT_HANDLE_T element;
    VC_DISPMANX_ALPHA_T alpha;
    uint32_t screen_image_prt, capture_image_prt;
    VC_RECT_T screen_rect, crop_rect;
    int ret;
    int fbfd = 0;
    char *fbp = 0;

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    // Totally opaque
    alpha.flags = DISPMANX_FLAGS_ALPHA_FROM_SOURCE | DISPMANX_FLAGS_ALPHA_FIXED_ALL_PIXELS;
    alpha.opacity = 255;
    alpha.mask = DISPMANX_NO_HANDLE;

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

    // Create resource (GPU image) to store captured image
    capture_resource = vc_dispmanx_resource_create(VC_IMAGE_RGB565, display_info.width, display_info.height, &capture_image_prt);
    if (!capture_resource) {
        syslog(LOG_ERR, "Unable to create capture buffer");
        close(fbfd);
        vc_dispmanx_display_close(display);
        return -1;
    }

    // Create resource (GPU image) to store result of cropping and scaling
    screen_resource = vc_dispmanx_resource_create(VC_IMAGE_RGB565, vinfo.xres, vinfo.yres, &screen_image_prt);
    if (!screen_resource) {
        syslog(LOG_ERR, "Unable to create screen buffer");
        close(fbfd);
        ret = vc_dispmanx_resource_delete(capture_resource);
        vc_dispmanx_display_close(display);
        return -1;
    }

    fbp = (char*) mmap(0, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fbp <= 0) {
        syslog(LOG_ERR, "Unable to create memory mapping");
        close(fbfd);
        ret = vc_dispmanx_resource_delete(screen_resource);
        ret = vc_dispmanx_resource_delete(capture_resource);
        vc_dispmanx_display_close(display);
        return -1;
    }

    vc_dispmanx_rect_set(&screen_rect, 0, 0, vinfo.xres, vinfo.yres);

    // Create offscreen display for cropping and scaling
    offscreen = vc_dispmanx_display_open_offscreen(screen_resource, DISPMANX_NO_ROTATE);
    if (!offscreen) {
        syslog(LOG_ERR, "Unable to open offscreen display");
        munmap(fbp, finfo.smem_len);
        close(fbfd);
        ret = vc_dispmanx_resource_delete(screen_resource);
        ret = vc_dispmanx_resource_delete(capture_resource);
        vc_dispmanx_display_close(display);
        return -1;
    }

    // In source rect of an element, it seems the components need to be shifted by 16
    // See: /opt/vc/src/hello_pi/hello_dispmanx ( https://github.com/raspberrypi/firmware/blob/3f20b832b27cd730deb6419b570f31a98167eef6/opt/vc/src/hello_pi/hello_dispmanx/dispmanx.c#L126 )
    vc_dispmanx_rect_set(&crop_rect, left << 16, top << 16, width << 16, height << 16);

    // Prepare content of offscreen display
    update = vc_dispmanx_update_start(0);
    element = vc_dispmanx_element_add(update, offscreen, 0, &screen_rect, capture_resource, &crop_rect, DISPMANX_PROTECTION_NONE, &alpha, NULL, transform);
    ret = vc_dispmanx_update_submit_sync(update);

    // Main loop
    while (1) {
        // Capture screenshot as a resource
        ret = vc_dispmanx_snapshot(display, capture_resource, DISPMANX_NO_ROTATE);

        // Crop and scale using offscreen display
        update = vc_dispmanx_update_start(0);
        ret = vc_dispmanx_element_modified(update, element, &crop_rect);
        ret = vc_dispmanx_update_submit_sync(update);

        // Read result into Linux framebuffer
        vc_dispmanx_resource_read_data(screen_resource, &screen_rect, fbp, vinfo.xres * vinfo.bits_per_pixel / 8);
        usleep(25 * 1000);
    }

    munmap(fbp, finfo.smem_len);
    close(fbfd);
    vc_dispmanx_display_close(offscreen);
    ret = vc_dispmanx_resource_delete(screen_resource);
    ret = vc_dispmanx_resource_delete(capture_resource);
    vc_dispmanx_display_close(display);
}

int main(int argc, char **argv) {
    int primary_display_index = 0;
    char* secondary_display_device = "/dev/fb1";
    int left = 0, top = 0;
    int width = -1, height = -1;    // Negative values mean default
    int rotation = 0;
    char* flip_str = "";
    int opt;

    setlogmask(LOG_UPTO(LOG_DEBUG));
    openlog("fbcp", LOG_NDELAY | LOG_PID, LOG_USER);

    while ((opt = getopt(argc, argv, "p:s:l:t:w:h:r:f:")) >= 0) {
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
            case 'r':
                rotation = atoi(optarg);
                break;
            case 'f':
		flip_str = optarg;
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
                fputs("\t-r ANGLE: Set rotation angle (0, 90, 180, 270).\n", stderr);
                fputs("\t-f FLIP_STR: Flip image to be displayed.\n", stderr);
		fputs("\t\tFLIP_STR can be \"v\" (vertical flip), \"h\" (horizontal), or \"vh\" (both).\n", stderr);
                exit(-1);
                break;
        }
    }

    // Tansform option for DispmanX
    DISPMANX_TRANSFORM_T transform;
    // Rotation component
    switch (rotation) {
        case 0:
            transform = DISPMANX_NO_ROTATE;
            break;
        case 90:
            transform = DISPMANX_ROTATE_90;
            break;
        case 180:
            transform = DISPMANX_ROTATE_180;
            break;
        case 270:
            transform = DISPMANX_ROTATE_270;
            break;
        default:
            fputs("Invalid rotation angle\n", stderr);
            exit(-1);
            break;
    }
    // Flip component
    int flip_str_len = strlen(flip_str);
    for (int i = 0; i < flip_str_len; i++) {
        switch (flip_str[i]) {
            case 'v':
            case 'V':
                transform |= DISPMANX_FLIP_VERT;
                break;
            case 'h':
            case 'H':
                transform |= DISPMANX_FLIP_HRIZ;
                break;
        }
    }

    return process(primary_display_index, secondary_display_device, left, top, width, height, transform);
}
