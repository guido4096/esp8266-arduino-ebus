# INTERNAL eBUS timing

The default PlatformIO environment is `esp32-c3-internal`. It runs the eBUS
controller, polling, commands, MQTT, and Home Assistant integration on the ESP.
The `esp32-c3` bridge environment is a separate legacy implementation intended
for an external controller such as ebusd. This timing replacement targets the
INTERNAL firmware; it does not replace the legacy bridge implementation.

## Implementation

The eBUS implementation lives in the sibling fork at `../ebus`
([guido4096/ebus](https://github.com/guido4096/ebus)). PlatformIO uses
`symlink://../ebus`, so edits to the fork are compiled directly without a
copy or build-time patch. Host tests use the same directory; override it with
`cmake -S test_host -B build/host -DEBUS_SOURCE_DIR=/path/to/ebus` if needed.

The timing policy is owned by the fork in `src/ebus/core/syn_timing.hpp`.
Arbitration timing and UART handling are in `src/ebus/platform/esp/bus_esp.cpp`;
protocol arbitration is in `src/ebus/core/request.cpp`. Edit those source files
in the fork. The application retains its post-link IRAM checker and board
configuration, but no longer rewrites dependency source files during builds.

Clone both repositories side by side on a new machine. CI checks out the fork's
`improve-timing` branch in the sibling directory, so publish the fork changes before
expecting CI to build the same implementation. Local builds include uncommitted
fork edits; use a committed fork revision when preparing a release.

The receive UART uses 2400 baud, 8N1, a one-byte RX interrupt threshold, an
IRAM interrupt, and a priority-24 bus task. Hardware handles bit serialization.
The bus task and ISRs never wait for a FreeRTOS tick to time arbitration.

A continuously running 1 MHz GPTimer timestamps falling RX edges. UART delivery
of SYN validates the four latest captured edges against the actual LSB-first
0xAA waveform: falling edges at 0, 3, 5, and 7 bit times. Each interval must match within 100 us. A SYN without that pattern, a stale SYN,
a batched UART event, or queued RX data cannot arm arbitration. There is no
nearest-edge or byte-delivery-time fallback.

Arbitration programs an absolute alarm in that same GPTimer clock domain.
`window_us` selects the target elapsed time after the captured SYN start.
The application default is **4400 us**. The obsolete bus offset setting is
ignored. The legal protocol window is 4300–4456 us; configured targets must
be inside **4332–4424 us**. ISR writes may occur from the target through
4456 us.

Immediately before FIFO access, the ISR checks the current timer count, the
captured edge generation, the live request generation, cancellation, UART TX
idle, unread hardware RX, and queued UART events. An intervening falling edge yields the slot,
including to a contender that starts earlier. A delayed callback writes
nothing. Failure is recovered on the bus task; no application callbacks,
formatting, or logging run inside the timer interrupt. A request that is
cancelled and replaced cannot inherit an old timer, even at the same address.

Normal master transmission is paced by verified echoes. At most one successor
byte can enter an empty TX FIFO after the preceding echo; hardware completes
the preceding stop bit before sending it. There is no asynchronous TX ring or
preloaded telegram tail. Writes behind RX backlog are refused, and a framing,
parity, break, or overflow error clears pending arbitration and resynchronizes
on a captured SYN. An event larger than the receive scratch buffer is rejected
before reading it. External clients in INTERNAL mode also wait for captured
SYN arbitration; their former idle-time immediate-transmit shortcut is disabled.

SYN generation still uses the second hardware timer and carrier sensing. Its
notifications run on the bus task rather than from ISR. Shutdown quiesces the
worker and GPIO interrupt before deleting hardware resources.

## Build and checks

```sh
pio run                         # defaults to INTERNAL
pio run -e esp32-c3-internal
cmake -S test_host -B build/host
cmake --build build/host -j 4
ctest --test-dir build/host --output-on-failure
python3 scripts/check_ebus_iram.py .pio/build/esp32-c3-internal/firmware.elf
```

The firmware build automatically checks that the three project timing ISR
bodies are in IRAM and that their direct calls do not enter flash. This catches
compiler-generated helpers as well as ordinary function calls; it does not
prove the full transitive driver call graph. The ISR-visible flags use aligned
word load/store builtins because this toolchain can emit flash-resident
`std::atomic<bool>::load` helpers even for a lock-free-looking source operation.

The host tests cover the real SYN waveform, invalid edges, absolute deadlines,
late and early callbacks, occupied UART, intervening edges, configuration
bounds, and 32-bit microsecond wrap. They verify the timing policy, not physical
GPIO interrupt latency or adapter propagation delay.

The firmware enables `CONFIG_GPTIMER_ISR_CACHE_SAFE`,
`CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM`, and `CONFIG_UART_ISR_IN_IRAM`. For existing
PlatformIO build directories, verify the generated `sdkconfig` has these enabled:
ESP-IDF defaults do not necessarily override saved configuration. See
[Espressif's GPTimer cache-safety documentation](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c3/api-reference/peripherals/gptimer.html#cache-safety).

Application logging uses USB Serial/JTAG. UART0's default pins overlap GPIO20/21
used by eBUS, so `app_main` no longer initializes the separate debug UART.
Connect the native USB interface for firmware logs. This does not change ROM
boot output or board wiring. RESET_PIN=20 remains a boot-time input check before
UART initialization; do not reconfigure that pin during bus operation.

## Required board validation

The 32 us margins are engineering allowances, **not measured calibration**. The combined capture and launch uncertainty must
fit within those margins.
GPIO timestamps are recorded when the ISR executes, rather than by a hardware
edge-capture latch. A common delay affecting the timestamps can escape waveform
validation. Therefore a successful build or unit test cannot certify the bus
start window on a particular adapter.

Use a logic analyzer to capture comparator RX, ESP TX, and the conditioned bus
signal. Measure from the actual SYN start edge to the actual address start on
the bus. Exercise both arbitration rounds, competing masters, Wi-Fi reconnects,
MQTT/HTTP load, NVS writes, and OTA/flash operations. Confirm every transmitted
address starts within 4300–4456 us; confirm delayed attempts produce no byte.
Check stop bits, inter-byte gaps, echo mismatches, and that collisions emit no
remaining telegram bytes. Inspect existing request window/error/backlog metrics
alongside the physical capture; software FIFO-write timestamps omit UART and
adapter propagation delay.

If measured capture and launch uncertainty exceeds the reserved margins, do
not merely widen the allowed window or tune outliers with an average offset.
Reduce interrupt blocking or use a dedicated bus controller with hardware edge
capture. The design deliberately trades missed opportunities for avoiding a
known late transmission; sustained contention can reduce throughput.

## Eigen integratiebranch

Gebruik `own/all-changes` in de firmware-repo en `improve-timing` in de
eBUS-repo. Deze combinatie bevat de losse upstream-bugfixes, de optionele
Weishaupt-uitbreiding en de lokale timing-integratie.
`config/weishaupt-wtc.json` stelt `"write_type": "weishaupt"` expliciet in voor de
zes normale/verlaagde temperatuurinstellingen. De JSON wordt niet automatisch
geïnstalleerd; upload hem na het flashen van deze firmware.
