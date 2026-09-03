/* Read the part's FIFO without configuring it, so their settings can be read with our code.
 *
 * The published saturation does not track a desaturation - a finger meter went from 99 to 91 across
 * four breath holds and our figure sat between 96 and 98 - and no curve fixes that, because a
 * calibration maps one number onto another and cannot make a number that does not move start
 * moving. But their daemon gets a working saturation from this same sensor, the same registers and
 * the same FIFO, so the photons carry the information and our extraction is losing it.
 *
 * This splits acquisition from arithmetic. Their daemon configures the part and starts it; it is
 * then frozen with SIGSTOP, which leaves its /dev/gh_tools fd open so the driver never halts the
 * part; and this drains the FIFO and writes the samples out. It has been run, and the answer is
 * that our arithmetic is at fault - see docs/gh3011.md. From their stream, their configuration and
 * their gain, our formula gives R = 1.197 where their daemon reading the same samples reported 98
 * percent, which on their own curve is R = 0.496.
 *
 * Three designs were tried before one worked, and the failures are worth keeping:
 *
 *   reading alongside their daemon   two processes draining one FIFO each get a decimated stream
 *                                    full of gaps, and a one hertz pulse cannot be measured in the
 *                                    quarter-second bursts that survive
 *   kill -9 then read                closing their fd makes the driver halt the part on release:
 *                                    295 samples in 3 reads, then 2845 empty polls over 30 seconds
 *   polling instead of waiting       re-reads a slowly-updating register rather than draining the
 *                                    FIFO: 92,967 samples in 30 s, three thousand a second where
 *                                    two channels at a hundred hertz give two hundred, consecutive
 *                                    values differing by one or two counts
 *
 * Waiting on the interrupt with the daemon frozen gives 6200 samples in 31 reads over 30.9 seconds,
 * which is 200 a second, which is right.
 *
 *     fifograb <seconds> <outfile> [noarm]
 *
 * It writes one unsigned sample per line, the two channels interleaved, which is the format ppgd's
 * RAWDUMP uses and the analysis already reads.
 *
 * The one write it makes is `dd dd c3`, arming the next read, which is exactly what their daemon
 * does between reads - without it the FIFO fills once and stops. `noarm` suppresses even that, for
 * checking whether the stream survives on its own.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/ioctl.h>

#define XFER 0xc0084704u
#define WAIT 0x00004701u          /* blocks until the part has a burst ready */
#define ADDR 0x14
#define FIFO_LEVEL 0x004a
#define FIFO_DATA  0xaaaa
#define CMD_REG    0xdddd
#define CMD_ARM    0xc3

struct msg { unsigned short addr, flags, len; unsigned char *buf; };
struct rdwr { struct msg *msgs; int n; };

static int fd = -1;

static int wr(unsigned char *p, int n)
{
    struct msg m; struct rdwr r;
    m.addr = ADDR; m.flags = 0; m.len = (unsigned short)n; m.buf = p;
    r.msgs = &m; r.n = 1;
    return ioctl(fd, XFER, &r);
}

static int wr8(unsigned short g, unsigned char v)
{
    unsigned char p[3];
    p[0] = (unsigned char)(g >> 8); p[1] = (unsigned char)g; p[2] = v;
    return wr(p, 3);
}

static int rd16(unsigned short g, unsigned short *v)
{
    unsigned char a[2], d[2];
    struct msg m[2]; struct rdwr r;
    a[0] = (unsigned char)(g >> 8); a[1] = (unsigned char)g; d[0] = d[1] = 0;
    m[0].addr = ADDR; m[0].flags = 0; m[0].len = 2; m[0].buf = a;
    m[1].addr = ADDR; m[1].flags = 1; m[1].len = 2; m[1].buf = d;
    r.msgs = m; r.n = 2;
    if (ioctl(fd, XFER, &r) < 0) return -1;
    *v = (unsigned short)((d[0] << 8) | d[1]);
    return 0;
}

static int rdn(unsigned short g, unsigned char *out, int n)
{
    unsigned char a[2];
    struct msg m[2]; struct rdwr r;
    a[0] = (unsigned char)(g >> 8); a[1] = (unsigned char)g;
    m[0].addr = ADDR; m[0].flags = 0; m[0].len = 2; m[0].buf = a;
    m[1].addr = ADDR; m[1].flags = 1; m[1].len = (unsigned short)n; m[1].buf = out;
    r.msgs = m; r.n = 2;
    return ioctl(fd, XFER, &r);
}

static double now(void)
{
    struct timeval t;
    gettimeofday(&t, 0);
    return t.tv_sec + t.tv_usec / 1e6;
}

int main(int argc, char **argv)
{
    unsigned char buf[3000];
    double secs, t0;
    FILE *f;
    int arm = 1, total = 0, rounds = 0, empty = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <seconds> <outfile> [noarm]\n", argv[0]);
        return 2;
    }
    secs = atof(argv[1]);
    if (argc > 3 && strcmp(argv[3], "noarm") == 0) arm = 0;

    fd = open("/dev/gh_tools", O_RDWR);
    if (fd < 0) { fprintf(stderr, "open /dev/gh_tools: %s\n", strerror(errno)); return 1; }

    f = fopen(argv[2], "w");
    if (!f) { fprintf(stderr, "open %s: %s\n", argv[2], strerror(errno)); return 1; }

    t0 = now();
    while (now() - t0 < secs) {
        unsigned short lvl = 0;
        int want, k;

        /* Wait for the burst rather than polling for it.
         *
         * Polling read the same value over and over: 92,967 samples in thirty seconds, three
         * thousand a second where two channels at a hundred hertz give two hundred, and consecutive
         * values differing by one or two counts. That is a slowly-updating current-value register
         * being re-read, not a FIFO being drained.
         *
         * The race this was avoiding does not exist here. The daemon is frozen, not running, so
         * nothing else is waiting on the interrupt and blocking on it is safe.
         */
        if (arm) wr8(CMD_REG, CMD_ARM);
        if (ioctl(fd, WAIT, 0) < 0) { empty++; usleep(2000); continue; }

        if (rd16(FIFO_LEVEL, &lvl) < 0) { usleep(5000); continue; }
        want = (int)lvl * 3;
        if (want <= 0) { empty++; continue; }
        if (want > (int)sizeof buf) want = (int)sizeof buf;

        if (rdn(FIFO_DATA, buf, want) < 0) { usleep(5000); continue; }
        for (k = 0; k + 2 < want; k += 3) {
            unsigned int v = ((unsigned int)buf[k] << 16) | (buf[k+1] << 8) | buf[k+2];
            fprintf(f, "%u\n", v);
            total++;
        }
        rounds++;
    }

    fclose(f);
    close(fd);
    fprintf(stderr, "%d samples in %d reads over %.1f s (%d empty polls)\n",
            total, rounds, now() - t0, empty);
    return total > 0 ? 0 : 1;
}
