# The List of changes to fit zephyr.

Need to take care to not break these changes when updating pico-sdk.

## Patch List:
  - [#3] pico-sdk: Rename adc_read() to pico_adc_read()
    - src/rp2_common/hardware_adc/include/hardware/adc.h
  - [#2] pico-sdk: Patch occurrences of KHZ/MHZ to PICO_KHZ/PICO_MHZ
    - src/rp2_common/hardware_clocks/clocks.c
    - src/rp2_common/hardware_clocks/include/hardware/clocks.h
    - src/rp2_common/hardware_pll/pll.c
    - src/rp2_common/hardware_xosc/xosc.c
