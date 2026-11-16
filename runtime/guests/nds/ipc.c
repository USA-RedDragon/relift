/**
 * @file ipc.c
 * @brief Inter-process communication between the CPUs: IPCSYNC with its
 * remote IRQ, and the two 16-word FIFOs behind IPCFIFOCNT, IPCFIFOSEND and
 * IPCFIFORECV, with the error bit, the enable bit and the edge-triggered
 * send-empty and receive-not-empty IRQs.
 *
 * Fresh C from GBATEK ("DS Inter Process Communication (IPC)").
 */
#include "nds_int.h"

#include <string.h>

enum {
    CNT_SEND_IRQ = 1u << 2,
    CNT_SEND_CLEAR = 1u << 3,
    CNT_RECV_IRQ = 1u << 10,
    CNT_ERROR = 1u << 14,
    CNT_ENABLE = 1u << 15,
};

void nds_ipc_reset(rl_nds *m) { memset(&m->ipc, 0, sizeof m->ipc); }

static void edges(rl_nds *m, uint64_t now)
{
    nds_ipc *p = &m->ipc;
    for (unsigned c = 0; c < 2; c++) {
        bool send = (p->cnt[c] & CNT_SEND_IRQ) && p->fifo[c].n == 0;
        bool recv = (p->cnt[c] & CNT_RECV_IRQ) && p->fifo[c ^ 1].n != 0;
        if (send && !p->send_cond[c])
            nds_irq_set(m, c, RL_NDS_IRQ_IPC_SEND_EMPTY, now);
        if (recv && !p->recv_cond[c])
            nds_irq_set(m, c, RL_NDS_IRQ_IPC_RECV, now);
        p->send_cond[c] = send;
        p->recv_cond[c] = recv;
    }
}

static uint32_t cnt_read(const rl_nds *m, unsigned me)
{
    const nds_ipc *p = &m->ipc;
    const nds_fifo *tx = &p->fifo[me], *rx = &p->fifo[me ^ 1];
    return (p->cnt[me] & (CNT_SEND_IRQ | CNT_RECV_IRQ | CNT_ERROR | CNT_ENABLE)) | (tx->n == 0) |
           (uint32_t)(tx->n == NDS_FIFO_WORDS) << 1 | (uint32_t)(rx->n == 0) << 8 | (uint32_t)(rx->n == NDS_FIFO_WORDS) << 9;
}

static void send(rl_nds *m, unsigned me, uint32_t v, uint64_t now)
{
    nds_ipc *p = &m->ipc;
    nds_fifo *f = &p->fifo[me];
    if (!(p->cnt[me] & CNT_ENABLE))
        return;
    if (f->n == NDS_FIFO_WORDS) {
        p->cnt[me] |= CNT_ERROR;
        return;
    }
    f->q[(f->rd + f->n) % NDS_FIFO_WORDS] = v;
    f->n++;
    edges(m, now);
}

static uint32_t recv(rl_nds *m, unsigned me, uint64_t now)
{
    nds_ipc *p = &m->ipc;
    nds_fifo *f = &p->fifo[me ^ 1];
    if (f->n == 0) {
        p->cnt[me] |= CNT_ERROR;
        return p->last[me];
    }
    uint32_t v = f->q[f->rd];
    if (!(p->cnt[me] & CNT_ENABLE))
        return v;
    f->rd = (uint8_t)((f->rd + 1) % NDS_FIFO_WORDS);
    f->n--;
    p->last[me] = v;
    edges(m, now);
    return v;
}

uint32_t nds_ipc_read(rl_nds *m, unsigned cpu, uint32_t off, uint64_t now)
{
    const nds_ipc *p = &m->ipc;
    switch (off) {
    case 0x180:
        return p->sync_out[cpu ^ 1] | (uint32_t)p->sync_out[cpu] << 8 | (uint32_t)p->sync_irq[cpu] << 14;
    case 0x184:
        return cnt_read(m, cpu);
    case NDS_FIFORECV:
        return recv(m, cpu, now);
    default:
        return 0;
    }
}

void nds_ipc_write(rl_nds *m, unsigned cpu, uint32_t off, uint32_t v, uint32_t mask, uint64_t now)
{
    nds_ipc *p = &m->ipc;
    switch (off) {
    case 0x180:
        if (!(mask & 0xFF00))
            return;
        p->sync_out[cpu] = (uint8_t)((v >> 8) & 15);
        p->sync_irq[cpu] = (v >> 14) & 1;
        if ((v & 0x2000) && p->sync_irq[cpu ^ 1])
            nds_irq_set(m, cpu ^ 1, RL_NDS_IRQ_IPC_SYNC, now);
        return;
    case 0x184:
        if (mask & 0xFF) {
            p->cnt[cpu] = (uint16_t)((p->cnt[cpu] & ~CNT_SEND_IRQ) | (v & CNT_SEND_IRQ));
            if (v & CNT_SEND_CLEAR) {
                p->fifo[cpu].n = 0;
                p->fifo[cpu].rd = 0;
                p->last[cpu ^ 1] = 0;
            }
        }
        if (mask & 0xFF00) {
            p->cnt[cpu] = (uint16_t)((p->cnt[cpu] & ~(CNT_RECV_IRQ | CNT_ENABLE)) | (v & (CNT_RECV_IRQ | CNT_ENABLE)));
            if (v & CNT_ERROR)
                p->cnt[cpu] &= (uint16_t)~CNT_ERROR;
        }
        edges(m, now);
        return;
    case 0x188:
        send(m, cpu, v, now);
        return;
    default:
        return;
    }
}
