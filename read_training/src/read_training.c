#include "read_training.h"

#include <stdio.h>
#include <string.h>

/*
 * KO_NOTE:
 * 이 파일은 T07 read training을 PC test와 FW build 양쪽에서 같이 쓰기 위한 구현입니다.
 * 큰 흐름은 아래 순서입니다.
 *
 * 1. rd_tr_run_read_training_sweep()
 *    pc -> mck_dly -> bit_dly -> pe_dly 순서로 delay를 sweep합니다.
 *
 * 2. 각 delay point마다 g_rd_tr_apply_delay()로 DQ delay를 실제 HW/FW 쪽에 적용합니다.
 *
 * 3. rd_tr_read_training_results()가 HW result table을 읽고, read_en으로 표시된 bit만 모아
 *    RdTrValidRxEntry 형태의 compact sample을 만듭니다.
 *
 * 4. rd_tr_check_dq_lfsr()가 DQ별 compact sample이 expected_read_lfsr와 맞는지 검사합니다.
 *
 * 5. pass가 연속되는 pe_dly 구간을 rd_tr_record_pass_zone()에서 zone으로 닫고 저장합니다.
 *    이때 zone 저장 슬롯이 꽉 차도 best_center는 계속 갱신됩니다.
 *
 * 나중에 주석을 제거하려면 "KO_NOTE"로 검색하면 됩니다.
 */

#define CMD_CODE 0x93U
#define ACK_OK 0xF0U
#define ACK_TBL_ERR 0x51U
#define ACK_SEG_ERR 0x52U

#ifndef RESULT_READ_RETRY_COUNT
#define RESULT_READ_RETRY_COUNT 3U
#endif

#ifndef RD_TR_ERROR_PACKET_SEQUENCE
#define RD_TR_ERROR_PACKET_SEQUENCE (-8)
#endif

#ifndef RD_TR_ERROR_LFSR_MISMATCH
#define RD_TR_ERROR_LFSR_MISMATCH (-9)
#endif

#ifndef RD_TR_ERROR_DELAY_APPLY
#define RD_TR_ERROR_DELAY_APPLY (-10)
#endif

#ifndef RESULT_CMD_ADDR
#define RESULT_CMD_ADDR ((uintptr_t)0U)
#endif

#ifndef RESULT_RSP_ADDR
#define RESULT_RSP_ADDR ((uintptr_t)0U)
#endif

#if defined(USE_NATIVE_XIL_IO)
#include "xil_io.h"

/*
 * Board build path.
 * Xil_Out64/Xil_In64 may use Xilinx-specific integer types, so keep small
 * wrappers that match RdTrOut64Fn/RdTrIn64Fn exactly.
 */
static void rd_tr_native_out64(uintptr_t addr, u64 value)
{
    Xil_Out64((UINTPTR)addr, value);
}

static u64 rd_tr_native_in64(uintptr_t addr)
{
    return Xil_In64((UINTPTR)addr);
}

static RdTrOut64Fn g_rd_tr_out64 = rd_tr_native_out64;
static RdTrIn64Fn g_rd_tr_in64 = rd_tr_native_in64;
#else
/*
 * PC test path.
 * Tests inject mock functions with rd_tr_set_io(), so this file can be built
 * without Xilinx headers or hardware.
 *
 * KO_NOTE:
 * PC 테스트에서는 실제 Xil_Out64/Xil_In64가 없으므로 함수 포인터를 NULL로 둡니다.
 * 테스트 코드가 rd_tr_set_io(mock_out64, mock_in64)를 호출해서 가짜 IO를 연결합니다.
 */
static RdTrOut64Fn g_rd_tr_out64 = NULL;
static RdTrIn64Fn g_rd_tr_in64 = NULL;
#endif

/*
 * KO_NOTE:
 * 아래 세 포인터는 FW 쪽에서 연결해주는 "외부 의존성"입니다.
 * - g_rd_tr_out64 / g_rd_tr_in64: result table command/response register 접근
 * - g_rd_tr_apply_delay: 현재 sweep point의 delay 값을 실제 PHY/DQ에 적용
 * - g_rd_tr_pass_zone_log: zone 발견 이벤트를 UART/file/buffer 등으로 내보내는 선택 기능
 */
