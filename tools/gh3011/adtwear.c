/* Ask the chip whether it is on a wrist, the way its own driver does.
 *
 * Three earlier attempts read bit 8 of a register and believed the answer. It is not a state that
 * can be read: FUN_00018c9c reads the interrupt status at 0x0008 first, and only looks at 0x00c0
 * when bit 4 of that status says a wear or unwear event has happened. Read at any other moment
 * 0x00c0 holds whatever the last event left in it, which off a wrist is "worn" and stays there
 * however long you wait.
 *
 * So this waits for the event rather than sampling for a level:
 *
 *   1  halt, apply the auto-detect configuration, start
 *   2  poll 0x0008 until bit 4 is set, or give up
 *   3  read 0x00c0; bit 8 clear is on a wrist, set is off it
 *   4  stop
 *
 * Step 2 is the part that has no answer of its own. A timeout means no transition occurred while
 * this was watching, not that the watch is off a wrist, and it reports that as worn=-1 rather than
 * picking the more convenient of the two. The vendor has the interrupt line and never has to guess;
 * we poll, and a poll can miss.
 *
 * The halt-read-resume around each poll is theirs too, from the top and bottom of the same
 * function: command 0xc0 and 500us before touching the status, 0xc1 to resume or 0xc4 to stop.
 *
 *   adtwear          watch for up to five seconds, with the registers shown
 *   adtwear -q       the same, one line
 *   adtwear -w N     watch for N milliseconds
 *   adtwear -v       print every poll, to see whether the status is moving at all
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
#define IRQ  0x40044707u       /* GH_IOC_ENABLE_IRQ / GH_IOC_DISABLE_IRQ, one command taking 1 or 0 */
#define WAIT 0x00004701u       /* gh_dev_wait_irq: blocks until the chip raises one */
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

static int rd1(unsigned short g, unsigned short *v)
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

/* Twice, keeping the second.
 *
 * This passthrough returns the previous read's value often enough to invent a result: a scan of
 * 0x00b0 to 0x00e0 followed by a read of another address returned 0x00dc's value, and that was
 * taken for a wear bit for most of an afternoon. wearreg.c records an earlier instance. A second
 * read costs a millisecond and removes the whole class of mistake.
 */
static int rd16(unsigned short g, unsigned short *v)
{
    unsigned short a, b;
    if (rd1(g, &a) < 0) return -1;
    if (getenv("SINGLE")) { *v = a; return 0; }
    if (rd1(g, &b) < 0) return -1;
    *v = b;
    return 0;
}

/* Let the chip drive its interrupt line.
 *
 * Our start sequence has never produced an interrupt, for wear or for anything else, and this is
 * the difference: the vendor's init ends by clearing 0x0020 and setting bit 0 of 0x0022 through a
 * read-modify-write, and we do neither. These notes had 0x0022 as a register read once at start-up
 * whose bit 0 is tested, which is what it looks like from the daemon's side - it reads 0x1a80 and
 * writes back 0x1a81, and only the write matters.
 *
 * The host-side ioctl is not a substitute. GH_IOC_ENABLE_IRQ tells the kernel to listen to the
 * line; nothing tells the chip to pull it.
 */
static void int_enable(void)
{
    unsigned short v = 0;

    wr16(0x0020, 0x0000);
    if (rd16(0x0022, &v) == 0) wr16(0x0022, (unsigned short)(v | 1));
    if (verbose) printf("0x0022 was %04x, wrote %04x\n", v, v | 1);
}

/* Apply the detector configuration and start it. */
static void adt_start(void)
{
    int i;

    cmd(CMD_HALT);
    usleep(20000);
    for (i = 0; i < (int)(sizeof adt_hb / sizeof adt_hb[0]); i++) {
        unsigned short reg = adt_hb[i].reg;

        /* The enable is left until the table is in - below. */
        if (reg == 0x10c0) continue;
        wr16(reg, adt_hb[i].val);
    }

    /* The enable last, and as a read-modify-write.
     *
     * Two corrections to the table's own entry. The address first: it gives 0x10c0, which carries
     * a flag in the top nibble that this passthrough does not decode - written there it does
     * nothing and reads back zero however long you wait.
     *
     * Then the value. 0x00c0 reads 0x0003, and the table's 0x0001 written flat clears bit 1,
     * after which every register in the space reads zero. That is the state this looked like it
     * was in when the LED still pulsed and nothing could be read - blamed at the time on a table
     * length, and then on a bus. Bit 0 is the enable; bit 1 is something the part needs. Set the
     * one without clearing the other, which is how the vendor writes where we can watch it:
     * 0x0022 read as 0x1a80 and written back as 0x1a81.
     *
     * Last rather than in its table position because a read of 0x00c0 partway through the table
     * fails, and a read-modify-write needs the read to work.
     */
    {
        unsigned short cur = 0;
        int ok = rd16(0x00c0, &cur) == 0;
        if (ok) wr16(0x00c0, (unsigned short)(cur | 1));
        if (verbose) printf("0x00c0 read %s, was %04x, wrote %04x\n",
                            ok ? "ok" : "FAILED", cur, cur | 1);
    }
    if (!getenv("NOINT")) int_enable();
    cmd(CMD_START);
}

