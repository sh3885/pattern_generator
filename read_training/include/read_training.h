#ifndef READ_TRAINING_H
#define READ_TRAINING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#ifndef RESULT_SEGMENT_COUNT
#define RESULT_SEGMENT_COUNT 20U
#endif
#ifndef RESULT_DATA_BYTES
#define RESULT_DATA_BYTES 77U
#endif
#ifndef RX_DQ_COUNT
#define RX_DQ_COUNT 64U
#endif
#ifndef PC_COUNT
#define PC_COUNT 2U
#endif
#ifndef DQ_PER_PC
#define DQ_PER_PC 32U
#endif
#ifndef MCK_DLY_COUNT
#define MCK_DLY_COUNT 16U
#endif
#ifndef BIT_DLY_COUNT
#define BIT_DLY_COUNT 8U
#endif
#ifndef PE_DLY_COUNT
#define PE_DLY_COUNT 64U
#endif
#ifndef MAX_PASS_ZONES_PER_DQ
#define MAX_PASS_ZONES_PER_DQ 16U
#endif
#ifndef READ_LFSR_LENGTH
#define READ_LFSR_LENGTH 6U
#endif
#ifndef READ_LFSR_DQ_GROUP_SIZE
#define READ_LFSR_DQ_GROUP_SIZE 8U
#endif
#ifndef MODE_READ
#define MODE_READ 0x0CU
#endif
#ifndef RESULT_FRAME_NUM
#define RESULT_FRAME_NUM 0U
#endif
#ifndef RESULT_READ_RETRY_COUNT
#define RESULT_READ_RETRY_COUNT 3U
#endif

#if PE_DLY_COUNT > 65535U
#error "PE_DLY_COUNT must fit in u16"
#endif

#define RD_TR_OK 0
#define RD_TR_ERROR_INVALID_ARGUMENT (-1)
#define RD_TR_ERROR_BUFFER_TOO_SMALL (-2)
#define RD_TR_ERROR_IO_NOT_CONFIGURED (-3)
#define RD_TR_ERROR_ACK (-4)
#define RD_TR_ERROR_TABLE (-5)
#define RD_TR_ERROR_SEGMENT (-6)
#define RD_TR_ERROR_READ_ENABLE (-7)
#define RD_TR_ERROR_PACKET_SEQUENCE (-8)
#define RD_TR_ERROR_LFSR_MISMATCH (-9)
#define RD_TR_ERROR_DELAY_APPLY (-10)

#define RD_TR_PC0 0U
#define RD_TR_PC1 1U

typedef struct RdTrResultEntry {
    /*
     * KO_NOTE:
     * HW에서 읽어온 T07 result table 한 줄을 C 구조체로 풀어놓은 형태입니다.
     * 실제 HW 응답은 20개의 32-bit segment로 오고, read_training.c에서 byte
     * 단위로 다시 조립한 뒤 이 구조체에 채웁니다.
     */
    u8 time_ptr;
    /* [0]=dq7_0, [1]=dq15_8, [2]=dq23_16, [3]=dq31_24 */
    u8 comp_results_pc0[4];
    /* [0]=dq7_0, [1]=dq15_8, [2]=dq23_16, [3]=dq31_24 */
    u8 comp_results_pc1[4];
    u8 read_en_pc0;
    u8 read_en_pc1;
    u8 read_tph_pc0;
    u8 read_tph_pc1;
    u8 rx_dq[RX_DQ_COUNT];
} RdTrResultEntry;

typedef struct RdTrValidRxEntry {
    /*
     * KO_NOTE:
     * read_en으로 표시된 valid bit들만 모아서 만든 "검사용 RX 샘플"입니다.
     * 8개의 valid bit가 모이면 rx_dq[0..63] 각각에 8-bit 값 하나가 생깁니다.
     */
    u8 rx_dq[RX_DQ_COUNT];
} RdTrValidRxEntry;

typedef struct RdTrPassZone {
    /*
     * One continuous pass run in the flattened sweep order.
     * point = ((mck_dly * BIT_DLY_COUNT) + bit_dly) * PE_DLY_COUNT + pe_dly.
     */
    u8 pc;
    u8 dq;
    u32 point_start;
    u32 point_end;
    u32 point_count;
} RdTrPassZone;

typedef struct RdTrPassCenter {
    /*
     * Center of the longest pass run for one PC/DQ, decoded back to mck/bit/pe.
     */
    u8 valid;
    u8 pc;
    u8 dq;
    u8 mck_dly;
    u8 bit_dly;
    u16 pe_dly;
    u32 point_start;
    u32 point_end;
    u32 point_count;
} RdTrPassCenter;

