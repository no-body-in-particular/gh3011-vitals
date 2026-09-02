/* vitalsd - measure vitals on request, over a socket.
 *
 * The launcher cannot do this itself. Driving the chip needs the vendor daemon stopped, which
 * needs root, and wsu does not give the app process root - and RootShell's twenty second timeout
 * is shorter than a forty second measurement anyway. So the privileged part lives here, started
 * from init, and the app only speaks to a socket.
 *
 * Protocol, one line each way:
 *
 *     ->  hr            green LED, heart rate
 *     ->  spo2          red and IR, adds the ratio of ratios and the pulse shape
 *     ->  wear          the thermometer alone: no LEDs, no measurement
 *     <-  hr=49 spread=2 hz=24.9 ... spo2=98 sbp=102 dbp=66
 *     <-  hr=0 reason=...            when nothing trustworthy came out
 *
 * The socket is in Linux's abstract namespace, which is what android.net.LocalSocket speaks by
 * default, so the Java side needs no filesystem permissions.
 *
 * Only one measurement runs at a time: the sensor is a single piece of hardware, and two
 * overlapping requests would fight over it exactly as we and the vendor daemon did.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <stddef.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>

#define SOCKNAME "watchvitals"      /* abstract: android.net.LocalSocket, ABSTRACT namespace */
#define HELPER   "/data/local/tmp/ppgd"

/* The chip's own wear detector, run as a helper for the same reason ppgd is: it needs
 * /dev/gh_tools to itself, and a daemon holding that open cannot also hand it to a measurement. */
#define ADTHELPER "/data/local/tmp/adtwear"
#define SECS_HR   "40"   /* green is 25 Hz: it needs the time to fill enough windows */
#define SECS_SPO2 "45"   /* red is 100 Hz - 4,500 samples, plenty for the
                          * windows and for the beat the shape is built on */
/* The balanced pass. The vendor spends about eight seconds here and we did too, but eight is
 * not enough on this sensor: four runs at 8 s gave R of 1.048, 0.907, 0.751 and 0.782, and the
 * same wrist at 25 s gave 0.877, 0.841 and 0.741 - half the spread. The extra seventeen seconds
 * are the cheapest accuracy available, and the pass is still shorter than the one after it. */
#define SECS_RATIO "25"

/* Where the short pass leaves its samples for the long pass to explain. See measure(). */
#define KEEP "/data/local/tmp/pass1.txt"

/* Roughly a day of measurements, at seventy kilobytes each. */
#define KEEP_WAVES 200


#define TEMP_ENABLE "/sys/devices/virtual/input/input6/enable"
#define TEMP_VALUE  "/sys/devices/virtual/input/input6/value"

/* Hundredths of a degree. Skin holds the thermopile in the low thirties - 34.57 on the wrist it
 * was measured against - while on a table it falls to room temperature within minutes. Thirty
 * sits between the two with room on either side. */
#define WORN_C 3000

/* The whole of a small file. */
static int slurp(const char *path, char *buf, size_t n)
{
    ssize_t got;
    int f = open(path, O_RDONLY);
    if (f < 0) return -1;
    got = read(f, buf, n - 1);
    close(f);
    if (got <= 0) return -1;
    buf[got] = 0;
    return 0;
}

/* What the chip's own detector says: 1 on a wrist, 0 off it, -1 no answer.
 *
 * Not a second opinion on the thermometer so much as a differently shaped one. The thermometer
 * reports a level and will always answer, given six seconds. This reports an event - bit 4 of
 * 0x0008 - and answers immediately or not at all, because bit 8 of 0x00c0 only means anything
 * when an event has set it. A watch that has been still on a wrist for an hour has generated no
 * transition and gets -1, which is the honest answer to a question asked of an edge detector.
 *
 * Whether starting the detector makes it evaluate the current state, and so answer without
 * waiting for the wearer to do anything, is the one thing this cannot be reasoned into: it is
 * reported alongside the thermometer's answer on every wear request so the logs can settle it.
 */
static int adt_worn(int waitms)
{
    char cmd[128], line[128];
    FILE *p;
    int v = -1;

    if (access(ADTHELPER, X_OK) != 0) return -1;
    snprintf(cmd, sizeof cmd, "%s -q -w %d 2>/dev/null", ADTHELPER, waitms);
    p = popen(cmd, "r");
    if (!p) return -1;
    while (fgets(line, sizeof line, p)) {
        char *at = strstr(line, "worn=");
        if (at) v = atoi(at + 5);
    }
    pclose(p);
    return v;
}

/* Wrist temperature in hundredths of a degree, or -1 if the sensor will not say.
 *
 * A gxts02s thermopile, reported through the "temperature" input device rather than a thermal
 * zone - the zones are the CPU, GPU, charger and board, none of which touch the wearer. It reads
 * "0 0" until the driver has produced a sample, which takes about six seconds from cold, so this
 * enables it and waits instead of believing the first look.
 *
 * Left enabled afterwards: disabling would save a little current, but the next measurement would
 * pay those six seconds again and the framework may be sharing the sensor.
 */
static int read_temp(int patience)
{
    char buf[64];
    int f, t, tries;

    f = open(TEMP_ENABLE, O_WRONLY);
    if (f >= 0) { write(f, "1\n", 2); close(f); }

    for (tries = 0; tries < patience; tries++) {
        if (slurp(TEMP_VALUE, buf, sizeof buf) == 0) {
            t = atoi(buf);
            if (t > 0) return t;
        }
        sleep(1);
    }
    return -1;
}


