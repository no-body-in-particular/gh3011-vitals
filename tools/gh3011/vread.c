/* Read samples the way the vendor does, and find out what comes back.
 *
 * Their measurement loop does not poll the FIFO over the passthrough at all:
 *
 *     IOCTL 00004701   wait for the interrupt, which blocks for them
 *     R 0008 = 0002    new data
 *     IOCTL 825a470a   602 bytes of samples in one call
 *     W dd dd c3       arm the next read
 *
 * 602 is 2 + 100 * 6, which is the shape the accelerometer ioctl already uses - a count followed
 * by six-byte records - so that is the first guess and this checks it rather than assuming.
 *
 * Two things are being tested at once and they are separable. Whether the interrupt blocks for us
 * when the chip is configured their way, which it has never done under ours; and what the bulk
 * read returns. If the interrupt still returns immediately, the read is still worth having: it
 * would replace a FIFO poll over a bus that returns the previous value often enough to have
 * produced three wrong conclusions in these notes.
 *
 * IT DOES NOT WORK YET, and what has been ruled out is worth as much as what has not.
 *
 * With all forty-seven of their registers applied in their order, after the chip init adtwear
 * established, with the interrupt enabled through 0x40044707 and the mode announced through
 * 0x40184709 exactly as their trace does: no LED lights, the wait times out at eight seconds
 * every time, and the bulk read returns six hundred bytes of zero. Their daemon on the same
 * hardware minutes earlier reported 97% and lit the LED.
 *
 * So the missing piece is not the register set, not the init, not the interrupt enable and not
 * the mode call. Something else about how the driver is brought up only happens for their
 * process - it is started by init rather than from a shell, and it holds the device open
 * continuously rather than for one measurement.
 *
 * Two things not yet tried, in the order worth trying them. The published GH30x driver sources
 * document how the interrupt is armed and nobody here has read them - that is the cheap one. And
 * their daemon can be run under the tap with the device already open, then this run alongside it,
 * to see which call ours is missing rather than guessing at it.
 *
 *   vread            their configuration, then read what arrives
 *   vread -n N       stop after N reads, default 5
 *   vread -o         our configuration instead, for comparison
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <signal.h>
#include <setjmp.h>

#define DEV   "/dev/gh_tools"
#define XFER  0xc0084704u
#define PWR   0x40044702u
#define WAIT  0x00004701u
#define IRQEN 0x40044707u        /* interrupt enable, 1 or 0 */
#define MODE  0x40184709u        /* 24 bytes; the vendor calls it right after the start */
#define BULK  0x825a470au        /* _IOR('G', 0x0a, 602) - the vendor's sample read */
#define ADDR  0x14

struct msg { unsigned short addr, flags, len; unsigned char *buf; };
struct rdwr { struct msg *msgs; int n; };

static int fd = -1;
static sigjmp_buf jb;

static void on_alarm(int sig) { (void) sig; siglongjmp(jb, 1); }

static int wr(unsigned char *p, int n)
{
    struct msg m; struct rdwr r;
    m.addr = ADDR; m.flags = 0; m.len = (unsigned short) n; m.buf = p;
    r.msgs = &m; r.n = 1;
    return ioctl(fd, XFER, &r);
}

static int wr16(unsigned short g, unsigned short v)
{
    unsigned char p[4];
    p[0] = (unsigned char)(g >> 8); p[1] = (unsigned char) g;
    p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char) v;
    return wr(p, 4);
}

static int cmd(unsigned char c)
{
    unsigned char p[3];
    p[0] = 0xdd; p[1] = 0xdd; p[2] = c;
    return wr(p, 3);
}

static int rd16(unsigned short g, unsigned short *v)
{
    unsigned char a[2], b[2];
    struct msg m[2]; struct rdwr r;
    a[0] = (unsigned char)(g >> 8); a[1] = (unsigned char) g;
    b[0] = b[1] = 0;
    m[0].addr = ADDR; m[0].flags = 0; m[0].len = 2; m[0].buf = a;
    m[1].addr = ADDR; m[1].flags = 1; m[1].len = 2; m[1].buf = b;
    r.msgs = m; r.n = 2;
    if (ioctl(fd, XFER, &r) < 0) return -1;
    *v = (unsigned short)((b[0] << 8) | b[1]);
    return 0;
}

