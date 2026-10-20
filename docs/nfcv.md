# NFC-V on the Ultra

The trick is to treat the nRF52840's NFC peripheral as a detector and use the
Ultra's antenna switch as a transmitter. The chip normally supports NFC-A tag
operation, not NFC-V. Both use a 13.56 MHz reader carrier, but their signal
coding and frames differ, so the native controller cannot handle NFC-V directly.

## Receive in software

The firmware watches the NFCT peripheral's documented field/lock indicators.
Changes in `LOCKDETECT` provide timing observations of the reader's field
reductions. Software measures their positions with the CPU cycle counter and
decodes NFC-V pulse-position modulation: 1-of-4 or 1-of-256. It checks the
frame CRC before handing the command to the ISO15693/SLIX engine.

This repurposes a detector; it does not make NFCT a native NFC-V receiver.
The command engine supplies the response bytes and operates on the active tag's
RAM state. Reader writes never automatically modify the stored original.

## Reply with the board's switch

The external reader supplies the RF carrier. The Ultra replies by repeatedly
toggling its physical `HF_ANT_SEL` switch (P1.10), changing the antenna load seen
by the reader. That is load modulation: the reader detects the small changes
caused by the tag, while the Ultra generates no reader field of its own.

Switching at 423.75 kHz creates a single subcarrier. Software turns that
switching on and off to encode the response, including framing and CRC, at the
requested high or low rate. NFCT stays active during transmission to avoid
recalibration before the next command; the reply uses the physical switch
rather than hidden NFCT modulation registers.

Timing is the difficult part. Critical code runs from RAM with interrupts masked
during the RF window. At 64 MHz, software alternates 75- and 76-cycle switch
intervals to approximate the subcarrier without accumulating rounding drift,
with paired +8/−8-cycle offsets to lengthen the low half of each period.
Response segments are derived directly from the frame, avoiding an expanded
waveform buffer, and adjacent carrier segments share one edge loop.
After transmission it restores the switch for the next command. This
relies on the Ultra's antenna circuit; another nRF52840 board would need its
own suitable physical load-modulation path.

See [RF maintenance rules](development.md#rf-timing) for timing constraints and
[the GPIO PHY](../src/platform/ultra/nfcv_gpio.c) for the implementation.

[Project README](../README.md)