static RdTrApplyDelayFn g_rd_tr_apply_delay = NULL;
static RdTrPassZoneLogFn g_rd_tr_pass_zone_log = NULL;
static void *g_rd_tr_pass_zone_log_context = NULL;

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
const u8 expected_read_lfsr[READ_LFSR_LENGTH][READ_LFSR_DQ_GROUP_SIZE] = {
    {0x21U, 0x28U, 0x2FU, 0x36U, 0x3DU, 0x44U, 0x4BU, 0x52U},
    {0x52U, 0x59U, 0x60U, 0x67U, 0x6EU, 0x75U, 0x7CU, 0x83U},
    {0x83U, 0x8AU, 0x91U, 0x98U, 0x9FU, 0xA6U, 0xADU, 0xB4U},
    {0xB4U, 0xBBU, 0xC2U, 0xC9U, 0xD0U, 0xD7U, 0xDEU, 0xE5U},
    {0xE5U, 0xECU, 0xF3U, 0xFAU, 0x01U, 0x08U, 0x0FU, 0x16U},
    {0x16U, 0x1DU, 0x24U, 0x2BU, 0x32U, 0x39U, 0x40U, 0x47U}
};

int dbg_rd_tr_decode_point(u32 point,
                           u8 *mck_dly,
                           u8 *bit_dly,
                           u16 *pe_dly)
{
    const u32 points_per_mck = (u32)BIT_DLY_COUNT * (u32)PE_DLY_COUNT;
    const u32 points_per_pc = (u32)MCK_DLY_COUNT * points_per_mck;
    u32 rem;

    if (mck_dly == NULL ||
        bit_dly == NULL ||
        pe_dly == NULL ||
        point >= points_per_pc) {
        return RD_TR_ERROR_INVALID_ARGUMENT;
    }

    *mck_dly = (u8)(point / points_per_mck);
    rem = point % points_per_mck;
    *bit_dly = (u8)(rem / (u32)PE_DLY_COUNT);
    *pe_dly = (u16)(rem % (u32)PE_DLY_COUNT);

    return RD_TR_OK;
}

int dbg_rd_tr_format_pass_zone(const RdTrPassZone *zone,
                               char *out,
                               size_t out_size)
{
    u8 start_mck;
    u8 start_bit;
    u16 start_pe;
    u8 end_mck;
    u8 end_bit;
    u16 end_pe;
    int written;
    int status;

    if (zone == NULL || out == NULL || out_size == 0U) {
        return RD_TR_ERROR_INVALID_ARGUMENT;
    }

    status = dbg_rd_tr_decode_point(zone->point_start, &start_mck, &start_bit, &start_pe);
    if (status != RD_TR_OK) {
        return status;
    }
    status = dbg_rd_tr_decode_point(zone->point_end, &end_mck, &end_bit, &end_pe);
    if (status != RD_TR_OK) {
        return status;
    }

    written = snprintf(out,
                       out_size,
                       "pc%u dq%02u m%02u b%u pe%03u..m%02u b%u pe%03u len%u",
                       (unsigned int)zone->pc,
                       (unsigned int)zone->dq,
                       (unsigned int)start_mck,
                       (unsigned int)start_bit,
                       (unsigned int)start_pe,
                       (unsigned int)end_mck,
                       (unsigned int)end_bit,
                       (unsigned int)end_pe,
                       (unsigned int)zone->point_count);

    if (written < 0) {
        return RD_TR_ERROR_INVALID_ARGUMENT;
    }
    if ((size_t)written >= out_size) {
        return RD_TR_ERROR_BUFFER_TOO_SMALL;
    }

    return RD_TR_OK;
}

