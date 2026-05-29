#include "read_training.h"

#include <ctype.h>
#include <string.h>

/*
 * KO_NOTE:
 * 이 파일은 T07 read training을 PC test와 FW build 양쪽에서 같이 쓰기 위한 구현입니다.
 * 큰 흐름은 아래 순서입니다.
 *
 * 1. t07_run_read_training_sweep()
 *    pc -> mck_dly -> bit_dly -> pe_dly 순서로 delay를 sweep합니다.
 *
 * 2. 각 delay point마다 g_t07_apply_delay()로 DQ delay를 실제 HW/FW 쪽에 적용합니다.
 *
 * 3. t07_read_training_results()가 HW result table을 읽고, read_en으로 표시된 bit만 모아
 *    T07ValidRxEntry 형태의 compact sample을 만듭니다.
 *
 * 4. t07_check_dq_lfsr()가 DQ별 compact sample이 expected_read_lfsr와 맞는지 검사합니다.
 *
 * 5. pass가 연속되는 pe_dly 구간을 t07_add_pass_zone()에서 zone으로 닫고 저장합니다.
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

#ifndef TRAINING_ERROR_PACKET_SEQUENCE
#define TRAINING_ERROR_PACKET_SEQUENCE (-8)
#endif

#ifndef TRAINING_ERROR_LFSR_MISMATCH
#define TRAINING_ERROR_LFSR_MISMATCH (-9)
#endif

#ifndef TRAINING_ERROR_DELAY_APPLY
#define TRAINING_ERROR_DELAY_APPLY (-10)
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
 *
 * KO_NOTE:
 * PC 테스트에서는 실제 Xil_Out64/Xil_In64가 없으므로 함수 포인터를 NULL로 둡니다.
 * 테스트 코드가 t07_set_io(mock_out64, mock_in64)를 호출해서 가짜 IO를 연결합니다.
 */
static T07Out64Fn g_t07_out64 = NULL;
static T07In64Fn g_t07_in64 = NULL;
#endif

/*
 * KO_NOTE:
 * 아래 세 포인터는 FW 쪽에서 연결해주는 "외부 의존성"입니다.
 * - g_t07_out64 / g_t07_in64: result table command/response register 접근
 * - g_t07_apply_delay: 현재 sweep point의 delay 값을 실제 PHY/DQ에 적용
 * - g_t07_pass_zone_log: zone 발견 이벤트를 UART/file/buffer 등으로 내보내는 선택 기능
 */
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
const u8 expected_read_lfsr[READ_LFSR_LENGTH][READ_LFSR_DQ_GROUP_SIZE] = {
    {0x21U, 0x28U, 0x2FU, 0x36U, 0x3DU, 0x44U, 0x4BU, 0x52U},
    {0x52U, 0x59U, 0x60U, 0x67U, 0x6EU, 0x75U, 0x7CU, 0x83U},
    {0x83U, 0x8AU, 0x91U, 0x98U, 0x9FU, 0xA6U, 0xADU, 0xB4U},
    {0xB4U, 0xBBU, 0xC2U, 0xC9U, 0xD0U, 0xD7U, 0xDEU, 0xE5U},
    {0xE5U, 0xECU, 0xF3U, 0xFAU, 0x01U, 0x08U, 0x0FU, 0x16U},
    {0x16U, 0x1DU, 0x24U, 0x2BU, 0x32U, 0x39U, 0x40U, 0x47U}
};

/*
 * KO_NOTE:
 * 여기부터 rt_* 함수들은 read_training 예제용 작은 공통 함수입니다.
 * T07 training 핵심과는 직접 관련이 없고, 기본 C library/test 구조를 보여주는 용도입니다.
 */
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
#if defined(USE_NATIVE_XIL_IO)
    /*
     * KO_NOTE:
     * Board build에서는 out64/in64를 NULL로 넘기면 native Xil_Out64/Xil_In64 wrapper를 씁니다.
     * 테스트나 특수 FW에서 다른 IO layer를 쓰고 싶으면 NULL이 아닌 함수 포인터를 넘기면 됩니다.
     */
    g_t07_out64 = out64 != NULL ? out64 : t07_native_out64;
    g_t07_in64 = in64 != NULL ? in64 : t07_native_in64;
