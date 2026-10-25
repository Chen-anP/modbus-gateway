/*
 * mb_master.c - Modbus RTU 主机请求层，说明见 mb_master.h
 *
 * 一次请求的过程（transact_once）：
 *   1. 帧间隔：距离上次收到回应不足 turnaround_ms，先等一等
 *   2. 清掉接收缓冲区里的残留数据，发送请求
 *   3. 循环读，把字节喂给帧组装器 mb_framer_feed()，凑够一整帧就返回；
 *      收到的字节如果和请求逐字节一样（收发器回显），跳过
 *   4. 超时还没凑够 → 失败，按 DEBUG 等级打印收到了什么，方便排查
 * 外层 mb_master_request() 负责解析、重试和计数。
 *
 * 这部分逻辑来自第 2 步的 bus_probe.c，第 3 步把它从测试任务里提出来，变成正式模块。
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <string.h>
#include "mb_master.h"
#include "log.h"

#define BYTE_GAP_MS     5u      /* 回应中两个字节间隔超过 5 ms，丢弃半帧 */
#define TURNAROUND_MS   2u      /* 规范：波特率 > 19200 时帧间隔取 1.75 ms，这里取 2 ms */
#define SEND_TIMEOUT_MS 50u     /* 发一帧最长 256 字节，115200 下约 22 ms */

void mb_master_init(mb_master_t *m, const mb_io_t *io, uint32_t rsp_timeout_ms, uint8_t retries)
{
    memset(m, 0, sizeof(*m));
    m->io             = io;
    m->rsp_timeout_ms = rsp_timeout_ms;
    m->byte_gap_ms    = BYTE_GAP_MS;
    m->turnaround_ms  = TURNAROUND_MS;
    m->retries        = retries;
    m->last_rx_ms     = io->now_ms() - TURNAROUND_MS;
}

/* 把一段字节喂给帧组装器；凑成一整帧时返回 1，并把帧复制到 rsp */
static int feed(mb_framer_t *f, const uint8_t *d, uint16_t n, uint32_t now,
                uint8_t *rsp, uint16_t *rsp_len)
{
    uint16_t used;

    while (n > 0) {
        if (mb_framer_feed(f, d, n, now, &used)) {
            memcpy(rsp, f->buf, f->len);
            *rsp_len = f->len;
            return 1;
        }
        d += used;
        n = (uint16_t)(n - used);
    }
    return 0;
}

/* 一次尝试。返回回应长度（>0）；0 = 超时；-1 = 发送失败 */
static int transact_once(mb_master_t *m, const uint8_t *req, uint16_t req_len, uint8_t *rsp)
{
    const mb_io_t *io = m->io;
    mb_framer_t f;
    uint8_t chunk[64];
    uint8_t raw[32];                /* 诊断用：这次收到的前 32 个原始字节 */
    uint16_t raw_len = 0, rsp_len = 0, n;
    uint16_t echo_pos = 0;          /* 已经和请求逐字节对上的个数 */
    int echo_check = (m->echo != MB_ECHO_NONE);     /* 还在检查"是不是回显" */
    uint32_t start, since_rx;

    /* 1. 帧间隔 */
    since_rx = io->now_ms() - m->last_rx_ms;
    if (since_rx < m->turnaround_ms)
        io->delay_ms(m->turnaround_ms - since_rx);

    /* 2. 发送 */
    mb_framer_init(&f, 0, m->byte_gap_ms);      /* 0 = 按"回应帧"格式判断长度 */
    io->flush(io->ctx);
    log_hex(LOG_LVL_DEBUG, io->name, "tx", req, req_len);
    if (io->send(io->ctx, req, req_len, SEND_TIMEOUT_MS) != 0)
        return -1;
    if (req[0] == MB_ADDR_BROADCAST) {          /* 广播没有回应，不用等 */
        m->last_rx_ms = io->now_ms();
        return 0;
    }

    /* 3. 收 */
    start = io->now_ms();
    for (;;) {
        uint32_t elapsed = io->now_ms() - start;
        const uint8_t *d = chunk;

        if (elapsed >= m->rsp_timeout_ms)
            break;
        n = io->read(io->ctx, chunk, sizeof(chunk), m->rsp_timeout_ms - elapsed);
        if (n == 0)
            break;
        for (uint16_t k = 0; k < n && raw_len < sizeof(raw); k++)
            raw[raw_len++] = chunk[k];

        /* 回显：收到的字节和请求逐个比较。整帧都对上 → 是回显，丢掉；
         * 中途对不上 → 不是回显（正常回应的开头本来就和请求一样），把跳过的字节补喂回去 */
        while (echo_check && n > 0) {
            if (*d == req[echo_pos]) {
                d++; n--;
                if (++echo_pos == req_len)
                    echo_check = 0;
            } else {
                echo_check = 0;
                if (feed(&f, req, echo_pos, io->now_ms(), rsp, &rsp_len))
                    break;
            }
        }
        if (rsp_len == 0 && feed(&f, d, n, io->now_ms(), rsp, &rsp_len) == 0)
            continue;                           /* 还没凑够一帧，继续读 */

        m->last_rx_ms = io->now_ms();
        log_hex(LOG_LVL_DEBUG, io->name, "rx", rsp, rsp_len);
        return rsp_len;
    }

    /* 4. 超时。AUTO 模式下的特例：写单个线圈 / 寄存器的回应和请求一模一样，
     *    只收到一份时分不清是回显还是回应——按回应算（见 mb_master.h 里 mb_echo_t 的说明） */
    if (m->echo == MB_ECHO_AUTO && echo_pos == req_len && raw_len == req_len
        && (req[1] == MB_FC_WRITE_COIL || req[1] == MB_FC_WRITE_REG)) {
        memcpy(rsp, req, req_len);
        m->last_rx_ms = io->now_ms();
        LOGD(io->name, "single copy of 05/06 frame taken as reply (echo mode auto)");
        return req_len;
    }

    /* 超时：记下收到了什么（DEBUG 等级，掉线设备每次都会走到这里，INFO 下不刷屏） */
    if (raw_len == 0)
        LOGD(io->name, "no byte received in %u ms", (unsigned)m->rsp_timeout_ms);
    else if (!echo_check && echo_pos == req_len && raw_len == req_len)
        LOGD(io->name, "only echo received, no reply");
    else
        log_hex(LOG_LVL_DEBUG, io->name, "incomplete", raw, raw_len);
    if (raw_len > 0)
        m->last_rx_ms = io->now_ms();
    return 0;
}