int dbg_rd_tr_format_pass_center(const RdTrPassCenter *center,
                                 char *out,
                                 size_t out_size)
{
    u8 start_mck;
    u8 start_bit;
    u16 start_pe;
    u8 end_mck;
    u8 end_bit;
    u16 end_pe;
    int written;
    int status;

    if (center == NULL || out == NULL || out_size == 0U) {
        return RD_TR_ERROR_INVALID_ARGUMENT;
    }

    if (center->valid == 0U) {
        written = snprintf(out, out_size, "center=none");
    } else {
        status = dbg_rd_tr_decode_point(center->point_start, &start_mck, &start_bit, &start_pe);
        if (status != RD_TR_OK) {
            return status;
        }
        status = dbg_rd_tr_decode_point(center->point_end, &end_mck, &end_bit, &end_pe);
        if (status != RD_TR_OK) {
            return status;
        }

        written = snprintf(out,
                           out_size,
                           "pc%u dq%02u center=m%02u b%u pe%03u longest=m%02u b%u pe%03u..m%02u b%u pe%03u len%u",
                           (unsigned int)center->pc,
                           (unsigned int)center->dq,
                           (unsigned int)center->mck_dly,
                           (unsigned int)center->bit_dly,
                           (unsigned int)center->pe_dly,
                           (unsigned int)start_mck,
                           (unsigned int)start_bit,
                           (unsigned int)start_pe,
                           (unsigned int)end_mck,
                           (unsigned int)end_bit,
                           (unsigned int)end_pe,
                           (unsigned int)center->point_count);
    }

    if (written < 0) {
        return RD_TR_ERROR_INVALID_ARGUMENT;
    }
    if ((size_t)written >= out_size) {
        return RD_TR_ERROR_BUFFER_TOO_SMALL;
    }

    return RD_TR_OK;
}

void rd_tr_set_io(RdTrOut64Fn out64, RdTrIn64Fn in64)
{
#if defined(USE_NATIVE_XIL_IO)
    /*
     * KO_NOTE:
     * Board build에서는 out64/in64를 NULL로 넘기면 native Xil_Out64/Xil_In64 wrapper를 씁니다.
     * 테스트나 특수 FW에서 다른 IO layer를 쓰고 싶으면 NULL이 아닌 함수 포인터를 넘기면 됩니다.
     */
    g_rd_tr_out64 = out64 != NULL ? out64 : rd_tr_native_out64;
    g_rd_tr_in64 = in64 != NULL ? in64 : rd_tr_native_in64;
#else
    /*
     * KO_NOTE:
     * PC build에서는 native IO가 없으므로 NULL이면 "IO 미설정" 상태가 됩니다.
     * 이 상태에서 t07_result_read()를 부르면 RD_TR_ERROR_IO_NOT_CONFIGURED가 반환됩니다.
     */
    g_rd_tr_out64 = out64;
    g_rd_tr_in64 = in64;
#endif
}

void rd_tr_set_delay_apply(RdTrApplyDelayFn apply_delay)
{
    /*
     * KO_NOTE:
     * sweep loop는 delay 값을 계산만 하고, 실제 register write 방법은 모릅니다.
     * FW 쪽에서 이 콜백에 "pc/dq/mck/bit/pe를 HW에 적용하는 함수"를 연결해야 합니다.
     */
    g_rd_tr_apply_delay = apply_delay;
}

void rd_tr_set_pass_zone_log(RdTrPassZoneLogFn log_fn, void *user_context)
{
    /*
     * KO_NOTE:
     * pass zone이 발견될 때마다 로그를 남기고 싶으면 log_fn을 연결합니다.
     * user_context는 FILE*, UART handle, ring buffer 포인터 등 호출자가 원하는 값을 넘기면 됩니다.
     */
    g_rd_tr_pass_zone_log = log_fn;
    g_rd_tr_pass_zone_log_context = user_context;
}

/*
 * Lowest-level result packet read. Keep this function name as t07_result_read
 * because existing training code calls it directly.
 */