/* Everything measured, kept on the card.
 *
 * The reply carries far more than the launcher uses - the ratio, its window spread, the matched
 * amplitudes, the raw pulse shape before the gate - and all of it is thrown away the moment the
 * socket closes. Those are exactly the numbers needed to work out why a saturation will not hold
 * still, and they cannot be reconstructed afterwards from a heart rate.
 *
 * One line per measurement, appended, with the time in front of it. It is a few hundred bytes a
 * measurement and the card has gigabytes; it survives reboots, which logcat does not, and it can
 * be pulled whenever there is a question to ask of it.
 */
#define VLOG "/sdcard/vitals.log"

/* Cumulative steps from the DA217 at 2-0026, or -1.
 *
 * 0x0d is the high byte and 0x0e the low one.
 *
 * This was found by dumping the map, walking, and taking what moved by about the number of steps
 * taken - and that method got it half wrong. Only the low byte moves over a short walk, so
 * 0x0e/0x0f little endian tracked the change exactly as well as the truth did and was believed.
 * 0x0f is the range setting, not a count: the total it produced was 458 where the part held 9674,
 * and it would have jumped backwards the first time 0x0e wrapped, since 0x0d is what carries.
 *
 * The datasheet register map in github.com/gaupen1186/DA217_Driver settles it - STEPS_MSB 0x0d,
 * STEPS_LSB 0x0e, RES_RANGE 0x0f. A differential test cannot tell a counter from its neighbour
 * when only one byte is moving; something that knows the map has to.
 *
 * I2C_SLAVE_FORCE because the da217 driver holds the address, so the polite ioctl is refused.
 * This only ever reads, and the driver is not producing anything to disturb.
 */
#define I2C_SLAVE_FORCE 0x0706
#define DA217_ADDR      0x26
#define DA217_BUS       "/dev/i2c-2"
#define DA217_STEPS_MSB 0x0d
#define DA217_STEPS_LSB 0x0e

static int read_steps(void)
{
    unsigned char msb = DA217_STEPS_MSB, lsb = DA217_STEPS_LSB, hi = 0, lo = 0;
    int fd, ok = -1;

    fd = open(DA217_BUS, O_RDWR);
    if (fd < 0) return -1;
    if (ioctl(fd, I2C_SLAVE_FORCE, DA217_ADDR) >= 0
        && write(fd, &msb, 1) == 1 && read(fd, &hi, 1) == 1
        && write(fd, &lsb, 1) == 1 && read(fd, &lo, 1) == 1) {
        unsigned char hi2 = 0;
        /* Re-read the high byte: the two are read separately, so a carry landing between them
         * would pair an old high byte with a wrapped low one and report a count 255 too low.
         * If it moved, the low byte belongs to the new high byte and is small. */
        if (write(fd, &msb, 1) == 1 && read(fd, &hi2, 1) == 1 && hi2 != hi) hi = hi2;
        ok = (hi << 8) | lo;
    }
    close(fd);
    return ok;
}

/* The accelerometer, read the same way as the steps and for the same reason.
 *
 * Six bytes from 0x02 - X, Y, Z as LSB then MSB - each pair combined and shifted right by an
 * amount the resolution decides, then sign-extended. 0x0f carries both settings: bits 1:0 the
 * range (0=2g, 1=4g, 2=8g, 3=16g) and bits 3:2 the resolution (0=14 bit, 1=12, 2=10, 3=8).
 * This watch holds 0x01, so 4g at 14 bits: shift 2, full scale 8191, 4000/8191 mg per count.
 *
 * Read rather than assumed, because assuming a register cost a day's step counts already.
 *
 * From github.com/gaupen1186/DA217_Driver, which carries the datasheet this is not otherwise
 * published in.
 */
static int accel_shift = -1, accel_max = 8191;
static double accel_lsb_g = 0.0;

static int accel_setup(int fd)
{
    unsigned char reg = 0x0f, v = 0;
    int range_mg;

    if (write(fd, &reg, 1) != 1 || read(fd, &v, 1) != 1) return -1;
    switch ((v >> 2) & 3) {
    case 0: accel_shift = 2; accel_max = 8191; break;
    case 1: accel_shift = 4; accel_max = 2047; break;
    case 2: accel_shift = 6; accel_max =  511; break;
    default: accel_shift = 8; accel_max =  127; break;
    }
    switch (v & 3) {
    case 0: range_mg = 2000; break;
    case 1: range_mg = 4000; break;
    case 2: range_mg = 8000; break;
    default: range_mg = 16000; break;
    }
    accel_lsb_g = (double) range_mg / 1000.0 / (double) accel_max;
    return 0;
}

/* One sample in g, or -1. */
static int read_accel(int fd, double *gx, double *gy, double *gz)
{
    unsigned char reg = 0x02, b[6];
    int i;
    double *out[3];

    out[0] = gx; out[1] = gy; out[2] = gz;
    if (write(fd, &reg, 1) != 1 || read(fd, b, 6) != 6) return -1;
    for (i = 0; i < 3; i++) {
        int r = ((b[i * 2 + 1] << 8) | b[i * 2]) >> accel_shift;
        if (r > accel_max) r -= (accel_max + 1) * 2;
        *out[i] = r * accel_lsb_g;
    }
    return 0;
}

static int accel_open(void)
{
    int fd = open(DA217_BUS, O_RDWR);
    if (fd < 0) return -1;
    if (ioctl(fd, I2C_SLAVE_FORCE, DA217_ADDR) < 0) { close(fd); return -1; }
    if (accel_shift < 0 && accel_setup(fd) < 0) { close(fd); return -1; }
    return fd;
}

