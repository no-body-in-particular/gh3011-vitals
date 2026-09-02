/* Power the sensor down and up again, and nothing else.
 *
 * The ratio drifts over an evening - 0.71 on a freshly rebooted watch, 1.9 after a few hours of
 * measurements - and a reboot clears it. A reboot is a heavy way to fix a sensor, and if the state
 * that drifts lives in the part rather than in the driver then cutting its power should do as
 * well. The daemon does exactly this before every configuration: ioctl(_IOW('G',2,4), 0) then 1.
 *
 * Only that. No registers, no start, so anything measured afterwards is measured by whatever runs
 * next rather than by this.
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define DEV "/dev/gh_tools"
#define PWR  0x40044702u
#define INIT 0x40044706u   /* the driver's own init, never called here during a measurement */

int main(int argc, char **argv)
{
    int fd = open(DEV, O_RDWR), off = 0, on = 1;

    (void) argv;

    if (fd < 0) { printf("cannot open %s\n", DEV); return 1; }
    printf("power off rc=%d\n", ioctl(fd, PWR, &off));
    usleep(400000);
    printf("power on  rc=%d\n", ioctl(fd, PWR, &on));
    usleep(200000);

    /* And the driver init, if asked.
     *
     * Cutting the chip's power does not clear the drift - R read 1.178 before a cycle and 1.133
     * after, then 2.764 - while a reboot takes it back to 0.71. So whatever accumulates is in
     * the driver rather than the part, and this is the only call that plausibly resets it.
     */
    if (argc > 1) {
        int one = 1;
        printf("driver init rc=%d\n", ioctl(fd, INIT, &one));
        usleep(200000);
    }
    close(fd);
    return 0;
}
