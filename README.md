# navocap-kernel-modules
01234567890123456789012346578901234567890123456789012346578901234567890123456789
By using the Linux kernel in Navocap's products we agree to comply with the
GPLv2 and give the entitlement to anyone to check what run besides it. This
repository also serves as a memo.

There are the kernel modules included in our distribution. They are not intented
 to be merged in any way, and certainly do not comply with upstream 
requirements. But they respond to real "industry" need. Obviously I would be
honored to receive advises on how I should have written them.

1- odo.c
An odometer is plugged into our core through a GPIO pin muxed as a General
Purpose Timer input pin. The aim of this module is just to make the interface
between this GPT and an easy-to-use virtual file at the root of /sys (I know
this is ugly but still very pleasant).

2- picodo.c
Same use as the previous one, but dealing with a PIC controller wired through
I2C and making itself the job of counting.

3- imx27_internals.c
Just a read in the memory at specific areas (for Freescale i.MX27) to get both
chip ID and possible MAC address to use.

4- thelma7_hw_wd.c
An hardware watchdog runs besides the main core, it receives a 1Hz clock that
may be inhibited (pules may be monitored but not counted with interruptions
because the GPIO used is on an I2C GPIO expander). The module offers, through
a GPIO, the possibility to manually trigger the reset of the watchdog.
