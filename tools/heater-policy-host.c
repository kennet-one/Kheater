#include "../main/heater_policy.h"
#include <stdio.h>

int main(void)
{
    if (!heater_policy_self_test()) {
        fputs("Heater relay policy selftest failed\n", stderr);
        return 1;
    }
    puts("Heater relay policy selftest passed (no hardware)");
    return 0;
}