static void adt_stop(void)
{
    cmd(CMD_STOP);
    usleep(500);
}

/* Wait for the chip to raise an interrupt, or give up. 1 if one arrived, 0 if not.
 *
 * Polling the status register for the event does not work and cannot be made to: this file's own
 * notes record that 0x0008 stays 0x0000 and no interrupt ever arrives under our start sequence,
 * which is why the measurement path polls the FIFO level directly instead. Wear detection cannot
 * do that - there is no level to poll, only an event - so it has to use the interrupt or nothing.
 *
 * The reason the measurement path avoids the interrupt does not apply here. It avoids it because
 * an enabled IRQ lets the kernel driver service the chip and drain the FIFO before we can read
 * it; wear detection wants no FIFO, only the fact that something happened, so the kernel draining
 * data we do not want costs nothing.
 *
 * In a child with an alarm because the wait blocks forever when the chip is not sampling - ten
 * minutes, on a powered sensor on a wrist, the time it was called directly.
 */
static int wait_irq(int fd, int secs)
{
    pid_t pid = fork();
    int status = 0;

    if (pid < 0) return 0;
    if (pid == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = SIG_DFL;
        /* No SA_RESTART: the alarm has to interrupt the ioctl rather than let it resume. */
        sigaction(SIGALRM, &sa, NULL);
        alarm(secs);
        _exit(ioctl(fd, WAIT, 0) < 0 ? 2 : 0);
    }
    if (waitpid(pid, &status, 0) < 0) return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int main(int argc, char **argv)
{
    int quiet = 0, waitms = 5000, i, on = 1, worn = -1, polls = 0;
    int poll_mode = 0, got_irq = 0;
    unsigned short st = 0, c0 = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0) quiet = 1;
        else if (strcmp(argv[i], "-v") == 0) verbose = 1;
        else if (strcmp(argv[i], "-p") == 0) poll_mode = 1;
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) waitms = atoi(argv[++i]);
    }

    fd = open(DEV, O_RDWR);
    if (fd < 0) { printf("cannot open %s\n", DEV); return 1; }

    ioctl(fd, PWR, &on);
    usleep(50000);

    if (!poll_mode) ioctl(fd, IRQ, 1);
    adt_start();

    if (poll_mode) {
        /* Kept because it is the obvious thing to try, and because seeing 0x0008 sit at zero for
         * the whole window is the evidence that the interrupt is not optional. */
        for (; polls * 100 < waitms; polls++) {
            usleep(100000);
            cmd(CMD_HALT);
            usleep(500);
            if (rd16(0x0008, &st) < 0) st = 0;
            if (st & ST_EVENT) {
                if (rd16(0x00c0, &c0) == 0) worn = (c0 & C0_UNWORN) ? 0 : 1;
                break;
            }
            if (verbose) printf("poll %2d  0008=%04x\n", polls, st);
            cmd(CMD_START);
        }
    } else if (wait_irq(fd, (waitms + 999) / 1000)) {
        /* Halt before reading, the way their interrupt path does. */
        cmd(CMD_HALT);
        usleep(500);
        rd16(0x0008, &st);
        if (rd16(0x00c0, &c0) == 0) worn = (c0 & C0_UNWORN) ? 0 : 1;
        got_irq = 1;
    }
    /* Everything that is not zero, for comparing one wrist state against another.
     *
     * If the detector leaves its verdict anywhere readable, it has to show up as a register that
     * differs on and off a wrist. If nothing differs, it does not, and no amount of deciding which
     * bit ought to mean what will change that.
     */
    if (getenv("DUMP")) {
        /* Arm the read first.
         *
         * A running chip returns zero for every register in this space, which looked like the part
         * being broken by something written to it and is not: ppgd reads the FIFO level from a
         * running chip all day. The difference is command 0xc3, captured from the vendor between
         * bursts and noted in the docs as arming the read.
         */
        cmd(0xc3);
        usleep(2000);
        unsigned short a, v;
        for (a = 0x0000; a < 0x0200; a += 2)
            if (rd16(a, &v) == 0 && v != 0) printf("%s %04x %04x\n", getenv("DUMP"), a, v);
    }

    adt_stop();
    if (!poll_mode) ioctl(fd, IRQ, 0);

    if (quiet) {
        printf("worn=%d\n", worn);
    } else {
        printf("%s, %d ms\n",
               poll_mode ? "polled" : "waited on the interrupt", waitms);
        printf("status 0x0008 = 0x%04x   event=%d\n", st, (st & ST_EVENT) ? 1 : 0);
        if (worn < 0) {
            printf("worn=-1   no event while watching, which is not an answer either way\n");
        } else {
            printf("wear   0x00c0 = 0x%04x   bit8=%d\n", c0, (c0 & C0_UNWORN) ? 1 : 0);
            printf("worn=%d\n", worn);
        }
    }
    close(fd);
    return 0;
}
