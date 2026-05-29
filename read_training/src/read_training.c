#include "read_training.h"

#include <ctype.h>
#include <string.h>

#define T07_CMD_CODE 0x93U
#define T07_ACK_OK 0xF0U
#define T07_ACK_TBL_ERR 0x51U
#define T07_ACK_SEG_ERR 0x52U

#ifndef T07_RESULT_READ_RETRY_COUNT
#define T07_RESULT_READ_RETRY_COUNT 3U
#endif

#ifndef T07_ERROR_PACKET_SEQUENCE
#define T07_ERROR_PACKET_SEQUENCE (-8)
#endif

#ifndef T07_ERROR_LFSR_MISMATCH
#define T07_ERROR_LFSR_MISMATCH (-9)
#endif

#ifndef T07_ERROR_DELAY_APPLY
#define T07_ERROR_DELAY_APPLY (-10)
#endif

#ifndef T07_RESULT_CMD_ADDR
#define T07_RESULT_CMD_ADDR ((uintptr_t)0U)
#endif

#ifndef T07_RESULT_RSP_ADDR
#define T07_RESULT_RSP_ADDR ((uintptr_t)0U)
#endif

#if defined(T07_USE_NATIVE_XIL_IO)
#include "xil_io.h"

/*
 * Board build path.
 * Xil_Out64/Xil_In64 may use Xilinx-specific integer types, so keep small
 * wrappers that match T07Out64Fn/T07In64Fn exactly.
 */
static void t07_native_out64(uintptr_t addr, u64 value)
{
    Xil_Out64((UINTPTR)addr, value);
}

static u64 t07_native_in64(uintptr_t addr)
{
    return Xil_In64((UINTPTR)addr);
}

static T07Out64Fn g_t07_out64 = t07_native_out64;
static T07In64Fn g_t07_in64 = t07_native_in64;
#else
/*
 * PC test path.
 * Tests inject mock functions with t07_set_io(), so this file can be built
 * without Xilinx headers or hardware.
 */
static T07Out64Fn g_t07_out64 = NULL;
static T07In64Fn g_t07_in64 = NULL;
#endif

static T07ApplyDelayFn g_t07_apply_delay = NULL;
static T07PassZoneLogFn g_t07_pass_zone_log = NULL;
static void *g_t07_pass_zone_log_context = NULL;

/*
 * expected_read_lfsr[valid_rx_index][dq0..dq7]
 *
 * valid_rx is still stored as dq0..dq63, but the expected pattern only has
 * eight DQ values. The same dq0..dq7 pattern repeats for every 8-DQ group:
 *   dq00..07 use expected_read_lfsr[n][0..7]
 *   dq08..15 use expected_read_lfsr[n][0..7]
 *   ...
 *   dq56..63 use expected_read_lfsr[n][0..7]
 *
 * The LFSR sequence length is 6, so valid_rx[6] starts again from row 0.
 */
const u8 expected_read_lfsr[T07_READ_LFSR_LENGTH][T07_READ_LFSR_DQ_GROUP_SIZE] = {
    {0x21U, 0x28U, 0x2FU, 0x36U, 0x3DU, 0x44U, 0x4BU, 0x52U},
    {0x52U, 0x59U, 0x60U, 0x67U, 0x6EU, 0x75U, 0x7CU, 0x83U},
    {0x83U, 0x8AU, 0x91U, 0x98U, 0x9FU, 0xA6U, 0xADU, 0xB4U},
    {0xB4U, 0xBBU, 0xC2U, 0xC9U, 0xD0U, 0xD7U, 0xDEU, 0xE5U},
    {0xE5U, 0xECU, 0xF3U, 0xFAU, 0x01U, 0x08U, 0x0FU, 0x16U},
    {0x16U, 0x1DU, 0x24U, 0x2BU, 0x32U, 0x39U, 0x40U, 0x47U}
};