typedef struct RdTrPassData {
    /*
     * Only pass zones are stored. This keeps memory almost fixed even if
     * PE_DLY_COUNT grows from 64 to 1024.
     * best_center is still updated for every detected zone, including zones
     * that cannot be retained after MAX_PASS_ZONES_PER_DQ is reached.
     *
     * KO_NOTE:
     * zones[][][]는 디버그/조회용으로 "앞에서부터 최대 16개"만 저장합니다.
     * 하지만 best_center[][]는 zone 저장 성공 여부와 상관없이 매번 갱신됩니다.
     * 그래서 17번째 이후 zone이 저장되지 않아도, 더 긴 zone이면 center는 그 zone으로 바뀝니다.
     *
     * local_dq is 0..31.
     * pc0 local_dq 0..31 means global dq0..dq31.
     * pc1 local_dq 0..31 means global dq32..dq63.
     */
    u8 zone_count[PC_COUNT][DQ_PER_PC];
    RdTrPassZone zones[PC_COUNT][DQ_PER_PC][MAX_PASS_ZONES_PER_DQ];
    RdTrPassCenter best_center[PC_COUNT][DQ_PER_PC];
} RdTrPassData;

typedef void (*RdTrOut64Fn)(uintptr_t addr, u64 value);
typedef u64 (*RdTrIn64Fn)(uintptr_t addr);
typedef int (*RdTrApplyDelayFn)(u8 pc, u8 dq, u8 mck_dly, u8 bit_dly, u16 pe_dly);
/*
 * KO_NOTE:
 * pass zone이 발견될 때마다 호출되는 디버그용 콜백입니다.
 * stored=1이면 RdTrPassData.zones에 저장된 zone이고,
 * stored=0이면 MAX_PASS_ZONES_PER_DQ 제한 때문에 저장은 못 했지만 실제로 발견된 zone입니다.
 */
typedef void (*RdTrPassZoneLogFn)(const RdTrPassZone *zone, int stored, void *user_context);

int dbg_rd_tr_decode_point(u32 point,
                           u8 *mck_dly,
                           u8 *bit_dly,
                           u16 *pe_dly);

int dbg_rd_tr_format_pass_zone(const RdTrPassZone *zone,
                               char *out,
                               size_t out_size);

int dbg_rd_tr_format_pass_center(const RdTrPassCenter *center,
                                 char *out,
                                 size_t out_size);

void rd_tr_set_io(RdTrOut64Fn out64, RdTrIn64Fn in64);
void rd_tr_set_delay_apply(RdTrApplyDelayFn apply_delay);
void rd_tr_set_pass_zone_log(RdTrPassZoneLogFn log_fn, void *user_context);

int rd_tr_run_read_training_sweep(u16 result_len,
                                RdTrResultEntry *entries,
                                RdTrValidRxEntry *valid_rx,
                                size_t valid_rx_capacity,
                                RdTrPassData *pass_data,
                                RdTrPassCenter centers[PC_COUNT][DQ_PER_PC]);

#if defined(READ_TRAINING_EXPOSE_TEST_HELPERS)
/*
 * KO_NOTE:
 * 아래 함수들은 PC unit test와 visual log 생성용 helper입니다.
 * FW 적용 시에는 보통 rd_tr_run_read_training_sweep() 중심으로 쓰면 되고,
 * 이 helper prototype들은 READ_TRAINING_EXPOSE_TEST_HELPERS를 켰을 때만 노출됩니다.
 */
extern const u8 expected_read_lfsr[READ_LFSR_LENGTH][READ_LFSR_DQ_GROUP_SIZE];

int t07_result_read(u8 mode,
                   u8 frame_num,
                   u16 bram_addr,
                   u8 *p_pkt_cnt,
                   u8 *p_seg_cnt,
                   u32 *p_data);

int rd_tr_read_training_results(u16 result_len,
                              u8 pc,
                              RdTrResultEntry *entries,
                              RdTrValidRxEntry *valid_rx,
                              size_t valid_rx_capacity,
                              size_t *valid_rx_count);

int rd_tr_check_valid_rx_lfsr(const RdTrValidRxEntry *valid_rx,
                            size_t valid_rx_count,
                            size_t *failed_sample,
                            size_t *failed_dq);

int rd_tr_check_dq_lfsr(const RdTrValidRxEntry *valid_rx,
                      size_t valid_rx_count,
                      u8 dq,
                      size_t *failed_sample);

int rd_tr_get_pass(const RdTrPassData *pass_data,
                 u8 pc,
                 u8 dq,
                 u8 mck_dly,
                 u8 bit_dly,
                 u16 pe_dly);

size_t rd_tr_collect_pass_zones(const RdTrPassData *pass_data,
                              u8 pc,
                              u8 dq,
                              RdTrPassZone *zones,
                              size_t zone_capacity);
#endif

#ifdef __cplusplus
}
#endif

#endif
