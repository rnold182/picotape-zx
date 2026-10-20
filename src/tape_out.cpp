#include "tape_out.h"

#include <hardware/clocks.h>
#include <hardware/pio.h>
#include <hardware/pio_instructions.h>

namespace tape_out {
namespace {

PIO pio = pio1;  // pio0 is left for the NeoPixel library
uint sm;
uint offset;
uint outPin;

// Per word: pull + out + out + (x+1) jmp iterations = x + 4 cycles.
constexpr uint32_t OVERHEAD = 4;

uint16_t program[] = {
    0, 0, 0, 0,  // filled in begin(); built at runtime so no pioasm step is needed
};

}  // namespace

void begin(uint pin) {
    outPin = pin;
    program[0] = pio_encode_pull(false, true);   // pull block
    program[1] = pio_encode_out(pio_pins, 1);    // out pins, 1   (level)
    program[2] = pio_encode_out(pio_x, 31);      // out x, 31     (duration)
    program[3] = pio_encode_jmp_x_dec(3);        // loop: jmp x-- loop

    pio_program_t prog = {};
    prog.instructions = program;
    prog.length = 4;
    prog.origin = -1;

    sm = pio_claim_unused_sm(pio, true);
    offset = pio_add_program(pio, &prog);

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset, offset + 3);
    sm_config_set_out_pins(&c, pin, 1);
    sm_config_set_out_shift(&c, true, false, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    // One PIO cycle per T-state.
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / CLOCK_HZ);

    pio_gpio_init(pio, pin);
    gpio_disable_pulls(pin);
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, true);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_pins_with_mask(pio, sm, 0, 1u << pin);
    pio_sm_set_enabled(pio, sm, true);
}

bool canPush() { return !pio_sm_is_tx_fifo_full(pio, sm); }

void push(bool level, uint32_t tstates) {
    uint32_t x = tstates > OVERHEAD ? tstates - OVERHEAD : 1;
    pio_sm_put(pio, sm, (x << 1) | (level ? 1u : 0u));
}

bool fifoEmpty() { return pio_sm_is_tx_fifo_empty(pio, sm); }

void abort() {
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_jmp(offset));
    pio_sm_set_pins_with_mask(pio, sm, 0, 1u << outPin);
    pio_sm_set_enabled(pio, sm, true);
}

void release() { gpio_set_oeover(outPin, GPIO_OVERRIDE_LOW); }

void drive() { gpio_set_oeover(outPin, GPIO_OVERRIDE_NORMAL); }

}  // namespace tape_out