const char *rt_status_message(RtStatus status)
{
    switch (status) {
    case RT_OK:
        return "ok";
    case RT_ERROR_INVALID_ARGUMENT:
        return "invalid argument";
    case RT_ERROR_BUFFER_TOO_SMALL:
        return "buffer too small";
    default:
        return "unknown status";
    }
}

RtStatus rt_trim_line(const char *line, char *out, size_t out_size, size_t *written)
{
    const unsigned char *begin;
    const unsigned char *end;
    size_t length;
    size_t required;

    if (line == NULL) {
        return RT_ERROR_INVALID_ARGUMENT;
    }

    begin = (const unsigned char *)line;
    while (*begin != '\0' && isspace(*begin)) {
        ++begin;
    }

    end = (const unsigned char *)line + strlen(line);
    while (end > begin && isspace(*(end - 1))) {
        --end;
    }

    length = (size_t)(end - begin);
    required = length + 1U;

    if (written != NULL) {
        *written = required;
    }

    if (out == NULL) {
        return RT_OK;
    }

    if (out_size < required) {
        return RT_ERROR_BUFFER_TOO_SMALL;
    }

    if (length > 0U) {
        memcpy(out, begin, length);
    }
    out[length] = '\0';

    return RT_OK;
}

void t07_set_io(T07Out64Fn out64, T07In64Fn in64)
{
#if defined(T07_USE_NATIVE_XIL_IO)
    g_t07_out64 = out64 != NULL ? out64 : t07_native_out64;
    g_t07_in64 = in64 != NULL ? in64 : t07_native_in64;
#else
    g_t07_out64 = out64;
    g_t07_in64 = in64;
#endif
}

void t07_set_delay_apply(T07ApplyDelayFn apply_delay)
{
    g_t07_apply_delay = apply_delay;
}

void t07_set_pass_zone_log(T07PassZoneLogFn log_fn, void *user_context)
{
    g_t07_pass_zone_log = log_fn;
    g_t07_pass_zone_log_context = user_context;
}

int t07_rsult_read(u8 mode,
                   u8 frame_num,
                   u16 bram_addr,
                   u8 *p_pkt_cnt,
                   u8 *p_seg_cnt,
                   u32 *p_data)
{
    u64 cmd;
    unsigned int attempt;

    if (p_pkt_cnt == NULL || p_seg_cnt == NULL || p_data == NULL) {
        return T07_ERROR_INVALID_ARGUMENT;
    }
    if (mode > 0x0FU || frame_num > 0x0FU) {
        return T07_ERROR_INVALID_ARGUMENT;
    }
    if (g_t07_out64 == NULL || g_t07_in64 == NULL) {
        return T07_ERROR_IO_NOT_CONFIGURED;
    }

    /*
     * Command format:
     * [63:56] command code
     * [55:52] mode
     * [51:48] frame number
     * [47:32] BRAM address
     * [31:00] reserved
     */
    cmd = ((u64)T07_CMD_CODE << 56) |
          ((u64)mode << 52) |
          ((u64)frame_num << 48) |
          ((u64)bram_addr << 32);

    /*
     * One packet has 20 segments. Each Xil_In64() response carries one
     * 32-bit segment in [31:0], plus status/count fields in the upper bits.
     *
     * Expected order for a one-packet read:
     *   response  0..18: pkt_cnt=0, seg_cnt=1..19
     *   response     19: pkt_cnt=1, seg_cnt=0
     * If the packet/segment sequence is wrong, send the command again and
     * retry the whole packet read.
     */
    for (attempt = 0U; attempt < T07_RESULT_READ_RETRY_COUNT; ++attempt) {
        u32 retry_data[T07_RESULT_SEGMENT_COUNT];
        u8 last_pkt_cnt = 0U;
        u8 last_seg_cnt = 0U;
        int sequence_ok = 1;
        size_t i;

        g_t07_out64(T07_RESULT_CMD_ADDR, cmd);

        for (i = 0U; i < T07_RESULT_SEGMENT_COUNT; ++i) {
            u64 rsp_raw = g_t07_in64(T07_RESULT_RSP_ADDR);
            u8 rsp_cmd = (u8)((rsp_raw >> 56) & 0xFFU);
            u8 ack = (u8)((rsp_raw >> 48) & 0xFFU);
            u8 pkt_cnt = (u8)((rsp_raw >> 40) & 0x0FU);
            u8 seg_cnt = (u8)((rsp_raw >> 32) & 0xFFU);
            u8 expected_pkt_cnt = i == (T07_RESULT_SEGMENT_COUNT - 1U) ? 1U : 0U;
            u8 expected_seg_cnt = i == (T07_RESULT_SEGMENT_COUNT - 1U) ? 0U : (u8)(i + 1U);

            if (rsp_cmd != T07_CMD_CODE) {
                return T07_ERROR_ACK;
            }
            if (ack == T07_ACK_TBL_ERR) {
                return T07_ERROR_TABLE;
            }
            if (ack == T07_ACK_SEG_ERR) {
                return T07_ERROR_SEGMENT;
            }
            if (ack != T07_ACK_OK) {
                return T07_ERROR_ACK;
            }

            if (pkt_cnt != expected_pkt_cnt || seg_cnt != expected_seg_cnt) {
                sequence_ok = 0;
                break;
            }

            last_pkt_cnt = pkt_cnt;
            last_seg_cnt = seg_cnt;
            retry_data[i] = (u32)(rsp_raw & 0xFFFFFFFFU);
        }

        if (sequence_ok != 0) {
            memcpy(p_data, retry_data, sizeof(retry_data));
            *p_pkt_cnt = last_pkt_cnt;
            *p_seg_cnt = last_seg_cnt;
            return T07_OK;
        }
    }

    return T07_ERROR_PACKET_SEQUENCE;
}