static void logline(const char *mode, const char *line)
{
    FILE *f = fopen(VLOG, "a");
    time_t now;
    struct tm *tmv;
    char when[32];

    if (!f) return;
    now = time(NULL);
    tmv = localtime(&now);
    if (tmv && strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", tmv) > 0) {
        fprintf(f, "%s %s %s", when, mode, line);
    } else {
        fprintf(f, "%ld %s %s", (long)now, mode, line);
    }
    if (line[0] && line[strlen(line)-1] != 0x0a) fputc(0x0a, f);
    fclose(f);
}


/* A running view of the ratio, across measurements rather than within one.
 *
 * Saturation does not move quickly. A healthy wearer at rest holds the same figure for hours, so
 * combining the last several measurements is not smoothing away a signal - there is nothing there
 * moving fast enough to smooth away. What it does remove is the part that changes between one
 * measurement and the next, which on this sensor is most of what R does.
 *
 * The median, not the mean: a pass where the band slipped produces a wild value rather than a
 * slightly wrong one, and the mean would carry it.
 *
 * Only passes that survived a quality check go in. A ratio measured on eight beats that the
 * matched filter and the bin estimate disagree about by a factor of nine is not a worse
 * measurement of saturation - it is not a measurement of saturation, and averaging more of them
 * together makes a confident wrong answer rather than an honest empty one.
 */
#define RING 9


/* What the rate was last time, kept across restarts.
 *
 * The helper uses it only to choose between candidate rates a moving wrist has made ambiguous,
 * and to refuse one that lands where a heart cannot have gone. Held in a file rather than in
 * memory because init restarts this daemon, and a hint that dies with the process is no use after
 * a reboot - which is exactly when a first measurement has nothing to go on.
 *
 * Staleness needs no handling here. The helper will not accept a candidate more than half above
 * or an eighth below the hint, so an hours-old value cannot drag a genuine reading anywhere; if
 * nothing fits, it is ignored and the measurement stands on its own.
 */
#define RATEFILE "/data/local/tmp/lastrate"

static int last_rate(void)
{
    FILE *f = fopen(RATEFILE, "r");
    int v = 0;
    if (!f) return 0;
    if (fscanf(f, "%d", &v) != 1) v = 0;
    fclose(f);
    return (v > 30 && v < 210) ? v : 0;
}

/* Their convergence rule, from the constant pool of FUN_00022928: keep four fifths of what we
 * had and take one fifth of what arrived, unless the two are ten or more apart, in which case
 * replace outright. The jump is the half that matters - a plain exponential average drags towards
 * a bad reading and back, so one wild measurement bends the next several, while jumping follows a
 * real change at once and still smooths small disagreements.
 */
static void store_rate(int fresh)
{
    int prev = last_rate();
    int keep = fresh;
    FILE *f;

    if (prev > 30 && fresh > 30) {
        int diff = fresh > prev ? fresh - prev : prev - fresh;
        if (diff < 10) keep = (int)(prev * 0.8 + fresh * 0.2 + 0.5);
    }
    f = fopen(RATEFILE, "w");
    if (!f) return;
    fprintf(f, "%d\n", keep);
    fclose(f);
}

static double ring[RING];
static int ring_n = 0, ring_at = 0;

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y);
}

static void ring_add(double r)
{
    ring[ring_at] = r;
    ring_at = (ring_at + 1) % RING;
    if (ring_n < RING) ring_n++;
}

/* Saturation, as a movement away from this sensor's own recent baseline.
 *
 * An absolute figure is not available and the reason is now understood rather than suspected. R
 * drifted from 0.32 to 0.98 over eight hours on a wrist that never moved and a watch nobody
 * touched - a threefold change in the ratio the whole method rests on. Anchoring that to a
 * saturation gives a number that is wrong by several points within a day, whichever hour is
 * chosen to anchor it.
 *
 * But the drift is slow and desaturation is not. An apnoea lasts tens of seconds; the instrument
 * takes hours to wander that far. So the slow part can be treated as the baseline and subtracted,
 * which is exactly what it deserves: a running median of the recent ratios is what the sensor
 * currently calls normal for this wrist, and the distance below it is what carries information.
 *
 * The published figure is therefore an assumption plus a measurement: 97 for a healthy adult at
 * rest, plus the textbook slope of 25 points per unit of R applied to the deviation. A fall is
 * real and worth acting on. The absolute number is not a measurement of anyone's saturation and
 * docs/vitals.md says so at greater length.
 *
 * BASE_MIN is what makes it honest. Fewer than that and there is no baseline yet, so nothing is
 * reported rather than a deviation from one measurement.
 */
#define BASE_MIN 5
#define SPO2_ASSUMED_REST 97.0
#define SPO2_SLOPE 25.0

/* The baseline, and whether there is enough of one to use. */
static int spo2_from_baseline(double r, double *out)
{
    double tmp[RING];
    int i, n = ring_n;

    *out = 0;
    if (n < BASE_MIN || r <= 0) return 0;
    for (i = 0; i < n; i++) tmp[i] = ring[i];
    qsort(tmp, n, sizeof tmp[0], cmp_dbl);
    {
        double base = tmp[n / 2];
        double v;
        if (base <= 0) return 0;
        v = SPO2_ASSUMED_REST - SPO2_SLOPE * (r - base);
        /* Above the assumed rest is the baseline moving, not the wearer improving: a healthy
         * adult at rest has nowhere up to go. Clamped rather than reported. */
        if (v > 100.0) v = 100.0;
        if (v < 70.0) return 0;      /* further than this from baseline is the sensor, not blood */
        *out = v;
        return 1;
    }
}