#else
    /*
     * KO_NOTE:
     * PC build에서는 native IO가 없으므로 NULL이면 "IO 미설정" 상태가 됩니다.
     * 이 상태에서 t07_rsult_read()를 부르면 TRAINING_ERROR_IO_NOT_CONFIGURED가 반환됩니다.
     */
    g_t07_out64 = out64;
    g_t07_in64 = in64;
#endif
}

void t07_set_delay_apply(T07ApplyDelayFn apply_delay)
{
    /*
     * KO_NOTE:
     * sweep loop는 delay 값을 계산만 하고, 실제 register write 방법은 모릅니다.
     * FW 쪽에서 이 콜백에 "pc/dq/mck/bit/pe를 HW에 적용하는 함수"를 연결해야 합니다.
     */
    g_t07_apply_delay = apply_delay;
}

void t07_set_pass_zone_log(T07PassZoneLogFn log_fn, void *user_context)
{
    /*
     * KO_NOTE:
     * pass zone이 발견될 때마다 로그를 남기고 싶으면 log_fn을 연결합니다.
     * user_context는 FILE*, UART handle, ring buffer 포인터 등 호출자가 원하는 값을 넘기면 됩니다.
     */
    g_t07_pass_zone_log = log_fn;
    g_t07_pass_zone_log_context = user_context;
}

/*
 * KO_NOTE:
 * HW result table에서 20 segment짜리 packet 하나를 읽는 가장 낮은 단계 함수입니다.
 * 함수명에 rsult 오타가 있지만, 이미 API로 쓰고 있어서 현재는 그대로 둔 상태입니다.
 */
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
        return TRAINING_ERROR_INVALID_ARGUMENT;
    }
    if (mode > 0x0FU || frame_num > 0x0FU) {
        return TRAINING_ERROR_INVALID_ARGUMENT;
    }
    if (g_t07_out64 == NULL || g_t07_in64 == NULL) {
        return TRAINING_ERROR_IO_NOT_CONFIGURED;
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

        g_t07_out64(RESULT_CMD_ADDR, cmd);

        for (i = 0U; i < RESULT_SEGMENT_COUNT; ++i) {
            /*
             * KO_NOTE:
             * rsp_raw 상위 byte에는 command/ack/pkt/segment 정보가 있고,
             * 하위 32-bit에는 실제 result table segment 하나가 들어 있습니다.
             */
            u64 rsp_raw = g_t07_in64(RESULT_RSP_ADDR);
            u8 rsp_cmd = (u8)((rsp_raw >> 56) & 0xFFU);
            u8 ack = (u8)((rsp_raw >> 48) & 0xFFU);
            u8 pkt_cnt = (u8)((rsp_raw >> 40) & 0x0FU);
            u8 seg_cnt = (u8)((rsp_raw >> 32) & 0xFFU);
            u8 expected_pkt_cnt = i == (RESULT_SEGMENT_COUNT - 1U) ? 1U : 0U;
            u8 expected_seg_cnt = i == (RESULT_SEGMENT_COUNT - 1U) ? 0U : (u8)(i + 1U);

            if (rsp_cmd != CMD_CODE) {
                return TRAINING_ERROR_ACK;
            }
            if (ack == ACK_TBL_ERR) {
                return TRAINING_ERROR_TABLE;
            }
            if (ack == ACK_SEG_ERR) {
                return TRAINING_ERROR_SEGMENT;
            }
            if (ack != ACK_OK) {
                return TRAINING_ERROR_ACK;
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
            return TRAINING_OK;
        }
    }

    return TRAINING_ERROR_PACKET_SEQUENCE;
}

/*
 * KO_NOTE:
 * t07_rsult_read()로 읽은 raw segment들을 T07ResultEntry로 풀고,
 * read_en이 켜진 bit들만 모아서 valid_rx compact sample을 만듭니다.
 */