int t07_read_training_results(u16 result_len,
                              T07Pc pc,
                              T07ResultEntry *entries,
                              T07ValidRxEntry *valid_rx,
                              size_t valid_rx_capacity,
                              size_t *valid_rx_count)
{
    u8 working_rx[T07_RX_DQ_COUNT];
    size_t produced = 0U;
    unsigned int bits_in_run = 0U;
    u16 result_index;

    if ((result_len > 0U && entries == NULL) || valid_rx_count == NULL) {
        return T07_ERROR_INVALID_ARGUMENT;
    }
    if (pc != T07_PC0 && pc != T07_PC1) {
        return T07_ERROR_INVALID_ARGUMENT;
    }

    memset(working_rx, 0, sizeof(working_rx));
    *valid_rx_count = 0U;

    /*
     * result_index is the BRAM address. For each result, read one 20-segment
     * packet, unpack it into T07ResultEntry, then collect enabled RX bits.
     */
    for (result_index = 0U; result_index < result_len; ++result_index) {
        u32 raw_segments[T07_RESULT_SEGMENT_COUNT];
        u8 bytes[T07_RESULT_SEGMENT_COUNT * 4U];
        u8 pkt_cnt = 0U;
        u8 seg_cnt = 0U;
        u8 read_en;
        size_t i;
        unsigned int bit_step;
        int status;

        status = t07_rsult_read(T07_MODE_READ,
                                T07_RESULT_FRAME_NUM,
                                result_index,
                                &pkt_cnt,
                                &seg_cnt,
                                raw_segments);
        if (status != T07_OK) {
            return status;
        }

        (void)pkt_cnt;
        (void)seg_cnt;

        /*
         * This stores raw_segments[i][7:0] into bytes[i*4+0] and
         * raw_segments[i][31:24] into bytes[i*4+3].
         *
         * In other words:
         *   raw segment 0 bits [7:0] are time_ptr.
         *   raw segment 0 bits [15:8] are comp_results_dq7_0_pc0.
         * This matches the hardware table order when byte 0 is carried in
         * the 32-bit segment LSB.
         */
        for (i = 0U; i < T07_RESULT_SEGMENT_COUNT; ++i) {
            bytes[(i * 4U) + 0U] = (u8)(raw_segments[i] & 0xFFU);
            bytes[(i * 4U) + 1U] = (u8)((raw_segments[i] >> 8) & 0xFFU);
            bytes[(i * 4U) + 2U] = (u8)((raw_segments[i] >> 16) & 0xFFU);
            bytes[(i * 4U) + 3U] = (u8)((raw_segments[i] >> 24) & 0xFFU);
        }

        /*
         * Data table byte map:
         *   byte[ 0] time_ptr
         *   byte[ 1] comp_results_dq7_0_pc0
         *   byte[ 2] comp_results_dq15_8_pc0
         *   byte[ 3] comp_results_dq23_16_pc0
         *   byte[ 4] comp_results_dq31_24_pc0
         *   byte[ 5] comp_results_dq7_0_pc1
         *   byte[ 6] comp_results_dq15_8_pc1
         *   byte[ 7] comp_results_dq23_16_pc1
         *   byte[ 8] comp_results_dq31_24_pc1
         *   byte[ 9] read_en_pc0
         *   byte[10] read_en_pc1
         *   byte[11] read_tph_pc0
         *   byte[12] read_tph_pc1
         *   byte[13..76] rx_dq0..rx_dq63
         *   byte[77..79] unused padding from the 20 x 32-bit segments
         */
        entries[result_index].time_ptr = bytes[0U];
        entries[result_index].comp_results_pc0[0] = bytes[1U];
        entries[result_index].comp_results_pc0[1] = bytes[2U];
        entries[result_index].comp_results_pc0[2] = bytes[3U];
        entries[result_index].comp_results_pc0[3] = bytes[4U];
        entries[result_index].comp_results_pc1[0] = bytes[5U];
        entries[result_index].comp_results_pc1[1] = bytes[6U];
        entries[result_index].comp_results_pc1[2] = bytes[7U];
        entries[result_index].comp_results_pc1[3] = bytes[8U];
        entries[result_index].read_en_pc0 = bytes[9U];
        entries[result_index].read_en_pc1 = bytes[10U];
        entries[result_index].read_tph_pc0 = bytes[11U];
        entries[result_index].read_tph_pc1 = bytes[12U];
        memcpy(entries[result_index].rx_dq,
               &bytes[13U],
               sizeof(entries[result_index].rx_dq));

        read_en = pc == T07_PC0 ?
                  entries[result_index].read_en_pc0 :
                  entries[result_index].read_en_pc1;

        /*
         * read_en marks which bit positions in this result are valid.
         * Hardware capture is handled LSB-first:
         *   scan bit0 -> bit7
         *   first valid bit fills output bit0, next fills output bit1, ...
         */
        for (bit_step = 0U; bit_step < 8U; ++bit_step) {
            unsigned int bit = bit_step;
            unsigned int enabled = ((unsigned int)read_en >> bit) & 0x01U;

            if (enabled != 0U) {
                size_t dq;

                if (bits_in_run == 0U) {
                    memset(working_rx, 0, sizeof(working_rx));
                }

                for (dq = 0U; dq < T07_RX_DQ_COUNT; ++dq) {
                    u8 bit_value = (u8)((entries[result_index].rx_dq[dq] >> bit) & 0x01U);
                    working_rx[dq] = (u8)(working_rx[dq] | (u8)(bit_value << bits_in_run));
                }

                ++bits_in_run;
                if (bits_in_run == 8U) {
                    /* Eight continuous read_en bits become one compact RX sample. */
                    if (valid_rx != NULL) {
                        if (produced >= valid_rx_capacity) {
                            *valid_rx_count = produced;
                            return T07_ERROR_BUFFER_TOO_SMALL;
                        }
                        memcpy(valid_rx[produced].rx_dq,
                               working_rx,
                               sizeof(valid_rx[produced].rx_dq));
                    }
                    ++produced;
                    bits_in_run = 0U;
                }
            } else if (bits_in_run != 0U) {
                *valid_rx_count = produced;
                return T07_ERROR_READ_ENABLE;
            }
        }
    }

    if (bits_in_run != 0U) {
        *valid_rx_count = produced;
        return T07_ERROR_READ_ENABLE;
    }

    *valid_rx_count = produced;
    return T07_OK;
}

