# The List of changes to fit zephyr.

Need to take care to not break these changes when updating pico-sdk.

## Patch List:
  - [#5] pico-sdk: Rename is_irq_enabled() to pico_is_irq_enabled()
    - src/rp2_common/hardware_irq/include/hardware/irq.h
    - src/rp2_common/hardware_irq/irq.c
    - src/rp2_common/pico_multicore/multicore.c
  - [#4] pico-sdk: Additional modifications for MHZ/PICO_MHZ changes
    - src/rp2_common/hardware_pll/pll.c
  - [#3] pico-sdk: Rename adc_read() to pico_adc_read()
    - src/rp2_common/hardware_adc/include/hardware/adc.h
  - [#2] pico-sdk: Patch occurrences of KHZ/MHZ to PICO_KHZ/PICO_MHZ
    - src/rp2_common/hardware_clocks/clocks.c
    - src/rp2_common/hardware_clocks/include/hardware/clocks.h
    - src/rp2_common/hardware_pll/pll.c
    - src/rp2_common/hardware_xosc/xosc.c