int t07_read_training_results(u16 result_len,
                              u8 pc,
                              T07ResultEntry *entries,
                              T07ValidRxEntry *valid_rx,
                              size_t valid_rx_capacity,
                              size_t *valid_rx_count)
{
    u8 working_rx[RX_DQ_COUNT];
    size_t produced = 0U;
    unsigned int bits_in_run = 0U;
    u16 result_index;

    if ((result_len > 0U && entries == NULL) || valid_rx_count == NULL) {
        return TRAINING_ERROR_INVALID_ARGUMENT;
    }
    if (pc != PC0 && pc != PC1) {
        return TRAINING_ERROR_INVALID_ARGUMENT;
    }

    memset(working_rx, 0, sizeof(working_rx));
    *valid_rx_count = 0U;

    /*
     * result_index is the BRAM address. For each result, read one 20-segment
     * packet, unpack it into T07ResultEntry, then collect enabled RX bits.
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

        status = t07_rsult_read(MODE_READ,
                                RESULT_FRAME_NUM,
                                result_index,
                                &pkt_cnt,
                                &seg_cnt,
                                raw_segments);
        if (status != TRAINING_OK) {
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

        read_en = pc == PC0 ?
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
                            return TRAINING_ERROR_BUFFER_TOO_SMALL;
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
                return TRAINING_ERROR_READ_ENABLE;
            }
        }
    }

    if (bits_in_run != 0U) {
        *valid_rx_count = produced;
        return TRAINING_ERROR_READ_ENABLE;
    }

    *valid_rx_count = produced;
    return TRAINING_OK;
}

static int t07_dq_to_local(u8 pc, u8 dq, size_t *pc_index, size_t *local_dq)
{
    /*
     * KO_NOTE:
     * 외부에서는 PC0 dq0..31, PC1 dq32..63처럼 global DQ 번호를 씁니다.
     * T07PassData 내부 배열은 PC별 local_dq 0..31로 저장하므로 여기서 변환합니다.
     */
    if (pc == PC0) {
        if (dq >= DQ_PER_PC) {
            return TRAINING_ERROR_INVALID_ARGUMENT;
        }
        *pc_index = 0U;
        *local_dq = (size_t)dq;
        return TRAINING_OK;
    }
    if (pc == PC1) {
        if (dq < DQ_PER_PC || dq >= RX_DQ_COUNT) {
            return TRAINING_ERROR_INVALID_ARGUMENT;
        }
        *pc_index = 1U;
        *local_dq = (size_t)(dq - DQ_PER_PC);
        return TRAINING_OK;
    }

    return TRAINING_ERROR_INVALID_ARGUMENT;
}

static int t07_add_pass_zone(T07PassData *pass_data,
                             u8 pc,
                             u8 dq,
                             u8 mck_dly,
                             u8 bit_dly,
                             u16 pe_start,
                             u16 pe_end)
{
    /*
     * KO_NOTE:
     * pass가 연속된 pe_dly 구간을 하나의 zone으로 닫는 함수입니다.
     * 중요한 점은 저장 공간(zones)은 최대 16개지만 center 계산은 저장 성공 여부와 독립이라는 것입니다.
     */
    size_t pc_index;
    size_t local_dq;
    T07PassZone zone;
    T07PassCenter *best;
    int stored = 0;
    int status;

    if (pass_data == NULL) {
        return TRAINING_ERROR_INVALID_ARGUMENT;
    }
    if (mck_dly >= MCK_DLY_COUNT ||
        bit_dly >= BIT_DLY_COUNT ||
        pe_start >= PE_DLY_COUNT ||
        pe_end >= PE_DLY_COUNT ||
        pe_end < pe_start) {
        return TRAINING_ERROR_INVALID_ARGUMENT;
    }

    status = t07_dq_to_local(pc, dq, &pc_index, &local_dq);
    if (status != TRAINING_OK) {
        return status;
    }

    zone.pc = (u8)pc;
    zone.dq = dq;
    zone.mck_dly = mck_dly;
    zone.bit_dly = bit_dly;
    zone.pe_start = pe_start;
    zone.pe_end = pe_end;
    zone.pe_count = (u16)(pe_end - pe_start + 1U);

    /*
     * KO_NOTE:
     * zones[][][]는 디버그/조회용 보관소입니다.
     * 꽉 차면 더 이상 저장하지 않지만, 아래 log callback과 best_center 갱신은 계속 진행합니다.
     */
    if (pass_data->zone_count[pc_index][local_dq] < MAX_PASS_ZONES_PER_DQ) {
        u8 zone_index = pass_data->zone_count[pc_index][local_dq];

        pass_data->zones[pc_index][local_dq][zone_index] = zone;
        ++pass_data->zone_count[pc_index][local_dq];
        stored = 1;
    }

    /*
     * KO_NOTE:
     * stored=0인 callback은 "실제 zone은 발견됐지만 T07PassData에는 저장되지 못했다"는 뜻입니다.
     * 이 덕분에 max 16개 제한 때문에 사라지는 zone도 UART/file 로그로 확인할 수 있습니다.
     */
    if (g_t07_pass_zone_log != NULL) {
        g_t07_pass_zone_log(&zone, stored, g_t07_pass_zone_log_context);
    }

    best = &pass_data->best_center[pc_index][local_dq];
    /*
     * KO_NOTE:
     * center는 가장 긴 pass zone의 가운데 pe_dly를 선택합니다.
     * 이 코드는 zone 저장 if문 밖에 있으므로 overflow zone도 center 후보가 됩니다.
     */
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
    return TRAINING_OK;
}