static int t07_dq_to_local(T07Pc pc, u8 dq, size_t *pc_index, size_t *local_dq)
{
    if (pc == T07_PC0) {
        if (dq >= T07_DQ_PER_PC) {
            return T07_ERROR_INVALID_ARGUMENT;
        }
        *pc_index = 0U;
        *local_dq = (size_t)dq;
        return T07_OK;
    }
    if (pc == T07_PC1) {
        if (dq < T07_DQ_PER_PC || dq >= T07_RX_DQ_COUNT) {
            return T07_ERROR_INVALID_ARGUMENT;
        }
        *pc_index = 1U;
        *local_dq = (size_t)(dq - T07_DQ_PER_PC);
        return T07_OK;
    }

    return T07_ERROR_INVALID_ARGUMENT;
}

static int t07_add_pass_zone(T07PassData *pass_data,
                             T07Pc pc,
                             u8 dq,
                             u8 mck_dly,
                             u8 bit_dly,
                             u16 pe_start,
                             u16 pe_end)
{
    size_t pc_index;
    size_t local_dq;
    T07PassZone zone;
    T07PassCenter *best;
    int stored = 0;
    int status;

    if (pass_data == NULL) {
        return T07_ERROR_INVALID_ARGUMENT;
    }
    if (mck_dly >= T07_MCK_DLY_COUNT ||
        bit_dly >= T07_BIT_DLY_COUNT ||
        pe_start >= T07_PE_DLY_COUNT ||
        pe_end >= T07_PE_DLY_COUNT ||
        pe_end < pe_start) {
        return T07_ERROR_INVALID_ARGUMENT;
    }

    status = t07_dq_to_local(pc, dq, &pc_index, &local_dq);
    if (status != T07_OK) {
        return status;
    }

    zone.pc = (u8)pc;
    zone.dq = dq;
    zone.mck_dly = mck_dly;
    zone.bit_dly = bit_dly;
    zone.pe_start = pe_start;
    zone.pe_end = pe_end;
    zone.pe_count = (u16)(pe_end - pe_start + 1U);

    if (pass_data->zone_count[pc_index][local_dq] < T07_MAX_PASS_ZONES_PER_DQ) {
        u8 zone_index = pass_data->zone_count[pc_index][local_dq];

        pass_data->zones[pc_index][local_dq][zone_index] = zone;
        ++pass_data->zone_count[pc_index][local_dq];
        stored = 1;
    }

    if (g_t07_pass_zone_log != NULL) {
        g_t07_pass_zone_log(&zone, stored, g_t07_pass_zone_log_context);
    }

    best = &pass_data->best_center[pc_index][local_dq];
    if (best->valid == 0U || zone.pe_count > best->pe_count) {
        best->valid = 1U;
        best->pc = zone.pc;
        best->dq = zone.dq;
        best->mck_dly = zone.mck_dly;
        best->bit_dly = zone.bit_dly;
        best->pe_start = zone.pe_start;
        best->pe_end = zone.pe_end;
        best->pe_count = zone.pe_count;
        best->pe_dly = (u16)(zone.pe_start + (zone.pe_count / 2U));
    }
    return T07_OK;
}