/* The OTP window: address, pulse, read. */
static unsigned short otp(unsigned short which)
{
    unsigned short v = 0;
    wr16(0x0064, which);
    wr16(0x006a, 0x0001);
    wr16(0x006a, 0x0000);
    rd16(0x006c, &v);
    return v;
}

/* The init the part needs before anything runs at all.
 *
 * Left out of the first version of this, and the result was exactly what that looks like: the
 * bulk read succeeded and returned six hundred bytes of zero, the interrupt never came, and the
 * LED stayed dark. adtwear established this sequence and the vendor's own trace repeats it before
 * every measurement.
 */
static void chip_init(void)
{
    unsigned short v = 0;

    cmd(0xc0);
    rd16(0x0028, &v);
    rd16(0x0016, &v);
    rd16(0x0008, &v);
    wr16(0x0182, 0x84db);
    wr16(0x0180, 0x008d);
    rd16(0x00e4, &v);
    otp(0x0020);
    otp(0x0022);
    otp(0x0024);
    rd16(0x0194, &v);
    wr16(0x0194, 0x0003);
    rd16(0x018a, &v);
    wr16(0x018a, 0x08a4);
    wr16(0x018c, 0x005d);
    rd16(0x0084, &v); rd16(0x0118, &v); rd16(0x0136, &v);
    rd16(0x0080, &v); rd16(0x0082, &v); rd16(0x0186, &v);
    wr16(0x0020, 0x0000);
    rd16(0x0022, &v);
    wr16(0x0022, (unsigned short)(v | 1));
    cmd(0xc4);
}

/* Their configuration for a saturation, from docs/vendor-spo2-writes.txt - the writes captured
 * between their halt and their start, in the order they made them. */
static const struct { unsigned short reg, val; } vendor_spo2[] = {
    { 0x0100, 0xf530 }, { 0x0102, 0x4e20 }, { 0x0104, 0xf530 }, { 0x0106, 0x4e20 },
    { 0x0108, 0xf530 }, { 0x010a, 0x2710 }, { 0x010c, 0xf148 }, { 0x010e, 0x57e4 },
    { 0x0110, 0xf148 }, { 0x0112, 0x57e4 }, { 0x0114, 0xf148 }, { 0x0116, 0x30d4 },
    { 0x011c, 0x01ff }, { 0x011e, 0x01ff }, { 0x0120, 0x01ff }, { 0x0126, 0x0202 },
    { 0x0128, 0x0002 }, { 0x0130, 0x0346 }, { 0x0132, 0x0446 }, { 0x0134, 0x0546 },
    { 0x0016, 0x0147 }, { 0x0080, 0x0605 }, { 0x0082, 0x01c6 }, { 0x0084, 0x0023 },
    { 0x0118, 0x9055 }, { 0x011a, 0x0000 }, { 0x012e, 0x0000 }, { 0x0136, 0x0000 },
    { 0x0186, 0x0406 }, { 0x0180, 0x004d }, { 0x012a, 0x0303 }, { 0x012c, 0x0003 },
    { 0x00c2, 0xffff }, { 0x00c4, 0x0528 }, { 0x00c6, 0xffff }, { 0x00c8, 0x0528 },
    { 0x00ca, 0x00a0 }, { 0x00cc, 0x006e }, { 0x00ce, 0x042e }, { 0x00d0, 0x0000 },
    { 0x00d4, 0x042e }, { 0x00d6, 0x0000 }, { 0x00d8, 0x0303 }, { 0x00da, 0x0101 },
    { 0x00dc, 0x0101 }, { 0x00de, 0x0000 }, { 0x00c0, 0x0001 },
};

