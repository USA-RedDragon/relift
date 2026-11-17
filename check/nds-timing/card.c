/* hwtest: cartridge transfer cases (ARM9, which owns the slot-1 bus). Each times one ROMCTRL
   transfer of the chip ID command (B8h), from the start bit to the busy bit clearing with every
   data word read as it is ready, and two such transfers back to back; the difference is one
   transfer. The DS drives the latencies and the clock, so the times do not depend on the card's
   answer (or on there being a card). Run last: a flashcart's own driver may need its settings
   again afterwards. */

#define CARD_CNT (*(vu16 *)0x040001A0)
#define CARD_ROMCTRL (*(vu32 *)0x040001A4)
#define CARD_CMD ((vu8 *)0x040001A8)
#define CARD_DATA (*(vu32 *)0x04100010)

typedef struct {
    const char *name;
    u32 ctl; /* ROMCTRL without the start bit */
} card_case;

/* latency 1 (bits 0-12), latency 2 (16-21), block size (24-26), clock (27: 4.2 MHz), and the
   ROMCTRL a cartridge's header gives for normal commands without its KEY2 bits (latency 1 657h,
   latency 2 01h, 6.7 MHz) */
#define GAME_GAPS 0x00010657u
static const card_case card_cases[] = {
    {"card_cmd", 0},
    {"card_4", 7u << 24},
    {"card_512", 1u << 24},
    {"card_4k", 4u << 24},
    {"card_cmd_slow", 1u << 27},
    {"card_4_slow", 7u << 24 | 1u << 27},
    {"card_512_slow", 1u << 24 | 1u << 27},
    {"card_cmd_game", GAME_GAPS},
    {"card_4_game", GAME_GAPS | 7u << 24},
    {"card_512_game", GAME_GAPS | 1u << 24},
    {"card_4k_game", GAME_GAPS | 4u << 24},
    {"card_512_game_slow", GAME_GAPS | 1u << 24 | 1u << 27},
    /* hwtest 3c: gap2 and gap1 at the BIOS's 18h, and with bit 28 (clock pulses during the gaps) */
    {"card_512_gap2_18", 0x18u << 16 | 1u << 24},
    {"card_4k_gap2_18", 0x18u << 16 | 4u << 24},
    {"card_512_gap1_18", 0x18u | 1u << 24},
    {"card_512_gaps_clk", 0x18u << 16 | 0x18u | 1u << 24 | 1u << 28},
};
#define NCARD (sizeof card_cases / sizeof card_cases[0])

#ifdef HWTEST_BARE
/* bare.S: card_one and the timed stretch of card_measure as the SDK build compiled them */
u32 card_time(u32 ctl, u32 twice);
#else
static void card_one(u32 ctl)
{
    CARD_CMD[0] = 0xB8;
    for (int i = 1; i < 8; i++)
        CARD_CMD[i] = 0;
    CARD_ROMCTRL = ctl | 1u << 31 | 1u << 29;
    while (CARD_ROMCTRL & 1u << 31)
        if (CARD_ROMCTRL & 1u << 23)
            (void)CARD_DATA;
}
#endif

static void card_measure(unsigned i, u32 *t1, u32 *t2)
{
    u32 ctl = card_cases[i].ctl;
    u32 best[2] = {~0u, ~0u};
    CARD_CNT = 0x8000;
#ifdef HWTEST_BARE
    card_time(ctl, 0);
#else
    card_one(ctl);
#endif
    for (int r = 0; r < HW_REPS; r++)
        for (int k = 0; k < 2; k++) {
#ifdef HWTEST_BARE
            u32 t = card_time(ctl, (u32)k);
#else
            u32 t0 = hw_now();
            card_one(ctl);
            if (k)
                card_one(ctl);
            u32 t = hw_now() - t0;
#endif
            if (t < best[k])
                best[k] = t;
        }
    *t1 = best[0];
    *t2 = best[1];
}
