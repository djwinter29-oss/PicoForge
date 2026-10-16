#include "integration/levelx/levelx_nor.h"
#include "pico/stdlib.h"

/* Link the complete port without opening, provisioning or touching any flash. */
int main(void)
{
    static pf_levelx_nor_t volume;
    if (pf_levelx_nor_open(&volume, NULL, 0u, 0u) == LX_SUCCESS) {
        panic("LevelX accepted an invalid device");
    }
    for (;;) {
        tight_loop_contents();
    }
}
