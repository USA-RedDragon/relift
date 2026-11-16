/**
 * @file test_ipc.c
 * @brief The interrupt controllers, IPCSYNC and the IPC FIFOs: values,
 * flags, the error and enable bits and the edges of the FIFO IRQs.
 */
#include "ndst.h"

#define IME 0x04000208u
#define IE 0x04000210u
#define IF 0x04000214u
#define SYNC 0x04000180u
#define CNT 0x04000184u
#define SEND 0x04000188u
#define RECV 0x04100000u

TEST(irq_registers_are_per_cpu_and_if_is_acknowledged_by_writing_one)
{
    ndst *t = ndst_new();
    w9(t, IE, 4, 0xFFFFFFFFu);
    w9(t, IME, 4, 3);
    CHECK_EQ(r9(t, IME, 4), 1);
    CHECK_EQ(r9(t, IE, 4), 0xFFFFFFFFu);
    CHECK_EQ(r7(t, IE, 4), 0);
    rl_nds_request_irq(t->m, RL_NDS_ARM9, 0x00050009u);
    CHECK_EQ(r9(t, IF, 4), 0x00050009u);
    CHECK_EQ(r7(t, IF, 4), 0);
    w9(t, IF, 4, 0x00010001u);
    CHECK_EQ(r9(t, IF, 4), 0x00040008u);
    w9(t, IF + 2, 1, 0x04);
    CHECK_EQ(r9(t, IF, 4), 0x00000008u);
    w9(t, IF, 4, 0);
    CHECK_EQ(r9(t, IF, 4), 0x00000008u);
}

TEST(the_irq_input_rises_two_ticks_after_ime_ie_and_if_meet)
{
    ndst *t = ndst_new();
    rl_nds *m = t->m;
    ndst_to(t, RL_NDS_ARM7, 100);
    rl_nds_request_irq(m, RL_NDS_ARM7, RL_NDS_IRQ_TIMER0);
    CHECK_EQ(rl_nds_irq_at(m, RL_NDS_ARM7), UINT64_MAX);
    w7(t, IE, 4, RL_NDS_IRQ_TIMER0);
    CHECK_EQ(rl_nds_irq_at(m, RL_NDS_ARM7), UINT64_MAX);
    w7(t, IME, 2, 1);
    CHECK_EQ(rl_nds_irq_at(m, RL_NDS_ARM7), 102);
    CHECK(!rl_nds_irq_line(m, RL_NDS_ARM7));
    ndst_to(t, RL_NDS_ARM7, 102);
    CHECK(rl_nds_irq_line(m, RL_NDS_ARM7));
    rl_nds_request_irq(m, RL_NDS_ARM7, RL_NDS_IRQ_TIMER0);
    CHECK_EQ(rl_nds_irq_at(m, RL_NDS_ARM7), 102);
    w7(t, IF, 4, RL_NDS_IRQ_TIMER0);
    CHECK_EQ(rl_nds_irq_at(m, RL_NDS_ARM7), UINT64_MAX);
    CHECK(!rl_nds_irq_line(m, RL_NDS_ARM7));
    CHECK(!rl_nds_irq_line(m, RL_NDS_ARM9));
}

TEST(ipcsync_crosses_its_nibbles_and_sends_irqs_only_when_enabled)
{
    ndst *t = ndst_new();
    w9(t, SYNC, 2, 0x0A00);
    CHECK_EQ(r7(t, SYNC, 2), 0x000A);
    CHECK_EQ(r9(t, SYNC, 2), 0x0A00);
    w7(t, SYNC, 2, 0x4F05);
    CHECK_EQ(r9(t, SYNC, 2), 0x0A0F);
    CHECK_EQ(r7(t, SYNC, 2), 0x4F0A);
    w9(t, SYNC, 2, 0x2300);
    CHECK_EQ(r7(t, IF, 4), RL_NDS_IRQ_IPC_SYNC);
    CHECK_EQ(r9(t, SYNC, 2), 0x030F);
    w7(t, SYNC, 2, 0x2000);
    CHECK_EQ(r9(t, IF, 4), 0);
    w9(t, SYNC, 1, 0x00);
    CHECK_EQ(r7(t, SYNC, 2), 0x0003);
    w9(t, SYNC + 1, 1, 0x47);
    CHECK_EQ(r7(t, SYNC, 2), 0x0007);
    w7(t, SYNC, 4, 0x2000);
    CHECK_EQ(r9(t, IF, 4), RL_NDS_IRQ_IPC_SYNC);
}