int main(int argc, char **argv)
{
    unsigned char buf[602];
    int i, on = 1, want = 5, ours = 0, got = 0;
    unsigned short v = 0;
    struct timeval t0, t1;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) want = atoi(argv[++i]);
        else if (strcmp(argv[i], "-o") == 0) ours = 1;
    }

    fd = open(DEV, O_RDWR);
    if (fd < 0) { printf("cannot open %s\n", DEV); return 1; }

    ioctl(fd, PWR, &on);
    usleep(50000);

    if (rd16(0x0028, &v) < 0 || v != 0x0031) {
        printf("chip id reads %04x, not 0031 - the bus is not returning registers\n", v);
        return 1;
    }
    printf("chip id ok\n");

    /* Arm the line before anything else. The docs list this and nothing in this project has
     * ever called it during a measurement - the wear work enabled it and saw no change, but that
     * was on a detector that was never configured. */
    {
        int one = 1;
        printf("irq enable rc=%d\n", ioctl(fd, IRQEN, &one));
    }

    chip_init();
    usleep(20000);
    cmd(0xc0);
    usleep(20000);
    if (!ours) {
        for (i = 0; i < (int)(sizeof vendor_spo2 / sizeof vendor_spo2[0]); i++)
            wr16(vendor_spo2[i].reg, vendor_spo2[i].val);
        printf("applied %d of their registers\n",
               (int)(sizeof vendor_spo2 / sizeof vendor_spo2[0]));
    }
    cmd(0xc1);
    usleep(20000);

    /* The command between their start and their first read. We had c0, c1, c3 and c4; this one
     * appears once, there, and nowhere else in the capture. */
    cmd(0xa1);

    /* Announce the mode, which the vendor does immediately after starting and which we have only
     * ever used to publish a finished result. If the driver gates interrupt delivery on knowing
     * what is running - and something must, because ours has never delivered one - this is the
     * call that would do it. Five is red and infrared, as setmode uses. */
    {
        unsigned int w[6];
        memset(w, 0, sizeof w);
        w[0] = 5;
        printf("mode ioctl rc=%d\n", ioctl(fd, MODE, w));
    }

    for (got = 0; got < want; got++) {
        int rc;

        /* Under an alarm, because it can block forever. Their daemon is happy to sit here; a
         * probe that does the same has to be killed, and a killed probe leaves the chip started
         * and the next run inherits it. */
        gettimeofday(&t0, NULL);
        if (sigsetjmp(jb, 1) == 0) {
            signal(SIGALRM, on_alarm);
            alarm(8);
            rc = ioctl(fd, WAIT, 0);
            alarm(0);
        } else {
            rc = -2;
        }
        gettimeofday(&t1, NULL);
        {
            long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000;
            rd16(0x0008, &v);
            printf("\nwait rc=%d after %ld ms   status=%04x%s\n", rc, ms, v,
                   ms < 50 ? "   (did not wait - see the note in adtwear)" : "");
        }

        memset(buf, 0, sizeof buf);
        rc = ioctl(fd, BULK, buf);
        if (rc < 0) { printf("bulk read failed\n"); break; }

        {
            unsigned n = buf[0] | (buf[1] << 8);
            printf("bulk rc=%d  count field = %u", rc, n);
            if (n > 0 && n <= 100) {
                printf("  (%u six-byte records)\n", n);
                for (i = 0; i < (int)(n > 4 ? 4 : n); i++) {
                    const unsigned char *q = buf + 2 + i * 6;
                    printf("    %02x %02x %02x %02x %02x %02x"
                           "   as 24-bit: %8u %8u\n",
                           q[0], q[1], q[2], q[3], q[4], q[5],
                           (unsigned)(q[0] | (q[1] << 8) | (q[2] << 16)),
                           (unsigned)(q[3] | (q[4] << 8) | (q[5] << 16)));
                }
            } else {
                printf("  - not a count; first bytes:\n      ");
                for (i = 0; i < 18; i++) printf("%02x ", buf[i]);
                printf("\n");
            }
        }
        cmd(0xc3);
    }

    cmd(0xc4);
    close(fd);
    return 0;
}