/* The middle of what has been seen, and how far the middle half of it spreads. */
static int ring_view(double *med, double *spread)
{
    double tmp[RING];
    int i;

    *med = 0;
    *spread = 0;
    if (ring_n < 3) return ring_n;
    for (i = 0; i < ring_n; i++) tmp[i] = ring[i];
    qsort(tmp, ring_n, sizeof tmp[0], cmp_dbl);
    *med = tmp[ring_n / 2];
    *spread = tmp[(ring_n * 3) / 4] - tmp[ring_n / 4];
    return ring_n;
}

/* Read name=<number> out of a reply line, or -1. */
static double field_of(const char *line, const char *name)
{
    const char *at = strstr(line, name);
    if (!at) return -1.0;
    return atof(at + strlen(name));
}


/* Drop the oldest waveforms once there are too many.
 *
 * Names are the timestamp they were written at, so lexical order is chronological and the sweep
 * needs no stat() on anything - it counts what is there and removes from the front.
 */
static void sweep_waves(void)
{
    DIR *d = opendir("/sdcard/waves");
    struct dirent *e;
    static char names[512][32];
    int n = 0, i;

    if (!d) return;
    while ((e = readdir(d)) && n < 512) {
        if (e->d_name[0] == '.') continue;
        snprintf(names[n], sizeof names[0], "%s", e->d_name);
        n++;
    }
    closedir(d);
    if (n <= KEEP_WAVES) return;

    /* Insertion sort: n is small and bounded, and qsort on a 2-D array of char needs a
     * comparator that knows the stride. */
    for (i = 1; i < n; i++) {
        char tmp[32];
        int j = i - 1;
        snprintf(tmp, sizeof tmp, "%s", names[i]);
        while (j >= 0 && strcmp(names[j], tmp) > 0) {
            snprintf(names[j+1], sizeof names[0], "%s", names[j]);
            j--;
        }
        snprintf(names[j+1], sizeof names[0], "%s", tmp);
    }
    for (i = 0; i < n - KEEP_WAVES; i++) {
        char path[160];
        snprintf(path, sizeof path, "/sdcard/waves/%s", names[i]);
        unlink(path);
    }
}

static int listenfd = -1;

static void bye(int s)
{
    (void)s;
    if (listenfd >= 0) close(listenfd);
    /* The vendor daemon stays off: this has replaced it. Nothing to restore. */
    _exit(0);
}

/* Run one measurement and return its single line. The vendor daemon is stopped for the duration
 * and started again straight after, including on failure. */
static char sleepline[256];

