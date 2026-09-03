/* Send the vendor daemon a command, the way the sensor HAL does.
 *
 * Their daemon blocks in an ioctl on /dev/gh_tools waiting for work, and the command that arrives
 * decides which measurement runs: 4 and 5 are heart rate, 7 is saturation. The Android HAL only
 * ever sends 4, 5 and 6, so nothing on this watch has ever asked the daemon for saturation - which
 * is why triggering it through the app did nothing. The daemon accepts 7 all the same.
 *
 * That matters because their saturation curve is now known exactly - 110 - 25R, both constants read
 * out of the binary - so their R has to be near 0.48 to report the 98% a finger meter agrees with,
 * and ours reads 0.6 to 0.8 on the same wrist. This is how to get their R rather than infer it:
 * ask their daemon for a measurement and read the twelve values it logs.
 *
 *     ghcmd 7      start saturation
 *     ghcmd 6      stop
 *
 * The HAL's struct is 24 bytes with the command in the first word and the rest zero, sent with
 * _IOW('G', 9, 24) on a read-only fd; all of that is copied here rather than guessed.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>

#define GH_SET_MODE 0x40184709u

int main(int argc, char **argv)
{
    unsigned int pkt[6];
    int fd, r;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <cmd> [param]   (4,5 = heart rate, 6 = stop, 7 = spo2)\n", argv[0]);
        return 2;
    }

    memset(pkt, 0, sizeof pkt);
    pkt[0] = (unsigned int)strtoul(argv[1], NULL, 0);
    if (argc > 2) pkt[1] = (unsigned int)strtoul(argv[2], NULL, 0);

    fd = open("/dev/gh_tools", O_RDONLY);
    if (fd < 0) { fprintf(stderr, "open /dev/gh_tools: %s\n", strerror(errno)); return 1; }

    r = ioctl(fd, GH_SET_MODE, pkt);
    printf("cmd=%u param=%u -> ioctl %d%s\n", pkt[0], pkt[1], r,
           r < 0 ? strerror(errno) : "");
    close(fd);
    return r < 0;
}