int t07_check_dq_lfsr(const T07ValidRxEntry *valid_rx,
                      size_t valid_rx_count,
                      u8 dq,
                      size_t *failed_sample)
{
    size_t sample;

    if (valid_rx_count > 0U && valid_rx == NULL) {
        return T07_ERROR_INVALID_ARGUMENT;
    }
    if (dq >= T07_RX_DQ_COUNT) {
        return T07_ERROR_INVALID_ARGUMENT;
    }

    for (sample = 0U; sample < valid_rx_count; ++sample) {
        size_t lfsr_index = sample % T07_READ_LFSR_LENGTH;
        u8 expected = expected_read_lfsr[lfsr_index][dq % T07_READ_LFSR_DQ_GROUP_SIZE];

        if (valid_rx[sample].rx_dq[dq] != expected) {
            if (failed_sample != NULL) {
                *failed_sample = sample;
            }
            return T07_ERROR_LFSR_MISMATCH;
        }
    }

    if (failed_sample != NULL) {
        *failed_sample = valid_rx_count;
    }
    return T07_OK;
}

int t07_check_valid_rx_lfsr(const T07ValidRxEntry *valid_rx,
                            size_t valid_rx_count,
                            size_t *failed_sample,
                            size_t *failed_dq)
{
    size_t dq;

    if (valid_rx_count > 0U && valid_rx == NULL) {
        return T07_ERROR_INVALID_ARGUMENT;
    }

    for (dq = 0U; dq < T07_RX_DQ_COUNT; ++dq) {
        size_t failed_at_sample = valid_rx_count;
        int status = t07_check_dq_lfsr(valid_rx,
                                       valid_rx_count,
                                       (u8)dq,
                                       &failed_at_sample);

        if (status != T07_OK) {
            if (failed_sample != NULL) {
                *failed_sample = failed_at_sample;
            }
            if (failed_dq != NULL) {
                *failed_dq = dq;
            }
            return status;
        }
    }

    if (failed_sample != NULL) {
        *failed_sample = valid_rx_count;
    }
    if (failed_dq != NULL) {
        *failed_dq = T07_RX_DQ_COUNT;
    }
    return T07_OK;
}

