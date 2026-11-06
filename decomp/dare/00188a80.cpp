#include "dare.h"

extern int db2vol_value_dB_start_interval[128];

int find_index_start_interval_db2vol(int indB) {
    int i_mini = 0;
    int i_maxi = 0x7f;
    int i_cur;

    while (i_maxi - i_mini >= 2) {
        i_cur = i_mini + (i_maxi - i_mini) / 2;
        if (db2vol_value_dB_start_interval[i_cur] < indB) {
            i_maxi = i_cur;
        } else {
            i_mini = i_cur;
        }
    }
    return i_mini;
}
