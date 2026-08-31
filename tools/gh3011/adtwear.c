/* Ask the chip whether it is on a wrist, by doing what the vendor's daemon does.
 *
 * Everything before this file's current form was guesswork against the decompiler, and all of it
 * was wrong in the same way: the auto-detect table was applied and the chip started, and the wear
 * bit was read from a detector that had never been armed. The table alone does not arm it.
 *
 * This follows a capture instead. The vendor daemon was put back in its init slot with the i2c tap
 * attached and left until its app ran a wear check, which gave the sequence on the wire. Three
 * things in it were not guessable:
 *
 *   - A chip init before the table: the 0x0182 and 0x0180 writes, three OTP reads through the
 *     0x0064/0x006a/0x006c window, and the trims at 0x0194, 0x018a and 0x018c.
 *
 *   - A second pass after the table, overwriting six of the registers it just wrote with
 *     different values - notably the gain, which the table sets to 0x1f69 and this sets to
 *     0x2828, the measurement value. The table is not the configuration; it is most of it.
 *
 *   - A write of 0xfe30 to 0x0002 immediately before the start.
 *
 * The enable is written flat as 0x0001 at 0x00c0, not read-modify-written. An earlier version of
 * this file preserved bit 1, reasoning that 0x0003 was read there and writing 0x0001 would clear
 * something needed. The capture shows the vendor writing 0x0001 and reading 0x0001 back, so that
 * 0x0003 was a chip which had not been through the init above.
 *
 * Reading the answer needs an event: FUN_00018c9c reads the status at 0x0008 and only looks at
 * 0x00c0 when bit 4 says a wear or unwear event happened. Bit 8 clear is a wrist - the dispatcher
 * sends cause 4 to gh30x_wear_evt_handler and cause 5 to the unwear one.
 *
 *   adtwear          arm the detector and wait up to five seconds for an event
 *   adtwear -q       one line
 *   adtwear -w N     wait N milliseconds
 *   adtwear -v       show the sequence as it runs
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>

#include "adt_table.h"

#define DEV  "/dev/gh_tools"
#define XFER 0xc0084704u
#define PWR  0x40044702u
#define IRQ  0x40044707u
#define WAIT 0x00004701u       /* gh_dev_wait_irq */

#define ADDR 0x14

#define CMD_HALT   0xc0
#define CMD_START  0xc1
#define CMD_STOP   0xc4

#define ST_EVENT   0x0010      /* 0x0008 bit 4: a wear or unwear event */
#define C0_UNWORN  0x0100      /* 0x00c0 bit 8: set means off the wrist */

struct msg { unsigned short addr, flags, len; unsigned char *buf; };
struct rdwr { struct msg *msgs; int n; };

static int fd = -1;
static int verbose = 0;

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

/* One OTP word, through the window the capture shows: address into 0x0064, pulse 0x006a, read
 * 0x006c. The daemon reads three of these every time it configures the part and does nothing
 * visible with them on the bus, so they are trims the chip applies internally. */
static unsigned short otp(unsigned short which)
{
    unsigned short v = 0;
    wr16(0x0064, which);
    wr16(0x006a, 0x0001);
    wr16(0x006a, 0x0000);
    rd16(0x006c, &v);
    return v;
}

/* The init the daemon runs before it configures anything, verbatim from the capture. */
static void chip_init(void)
{
    unsigned short v = 0;

    cmd(CMD_HALT);
    rd16(0x0028, &v);
    if (verbose) printf("  id     0x0028 = %04x%s\n", v, v == 0x0031 ? "" : "   expected 0031");
    rd16(0x0016, &v);
    if (verbose) printf("  rate   0x0016 = %04x%s\n", v,
                        v == 0x051e ? "" : "   the vendor has 051e here");
    /* The rate before anything else.
     *
     * 0x0100 is the first entry in the table and is written f530, which reads back ea60 - exactly
     * 60000, so the part is clamping it. The vendor's chip is at 051e, 25Hz, when that write
     * lands, because its previous run left it there; ours has whatever ppgd last set, which for a
     * saturation pass is 0147 and four times the rate. The table does set 0x0016, but twenty
     * entries too late to matter for this one.
     */
    if (v != 0x051e) wr16(0x0016, 0x051e);
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

    /* The interrupt enable, and the reason an earlier attempt could set this and still see
     * nothing: on its own it does not arm a detector that was never configured. */
    wr16(0x0020, 0x0000);
    rd16(0x0022, &v);
    wr16(0x0022, (unsigned short)(v | 1));

    cmd(CMD_STOP);
}

/* The second pass, which the table does not contain and without which nothing fires. */
static void adt_overrides(void)
{
    unsigned short v = 0;

    cmd(CMD_STOP);
    cmd(CMD_HALT);
    rd16(0x0022, &v);

    wr16(0x0084, 0x0020);
    wr16(0x0118, 0x2828);       /* the measurement gain, not the table's 0x1f69 */
    wr16(0x0136, 0x0d20);
    wr16(0x0080, 0x0205);
    wr16(0x0082, 0x00c2);
    wr16(0x0186, 0x0001);

    rd16(0x00c0, &v);
    if (verbose) printf("  enable 0x00c0 = %04x before the final write\n", v);
    wr16(0x00c0, 0x0001);
    wr16(0x0002, 0xfe30);
}