static void measure(const char *mode, char *out, size_t outsz)
{
    char cmd[256];
    char ratio_out[192];
    char wave_path[128];
    size_t ratio_sz = sizeof ratio_out;
    FILE *p;

    ratio_out[0] = 0;

    int t;

    out[0] = 0;

    /* Do not light the sensor for forty-five seconds against a bedside table.
     *
     * Off the wrist a measurement cannot succeed - it ends in no_agreement once the windows have
     * failed to cluster - but it takes the whole run to get there with the LEDs on throughout.
     * The thermometer answers the same question in about a second.
     *
     * If the sensor will not say, measure anyway: a missing thermometer is a reason to fall back
     * to the slow answer, not to refuse to answer at all. */
    /* The same check before a measurement, and for the same reason it is quick now. This used to
     * spend eight seconds on the thermometer before every reading, including the ones that were
     * going to fail because nobody was wearing the watch. */
    {
        int adt = adt_worn(3000);
        if (adt == 0) {
            snprintf(out, outsz, "hr=0 reason=not_worn adt=0\n");
            return;
        }
        /* Still read the temperature, just without waiting for it: the reading carries temp=
         * and the launcher shows it. One try rather than eight because the thermopile is left
         * enabled between measurements, so it answers at once unless this is the first read
         * since boot - and a missing temperature is worth less than eight seconds. */
        t = adt > 0 ? read_temp(1) : read_temp(8);
    }
    if (t > 0 && t < WORN_C) {
        snprintf(out, outsz, "hr=0 reason=not_worn temp=%d.%02d\n", t / 100, t % 100);
        return;
    }
    /* Do NOT stop gh3011_daemon here. This process *is* that service now - it runs in the slot
     * init used to start the vendor's - so stopping it kills this daemon mid-measurement, which
     * is exactly what happened the first time. The vendor binary is disabled by virtue of being
     * replaced; there is nothing left to stop. */

    /* Two passes, the way the vendor firmware does it: a short one for the saturation and a
     * long one for the rate and the pressure.
     *
     * They want opposite configurations, which is why one pass cannot serve both. The ratio
     * needs channel 1 carrying signal, and that means zeroing 0x0180 to lift it from two counts
     * of pulse to thirty - but the same change drops channel 2 from 190-260 counts to 34-95, and
     * channel 2 is where the pulse shape behind the pressure comes from. Six measurements in the
     * balanced state found no usable beats at all.
     *
     * So the short pass runs balanced and reports only R, and the long pass runs as before. The
     * ratio costs twenty-five seconds on top of the forty-five. The vendor spends about eight
     * here and we did too; SECS_RATIO records why ours is no longer eight.
     */
    if (strcmp(mode, "spo2") == 0) {
        char rline[256];
        rline[0] = 0;
        /* Clear it first, or a failed pass leaves the last one's samples to be read again.
         *
         * The re-read takes whatever is at KEEP, and nothing said whose it was. Two consecutive
         * measurements reported a ratio of 1.899 with a spread of 0.259 and amplitudes matching
         * to three decimals - not a steady wearer, the same pass counted twice. Any
         * apparent agreement between neighbouring measurements has to be suspected wherever this
         * could have happened. */
        unlink(KEEP);
        snprintf(cmd, sizeof cmd, "%s %s %s ratio 2>/dev/null", HELPER, SECS_RATIO, KEEP);
        p = popen(cmd, "r");
        if (p) {
            char line[512];
            while (fgets(line, sizeof line, p)) {
                if (strstr(line, "r=")) {
                    strncpy(rline, line, sizeof rline - 1);
                    rline[sizeof rline - 1] = 0;
                }
            }
            pclose(p);
        }
        /* Carried on the reply so the ratio can be watched while it is being made to behave.
         * No saturation is derived from it: across four consecutive resting passes it came back
         * 1.40, 0.84, 0.84 and 1.13, and the frequency it was measured at wandered between 41
         * and 59 bpm, which was the pass being too short to lock a rate rather than anything
         * about the wearer. Measured while it ran for eight seconds, before SECS_RATIO went to
         * twenty-five; the rate still comes from the long pass either way. See docs/vitals.md. */
        if (rline[0]) {
            size_t at = strlen(rline);
            while (at > 0 && (rline[at-1] == 0x0a || rline[at-1] == 0x0d)) rline[--at] = 0;
            snprintf(ratio_out, ratio_sz, " pass1[%s]", rline);
        }
    }

    /* Keep the waveform of every long pass, named by the clock, so a change to the pulse
     * shape can be tried against recordings instead of against the next few beats of a
     * live wrist.
     *
     * Bounded, because a waveform is seventy kilobytes and a measurement happens every few
     * minutes: left alone this fills a card in a fortnight. The oldest are dropped once there
     * are more than KEEP_WAVES, which is enough to hold a night and a morning. */
    {
        time_t nowt = time(NULL);
        struct tm *tmv = localtime(&nowt);
        char wp[128];
        mkdir("/sdcard/waves", 0777);
        sweep_waves();
        if (tmv && strftime(wp, sizeof wp, "/sdcard/waves/%Y%m%d-%H%M%S.txt", tmv) > 0) {
            snprintf(wave_path, sizeof wave_path, "%s", wp);
        } else {
            snprintf(wave_path, sizeof wave_path, "/sdcard/waves/%ld.txt", (long)nowt);
        }
    }
    /* Tell the helper what the rate was last time.
     *
     * It uses it only to choose between candidate rates that a moving wrist has made ambiguous,
     * and to refuse one that lands where a heart cannot have gone. Without it the helper has to
     * treat every measurement as the first, which is why a wrist in motion can produce a reading
     * of 45 with a tighter spread than the correct 60 beside it - the windows agreed, about the
     * arm.
     *
     * Persisted through a file rather than held in memory, because this daemon is restarted by
     * init and a rate that only survives while the process does is no use across a reboot. Stale
     * is handled at the other end: the helper will not accept a cluster more than half above or
     * an eighth below the hint, so an hours-old value cannot drag a genuine reading anywhere,
     * and if nothing fits the hint it simply is not used.
     */
    {
        int hint = last_rate();
        char pfx[32];

        if (hint > 30) snprintf(pfx, sizeof pfx, "PREV_BPM=%d ", hint);
        else           pfx[0] = 0;

        snprintf(cmd, sizeof cmd, "%s%s %s %s %s 2>/dev/null", pfx, HELPER,
                 strcmp(mode, "spo2") == 0 ? SECS_SPO2 : SECS_HR, wave_path,
                 strcmp(mode, "spo2") == 0 ? "spo2" : "hr");
    }
    sleepline[0] = 0;
    p = popen(cmd, "r");
    if (p) {
        char line[512];
        while (fgets(line, sizeof line, p)) {
            /* The helper prints progress on some paths; the reading is the line with hr= on it. */
            if (strstr(line, "hr=")) {
                strncpy(out, line, outsz - 1);
                out[outsz - 1] = 0;
            }
            /* And the sleep sample, from the accelerometer this measurement was watching anyway.
             * It rides along on the reading rather than costing a wakeup of its own. */
            if (strncmp(line, "asleep ", 7) == 0) {
                strncpy(sleepline, line + 7, sizeof sleepline - 1);
                sleepline[sizeof sleepline - 1] = 0;
            }
        }
        pclose(p);
    }

    if (!out[0]) snprintf(out, outsz, "hr=0 reason=helper_gave_nothing\n");

    /* Append the sleep sample to the reading, in place of the newline it ends with. This is how a
     * measurement comes to record thirty to eighty seconds of continuous accelerometer where the
     * sleep service was managing five seconds every five minutes. */
    if (sleepline[0]) {
        size_t at = strlen(out);
        while (at > 0 && (out[at-1] == 0x0a || out[at-1] == 0x0d)) out[--at] = 0;
        snprintf(out + at, outsz - at, " %s", sleepline);
    }

    /* Now that the rate is known, read the short pass again at it.
     *
     * The short pass could not settle a rate of its own - four consecutive runs on a resting
     * wrist put it at 41, 45, 49 and 59 bpm, measured while it ran for eight seconds - and a
     * ratio measured at the wrong frequency is a ratio measured on noise. Whether twenty-five
     * seconds would now settle one has not been tested, and does not need to be: the long pass
     * settles it properly by window agreement, so the samples kept from the short one are read
     * back at that. No extra sensor time: the same twenty-five seconds, understood once there is
     * something to understand them with.
     */
    {
        const char *at = strstr(out, "hr=");
        int bpm = at ? atoi(at + 3) : 0;

        /* Carry it to the next measurement, through their blend. Only a rate that stood as a
         * measurement is stored - a zero or a refusal leaves the previous one alone, so a run of
         * failures under motion does not erase what was known before them. */
        if (bpm >= 30 && bpm <= 210) store_rate(bpm);

        if (bpm >= 30 && bpm <= 210) {
            char rcmd[320], line[512];
            snprintf(rcmd, sizeof rcmd, "%s 0 %s redo %d 2>/dev/null", HELPER, KEEP, bpm);
            p = popen(rcmd, "r");
            if (p) {
                while (fgets(line, sizeof line, p)) {
                    if (strstr(line, "redone=1")) {
                        size_t n2 = strlen(line);
                        while (n2 > 0 && (line[n2-1] == 0x0a || line[n2-1] == 0x0d)) line[--n2] = 0;
                        snprintf(ratio_out, ratio_sz, " pass1[%s]", line);
                    }
                }
                pclose(p);
            }
        }

        /* No rate of its own, but green found one: replay this capture at green's rate.
         *
         * The pressure only comes from the red pass, and the red pass is the one that most often
         * cannot find a rate - red and infrared carry 1 to 80 counts of pulse where green carries
         * 200 to 900, so the windows disagree and the whole pass is thrown away. The shape does
         * not need the pass to have found the rate, only to be told one, and green measured a
         * good one seconds earlier.
         *
         * So replay the samples we already have at that rate. No extra sensor time, and the
         * alternative is what was happening before: a wearer with a perfectly good waveform on
         * disk and no pressure published for hours.
         *
         * The rate is reported as the hint's, not as a measurement - hrfrom=hint says so, and the
         * launcher overwrites it with green's own figure anyway. Without a rate in the line the
         * reading is discarded before anything looks at the pressure.
         */
        if (bpm < 30 && strcmp(mode, "spo2") == 0 && wave_path[0]) {
            int hint = last_rate();

            if (hint >= 30 && hint <= 210) {
                char scmd[320], line[512], shape[256];

                shape[0] = 0;
                snprintf(scmd, sizeof scmd, "%s 0 %s shape %d 2>/dev/null",
                         HELPER, wave_path, hint);
                p = popen(scmd, "r");
                if (p) {
                    while (fgets(line, sizeof line, p)) {
                        const char *sb = strstr(line, "sbp=");
                        const char *db = strstr(line, "dbp=");
                        if (sb && db && atoi(sb + 4) > 0) {
                            snprintf(shape, sizeof shape,
                                     " hr=%d hrfrom=hint sbp=%d dbp=%d shape=replayed",
                                     hint, atoi(sb + 4), atoi(db + 4));
                        }
                    }
                    pclose(p);
                }

                /* Ahead of the existing text, because the launcher reads the first hr= it finds
                 * and the line already carries hr=0. */
                if (shape[0]) {
                    char merged[512];
                    snprintf(merged, sizeof merged, "%s %s", shape + 1, out);
                    strncpy(out, merged, outsz - 1);
                    out[outsz - 1] = 0;
                }
            }
        }
    }

    /* Judge the pass, then fold it into the running view.
     *
     * Two independent estimates of the same amplitude are available - the matched filter, which
     * projects onto the beat, and the bin, which keeps only the fundamental. When they agree the
     * pass held together; when they disagree by a factor, one of them is measuring noise and
     * neither is worth keeping. That is a check no single estimator can perform on itself.
     */
    if (ratio_out[0]) {
        double rm = field_of(ratio_out, "rmatch=");
        double rb = field_of(ratio_out, " r=");
        int beats = (int) field_of(ratio_out, "mbeats=");
        double med = 0, sp = 0;
        int n3;

        /* Two gates, and between them they separated ten logged passes exactly.
         *
         * The window spread is how far the sub-windows of one pass disagreed. Above about a
         * third the pass did not hold still, and the two passes that failed it were carrying
         * two and six counts of pulse on channel 1 - not a worse ratio, no ratio at all.
         *
         * Agreement between the two estimators is the other. The matched filter and the bin
         * measure the same amplitude by different routes, so when they part company by more
         * than half again, one of them is measuring noise and there is no way to tell which
         * from inside either. That is a check neither can perform on itself.
         *
         * On the ten passes those were fitted against, the six that survived had a median of
         * 0.667 and lay between 0.583 and 0.729; the four rejected were 0.190, 1.531, 2.044 and
         * 2.739. Ten is not many, and the thresholds are round numbers rather than fitted ones
         * for that reason.
         */
        double rsp = field_of(ratio_out, "spread=");
        if (rm > 0.05 && rm < 5.0 && beats >= 8 && rb > 0 && rsp >= 0 && rsp < 0.35) {
            double hi = rm > rb ? rm : rb, lo = rm > rb ? rb : rm;
            if (lo > 0 && hi / lo < 1.6) ring_add(rm);
        }
        n3 = ring_view(&med, &sp);
        if (n3 >= 3) {
            size_t at2 = strlen(ratio_out);
            snprintf(ratio_out + at2, ratio_sz - at2, " rstable=%.3f rspread=%.3f rn=%d",
                     med, sp, n3);
        }

        /* Only a pass that cleared both gates gets to move the reading, and only once there is
         * a baseline to move it against. */
        if (rm > 0.05 && rm < 5.0 && beats >= 8 && rsp >= 0 && rsp < 0.35) {
            double sat = 0;
            if (spo2_from_baseline(rm, &sat)) {
                size_t at3 = strlen(ratio_out);
                snprintf(ratio_out + at3, ratio_sz - at3, " spo2rel=%.0f", sat);
            }
        }

        /* And the vendor's own curve, where the pulse is big enough to carry it.
         *
         * Read out of gh3011_service: FUN_00020040 evaluates coef[0x2c]*r*r + coef[0x30]*r +
         * coef[0x34] on the ratio, and FUN_0001f8c0 fills those three with 0.0, -25.0 and 110.0.
         * So their shipped curve is 110 - 25R with no quadratic term - the textbook empirical one,
         * a default rather than a per-device fit, and nothing in their daemon writes over it.
         * docs/gh3011.md has the working.
         *
         * The amplitude gate is the whole difference between this and a number. R is a ratio of
         * two small differences, and at the amplitudes this sensor gives when perfusion is poor -
         * three to nine counts - a single ADC count moves it by more than a six point
         * desaturation does. Measured within minutes on a still wrist: ac2 of 6 and 9 gave R of
         * 1.840 and 1.223, which this curve would report as 64% and 79%. Neither is true.
         *
         * ratio_usable in ppgd already draws that line at ac1 >= 7.5 and ac2 >= 30 and only
         * labels it. Here it decides, because a saturation that is wrong is worse than one that
         * is missing.
         *
         * The vendor clamps their result to a floor of 70 before reporting it, so their display
         * cannot show what a bad ratio produces. This refuses instead: a reading below 70 from a
         * pulse this size is not a desaturation, it is a ratio that has come apart.
         */
        {
            double a1 = field_of(ratio_out, "ac1=");
            double a2 = field_of(ratio_out, "ac2=");

            if (a1 >= 7.5 && a2 >= 30.0 && rm > 0.05 && rm < 5.0
                && beats >= 8 && rsp >= 0 && rsp < 0.35) {
                double abs_sat = 110.0 - 25.0 * rm;

                if (abs_sat >= 70.0 && abs_sat <= 100.0) {
                    size_t at4 = strlen(ratio_out);
                    snprintf(ratio_out + at4, ratio_sz - at4, " spo2=%.0f", abs_sat);
                }
            }
        }
    }

    /* The short pass rides along on the same line. */
    if (ratio_out[0]) {
        size_t at = strlen(out);
        while (at > 0 && (out[at-1] == 0x0a || out[at-1] == 0x0d)) out[--at] = 0;
        snprintf(out + at, outsz - at, "%s\n", ratio_out);
    }

    /* Carry the temperature on the same line. It is a wrist and not a body - a few degrees above
     * the room and well below its owner, which is how the vendor once filed 21 C as a body
     * temperature - so converting it is the launcher's business, not this daemon's. */
    if (t > 0) {
        size_t at = strlen(out);
        while (at > 0 && (out[at-1] == 0x0a || out[at-1] == 0x0d)) out[--at] = 0;
        snprintf(out + at, outsz - at, " temp=%d.%02d\n", t / 100, t % 100);
    }
}