int t07_get_pass(const T07PassData *pass_data,
                 T07Pc pc,
                 u8 dq,
                 u8 mck_dly,
                 u8 bit_dly,
                 u16 pe_dly)
{
    size_t pc_index;
    size_t local_dq;
    size_t zone_index;

    if (pass_data == NULL) {
        return 0;
    }
    if (mck_dly >= T07_MCK_DLY_COUNT ||
        bit_dly >= T07_BIT_DLY_COUNT ||
        pe_dly >= T07_PE_DLY_COUNT) {
        return 0;
    }
    if (t07_dq_to_local(pc, dq, &pc_index, &local_dq) != T07_OK) {
        return 0;
    }

    for (zone_index = 0U;
         zone_index < pass_data->zone_count[pc_index][local_dq];
         ++zone_index) {
        const T07PassZone *zone = &pass_data->zones[pc_index][local_dq][zone_index];

        if (zone->mck_dly == mck_dly &&
            zone->bit_dly == bit_dly &&
            pe_dly >= zone->pe_start &&
            pe_dly <= zone->pe_end) {
            return 1;
        }
    }

    return 0;
}

size_t t07_collect_pass_zones(const T07PassData *pass_data,
                              T07Pc pc,
                              u8 dq,
                              T07PassZone *zones,
                              size_t zone_capacity)
{
    size_t pc_index;
    size_t local_dq;
    size_t stored_count;
    size_t i;

    if (pass_data == NULL) {
        return 0U;
    }
    if (t07_dq_to_local(pc, dq, &pc_index, &local_dq) != T07_OK) {
        return 0U;
    }

    stored_count = pass_data->zone_count[pc_index][local_dq];
    for (i = 0U; i < stored_count; ++i) {
        if (zones != NULL && i < zone_capacity) {
            zones[i] = pass_data->zones[pc_index][local_dq][i];
        }
    }

    return stored_count;
}

