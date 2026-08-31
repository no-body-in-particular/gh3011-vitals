/* The vendor's auto-detect configuration, read out of gh3011_service at 0x35944.
 *
 * FUN_00011de8 picks one of these by mode and applies it through FUN_00013178 before the chip will
 * say whether it is on a wrist. It is a complete configuration - the 0x0100 block, the slots, the
 * rate, the gain - ending in the detector thresholds at 0x00c2 to 0x00de.
 *
 * Forty-seven registers, and the length matters. A byte near the table reads 75 and looked like
 * its count; it is not. Entry 47 is 0x2a00, which is not a register address, and writing that far
 * put 28 invented registers into the chip - after which the LED still pulsed, so the setup looked
 * like it had worked, and every subsequent register read came back zero. The table ends where the
 * addresses stop being addresses.
 *
 * Generated from the binary rather than typed, so it cannot drift from what was read.
 */
static const struct { unsigned short reg, val; } adt_hb[] = {
    { 0x0100, 0xf530 },
    { 0x0102, 0x09c4 },
    { 0x0104, 0xba98 },
    { 0x0106, 0x2710 },
    { 0x0108, 0xf530 },
    { 0x010a, 0x2710 },
    { 0x010c, 0xf148 },
    { 0x010e, 0x0fa0 },
    { 0x0110, 0xb6b0 },
    { 0x0112, 0x30d4 },
    { 0x0114, 0xf148 },
    { 0x0116, 0x30d4 },
    { 0x011c, 0x01ff },
    { 0x011e, 0x01ff },
    { 0x0120, 0x01ff },
    { 0x0126, 0x0202 },
    { 0x0128, 0x0002 },
    { 0x0130, 0x0746 },
    { 0x0132, 0x0346 },
    { 0x0134, 0x0246 },
    { 0x0016, 0x051e },
    { 0x0080, 0x0405 },
    { 0x0082, 0x01c4 },
    { 0x0084, 0x0123 },
    { 0x0118, 0x1f69 },
    { 0x011a, 0x0000 },
    { 0x012e, 0x0000 },
    { 0x0136, 0x00a0 },
    { 0x0186, 0x1807 },
    { 0x0180, 0x006d },
    { 0x012a, 0x0606 },
    { 0x012c, 0x0006 },
    { 0x10c0, 0x0001 },
    { 0x00c2, 0xffff },
    { 0x00c4, 0x034b },
    { 0x00c6, 0xffff },
    { 0x00c8, 0x034b },
    { 0x00ca, 0x00a0 },
    { 0x00cc, 0x006e },
    { 0x00ce, 0x0251 },
    { 0x00d0, 0x0000 },
    { 0x00d4, 0x0251 },
    { 0x00d6, 0x0000 },
    { 0x00d8, 0x0505 },
    { 0x00da, 0x0000 },
    { 0x00dc, 0x0101 },
    { 0x00de, 0x0000 },
};
