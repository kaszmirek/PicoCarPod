// Diagnostics: watchdog, crash reporting across reboots, BTstack log routing.
#ifndef DIAG_H
#define DIAG_H

// Call once early in main(), after stdio_init_all(). Prints the reason for the
// previous reboot (hard fault PC/LR, panic, watchdog) and arms the watchdog.
void diag_init(void);

// Start the BTstack timer that feeds the watchdog (call after the run loop exists).
void diag_start_watchdog_feeder(void);

#endif