TEST(fifo_words_arrive_in_order_with_full_and_empty_flags)
{
    ndst *t = ndst_new();
    CHECK_EQ(r9(t, CNT, 2), 0x0101);
    w9(t, CNT, 2, 0x8000);
    w7(t, CNT, 2, 0x8000);
    for (uint32_t i = 0; i < 16; i++)
        w9(t, SEND, 4, 0x100 + i);
    CHECK_EQ(r9(t, CNT, 2), 0x8102);
    CHECK_EQ(r7(t, CNT, 2), 0x8201);
    w9(t, SEND, 4, 0xDEAD);
    CHECK_EQ(r9(t, CNT, 2), 0xC102);
    w9(t, CNT, 2, 0xC000);
    CHECK_EQ(r9(t, CNT, 2), 0x8102);
    for (uint32_t i = 0; i < 16; i++)
        CHECK_EQ(r7(t, RECV, 4), 0x100 + i);
    CHECK_EQ(r7(t, CNT, 2), 0x8101);
    CHECK_EQ(r7(t, RECV, 4), 0x10F);
    CHECK_EQ(r7(t, CNT, 2), 0xC101);
    w7(t, SEND, 4, 0x77);
    CHECK_EQ(r9(t, RECV, 4), 0x77);
}

TEST(a_disabled_fifo_ignores_sends_and_peeks_on_receive)
{
    ndst *t = ndst_new();
    w9(t, SEND, 4, 1);
    CHECK_EQ(r9(t, CNT, 2), 0x0101);
    w9(t, CNT, 2, 0x8000);
    w9(t, SEND, 4, 2);
    w9(t, SEND, 4, 3);
    CHECK_EQ(r7(t, RECV, 4), 2);
    CHECK_EQ(r7(t, RECV, 4), 2);
    CHECK_EQ(r7(t, CNT, 2), 0x0001);
    w7(t, CNT, 2, 0x8000);
    CHECK_EQ(r7(t, RECV, 4), 2);
    CHECK_EQ(r7(t, RECV, 4), 3);
}

TEST(clearing_the_send_fifo_empties_it_and_zeroes_the_last_word)
{
    ndst *t = ndst_new();
    w9(t, CNT, 2, 0x8000);
    w7(t, CNT, 2, 0x8000);
    w9(t, SEND, 4, 5);
    w9(t, SEND, 4, 6);
    CHECK_EQ(r7(t, RECV, 4), 5);
    w9(t, CNT, 2, 0x8008);
    CHECK_EQ(r9(t, CNT, 2), 0x8101);
    CHECK_EQ(r7(t, RECV, 4), 0);
    CHECK_EQ(r7(t, CNT, 2) & 0x4000, 0x4000);
}

TEST(fifo_irqs_are_edge_triggered)
{
    ndst *t = ndst_new();
    w9(t, CNT, 2, 0x8000);
    w7(t, CNT, 2, 0x8400);
    CHECK_EQ(r7(t, IF, 4), 0);
    w9(t, SEND, 4, 1);
    CHECK_EQ(r7(t, IF, 4), RL_NDS_IRQ_IPC_RECV);
    w7(t, IF, 4, RL_NDS_IRQ_IPC_RECV);
    w9(t, SEND, 4, 2);
    CHECK_EQ(r7(t, IF, 4), 0);
    w7(t, CNT, 2, 0x8000);
    w7(t, CNT, 2, 0x8400);
    CHECK_EQ(r7(t, IF, 4), RL_NDS_IRQ_IPC_RECV);
    w7(t, IF, 4, RL_NDS_IRQ_IPC_RECV);

    w9(t, CNT, 2, 0x8004);
    CHECK_EQ(r9(t, IF, 4), 0);
    r7(t, RECV, 4);
    CHECK_EQ(r9(t, IF, 4), 0);
    r7(t, RECV, 4);
    CHECK_EQ(r9(t, IF, 4), RL_NDS_IRQ_IPC_SEND_EMPTY);
    CHECK_EQ(r7(t, IF, 4), 0);
    w9(t, SEND, 4, 3);
    CHECK_EQ(r7(t, IF, 4), RL_NDS_IRQ_IPC_RECV);

    w9(t, IF, 4, RL_NDS_IRQ_IPC_SEND_EMPTY);
    w9(t, CNT, 2, 0x8000);
    r7(t, RECV, 4);
    CHECK_EQ(r9(t, IF, 4), 0);
    w9(t, CNT, 2, 0x8004);
    CHECK_EQ(r9(t, IF, 4), RL_NDS_IRQ_IPC_SEND_EMPTY);
}

TEST(ipc_irqs_carry_the_tick_of_the_sender)
{
    ndst *t = ndst_new();
    rl_nds *m = t->m;
    w7(t, IE, 4, RL_NDS_IRQ_IPC_SYNC);
    w7(t, IME, 4, 1);
    w7(t, SYNC, 2, 0x4000);
    ndst_to(t, RL_NDS_ARM9, 501);
    w9(t, SYNC, 2, 0x2000);
    CHECK_EQ(rl_nds_irq_at(m, RL_NDS_ARM7), 503);
}
