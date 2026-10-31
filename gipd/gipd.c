// gipd - userspace driver for Xbox One (GIP) USB pads on old Android kernels.
// Claims the GIP interface via usbdevfs, sends the power-on handshake and
// re-exposes the pad through uinput as an Xbox 360 pad (045e:028e), so
// Android uses its stock Vendor_045e_Product_028e.kl layout.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <linux/usbdevice_fs.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define EP_OUT 0x01
#define EP_IN 0x81
#define IFACE 0

static uint8_t seq;
static int init_steps = 5, step_delay;

static int find_pad(char *path, size_t len) {
    DIR *d = opendir("/sys/bus/usb/devices");
    struct dirent *e;
    if (!d) return -1;
    while ((e = readdir(d))) {
        char p[256], buf[16];
        snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/bDeviceSubClass", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        int sub = 0;
        if (fgets(buf, sizeof buf, f)) sub = strtol(buf, 0, 16);
        fclose(f);
        if (sub != 0x47) continue;
        int bus = 0, dev = 0;
        snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/busnum", e->d_name);
        if ((f = fopen(p, "r"))) { if (fscanf(f, "%d", &bus) != 1) bus = 0; fclose(f); }
        snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/devnum", e->d_name);
        if ((f = fopen(p, "r"))) { if (fscanf(f, "%d", &dev) != 1) dev = 0; fclose(f); }
        if (!bus || !dev) continue;
        snprintf(path, len, "/dev/bus/usb/%03d/%03d", bus, dev);
        closedir(d);
        return 0;
    }
    closedir(d);
    return -1;
}

static int xfer(int fd, unsigned ep, uint8_t *data, int len, int timeout) {
    struct usbdevfs_bulktransfer bt = {.ep = ep, .len = len, .timeout = timeout, .data = data};
    return ioctl(fd, USBDEVFS_BULK, &bt);
}

static int send_pkt(int fd, const uint8_t *pkt, int len) {
    uint8_t b[64];
    memcpy(b, pkt, len);
    b[2] = seq++;
    return xfer(fd, EP_OUT, b, len, 1000);
}

static int uinput_open(void) {
    int u = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (u < 0) return -1;
    static const int keys[] = {BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_SELECT,
                               BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR};
    ioctl(u, UI_SET_EVBIT, EV_KEY);
    ioctl(u, UI_SET_EVBIT, EV_ABS);
    for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++) ioctl(u, UI_SET_KEYBIT, keys[i]);
    static const int axes[] = {ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y};
    for (unsigned i = 0; i < sizeof axes / sizeof *axes; i++) ioctl(u, UI_SET_ABSBIT, axes[i]);

    struct uinput_user_dev ud;
    memset(&ud, 0, sizeof ud);
    snprintf(ud.name, sizeof ud.name, "Microsoft X-Box 360 pad");
    ud.id.bustype = BUS_USB;
    ud.id.vendor = 0x045e;
    ud.id.product = 0x028e;
    ud.id.version = 0x0110;
    // Ranges mirror the kernel xpad driver for 360 pads.
    int sticks[] = {ABS_X, ABS_Y, ABS_RX, ABS_RY};
    for (int i = 0; i < 4; i++) {
        ud.absmin[sticks[i]] = -32768;
        ud.absmax[sticks[i]] = 32767;
        ud.absflat[sticks[i]] = 128;
        ud.absfuzz[sticks[i]] = 16;
    }
    ud.absmax[ABS_Z] = ud.absmax[ABS_RZ] = 255;
    ud.absmin[ABS_HAT0X] = ud.absmin[ABS_HAT0Y] = -1;
    ud.absmax[ABS_HAT0X] = ud.absmax[ABS_HAT0Y] = 1;
    if (write(u, &ud, sizeof ud) != sizeof ud || ioctl(u, UI_DEV_CREATE) < 0) {
        close(u);
        return -1;
    }
    return u;
}

static void emit(int u, int type, int code, int val) {
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.code = code;
    ev.value = val;
    if (write(u, &ev, sizeof ev) < 0) perror("uinput write");
}

static void report(int u, const uint8_t *d) {
    emit(u, EV_KEY, BTN_START, !!(d[4] & 0x04));
    emit(u, EV_KEY, BTN_SELECT, !!(d[4] & 0x08));
    emit(u, EV_KEY, BTN_A, !!(d[4] & 0x10));
    emit(u, EV_KEY, BTN_B, !!(d[4] & 0x20));
    emit(u, EV_KEY, BTN_X, !!(d[4] & 0x40));
    emit(u, EV_KEY, BTN_Y, !!(d[4] & 0x80));
    emit(u, EV_KEY, BTN_TL, !!(d[5] & 0x10));
    emit(u, EV_KEY, BTN_TR, !!(d[5] & 0x20));
    emit(u, EV_KEY, BTN_THUMBL, !!(d[5] & 0x40));
    emit(u, EV_KEY, BTN_THUMBR, !!(d[5] & 0x80));
    emit(u, EV_ABS, ABS_HAT0X, !!(d[5] & 0x08) - !!(d[5] & 0x04));
    emit(u, EV_ABS, ABS_HAT0Y, !!(d[5] & 0x02) - !!(d[5] & 0x01));
    // Triggers are 10-bit, scaled to the 8-bit range of a 360 pad.
    emit(u, EV_ABS, ABS_Z, (d[6] | d[7] << 8) >> 2);
    emit(u, EV_ABS, ABS_RZ, (d[8] | d[9] << 8) >> 2);
    emit(u, EV_ABS, ABS_X, (int16_t)(d[10] | d[11] << 8));
    emit(u, EV_ABS, ABS_Y, ~(int16_t)(d[12] | d[13] << 8));
    emit(u, EV_ABS, ABS_RX, (int16_t)(d[14] | d[15] << 8));
    emit(u, EV_ABS, ABS_RY, ~(int16_t)(d[16] | d[17] << 8));
    emit(u, EV_SYN, SYN_REPORT, 0);
}

