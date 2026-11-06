#include "dare.h"
extern "C" char *strncpy(char *, const char *, unsigned int);
extern void SND_fn_vDisplayErrorEx(int, char *, int);
void SND_fn_vStrncpy(char *dest, char *source, unsigned int dest_size)
{
    dest[dest_size - 1] = '\0';
    strncpy(dest, source, dest_size);
    if (dest[dest_size - 1] != '\0') {
        SND_fn_vDisplayErrorEx(8, "string truncated", 3);
        dest[dest_size - 1] = '\0';
    }
}