int t07_result_read(u8 mode,
                   u8 frame_num,
                   u16 bram_addr,
                   u8 *p_pkt_cnt,
                   u8 *p_seg_cnt,
                   u32 *p_data)
{
    u64 cmd;
    unsigned int attempt;

    if (p_pkt_cnt == NULL || p_seg_cnt == NULL || p_data == NULL) {
        return RD_TR_ERROR_INVALID_ARGUMENT;
    }
    if (mode > 0x0FU || frame_num > 0x0FU) {
        return RD_TR_ERROR_INVALID_ARGUMENT;
    }
    if (g_rd_tr_out64 == NULL || g_rd_tr_in64 == NULL) {
        return RD_TR_ERROR_IO_NOT_CONFIGURED;
    }

    /*
     * Command format:
     * [63:56] command code
     * [55:52] mode
     * [51:48] frame number
     * [47:32] BRAM address
     * [31:00] reserved
     */
    cmd = ((u64)CMD_CODE << 56) |
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
    for (attempt = 0U; attempt < RESULT_READ_RETRY_COUNT; ++attempt) {
        /*
         * KO_NOTE:
         * retry_data에 먼저 임시 저장합니다.
         * 중간에 packet sequence가 깨지면 이 attempt 전체를 버리고 다시 읽어야 하기 때문입니다.
         */
        u32 retry_data[RESULT_SEGMENT_COUNT];
        u8 last_pkt_cnt = 0U;
        u8 last_seg_cnt = 0U;
        int sequence_ok = 1;
        size_t i;

        g_rd_tr_out64(RESULT_CMD_ADDR, cmd);

        for (i = 0U; i < RESULT_SEGMENT_COUNT; ++i) {
            /*
             * KO_NOTE:
             * rsp_raw 상위 byte에는 command/ack/pkt/segment 정보가 있고,
             * 하위 32-bit에는 실제 result table segment 하나가 들어 있습니다.
             */
            u64 rsp_raw = g_rd_tr_in64(RESULT_RSP_ADDR);
            u8 rsp_cmd = (u8)((rsp_raw >> 56) & 0xFFU);
            u8 ack = (u8)((rsp_raw >> 48) & 0xFFU);
            u8 pkt_cnt = (u8)((rsp_raw >> 40) & 0x0FU);
            u8 seg_cnt = (u8)((rsp_raw >> 32) & 0xFFU);
            u8 expected_pkt_cnt = i == (RESULT_SEGMENT_COUNT - 1U) ? 1U : 0U;
            u8 expected_seg_cnt = i == (RESULT_SEGMENT_COUNT - 1U) ? 0U : (u8)(i + 1U);

            if (rsp_cmd != CMD_CODE) {
                return RD_TR_ERROR_ACK;
            }
            if (ack == ACK_TBL_ERR) {
                return RD_TR_ERROR_TABLE;
            }
            if (ack == ACK_SEG_ERR) {
                return RD_TR_ERROR_SEGMENT;
            }
            if (ack != ACK_OK) {
                return RD_TR_ERROR_ACK;
            }

            /*
             * KO_NOTE:
             * 정상 packet은 segment 1..19가 오고 마지막에 pkt_cnt=1, seg_cnt=0이 옵니다.
             * 순서가 틀리면 HW가 아직 이전/다른 packet을 내보낸 상황일 수 있으므로 retry합니다.
             */
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
            return RD_TR_OK;
        }
    }

    return RD_TR_ERROR_PACKET_SEQUENCE;
}

/*
 * KO_NOTE:
 * t07_result_read()로 읽은 raw segment들을 RdTrResultEntry로 풀고,
 * read_en이 켜진 bit들만 모아서 valid_rx compact sample을 만듭니다.
 */