static int run(const char *path, int u) {
    fprintf(stderr, "gipd: opening %s\n", path);
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    unsigned int ifc = IFACE;
    struct usbdevfs_ioctl disc = {.ifno = IFACE, .ioctl_code = USBDEVFS_DISCONNECT};
    ioctl(fd, USBDEVFS_IOCTL, &disc);
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifc) < 0) {
        perror("claim");
        close(fd);
        return -1;
    }
    // Init sequence from the mainline xpad driver (xboxone_init_packets).
    static const uint8_t power_on[] = {0x05, 0x20, 0x00, 0x01, 0x00};
    static const uint8_t s_init[] = {0x05, 0x20, 0x00, 0x0f, 0x06};
    static const uint8_t led_on[] = {0x0a, 0x20, 0x00, 0x03, 0x00, 0x01, 0x14};
    static const uint8_t auth[] = {0x06, 0x20, 0x00, 0x02, 0x01, 0x00};
    static const uint8_t rumble_end[] = {0x09, 0x00, 0x00, 0x09, 0x00, 0x0f, 0x00, 0x00,
                                         0x00, 0x00, 0x00, 0x00, 0x00};
    fprintf(stderr, "gipd: claimed %s\n", path);
    const struct { const uint8_t *p; int len; const char *name; } init[] = {
        {power_on, sizeof power_on, "power_on"}, {s_init, sizeof s_init, "s_init"},
        {led_on, sizeof led_on, "led_on"},       {auth, sizeof auth, "auth"},
        {rumble_end, sizeof rumble_end, "rumble_end"},
    };
    for (int i = 0; i < init_steps && i < 5; i++) {
        sleep(step_delay);
        fprintf(stderr, "gipd: sending %s\n", init[i].name);
        fprintf(stderr, "gipd: %s -> %d\n", init[i].name, send_pkt(fd, init[i].p, init[i].len));
    }
    fprintf(stderr, "gipd: pad %s initialised (%d steps)\n", path, init_steps);

    uint8_t d[64];
    for (;;) {
        int n = xfer(fd, EP_IN, d, sizeof d, 0);
        if (n < 0) {
            if (errno == ETIMEDOUT) continue;
            fprintf(stderr, "gipd: read error %d\n", errno);
            break;  // unplugged
        }
        static int logged;
        if (logged < 40 && (d[0] != 0x20 || logged < 10)) {
            logged++;
            fprintf(stderr, "gipd: in[%d]", n);
            for (int i = 0; i < n && i < 24; i++) fprintf(stderr, " %02x", d[i]);
            fprintf(stderr, "\n");
        }
        if (n >= 18 && d[0] == 0x20) {
            report(u, d);
        } else if (n >= 5 && d[0] == 0x07) {
            if (d[1] == 0x30) {  // guide button needs an ack
                uint8_t ack[] = {0x01, 0x20, d[2], 0x09, 0x00, 0x07, 0x20, 0x02,
                                 0x00, 0x00, 0x00, 0x00, 0x00};
                xfer(fd, EP_OUT, ack, sizeof ack, 1000);
            }
            emit(u, EV_KEY, BTN_MODE, d[4] & 0x01);
            emit(u, EV_SYN, SYN_REPORT, 0);
        } else if (n >= 1 && d[0] == 0x02) {
            // Pad announced itself again (re-plug/reset): redo the handshake.
            send_pkt(fd, power_on, sizeof power_on);
            send_pkt(fd, led_on, sizeof led_on);
        }
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    // Debug knobs: gipd [init_steps [step_delay_s]]
    if (argc > 1) init_steps = atoi(argv[1]);
    if (argc > 2) step_delay = atoi(argv[2]);
    // gipd N D -d LOG: detach from the su/adb session and log to LOG.
    if (argc > 4 && !strcmp(argv[3], "-d")) {
        int lf = open(argv[4], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (daemon(0, 0) < 0) return 1;
        if (lf >= 0) dup2(lf, 2);
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    int u = uinput_open();
    if (u < 0) {
        perror("uinput");
        return 1;
    }
    char path[64];
    for (;;) {
        if (find_pad(path, sizeof path) == 0) run(path, u);
        sleep(2);
    }
}
