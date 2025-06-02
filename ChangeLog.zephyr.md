# The List of changes to fit zephyr.

Need to take care to not break these changes when updating pico-sdk.

## Patch List:
  - [#10] pico-sdk: Switch to picolibc
    - src/rp2040/boot_stage2/CMakeLists.txt
    - src/rp2040/boot_stage2/boot_stage2.ld
    - src/rp2350/boot_stage2/CMakeLists.txt
    - src/rp2350/boot_stage2/boot_stage2.ld
    - src/rp2_common/pico_runtime/CMakeLists.txt
    - src/rp2_common/pico_standard_link/CMakeLists.txt
  - [#9] pico-sdk: Disabling sanity check the IRQ status
    - src/rp2_common/hardware_gpio/gpio.c
  - [#8] pico-sdk: pico_platform_compiler: Do not redefine `__weak`
    - src/rp2_common/pico_platform_compiler/include/pico/platform/compiler.h
  - [#7] pico-sdk: hardware_timer: Don't add irq handler to interrupt vector
    - src/rp2_common/hardware_timer/timer.c
  - [#6] pico-sdk: hardware_timer: Add argument to irq handler to handle userdata
    - src/host/hardware_timer/include/hardware/timer.h
    - src/rp2_common/hardware_timer/include/hardware/timer.h
    - src/rp2_common/hardware_timer/timer.c
    - test/pico_time_test/pico_time_test.c
  - [#5] pico-sdk: Rename is_irq_enabled() to pico_is_irq_enabled()
    - src/host/hardware_irq/include/hardware/irq.h
    - src/rp2_common/hardware_irq/include/hardware/irq.h
    - src/rp2_common/hardware_irq/irq.c
    - src/rp2_common/pico_multicore/multicore.c
  - [#3] pico-sdk: Rename adc_read() to pico_adc_read()
    - src/rp2_common/hardware_adc/include/hardware/adc.h
  - [#2] pico-sdk: Patch occurrences of KHZ/MHZ to PICO_KHZ/PICO_MHZ
    - src/rp2_common/hardware_clocks/clocks.c
    - src/rp2_common/hardware_clocks/include/hardware/clocks.h
    - src/rp2_common/hardware_pll/include/hardware/pll.h
    - src/rp2_common/hardware_xosc/xosc.c
    - src/rp2_common/pico_runtime_init/runtime_init_clocks.c