int rd_tr_read_training_results(u16 result_len,
                              u8 pc,
                              RdTrResultEntry *entries,
                              RdTrValidRxEntry *valid_rx,
                              size_t valid_rx_capacity,
                              size_t *valid_rx_count)
{
    u8 working_rx[RX_DQ_COUNT];
    size_t produced = 0U;
    unsigned int bits_in_run = 0U;
    u16 result_index;

    if ((result_len > 0U && entries == NULL) || valid_rx_count == NULL) {
        return RD_TR_ERROR_INVALID_ARGUMENT;
    }
    if (pc != RD_TR_PC0 && pc != RD_TR_PC1) {
        return RD_TR_ERROR_INVALID_ARGUMENT;
    }

    memset(working_rx, 0, sizeof(working_rx));
    *valid_rx_count = 0U;

    /*
     * result_index is the BRAM address. For each result, read one 20-segment
     * packet, unpack it into RdTrResultEntry, then collect enabled RX bits.
     */
    for (result_index = 0U; result_index < result_len; ++result_index) {
        /*
         * KO_NOTE:
         * raw_segments는 HW에서 온 원본 20개 32-bit word입니다.
         * bytes는 그 word를 little-endian table byte 순서로 펼친 임시 buffer입니다.
         */
        u32 raw_segments[RESULT_SEGMENT_COUNT];
        u8 bytes[RESULT_SEGMENT_COUNT * 4U];
        u8 pkt_cnt = 0U;
        u8 seg_cnt = 0U;
        u8 read_en;
        size_t i;
        unsigned int bit_step;
        int status;

        status = t07_result_read(MODE_READ,
                                RESULT_FRAME_NUM,
                                result_index,
                                &pkt_cnt,
                                &seg_cnt,
                                raw_segments);
        if (status != RD_TR_OK) {
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
        for (i = 0U; i < RESULT_SEGMENT_COUNT; ++i) {
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

        read_en = pc == RD_TR_PC0 ?
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

                /*
                 * KO_NOTE:
                 * bits_in_run==0은 새 compact sample을 시작한다는 뜻입니다.
                 * 이전 sample 찌꺼기가 남지 않도록 64개 DQ working buffer를 모두 0으로 초기화합니다.
                 */
                if (bits_in_run == 0U) {
                    memset(working_rx, 0, sizeof(working_rx));
                }

                for (dq = 0U; dq < RX_DQ_COUNT; ++dq) {
                    /*
                     * KO_NOTE:
                     * result table의 rx_dq[dq] 안에는 bit0..bit7이 시간 순서로 들어 있습니다.
                     * read_en이 켜진 bit만 뽑아 compact sample의 bit0, bit1, ... 위치에 다시 쌓습니다.
                     */
                    u8 bit_value = (u8)((entries[result_index].rx_dq[dq] >> bit) & 0x01U);
                    working_rx[dq] = (u8)(working_rx[dq] | (u8)(bit_value << bits_in_run));
                }

                ++bits_in_run;
                if (bits_in_run == 8U) {
                    /* Eight continuous read_en bits become one compact RX sample. */
                    if (valid_rx != NULL) {
                        if (produced >= valid_rx_capacity) {
                            *valid_rx_count = produced;
                            return RD_TR_ERROR_BUFFER_TOO_SMALL;
                        }
                        memcpy(valid_rx[produced].rx_dq,
                               working_rx,
                               sizeof(valid_rx[produced].rx_dq));
                    }
                    ++produced;
                    bits_in_run = 0U;
                }
            } else if (bits_in_run != 0U) {
                /*
                 * KO_NOTE:
                 * valid bit 8개가 연속으로 모여야 정상 sample입니다.
                 * 중간에 read_en이 꺼지면 sample이 반쪽짜리가 되므로 에러 처리합니다.
                 */
                *valid_rx_count = produced;
                return RD_TR_ERROR_READ_ENABLE;
            }
        }
    }

    if (bits_in_run != 0U) {
        *valid_rx_count = produced;
        return RD_TR_ERROR_READ_ENABLE;
    }

    *valid_rx_count = produced;
    return RD_TR_OK;
}

