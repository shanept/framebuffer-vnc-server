/*
 * $Id$
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * This project is an adaptation of the original fbvncserver for the iPAQ
 * and Zaurus.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <sys/stat.h>
#include <sys/sysmacros.h>             /* For makedev() */

#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>

#include <assert.h>
#include <errno.h>

/* libvncserver */
#include "rfb/rfb.h"
#include "rfb/keysym.h"

/*****************************************************************************/
//#define LOG_FPS

#define BITS_PER_SAMPLE     5
#define SAMPLES_PER_PIXEL   2

static char fb_device[256] = "/dev/fb0";
static char kbd_device[256] = "/dev/input/event2";
static char mouse_device[256] = "/dev/input/event5";
static struct fb_var_screeninfo scrinfo;
static int fbfd = -1;
static int kbdfd = -1;
static int mousefd = -1;
static unsigned short int *fbmmap = MAP_FAILED;
static unsigned short int *vncbuf;
static unsigned short int *fbbuf;

static int vnc_port = 5900;
static rfbScreenInfoPtr server;
static size_t bytespp;

static int xmin, xmax;
static int ymin, ymax;

/* No idea, just copied from fbvncserver as part of the frame differerencing
 * algorithm.  I will probably be later rewriting all of this. */
static struct varblock_t
{
    int min_i;
    int min_j;
    int max_i;
    int max_j;
    int r_offset;
    int g_offset;
    int b_offset;
    int rfb_xres;
    int rfb_maxy;
} varblock;

/*****************************************************************************/

static void keyevent(rfbBool down, rfbKeySym key, rfbClientPtr cl);
static void ptrevent(int buttonMask, int x, int y, rfbClientPtr cl);

/*****************************************************************************/


static void init_fb(void)
{
    size_t pixels;

    if ((fbfd = open(fb_device, O_RDONLY)) == -1)
    {
        fprintf(stderr, "cannot open fb device %s\n", fb_device);
        exit(EXIT_FAILURE);
    }

    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &scrinfo) != 0)
    {
        fprintf(stderr, "ioctl error\n");
        exit(EXIT_FAILURE);
    }

    pixels = scrinfo.xres * scrinfo.yres;
    bytespp = scrinfo.bits_per_pixel / 8;

    fprintf(stderr, "xres=%d, yres=%d, xresv=%d, yresv=%d, xoffs=%d, yoffs=%d, bpp=%d\n",
            (int)scrinfo.xres, (int)scrinfo.yres,
            (int)scrinfo.xres_virtual, (int)scrinfo.yres_virtual,
            (int)scrinfo.xoffset, (int)scrinfo.yoffset,
            (int)scrinfo.bits_per_pixel);

    fprintf(stderr, "offset:length red=%d:%d green=%d:%d blue=%d:%d \n",
            (int)scrinfo.red.offset, (int)scrinfo.red.length,
            (int)scrinfo.green.offset, (int)scrinfo.green.length,
            (int)scrinfo.blue.offset, (int)scrinfo.blue.length
            );

    fbmmap = mmap(NULL, pixels * bytespp, PROT_READ, MAP_SHARED, fbfd, 0);

    if (fbmmap == MAP_FAILED)
    {
        fprintf(stderr, "mmap failed\n");
        exit(EXIT_FAILURE);
    }
}

static void cleanup_fb(void)
{
    if(fbfd != -1)
    {
        close(fbfd);
    }
}

static void init_kbd()
{
    if((kbdfd = open(kbd_device, O_RDWR)) == -1)
    {
        fprintf(stderr, "cannot open kbd device %s\n", kbd_device);
        exit(EXIT_FAILURE);
    }
}

static void cleanup_kbd()
{
    if(kbdfd != -1)
    {
        close(kbdfd);
    }
}

