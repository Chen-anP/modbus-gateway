/*
 * fwd.c - 转发，说明见 fwd.h
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 陈攀
 */
#include <string.h>
#include "crc32.h"
#include "fwd.h"

void fwd_init(fwd_t *f, fwd_transact_fn transact, void *ctx,
              uint16_t (*get_target)(void *ctx), void (*set_err)(void *ctx, uint16_t err))
{
    memset(f, 0, sizeof(*f));
    f->transact   = transact;
    f->ctx        = ctx;
    f->get_target = get_target;
    f->set_err    = set_err;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t echo(const uint8_t *req, uint16_t len, uint8_t *rsp)
{
    memcpy(rsp, req, len);              /* 06、15 的正常回应就是请求原样（CRC 也一样） */
    return len;
}

static uint16_t exception(fwd_t *f, const uint8_t *req, uint8_t *rsp, mb_ex_t ex, uint16_t err)
{
    uint16_t crc;

    f->errors++;
    if (err && f->set_err)
        f->set_err(f->ctx, err);
    rsp[0] = req[0];
    rsp[1] = (uint8_t)(req[1] | 0x80u);
    rsp[2] = (uint8_t)ex;
    crc = mb_crc16(rsp, 3);
    rsp[3] = (uint8_t)crc;
    rsp[4] = (uint8_t)(crc >> 8);
    return 5;
}

static uint16_t fail(fwd_t *f, const uint8_t *req, uint8_t *rsp, int rc)
{
    return exception(f, req, rsp, MB_EX_DEVICE_FAILURE, rc == MB_ERR_EXCEPTION ? FWD_ERR_EXCEPTION : FWD_ERR_NO_REPLY);
}

/* 06 写 TARGET_CMD */
static uint16_t do_cmd(fwd_t *f, const uint8_t *req, uint16_t len, uint8_t *rsp)
{
    uint16_t target = f->get_target ? f->get_target(f->ctx) : 0;
    uint8_t ch = (uint8_t)(target >> 8), dev = (uint8_t)target;
    uint16_t value = (uint16_t)((req[4] << 8) | req[5]);
    uint8_t out[MB_ADU_MAX];
    uint16_t n;
    mb_ex_t ex = MB_EX_NONE;
    int rc;

    if (ch < 1u || ch > 2u || dev < 1u || dev > 247u)
        return exception(f, req, rsp, MB_EX_DEVICE_FAILURE, FWD_ERR_TARGET);
    n = mb_build_write_single(out, dev, MB_TAB_HOLDING, SENSOR_CMD_REG, value);
    rc = f->transact(f->ctx, ch, out, n, FWD_CMD_TIMEOUT_MS, 2, &ex);
    if (rc != MB_OK)
        return fail(f, req, rsp, rc);
    f->cmds_fwd++;
    return echo(req, len, rsp);
}

/* 把一个 record 转给传感器 */
static int send_rec(fwd_t *f, uint8_t ch, uint8_t dev, uint16_t rec, const uint8_t *d, uint16_t n,
                    uint32_t timeout, uint8_t retries)
{
    uint8_t out[MB_ADU_MAX];
    uint16_t len = mb_build_file_record(out, dev, SENSOR_FILE_NO, rec, d, n);
    mb_ex_t ex = MB_EX_NONE;

    if (len == 0)
        return MB_ERR_PARAM;
    return f->transact(f->ctx, ch, out, len, timeout, retries, &ex);
}

/* 15 写文件记录（file_no 0x1CAA） */
static uint16_t do_file(fwd_t *f, const uint8_t *req, uint16_t len, uint8_t *rsp)
{
    uint16_t file_no = (uint16_t)((req[4] << 8) | req[5]);
    uint16_t rec     = (uint16_t)((req[6] << 8) | req[7]);
    uint16_t n       = (uint16_t)(((req[8] << 8) | req[9]) * 2u);
    const uint8_t *d = &req[10];
    uint8_t ch = (uint8_t)((file_no >> 8) & 0x0Fu), dev = (uint8_t)file_no;
    uint32_t eff;
    int rc, last;

    if (req[2] != (uint8_t)(7u + n) || req[3] != 6u || len != (uint16_t)(req[2] + 5u))   /* 只支持一个子请求 */
        return exception(f, req, rsp, MB_EX_ILLEGAL_VALUE, FWD_ERR_REQUEST);
    if (dev < 1u || dev > 247u)
        return exception(f, req, rsp, MB_EX_ILLEGAL_ADDRESS, FWD_ERR_TARGET);

    /* 文件头：先记下，不转发 */
    if (rec == 0) {
        if (n != 32u)
            return exception(f, req, rsp, MB_EX_ILLEGAL_VALUE, FWD_ERR_REQUEST);
        f->file_len = be32(&d[4]);
        f->file_crc = be32(&d[12]);
        if (f->file_len < 8u || f->file_len > SENSOR_APP_MAX) {
            f->state = FWD_BROKEN;
            return exception(f, req, rsp, MB_EX_ILLEGAL_VALUE, FWD_ERR_LENGTH);
        }
        memcpy(f->hdr, d, 32);
        f->file_no  = file_no;
        f->crc_acc  = 0;
        f->sent     = 0;
        f->next_rec = 1;
        f->last_rec = 0;
        f->state    = FWD_HDR_HELD;
        return echo(req, len, rsp);
    }

    if (f->state == FWD_IDLE || file_no != f->file_no)
        return exception(f, req, rsp, MB_EX_ILLEGAL_VALUE, FWD_ERR_SEQUENCE);
    if (f->state == FWD_BROKEN)
        return exception(f, req, rsp, MB_EX_DEVICE_FAILURE, FWD_ERR_BROKEN);
    if (rec == f->last_rec && f->last_rec != 0) {           /* 上位机没收到回应、重发了刚转发成功的那一包 */
        f->dup_skipped++;
        return echo(req, len, rsp);
    }
    if (f->state == FWD_DONE || rec != f->next_rec) {
        f->state = FWD_BROKEN;
        return exception(f, req, rsp, MB_EX_ILLEGAL_VALUE, FWD_ERR_SEQUENCE);
    }

    /* 这一包里属于文件的字节（最后一包可能多一个补齐的 0） */
    if (f->sent >= f->file_len) {
        f->state = FWD_BROKEN;
        return exception(f, req, rsp, MB_EX_ILLEGAL_VALUE, FWD_ERR_LENGTH);
    }
    eff = (f->file_len - f->sent < n) ? (f->file_len - f->sent) : n;
    last = (f->sent + eff == f->file_len);

    /* 第 1 包：看向量表，像传感器 App 才开始（这时才转发文件头，传感器才擦 Flash） */
    if (f->state == FWD_HDR_HELD) {
        uint32_t sp = le32(&d[0]), entry = le32(&d[4]);
        if (n < 8u || sp < SENSOR_RAM_START || sp > SENSOR_RAM_END || (entry & 1u) == 0u
            || (entry & ~1u) < SENSOR_APP_ADDR + 8u || (entry & ~1u) >= SENSOR_APP_ADDR + f->file_len) {
            f->state = FWD_BROKEN;
            return exception(f, req, rsp, MB_EX_ILLEGAL_VALUE, FWD_ERR_VECTORS);
        }
        rc = send_rec(f, ch, dev, 0, f->hdr, 32, FWD_HDR_TIMEOUT_MS, 1);   /* 擦 Flash：重来一次没关系 */
        if (rc != MB_OK) {
            f->state = FWD_BROKEN;
            return fail(f, req, rsp, rc);
        }
        f->state = FWD_SENDING;
    }

    /* 最后一包：先核对整个文件的 CRC */
    if (last && crc32_update(f->crc_acc, d, eff) != f->file_crc) {
        f->state = FWD_BROKEN;
        return exception(f, req, rsp, MB_EX_DEVICE_FAILURE, FWD_ERR_CRC);
    }

    rc = send_rec(f, ch, dev, rec, d, n, FWD_DATA_TIMEOUT_MS, 0);      /* 数据包不重试：重复会写错位 */
    if (rc != MB_OK) {
        f->state = FWD_BROKEN;
        return fail(f, req, rsp, rc);
    }
    f->crc_acc = crc32_update(f->crc_acc, d, eff);
    f->sent += eff;
    f->last_rec = rec;
    f->next_rec = (uint16_t)(rec + 1u);
    f->records_fwd++;
    if (last)
        f->state = FWD_DONE;
    return echo(req, len, rsp);
}

uint16_t fwd_handle(fwd_t *f, uint8_t my_addr, const uint8_t *req, uint16_t req_len, uint8_t *rsp, int *handled)
{
    uint16_t crc;

    *handled = 0;
    if (req_len < 8u || req[0] != my_addr)
        return 0;
    crc = mb_crc16(req, (uint16_t)(req_len - 2u));
    if (req[req_len - 2u] != (uint8_t)crc || req[req_len - 1u] != (uint8_t)(crc >> 8))
        return 0;

    if (req[1] == MB_FC_WRITE_REG && req_len == 8u && ((req[2] << 8) | req[3]) == FWD_SYS_TARGET_CMD) {
        *handled = 1;
        return do_cmd(f, req, req_len, rsp);
    }
    if (req[1] == MB_FC_WRITE_FILE_RECORD && req_len >= 12u) {
        uint16_t file_no = (uint16_t)((req[4] << 8) | req[5]);
        uint8_t ch = (uint8_t)((file_no >> 8) & 0x0Fu);
        if ((file_no & 0xF000u) == FWD_FILE_BASE && ch >= 1u && ch <= 2u) {
            *handled = 1;
            return do_file(f, req, req_len, rsp);
        }
    }
    return 0;
}

const char *fwd_state_name(fwd_state_t s)
{
    static const char *const k[] = { "idle", "header held", "sending", "done", "broken" };
    return ((unsigned)s < sizeof(k) / sizeof(k[0])) ? k[s] : "?";
}