static int rd_tr_record_pass_zone(RdTrPassData *pass_data,
                                  size_t pc_index,
                                  size_t local_dq,
                                  u8 pc,
                                  u8 dq,
                                  u32 point_start,
                                  u32 point_end)
{
    const u32 points_per_mck = (u32)BIT_DLY_COUNT * (u32)PE_DLY_COUNT;
    const u32 points_per_pc = (u32)MCK_DLY_COUNT * points_per_mck;
    RdTrPassZone zone;
    RdTrPassCenter *best;
    u32 center_point;
    u32 center_rem;
    int stored = 0;

    if (pass_data == NULL ||
        pc_index >= PC_COUNT ||
        local_dq >= DQ_PER_PC ||
        point_end < point_start ||
        point_end >= points_per_pc) {
        return RD_TR_ERROR_INVALID_ARGUMENT;
    }

    zone.pc = pc;
    zone.dq = dq;
    zone.point_start = point_start;
    zone.point_end = point_end;
    zone.point_count = point_end - point_start + 1U;

    if (pass_data->zone_count[pc_index][local_dq] < MAX_PASS_ZONES_PER_DQ) {
        u8 zone_index = pass_data->zone_count[pc_index][local_dq];

        pass_data->zones[pc_index][local_dq][zone_index] = zone;
        ++pass_data->zone_count[pc_index][local_dq];
        stored = 1;
    }

    if (g_rd_tr_pass_zone_log != NULL) {
        g_rd_tr_pass_zone_log(&zone, stored, g_rd_tr_pass_zone_log_context);
    }

    best = &pass_data->best_center[pc_index][local_dq];
    if (best->valid == 0U || zone.point_count > best->point_count) {
        center_point = zone.point_start + (zone.point_count / 2U);
        center_rem = center_point % points_per_mck;

        best->valid = 1U;
        best->pc = zone.pc;
        best->dq = zone.dq;
        best->mck_dly = (u8)(center_point / points_per_mck);
        best->bit_dly = (u8)(center_rem / (u32)PE_DLY_COUNT);
        best->pe_dly = (u16)(center_rem % (u32)PE_DLY_COUNT);
        best->point_start = zone.point_start;
        best->point_end = zone.point_end;
        best->point_count = zone.point_count;
    }

    return RD_TR_OK;
}
int rd_tr_check_dq_lfsr(const RdTrValidRxEntry *valid_rx,
                      size_t valid_rx_count,
                      u8 dq,
                      size_t *failed_sample)
{
    /*
     * KO_NOTE:
     * 한 DQ만 검사합니다.
     * valid_rx sample index가 0,1,2,...로 증가할 때 expected_read_lfsr는 6-row 주기로 반복됩니다.
     */
    size_t sample;

    if (valid_rx_count > 0U && valid_rx == NULL) {
        return RD_TR_ERROR_INVALID_ARGUMENT;
    }
    if (dq >= RX_DQ_COUNT) {
        return RD_TR_ERROR_INVALID_ARGUMENT;
    }

    for (sample = 0U; sample < valid_rx_count; ++sample) {
        size_t lfsr_index = sample % READ_LFSR_LENGTH;
        u8 expected = expected_read_lfsr[lfsr_index][dq % READ_LFSR_DQ_GROUP_SIZE];

        /*
         * KO_NOTE:
         * expected table은 dq0..dq7만 갖고 있고, dq8..15, dq16..23도 같은 8-DQ pattern을 반복합니다.
         * 그래서 dq % 8로 expected column을 고릅니다.
         */
        if (valid_rx[sample].rx_dq[dq] != expected) {
            if (failed_sample != NULL) {
                *failed_sample = sample;
            }
            return RD_TR_ERROR_LFSR_MISMATCH;
        }
    }

    if (failed_sample != NULL) {
        *failed_sample = valid_rx_count;
    }
    return RD_TR_OK;
}