int mb_master_request(mb_master_t *m, const uint8_t *req, uint16_t req_len,
                      uint16_t *values, uint16_t max_values, mb_ex_t *ex)
{
    uint8_t rsp[MB_ADU_MAX];
    int rc = MB_ERR_TIMEOUT;
    mb_ex_t dummy;

    if (ex == NULL)
        ex = &dummy;
    *ex = MB_EX_NONE;
    m->st.requests++;

    for (uint8_t attempt = 0; attempt <= m->retries; attempt++) {
        int n;

        if (attempt > 0)
            m->st.retries++;
        n = transact_once(m, req, req_len, rsp);

        if (req[0] == MB_ADDR_BROADCAST && n >= 0) {
            m->st.ok++;                 /* 广播：从机执行但不回应，发出去就算成功 */
            return MB_OK;
        }
        if (n < 0) {
            m->st.send_errors++;
            rc = MB_ERR_TIMEOUT;
            continue;
        }
        if (n == 0) {
            m->st.timeouts++;
            rc = MB_ERR_TIMEOUT;
            continue;
        }

        rc = mb_parse_rsp(req, req_len, rsp, (uint16_t)n, values, max_values, ex);
        if (rc == MB_OK) {
            m->st.ok++;
            return MB_OK;
        }
        if (rc == MB_ERR_EXCEPTION) {   /* 从机是活的，只是不接受这个请求：重试没有意义 */
            m->st.ok++;
            m->st.exceptions++;
            return rc;
        }
        if (rc == MB_ERR_PARAM)         /* 调用者的结果数组太小：重试没有意义 */
            return rc;
        if (rc == MB_ERR_CRC)
            m->st.crc_errors++;
        else
            m->st.frame_errors++;       /* 干扰、错位、别的从机的回应：重试一次可能就好了 */
    }
    return rc;
}

int mb_master_read(mb_master_t *m, uint8_t slave, mb_table_t tab, uint16_t addr, uint16_t count,
                   uint16_t *values, uint16_t max_values, mb_ex_t *ex)
{
    uint8_t req[8];
    uint16_t len = mb_build_read(req, slave, tab, addr, count);

    if (len == 0 || count > max_values)
        return MB_ERR_PARAM;
    return mb_master_request(m, req, len, values, max_values, ex);
}

int mb_master_write_single(mb_master_t *m, uint8_t slave, mb_table_t tab, uint16_t addr, uint16_t value,
                           mb_ex_t *ex)
{
    uint8_t req[8];
    uint16_t len = mb_build_write_single(req, slave, tab, addr, value);

    if (len == 0)
        return MB_ERR_PARAM;
    return mb_master_request(m, req, len, NULL, 0, ex);
}
