// Hand fixes on top of the generated dare.h (included last).

// COP0 Count (retail: mfc0 rX,Count, in the timer functions' inlined SND_ClockNative). mwccps2 rejects
// "mfc0 %0, 9" and "Count"; this form compiles (run 10).
inline unsigned int SND_fn_ulGetCount(void) {
    unsigned int c;
    __asm__ ("mfc0 %0, $9" : "=r" (c));
    return c;
}