int rd_tr_check_valid_rx_lfsr(const RdTrValidRxEntry *valid_rx,
                            size_t valid_rx_count,
                            size_t *failed_sample,
                            size_t *failed_dq)
{
    /*
     * KO_NOTE:
     * 전체 64개 DQ를 순서대로 검사하다가 첫 mismatch 위치를 failed_sample/failed_dq로 알려줍니다.
     * sweep에서는 DQ별 pass/fail이 필요하므로 더 자주 쓰는 것은 rd_tr_check_dq_lfsr()입니다.
     */
    size_t dq;

    if (valid_rx_count > 0U && valid_rx == NULL) {
        return RD_TR_ERROR_INVALID_ARGUMENT;
    }

    for (dq = 0U; dq < RX_DQ_COUNT; ++dq) {
        size_t failed_at_sample = valid_rx_count;
        int status = rd_tr_check_dq_lfsr(valid_rx,
                                       valid_rx_count,
                                       (u8)dq,
                                       &failed_at_sample);

        if (status != RD_TR_OK) {
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
        *failed_dq = RX_DQ_COUNT;
    }
    return RD_TR_OK;
}

int rd_tr_get_pass(const RdTrPassData *pass_data,
                 u8 pc,
                 u8 dq,
                 u8 mck_dly,
                 u8 bit_dly,
                 u16 pe_dly)
{
    /*
     * KO_NOTE:
     * 저장된 pass zone들만 보고 특정 delay point가 pass인지 조회합니다.
     * overflow로 저장되지 않은 zone은 여기서 보이지 않을 수 있습니다.
     * center가 맞는 것과 이 조회 함수가 모든 zone을 아는 것은 별개의 문제입니다.
     */
    size_t pc_index;
    size_t local_dq;
    size_t zone_index;
    u32 point;

    if (pass_data == NULL) {
        return 0;
    }
    if (mck_dly >= MCK_DLY_COUNT ||
        bit_dly >= BIT_DLY_COUNT ||
        pe_dly >= PE_DLY_COUNT) {
        return 0;
    }
    if (pc == RD_TR_PC0 && dq < DQ_PER_PC) {
        pc_index = 0U;
        local_dq = (size_t)dq;
    } else if (pc == RD_TR_PC1 && dq >= DQ_PER_PC && dq < RX_DQ_COUNT) {
        pc_index = 1U;
        local_dq = (size_t)(dq - DQ_PER_PC);
    } else {
        return 0;
    }

    point = (((u32)mck_dly * (u32)BIT_DLY_COUNT) + (u32)bit_dly) *
            (u32)PE_DLY_COUNT + (u32)pe_dly;

    for (zone_index = 0U;
         zone_index < pass_data->zone_count[pc_index][local_dq];
         ++zone_index) {
        const RdTrPassZone *zone = &pass_data->zones[pc_index][local_dq][zone_index];

        if (point >= zone->point_start && point <= zone->point_end) {
            return 1;
        }
    }

    return 0;
}

size_t rd_tr_collect_pass_zones(const RdTrPassData *pass_data,
                              u8 pc,
                              u8 dq,
                              RdTrPassZone *zones,
                              size_t zone_capacity)
{
    /*
     * KO_NOTE:
     * 디버그 출력용으로 저장된 zone 목록을 복사합니다.
     * 반환값은 실제 저장된 zone 개수이고, zones buffer가 작으면 앞쪽 일부만 복사됩니다.
     */
    size_t pc_index;
    size_t local_dq;
    size_t stored_count;
    size_t i;

    if (pass_data == NULL) {
        return 0U;
    }
    if (pc == RD_TR_PC0 && dq < DQ_PER_PC) {
        pc_index = 0U;
        local_dq = (size_t)dq;
    } else if (pc == RD_TR_PC1 && dq >= DQ_PER_PC && dq < RX_DQ_COUNT) {
        pc_index = 1U;
        local_dq = (size_t)(dq - DQ_PER_PC);
    } else {
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

int rd_tr_run_read_training_sweep(u16 result_len,
                                RdTrResultEntry *entries,
                                RdTrValidRxEntry *valid_rx,
                                size_t valid_rx_capacity,
                                RdTrPassData *pass_data,
                                RdTrPassCenter centers[PC_COUNT][DQ_PER_PC])
{
    /*
     * KO_NOTE:
     * T07 read training의 최상위 함수입니다.
     * 이 함수 하나를 FW training code에 붙이면 전체 sweep, result read, LFSR 판정, center 산출이 이어집니다.
     */
    size_t pc_index;

    if (result_len == 0U ||
        entries == NULL ||
        valid_rx == NULL ||
        pass_data == NULL ||
        valid_rx_capacity < READ_LFSR_LENGTH) {
        return RD_TR_ERROR_INVALID_ARGUMENT;
    }
    if (g_rd_tr_apply_delay == NULL) {
        return RD_TR_ERROR_IO_NOT_CONFIGURED;
    }

    memset(pass_data, 0, sizeof(*pass_data));

    /*
     * Full training sweep order:
     *   cur_pc    : 0..1
     *   i_mck_dly : 0..15
     *   i_bit_dly : 0..7
     *   i_pe_dly  : 0..(PE_DLY_COUNT - 1)
     *
     * For each delay point, set every DQ in the current PC to that delay,
     * read result data once, then judge each DQ in that PC independently.
     * Only pass zones are stored, so pass_data memory does not grow with
     * PE_DLY_COUNT.
     */
    for (pc_index = 0U; pc_index < PC_COUNT; ++pc_index) {
        u8 pc = pc_index == 0U ? RD_TR_PC0 : RD_TR_PC1;
        u8 dq_begin = pc == RD_TR_PC0 ? 0U : DQ_PER_PC;
        u8 dq_end = (u8)(dq_begin + DQ_PER_PC);
        u8 active[DQ_PER_PC];
        u32 point_start[DQ_PER_PC];
        u32 point_index = 0U;
        u8 mck_dly;

        memset(active, 0, sizeof(active));
        memset(point_start, 0, sizeof(point_start));

        for (mck_dly = 0U; mck_dly < MCK_DLY_COUNT; ++mck_dly) {
            u8 bit_dly;

            for (bit_dly = 0U; bit_dly < BIT_DLY_COUNT; ++bit_dly) {
                u16 pe_dly;

                for (pe_dly = 0U; pe_dly < PE_DLY_COUNT; ++pe_dly) {
                    size_t valid_rx_count = 0U;
                    u8 dq;
                    int status;

                    for (dq = dq_begin; dq < dq_end; ++dq) {
                        status = g_rd_tr_apply_delay(pc, dq, mck_dly, bit_dly, pe_dly);
                        if (status != RD_TR_OK) {
                            return status < 0 ? status : RD_TR_ERROR_DELAY_APPLY;
                        }
                    }

                    status = rd_tr_read_training_results(result_len,
                                                         pc,
                                                         entries,
                                                         valid_rx,
                                                         valid_rx_capacity,
                                                         &valid_rx_count);
                    if (status != RD_TR_OK) {
                        return status;
                    }

                    for (dq = dq_begin; dq < dq_end; ++dq) {
                        size_t local_dq = (size_t)(dq - dq_begin);
                        int passed = 0;

                        if (valid_rx_count >= READ_LFSR_LENGTH &&
                            rd_tr_check_dq_lfsr(valid_rx, valid_rx_count, dq, NULL) == RD_TR_OK) {
                            passed = 1;
                        }

                        if (passed != 0) {
                            if (active[local_dq] == 0U) {
                                active[local_dq] = 1U;
                                point_start[local_dq] = point_index;
                            }
                        } else if (active[local_dq] != 0U) {
                            status = rd_tr_record_pass_zone(pass_data,
                                                            pc_index,
                                                            local_dq,
                                                            pc,
                                                            dq,
                                                            point_start[local_dq],
                                                            point_index - 1U);
                            if (status != RD_TR_OK) {
                                return status;
                            }
                            active[local_dq] = 0U;
                        }
                    }

                    ++point_index;
                }
            }
        }

        if (point_index > 0U) {
            u8 dq;

            for (dq = dq_begin; dq < dq_end; ++dq) {
                size_t local_dq = (size_t)(dq - dq_begin);

                if (active[local_dq] != 0U) {
                    int status = rd_tr_record_pass_zone(pass_data,
                                                        pc_index,
                                                        local_dq,
                                                        pc,
                                                        dq,
                                                        point_start[local_dq],
                                                        point_index - 1U);
                    if (status != RD_TR_OK) {
                        return status;
                    }
                    active[local_dq] = 0U;
                }
            }
        }
    }
    if (centers != NULL) {
        /*
         * KO_NOTE:
         * pass_data 안에는 항상 최신 best_center가 들어 있습니다.
         * 호출자가 centers buffer를 주면 결과 확인을 편하게 하도록 복사해줍니다.
         */
        memcpy(centers, pass_data->best_center, sizeof(pass_data->best_center));
    }
    return RD_TR_OK;
}