int t07_check_dq_lfsr(const T07ValidRxEntry *valid_rx,
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
        return TRAINING_ERROR_INVALID_ARGUMENT;
    }
    if (dq >= RX_DQ_COUNT) {
        return TRAINING_ERROR_INVALID_ARGUMENT;
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
            return TRAINING_ERROR_LFSR_MISMATCH;
        }
    }

    if (failed_sample != NULL) {
        *failed_sample = valid_rx_count;
    }
    return TRAINING_OK;
}

int t07_check_valid_rx_lfsr(const T07ValidRxEntry *valid_rx,
                            size_t valid_rx_count,
                            size_t *failed_sample,
                            size_t *failed_dq)
{
    /*
     * KO_NOTE:
     * 전체 64개 DQ를 순서대로 검사하다가 첫 mismatch 위치를 failed_sample/failed_dq로 알려줍니다.
     * sweep에서는 DQ별 pass/fail이 필요하므로 더 자주 쓰는 것은 t07_check_dq_lfsr()입니다.
     */
    size_t dq;

    if (valid_rx_count > 0U && valid_rx == NULL) {
        return TRAINING_ERROR_INVALID_ARGUMENT;
    }

    for (dq = 0U; dq < RX_DQ_COUNT; ++dq) {
        size_t failed_at_sample = valid_rx_count;
        int status = t07_check_dq_lfsr(valid_rx,
                                       valid_rx_count,
                                       (u8)dq,
                                       &failed_at_sample);

        if (status != TRAINING_OK) {
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
    return TRAINING_OK;
}

int t07_get_pass(const T07PassData *pass_data,
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

    if (pass_data == NULL) {
        return 0;
    }
    if (mck_dly >= MCK_DLY_COUNT ||
        bit_dly >= BIT_DLY_COUNT ||
        pe_dly >= PE_DLY_COUNT) {
        return 0;
    }
    if (t07_dq_to_local(pc, dq, &pc_index, &local_dq) != TRAINING_OK) {
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
                              u8 pc,
                              u8 dq,
                              T07PassZone *zones,
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
    if (t07_dq_to_local(pc, dq, &pc_index, &local_dq) != TRAINING_OK) {
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
                                T07PassCenter centers[PC_COUNT][DQ_PER_PC])
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
        return TRAINING_ERROR_INVALID_ARGUMENT;
    }
    if (g_t07_apply_delay == NULL) {
        return TRAINING_ERROR_IO_NOT_CONFIGURED;
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
        /*
         * KO_NOTE:
         * PC0은 global dq0..31, PC1은 global dq32..63을 담당합니다.
         * dq_begin/dq_end를 잡아두면 아래 loop는 두 PC를 같은 코드로 처리할 수 있습니다.
         */
        u8 pc = pc_index == 0U ? PC0 : PC1;
        u8 dq_begin = pc == PC0 ? 0U : DQ_PER_PC;
        u8 dq_end = (u8)(dq_begin + DQ_PER_PC);
        u8 mck_dly;

        for (mck_dly = 0U; mck_dly < MCK_DLY_COUNT; ++mck_dly) {
            u8 bit_dly;

            for (bit_dly = 0U; bit_dly < BIT_DLY_COUNT; ++bit_dly) {
                /*
                 * KO_NOTE:
                 * active[local_dq]는 현재 pe_dly에서 pass run이 열려 있는지 표시합니다.
                 * pe_start[local_dq]는 그 run이 시작된 pe_dly입니다.
                 * 예: pe 3부터 pass가 시작되면 active=1, pe_start=3이 되고,
                 *     나중에 fail을 만나면 pe_start..(pe_dly-1)을 zone으로 저장합니다.
                 */
                u8 active[DQ_PER_PC];
                u16 pe_start[DQ_PER_PC];
                u16 pe_dly;
                u8 dq;

                memset(active, 0, sizeof(active));
                memset(pe_start, 0, sizeof(pe_start));

                for (pe_dly = 0U; pe_dly < PE_DLY_COUNT; ++pe_dly) {
                    size_t valid_rx_count = 0U;
                    int status;

                    /*
                     * KO_NOTE:
                     * 현재 PC의 32개 DQ에 같은 mck/bit/pe delay point를 적용합니다.
                     * 실제 register write는 g_t07_apply_delay 콜백 안에서 처리됩니다.
                     */
                    for (dq = dq_begin; dq < dq_end; ++dq) {
                        status = g_t07_apply_delay(pc, dq, mck_dly, bit_dly, pe_dly);
                        if (status != TRAINING_OK) {
                            return status < 0 ? status : TRAINING_ERROR_DELAY_APPLY;
                        }
                    }

                    status = t07_read_training_results(result_len,
                                                       pc,
                                                       entries,
                                                       valid_rx,
                                                       valid_rx_capacity,
                                                       &valid_rx_count);
                    if (status != TRAINING_OK) {
                        return status;
                    }

                    /*
                     * KO_NOTE:
                     * 한 delay point에서 result data는 한 번만 읽고,
                     * 그 compact valid_rx를 32개 DQ 각각에 대해 독립적으로 LFSR 검사합니다.
                     */
                    for (dq = dq_begin; dq < dq_end; ++dq) {
                        size_t local_dq = (size_t)(dq - dq_begin);
                        int passed = 0;

                        if (valid_rx_count >= READ_LFSR_LENGTH &&
                            t07_check_dq_lfsr(valid_rx, valid_rx_count, dq, NULL) == TRAINING_OK) {
                            passed = 1;
                        }

                        if (passed != 0) {
                            if (active[local_dq] == 0U) {
                                /*
                                 * KO_NOTE:
                                 * fail 상태였다가 pass를 처음 만난 순간입니다.
                                 * 여기서 pass zone 후보가 열립니다.
                                 */
                                active[local_dq] = 1U;
                                pe_start[local_dq] = pe_dly;
                            }
                        } else if (active[local_dq] != 0U) {
                            /*
                             * KO_NOTE:
                             * pass run이 열려 있었는데 이번 pe_dly에서 fail이 나왔습니다.
                             * 따라서 직전 pe_dly까지가 pass zone입니다.
                             */
                            status = t07_add_pass_zone(pass_data,
                                                       pc,
                                                       dq,
                                                       mck_dly,
                                                       bit_dly,
                                                       pe_start[local_dq],
                                                       (u16)(pe_dly - 1U));
                            if (status != TRAINING_OK) {
                                return status;
                            }
                            active[local_dq] = 0U;
                        }
                    }
                }

                if (PE_DLY_COUNT > 0U) {
                    /*
                     * KO_NOTE:
                     * pe_dly loop가 끝날 때까지 계속 pass였던 run은 fail을 만나지 못했으므로
                     * 여기서 마지막 pe_dly(PE_DLY_COUNT-1)까지의 zone으로 닫아줍니다.
                     */
                    for (dq = dq_begin; dq < dq_end; ++dq) {
                        size_t local_dq = (size_t)(dq - dq_begin);

                        if (active[local_dq] != 0U) {
                            int status = t07_add_pass_zone(pass_data,
                                                           pc,
                                                           dq,
                                                           mck_dly,
                                                           bit_dly,
                                                           pe_start[local_dq],
                                                           (u16)(PE_DLY_COUNT - 1U));

                            if (status != TRAINING_OK) {
                                return status;
                            }
                        }
                    }
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
    return TRAINING_OK;
}