static void adt_start(void)
{
    int i;

    /* Once. The capture shows the init block twice, two seconds apart, and running it twice here
     * leaves the chip not answering at all - the second pass reads the id as 0000. Those two are
     * the daemon handling a reset interrupt and then servicing the app's request, idle between,
     * rather than a part that needs configuring twice. */
    chip_init();

    cmd(CMD_HALT);
    for (i = 0; i < (int)(sizeof adt_hb / sizeof adt_hb[0]); i++) {
        unsigned short reg = adt_hb[i].reg, back = 0;

        /* The table gives the enable's address as 0x10c0, which is 0x00c0 with a flag in the top
         * nibble this passthrough does not decode: written there it does nothing. The vendor
         * writes 0x00c0 and does not read it back, which is why this one is not verified. */
        if (reg == 0x10c0) { wr16(0x00c0, adt_hb[i].val); continue; }

        wr16(reg, adt_hb[i].val);
        if (rd16(reg, &back) == 0 && back != adt_hb[i].val && verbose)
            printf("  %04x wrote %04x read back %04x\n", reg, adt_hb[i].val, back);
    }

    adt_overrides();
    cmd(CMD_START);
}

/* Stop, and turn off what was turned on.
 *
 * Stopping the part is not the same as disabling the detector: 0x00c0 bit 0 stays set through a
 * c4, so the detector remains armed for whatever runs next. That next thing is usually a
 * measurement - the launcher asks whether the watch is worn immediately before measuring - and a
 * detector still sampling underneath it is a second consumer of the same front end. Left enabled
 * it showed up as a gain search that ran away, 0x37b5 against the 0x2828 a good measurement uses,
 * with the DC reading zero and no window agreeing with any other.
 */
static void adt_stop(void)
{
    unsigned short v = 0;

    cmd(CMD_STOP);
    usleep(500);
    if (rd16(0x00c0, &v) == 0) wr16(0x00c0, (unsigned short)(v & ~1));
}

/* Wait for an interrupt, or give up. 1 if one arrived.
 *
 * In a child with an alarm because the wait blocks indefinitely when nothing comes. Time it before
 * believing a success: on a chip that had not been configured this returned in about a second out
 * of a twenty second window, and returned the same with the interrupt disabled, which is a success
 * meaning nothing happened.
 */
static int wait_irq(int secs)
{
    pid_t pid = fork();
    int status = 0;

    if (pid < 0) return 0;
    if (pid == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = SIG_DFL;
        sigaction(SIGALRM, &sa, NULL);      /* no SA_RESTART: the alarm must break the ioctl */
        alarm(secs);
        /* A buffer, not a null. The capture shows the daemon passing a pointer here
         * (IOCTL 00004701 arg=bc722708) and this was calling it with zero, which a driver that
         * writes the interrupt cause back through the argument would reject or fault on. */
        {
            unsigned int arg[8];
            memset(arg, 0, sizeof arg);
            _exit(ioctl(fd, WAIT, arg) < 0 ? 2 : 0);
        }
    }
    if (waitpid(pid, &status, 0) < 0) return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int main(int argc, char **argv)
{
    int quiet = 0, waitms = 5000, i, on = 1, worn = -1, got = 0;
    unsigned short st = 0, c0 = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0) quiet = 1;
        else if (strcmp(argv[i], "-v") == 0) verbose = 1;
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) waitms = atoi(argv[++i]);
    }

    fd = open(DEV, O_RDWR);
    if (fd < 0) { printf("cannot open %s\n", DEV); return 1; }

    ioctl(fd, PWR, &on);
    usleep(50000);
    /* The capture never shows the daemon calling this. It powers the part, waits on the
     * interrupt, and that is all - so the driver arms the line itself, and calling
     * GH_IOC_ENABLE_IRQ by hand is this code's invention rather than the vendor's. Kept behind a
     * switch because it is cheap to try both ways. */
    if (getenv("IRQON")) ioctl(fd, IRQ, 1);

    adt_start();

    /* Silence is an answer, and the measurements behind saying so.
     *
     * The detector fires within about a second on a wrist and not at all off one: five runs each
     * way with a three second window gave worn on every on-wrist run and silence on every
     * off-wrist one, including with the watch sitting still rather than being handled. So a window
     * that closes without an event means not worn, rather than no answer.
     *
     * -1 is kept for the cases where nothing was asked: the device would not open, or the wait
     * could not be started. Those are different from a detector that ran and saw nobody.
     */
    got = wait_irq((waitms + 999) / 1000);
    if (!got) worn = 0;
    if (got) {
        cmd(CMD_HALT);
        usleep(500);
        rd16(0x0008, &st);
        /* The interrupt is the event; the status bit is a bonus.
         *
         * Requiring bit 4 of 0x0008 here loses a race with the kernel's own handler, which reads
         * and clears the status before this gets to it. It is set often enough to look required
         * and absent often enough to throw away good answers: one run reported nothing while
         * 0x00c0 sat there reading 0x0001 with bit 8 clear, on a wrist.
         *
         * Whether an event happened is already known - the wait returned - so the status is
         * logged and not depended on.
         */
        if (rd16(0x00c0, &c0) == 0)
            worn = (c0 & C0_UNWORN) ? 0 : 1;
    }

    adt_stop();
    if (getenv("IRQON")) ioctl(fd, IRQ, 0);

    if (quiet) {
        printf("worn=%d\n", worn);
    } else {
        printf("interrupt %s\n", got ? "arrived" : "did not arrive");
        printf("status 0x0008 = 0x%04x   event=%d\n", st, (st & ST_EVENT) ? 1 : 0);
        printf("wear   0x00c0 = 0x%04x   bit8=%d\n", c0, (c0 & C0_UNWORN) ? 1 : 0);
        printf("worn=%d%s\n", worn,
               worn ? "" : "   the window closed with no event, which is off the wrist");
    }
    close(fd);
    return 0;
}