int t07_run_read_training_sweep(u16 result_len,
                                T07ResultEntry *entries,
                                T07ValidRxEntry *valid_rx,
                                size_t valid_rx_capacity,
                                T07PassData *pass_data,
                                T07PassCenter centers[T07_PC_COUNT][T07_DQ_PER_PC])
{
    size_t pc_index;

    if (result_len == 0U ||
        entries == NULL ||
        valid_rx == NULL ||
        pass_data == NULL ||
        valid_rx_capacity < T07_READ_LFSR_LENGTH) {
        return T07_ERROR_INVALID_ARGUMENT;
    }
    if (g_t07_apply_delay == NULL) {
        return T07_ERROR_IO_NOT_CONFIGURED;
    }

    memset(pass_data, 0, sizeof(*pass_data));

    /*
     * Full training sweep order:
     *   cur_pc    : 0..1
     *   i_mck_dly : 0..15
     *   i_bit_dly : 0..7
     *   i_pe_dly  : 0..(T07_PE_DLY_COUNT - 1)
     *
     * For each delay point, set every DQ in the current PC to that delay,
     * read result data once, then judge each DQ in that PC independently.
     * Only pass zones are stored, so pass_data memory does not grow with
     * T07_PE_DLY_COUNT.
     */
    for (pc_index = 0U; pc_index < T07_PC_COUNT; ++pc_index) {
        T07Pc pc = pc_index == 0U ? T07_PC0 : T07_PC1;
        u8 dq_begin = pc == T07_PC0 ? 0U : T07_DQ_PER_PC;
        u8 dq_end = (u8)(dq_begin + T07_DQ_PER_PC);
        u8 mck_dly;

        for (mck_dly = 0U; mck_dly < T07_MCK_DLY_COUNT; ++mck_dly) {
            u8 bit_dly;

            for (bit_dly = 0U; bit_dly < T07_BIT_DLY_COUNT; ++bit_dly) {
                u8 active[T07_DQ_PER_PC];
                u16 pe_start[T07_DQ_PER_PC];
                u16 pe_dly;
                u8 dq;

                memset(active, 0, sizeof(active));
                memset(pe_start, 0, sizeof(pe_start));

                for (pe_dly = 0U; pe_dly < T07_PE_DLY_COUNT; ++pe_dly) {
                    size_t valid_rx_count = 0U;
                    int status;

                    for (dq = dq_begin; dq < dq_end; ++dq) {
                        status = g_t07_apply_delay(pc, dq, mck_dly, bit_dly, pe_dly);
                        if (status != T07_OK) {
                            return status < 0 ? status : T07_ERROR_DELAY_APPLY;
                        }
                    }

                    status = t07_read_training_results(result_len,
                                                       pc,
                                                       entries,
                                                       valid_rx,
                                                       valid_rx_capacity,
                                                       &valid_rx_count);
                    if (status != T07_OK) {
                        return status;
                    }

                    for (dq = dq_begin; dq < dq_end; ++dq) {
                        size_t local_dq = (size_t)(dq - dq_begin);
                        int passed = 0;

                        if (valid_rx_count >= T07_READ_LFSR_LENGTH &&
                            t07_check_dq_lfsr(valid_rx, valid_rx_count, dq, NULL) == T07_OK) {
                            passed = 1;
                        }

                        if (passed != 0) {
                            if (active[local_dq] == 0U) {
                                active[local_dq] = 1U;
                                pe_start[local_dq] = pe_dly;
                            }
                        } else if (active[local_dq] != 0U) {
                            status = t07_add_pass_zone(pass_data,
                                                       pc,
                                                       dq,
                                                       mck_dly,
                                                       bit_dly,
                                                       pe_start[local_dq],
                                                       (u16)(pe_dly - 1U));
                            if (status != T07_OK) {
                                return status;
                            }
                            active[local_dq] = 0U;
                        }
                    }
                }

                if (T07_PE_DLY_COUNT > 0U) {
                    for (dq = dq_begin; dq < dq_end; ++dq) {
                        size_t local_dq = (size_t)(dq - dq_begin);

                        if (active[local_dq] != 0U) {
                            int status = t07_add_pass_zone(pass_data,
                                                           pc,
                                                           dq,
                                                           mck_dly,
                                                           bit_dly,
                                                           pe_start[local_dq],
                                                           (u16)(T07_PE_DLY_COUNT - 1U));

                            if (status != T07_OK) {
                                return status;
                            }
                        }
                    }
                }
            }
        }
    }

    if (centers != NULL) {
        memcpy(centers, pass_data->best_center, sizeof(pass_data->best_center));
    }
    return T07_OK;
}
