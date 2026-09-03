/* Read the part's registers while a measurement is running, and change nothing.
 *
 * Every comparison so far has diffed what the two sides *write*. That cannot see a register one
 * side never touches and the other leaves at a different value, and something like that has to be
 * what is left: fifty-seven writes agree, and yet our channel 2 is railed from the first sample at
 * a gain of 0x2828 while theirs is comfortable at 0x4f44, which is higher.
 *
 * This configures nothing, starts nothing and stops nothing.
 *
 * The reads are done three at a time because this bus returns the previous transaction's value
 * often enough that a plain sweep reads long runs of one number and calls them registers - which
 * is what the first version of this did, reporting sixty-five differences of which most were an
 * echo of 0x57e4 on one side and 0x0446 on the other. Reading the wanted register, then a known
 * different one, then the wanted register again defeats that: an echo would return the value of
 * the register read in between. Anything that will not settle is printed as unread rather than
 * guessed at.
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>

#define XFER 0xc0084704u
#define ADDR 0x14
#define SEPARATOR 0x0016u              /* a register with a stable, distinctive value */

struct msg { unsigned short addr, flags, len; unsigned char *buf; };
struct rdwr { struct msg *msgs; int n; };
static int fd = -1;

static int rd16(unsigned short g, unsigned short *v)
{
    unsigned char a[2], d[2];
    struct msg m[2]; struct rdwr r;
    a[0] = g >> 8; a[1] = (unsigned char)g; d[0] = d[1] = 0;
    m[0].addr = ADDR; m[0].flags = 0; m[0].len = 2; m[0].buf = a;
    m[1].addr = ADDR; m[1].flags = 1; m[1].len = 2; m[1].buf = d;
    r.msgs = m; r.n = 2;
    if (ioctl(fd, XFER, &r) < 0) return -1;
    *v = (unsigned short)((d[0] << 8) | d[1]);
    return 0;
}

int main(void)
{
    static const unsigned short ranges[][2] = {
        { 0x0000, 0x00e0 }, { 0x0100, 0x0140 }, { 0x0180, 0x01a0 }, { 0, 0 }
    };
    int i;

    fd = open("/dev/gh_tools", O_RDWR);
    if (fd < 0) { fprintf(stderr, "open: /dev/gh_tools\n"); return 1; }

    for (i = 0; ranges[i][1]; i++) {
        unsigned int g;
        for (g = ranges[i][0]; g <= ranges[i][1]; g += 2) {
            unsigned short v1 = 0, v2 = 0, sep = 0;
            int tries;

            for (tries = 0; tries < 8; tries++) {
                if (rd16((unsigned short)g, &v1) < 0) { usleep(2000); continue; }
                if (rd16(SEPARATOR, &sep) < 0)        { usleep(2000); continue; }
                if (rd16((unsigned short)g, &v2) < 0) { usleep(2000); continue; }
                if (v1 == v2 && (g == SEPARATOR || v1 != sep)) break;
                usleep(2000);
            }
            if (tries < 8) printf("%04x %04x\n", g, v1);
            else           printf("%04x ????\n", g);
        }
    }
    close(fd);
    return 0;
}