int main(void)
{
    struct sockaddr_un addr;
    socklen_t alen;

    signal(SIGTERM, bye);
    signal(SIGINT, bye);
    signal(SIGPIPE, SIG_IGN);       /* a launcher that hangs up mid-reply must not kill this */

    listenfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = 0;                                  /* abstract namespace */
    strncpy(addr.sun_path + 1, SOCKNAME, sizeof addr.sun_path - 2);
    alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(SOCKNAME));

    if (bind(listenfd, (struct sockaddr *)&addr, alen) < 0) { perror("bind"); return 1; }
    if (listen(listenfd, 4) < 0) { perror("listen"); return 1; }

    fprintf(stderr, "vitalsd: listening on abstract socket \"%s\"\n", SOCKNAME);

    for (;;) {
        char req[64], reply[512];
        int c = accept(listenfd, NULL, NULL);
        ssize_t n;
        if (c < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        n = read(c, req, sizeof req - 1);
        if (n <= 0) { close(c); continue; }
        req[n] = 0;
        while (n > 0 && (req[n-1] == '\n' || req[n-1] == '\r')) req[--n] = 0;

        if (strcmp(req, "wear") == 0) {
            /* Answerable without lighting an LED, so the launcher can skip a measurement it
             * already knows will fail. */
            /* The sensor decides, and the thermometer is the fallback.
             *
             * This was the other way round until the detector was made to work. It is the better
             * instrument and now behaves like it: about a second against the thermometer's eight,
             * and it detects a wrist rather than warmth, which a pocket or a radiator also
             * supplies. Five runs each way separated cleanly - worn on every on-wrist run,
             * including with the arm still, and silence on every off-wrist one.
             *
             * The thermometer stays for the case where the helper is missing or the device is
             * busy, where adtwear answers -1. Both are reported either way, so a disagreement
             * shows up in the log rather than being averaged away.
             */
            int adt = adt_worn(3000);
            int wt = adt >= 0 ? -1 : read_temp(8);
            int worn = adt >= 0 ? adt : (wt > 0 ? (wt >= WORN_C ? 1 : 0) : -1);
            int at = snprintf(reply, sizeof reply, "worn=%d adt=%d", worn, adt);

            if (wt > 0)
                at += snprintf(reply + at, sizeof reply - at, " temp=%d.%02d", wt / 100, wt % 100);
            else if (worn < 0)
                at += snprintf(reply + at, sizeof reply - at, " reason=no_source");
            snprintf(reply + at, sizeof reply - at, "\n");
        } else if (strncmp(req, "accel", 5) == 0) {
            /* The accelerometer, so nothing needs the vendor's driver.
             *
             * The DA217 provides both the step counter and the accelerometer, and the driver that
             * owns it delivers steps to nobody. Serving both from here makes that driver removable
             * rather than merely bypassed - which also ends the I2C_SLAVE_FORCE, since nothing
             * else would hold the address.
             *
             * A bare "accel" gives one sample. "accel <ms>" samples for that long and returns the
             * sums the sleep recorder builds, rather than the samples themselves: it reduces every
             * burst to these nine numbers anyway, and eighty triples do not fit in a reply.
             */
            int ms = atoi(req + 5);
            int afd = accel_open();

            if (afd < 0) {
                snprintf(reply, sizeof reply, "n=0 reason=no_accelerometer\n");
            } else if (ms <= 0) {
                double x = 0, y = 0, z = 0;
                if (read_accel(afd, &x, &y, &z) == 0)
                    snprintf(reply, sizeof reply, "x=%.4f y=%.4f z=%.4f\n", x, y, z);
                else
                    snprintf(reply, sizeof reply, "n=0 reason=read_failed\n");
                close(afd);
            } else {
                double sx = 0, sy = 0, sz = 0, smag = 0, smagsq = 0, senmo = 0;
                double lo = 1e9, hi = -1e9;
                /* The magnitudes are kept, not just summed, because the caller wants a mean
                 * absolute deviation and that cannot be recovered from the sums the way a
                 * standard deviation can - it needs the mean before it can measure against it. */
                static double mags[400];
                double mean = 0, mad = 0;
                int n = 0, ticks;

                if (ms > 20000) ms = 20000;
                /* Sixteen a second, which is what SENSOR_DELAY_UI gave the recorder. */
                for (ticks = 0; ticks < ms / 62; ticks++) {
                    double x = 0, y = 0, z = 0, mag;
                    if (read_accel(afd, &x, &y, &z) < 0) { usleep(62000); continue; }
                    mag = sqrt(x * x + y * y + z * z);
                    sx += x; sy += y; sz += z;
                    smag += mag; smagsq += mag * mag;
                    senmo += mag > 1.0 ? mag - 1.0 : 0.0;
                    if (mag < lo) lo = mag;
                    if (mag > hi) hi = mag;
                    if (n < (int)(sizeof mags / sizeof mags[0])) mags[n] = mag;
                    n++;
                    usleep(62000);
                }
                close(afd);
                if (n == 0)
                    snprintf(reply, sizeof reply, "n=0 reason=read_failed\n");
                else {
                    int k, kept = n < (int)(sizeof mags / sizeof mags[0])
                            ? n : (int)(sizeof mags / sizeof mags[0]);
                    mean = smag / n;
                    for (k = 0; k < kept; k++)
                        mad += mags[k] > mean ? mags[k] - mean : mean - mags[k];
                    if (kept) mad /= kept;
                    snprintf(reply, sizeof reply,
                             "n=%d sx=%.4f sy=%.4f sz=%.4f smag=%.4f smagsq=%.4f senmo=%.4f"
                             " minmag=%.4f maxmag=%.4f mad=%.5f\n",
                             n, sx, sy, sz, smag, smagsq, senmo, lo, hi, mad);
                }
            }
            logline("accel", reply);
        } else if (strcmp(req, "steps") == 0) {
            /* The step counter, read off the bus rather than through the sensor framework.
             *
             * The launcher registers for the counter both ways the framework offers, a listener
             * and a trigger, and has never had a number out of either - its input device is
             * enabled and delivered nothing across twenty-five seconds of walking. The chip is
             * counting perfectly well underneath that: a DA217 at 2-0026, whose 0x0d/0x0e count
             * rose by 21 over a thirty second walk and holds steady when the wrist is still.
             *
             * So the driver is where it stops. Reading the part directly needs root and the
             * launcher has none, which is the same reason the measurement lives here.
             */
            int n = read_steps();
            if (n >= 0) snprintf(reply, sizeof reply, "steps=%d\n", n);
            else snprintf(reply, sizeof reply, "steps=-1 reason=no_counter\n");
            logline(req, reply);
        } else if (strcmp(req, "temp") == 0) {
            /* The thermometer on its own, and nothing lit.
             *
             * "wear" already reads it, but only when the wear detector could not run - the
             * whole point of that detector is to avoid the eight seconds this takes. So there
             * was no way to ask for a temperature without either starting a measurement or
             * hoping the detector had failed.
             *
             * The launcher wanted one for two things, and both are cheap questions that were
             * being answered expensively: the temperature to report between measurements, and a
             * second opinion when the detector claims the watch is off a wrist while the skin
             * is at 34. It was reading the thermopile through the vendor's library to get them,
             * which is the last thing that library was still being used for.
             *
             * A wrist and not a body: converting one to the other needs an ambient reading this
             * device does not have, and is the caller's business either way.
             */
            int wt = read_temp(8);
            if (wt > 0)
                snprintf(reply, sizeof reply, "temp=%d.%02d\n", wt / 100, wt % 100);
            else
                snprintf(reply, sizeof reply, "temp=0 reason=no_source\n");
            logline(req, reply);
        } else {
            measure(req, reply, sizeof reply);
            logline(req, reply);
        }
        write(c, reply, strlen(reply));
        close(c);
    }

    close(listenfd);
    return 0;
}