static void init_mouse()
{
    struct input_absinfo info;

    if((mousefd = open(mouse_device, O_RDWR)) == -1)
    {
        fprintf(stderr, "cannot open mouse device %s\n", mouse_device);
        exit(EXIT_FAILURE);
    }

    // Get the range of X and Y
    if(ioctl(mousefd, EVIOCGABS(ABS_X), &info)) {
        fprintf(stderr, "cannot get ABS_X info, %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    xmin = info.minimum;
    xmax = info.maximum;

    if(ioctl(mousefd, EVIOCGABS(ABS_Y), &info)) {
        fprintf(stderr, "cannot get ABS_Y info, %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    ymin = info.minimum;
    ymax = info.maximum;
}

static void cleanup_mouse()
{
    if(mousefd != -1)
    {
        close(mousefd);
    }
}

/*****************************************************************************/

static void init_fb_server(int argc, char **argv)
{
    fprintf(stderr, "Initializing server...\n");

    /* Allocate the VNC server buffer to be managed (not manipulated) by
     * libvncserver. */
    vncbuf = calloc(scrinfo.xres * scrinfo.yres, bytespp);
    assert(vncbuf != NULL);

    /* Allocate the comparison buffer for detecting drawing updates from frame
     * to frame. */
    fbbuf = calloc(scrinfo.xres * scrinfo.yres, bytespp);
    assert(fbbuf != NULL);

    /* TODO: This assumes scrinfo.bits_per_pixel is 16. */
    server = rfbGetScreen(&argc, argv, scrinfo.xres, scrinfo.yres, BITS_PER_SAMPLE, SAMPLES_PER_PIXEL, bytespp);
    assert(server != NULL);

    server->desktopName = "framebuffer";
    server->frameBuffer = (char *)vncbuf;
    server->alwaysShared = TRUE;
    server->httpDir = NULL;
    server->port = vnc_port;

    server->kbdAddEvent = keyevent;
    server->ptrAddEvent = ptrevent;

    rfbInitServer(server);

    /* Mark as dirty since we haven't sent any updates at all yet. */
    rfbMarkRectAsModified(server, 0, 0, scrinfo.xres, scrinfo.yres);

    /* No idea. */
    varblock.r_offset = scrinfo.red.offset + scrinfo.red.length - BITS_PER_SAMPLE;
    varblock.g_offset = scrinfo.green.offset + scrinfo.green.length - BITS_PER_SAMPLE;
    varblock.b_offset = scrinfo.blue.offset + scrinfo.blue.length - BITS_PER_SAMPLE;
    varblock.rfb_xres = scrinfo.yres;
    varblock.rfb_maxy = scrinfo.xres - 1;
}

/*****************************************************************************/

void injectKeyEvent(uint16_t code, uint16_t value)
{
    struct input_event ev;

    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time, 0);

    ev.type  = EV_KEY;
    ev.code  = code;
    ev.value = value;

    if(write(kbdfd, &ev, sizeof(ev)) < 0)
    {
        fprintf(stderr, "write event failed, %s\n", strerror(errno));
    }

    fprintf(stderr, "injectKey (%d, %d)\n", code, value);
}

static int keysym2scancode(rfbBool down, rfbKeySym key, rfbClientPtr cl)
{
    int scancode = 0;
    int code = (int) key;

    if (code >= '0' && code <= '9') {
        scancode = (code & 0xF) - 1;
        if (scancode < 0) scancode += 10;
        scancode += KEY_1;
    } else if (code >= 0xFF50 && code <= 0xFF58) {
        static const uint16_t map[] =
            {   KEY_HOME, KEY_LEFT, KEY_UP, KEY_RIGHT,
                KEY_DOWN, 0, 0, KEY_END, 0 };
        scancode = map[code & 0xF];
    } else if (code >= 0xFFE1 && code <= 0xFFEE) {
        static const uint16_t map[] =
            {   KEY_LEFTSHIFT, KEY_LEFTSHIFT,
                KEY_COMPOSE,   KEY_COMPOSE,
                KEY_LEFTSHIFT, KEY_LEFTSHIFT,
                0, 0,
                KEY_LEFTALT,   KEY_RIGHTALT,
                0, 0, 0, 0 };
        scancode = map[code & 0xF];
    } else if ((code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z')) {
        static const uint16_t map[] = {
                KEY_A, KEY_B, KEY_C, KEY_D, KEY_E,
                KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
                KEY_K, KEY_L, KEY_M, KEY_N, KEY_O,
                KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
                KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z };
        scancode = map[(code & 0x5F) - 'A'];
    } else {
        switch (code) {
//          case 0x0003: scancode = KEY_CENTER;    break;
            case 0x0020: scancode = KEY_SPACE;     break;
            case 0x002C: scancode = KEY_COMMA;     break;
            case 0x003C: scancode = KEY_COMMA;     break;
            case 0x002E: scancode = KEY_DOT;       break;
            case 0x003E: scancode = KEY_DOT;       break;
            case 0x002F: scancode = KEY_SLASH;     break;
            case 0x003F: scancode = KEY_SLASH;     break;
            case 0x0032: scancode = KEY_EMAIL;     break;
            case 0x0040: scancode = KEY_EMAIL;     break;
            case 0xFF08: scancode = KEY_BACKSPACE; break;
            case 0xFF1B: scancode = KEY_BACK;      break;
            case 0xFF09: scancode = KEY_TAB;       break;
            case 0xFF0D: scancode = KEY_ENTER;     break;
//          case 0x002A: scancode = KEY_STAR;      break;
            case 0xFFBE: scancode = KEY_F1;        break; // F1
            case 0xFFBF: scancode = KEY_F2;        break; // F2
            case 0xFFC0: scancode = KEY_F3;        break; // F3
            case 0xFFC5: scancode = KEY_F4;        break; // F8
            case 0xFFC8: rfbShutdownServer(cl->screen, TRUE); break; // F11
        }
    }

    return scancode;
}

static void keyevent(rfbBool down, rfbKeySym key, rfbClientPtr cl)
{
    int scancode;

    fprintf(stderr, "Got keysym: %04x (down=%d)\n", (unsigned int) key, (int) down);

    if ((scancode = keysym2scancode(down, key, cl)))
    {
         injectKeyEvent(scancode, down);
    }
}

void injectMouseEvent(int down, int x, int y)
{
    struct input_event ev;

    // Calculate the final x and y
    // Fake touch screen always reports zero
    if (xmin != 0 && xmax != 0 && ymin != 0 && ymax != 0)
    {
        x = xmin + (x * (xmax - xmin)) / (scrinfo.xres);
        y = ymin + (y * (ymax - ymin)) / (scrinfo.yres);
    }

    memset(&ev, 0, sizeof(ev));

    // Then send a BTN_TOUCH (required for synchronization)
    gettimeofday(&ev.time, 0);
    ev.type  = EV_KEY;
    ev.code  = BTN_TOUCH;
    ev.value = down;
    if(write(mousefd, &ev, sizeof(ev)) < 0)
    {
        fprintf(stderr, "write event failed, %s\n", strerror(errno));
    }

    // Then send the X
    gettimeofday(&ev.time, 0);
    ev.type  = EV_ABS;
    ev.code  = ABS_X;
    ev.value = x;
    if(write(mousefd, &ev, sizeof(ev)) < 0)
    {
        fprintf(stderr, "write event failed, %s\n", strerror(errno));
    }

    // Then send the Y
    gettimeofday(&ev.time, 0);
    ev.type  = EV_ABS;
    ev.code  = ABS_Y;
    ev.value = y;
    if(write(mousefd, &ev, sizeof(ev)) < 0)
    {
        printf("write event failed, %s\n", strerror(errno));
    }

    // Finally send the SYN
    gettimeofday(&ev.time, 0);
    ev.type  = EV_SYN;
    ev.code  = 0;
    ev.value = 0;
    if(write(mousefd, &ev, sizeof(ev)) < 0)
    {
        fprintf(stderr, "write event failed, %s\n", strerror(errno));
    }

    fprintf(stderr, "injectPtrEvent (x=%d, y=%d, down=%d)\n", x, y, down);
}

static void ptrevent(int buttonMask, int x, int y, rfbClientPtr cl)
{
        /* Indicates either pointer movement or a pointer button press or release. The pinter is
now at (x-position, y-position), and the current state of buttons 1 to 8 are represented
by bits 0 to 7 of button-mask respectively, 0 meaning up, 1 meaning down (pressed).
On a conventional mouse, buttons 1, 2 and 3 correspond to the left, middle and right
buttons on the mouse. On a wheel mouse, each step of the wheel upwards is represented
by a press and release of button 4, and each step downwards is represented by
a press and release of button 5.
  From: http://www.vislab.usyd.edu.au/blogs/index.php/2009/05/22/an-headerless-indexed-protocol-for-input-1?blog=61 */

        //fprintf(stderr, "Got ptrevent: %04x (x=%d, y=%d)\n", buttonMask, x, y);
        if(buttonMask & 1) {
            // Simulate left mouse event
            injectMouseEvent(1, x, y);
            injectMouseEvent(0, x, y);
        }
}

/*****************************************************************************/

// sec
#define LOG_TIME    5

int timeToLogFPS() {
    static struct timeval now={0,0}, then={0,0};
    double elapsed, dnow, dthen;
    gettimeofday(&now,NULL);
    dnow  = now.tv_sec  + (now.tv_usec /1000000.0);
    dthen = then.tv_sec + (then.tv_usec/1000000.0);
    elapsed = dnow - dthen;
    if (elapsed > LOG_TIME)
      memcpy((char *)&then, (char *)&now, sizeof(struct timeval));
    return elapsed > LOG_TIME;
}

//#define COLOR_MASK  0x1f001f
#define COLOR_MASK  (((1 << BITS_PER_SAMPLE) << 1) - 1)
#define PIXEL_FB_TO_RFB(p,r_offset,g_offset,b_offset) ((p>>r_offset)&COLOR_MASK) | (((p>>g_offset)&COLOR_MASK)<<BITS_PER_SAMPLE) | (((p>>b_offset)&COLOR_MASK)<<(2*BITS_PER_SAMPLE))

static void update_screen(void)
{
#ifdef LOG_FPS
    static int frames = 0;
    frames++;
    if(timeToLogFPS())
    {
        double fps = frames / LOG_TIME;
        fprintf(stderr, "  fps: %f\n", fps);
        frames = 0;
    }
#endif

    varblock.min_i = varblock.min_j = 9999;
    varblock.max_i = varblock.max_j = -1;

    uint32_t *f = (uint32_t *)fbmmap;        /* -> framebuffer         */
    uint32_t *c = (uint32_t *)fbbuf;         /* -> compare framebuffer */
    uint32_t *r = (uint32_t *)vncbuf;        /* -> remote framebuffer  */

    int xstep = 4/bytespp;

    int y;
    for (y = 0; y < (int)scrinfo.yres; y++)
    {
        /* Compare every 1/2/4 pixels at a time */
        int x;
        for (x = 0; x < (int)scrinfo.xres; x += xstep)
        {
            uint32_t pixel = *f;

            if (pixel != *c)
            {
                *c = pixel;

#if 0
                /* XXX: Undo the checkered pattern to test the efficiency
                 * gain using hextile encoding. */
                if (pixel == 0x18e320e4 || pixel == 0x20e418e3)
                    pixel = 0x18e318e3;
#endif
                *r = PIXEL_FB_TO_RFB(pixel,
                                     varblock.r_offset, varblock.g_offset, varblock.b_offset);

                if (x < varblock.min_i)
                    varblock.min_i = x;
                else
                {
                    if (x > varblock.max_i)
                        varblock.max_i = x;

                    if (y > varblock.max_j)
                        varblock.max_j = y;
                    else if (y < varblock.min_j)
                        varblock.min_j = y;
                }
            }

            f++;
            c++;
            r++;
        }
    }

    if (varblock.min_i < 9999)
    {
        if (varblock.max_i < 0)
            varblock.max_i = varblock.min_i;

        if (varblock.max_j < 0)
            varblock.max_j = varblock.min_j;

        fprintf(stderr, "Dirty page: %dx%d+%d+%d...\n",
                (varblock.max_i+2) - varblock.min_i, (varblock.max_j+1) - varblock.min_j,
                varblock.min_i, varblock.min_j);

        rfbMarkRectAsModified(server, varblock.min_i, varblock.min_j,
                              varblock.max_i + 2, varblock.max_j + 1);

        rfbProcessEvents(server, 10000);
    }
}

/*****************************************************************************/

void print_usage(char **argv)
{
    fprintf(stderr, "%s [-f device] [-k device] [-m device] [-p port] [-h]\n"
                    "-p port: VNC port, default is 5900\n"
                    "-f device: framebuffer device node, default is /dev/fb0\n"
                    "-k device: keyboard device node, default is /dev/input/event2\n"
                    "-m device: mouse device node, default is /dev/input/event5\n"
                    "-h : print this help\n"
            , *argv);
}

int main(int argc, char **argv)
{
    if(argc > 1)
    {
        int i=1;
        while(i < argc)
        {
            if(*argv[i] == '-')
            {
                switch(*(argv[i] + 1))
                {
                case 'h':
                    print_usage(argv);
                    exit(0);
                    break;
                case 'f':
                    i++;
                    strcpy(fb_device, argv[i]);
                    break;
                case 'k':
                    i++;
                    strcpy(kbd_device, argv[i]);
                    break;
                case 'm':
                    i++;
                    strcpy(mouse_device, argv[i]);
                    break;
                case 'p':
                    i++;
                    vnc_port = atoi(argv[i]);
                    break;
                }
            }
            i++;
        }
    }

    fprintf(stderr, "Initializing framebuffer device %s...\n", fb_device);
    init_fb();
    fprintf(stderr, "Initializing keyboard device %s...\n", kbd_device);
    init_kbd();
    fprintf(stderr, "Initializing mouse device %s...\n", mouse_device);
    init_mouse();

    fprintf(stderr, "Initializing VNC server:\n");
    fprintf(stderr, "	width:  %d\n", (int)scrinfo.xres);
    fprintf(stderr, "	height: %d\n", (int)scrinfo.yres);
    fprintf(stderr, "	bpp:    %d\n", (int)scrinfo.bits_per_pixel);
    fprintf(stderr, "	port:   %d\n", (int)vnc_port);
    init_fb_server(argc, argv);

    /* Implement our own event loop to detect changes in the framebuffer. */
    while (1)
    {
        while (server->clientHead == NULL)
            rfbProcessEvents(server, 100000);

        rfbProcessEvents(server, 100000);
        update_screen();
    }

    fprintf(stderr, "Cleaning up...\n");
    cleanup_fb();
    cleanup_kbd();
    cleanup_mouse();
}
