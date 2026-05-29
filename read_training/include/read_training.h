#ifndef READ_TRAINING_H
#define READ_TRAINING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RT_VERSION_MAJOR 0
#define RT_VERSION_MINOR 1
#define RT_VERSION_PATCH 0

typedef enum RtStatus {
    RT_OK = 0,
    RT_ERROR_INVALID_ARGUMENT = -1,
    RT_ERROR_BUFFER_TOO_SMALL = -2
} RtStatus;

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#ifndef T07_RESULT_SEGMENT_COUNT
#define T07_RESULT_SEGMENT_COUNT 20U
#endif
#ifndef T07_RESULT_DATA_BYTES
#define T07_RESULT_DATA_BYTES 77U
#endif
#ifndef T07_RX_DQ_COUNT
#define T07_RX_DQ_COUNT 64U
#endif
#ifndef T07_PC_COUNT
#define T07_PC_COUNT 2U
#endif
#ifndef T07_DQ_PER_PC
#define T07_DQ_PER_PC 32U
#endif
#ifndef T07_MCK_DLY_COUNT
#define T07_MCK_DLY_COUNT 16U
#endif
#ifndef T07_BIT_DLY_COUNT
#define T07_BIT_DLY_COUNT 8U
#endif
#ifndef T07_PE_DLY_COUNT
#define T07_PE_DLY_COUNT 64U
#endif
#ifndef T07_MAX_PASS_ZONES_PER_DQ
#define T07_MAX_PASS_ZONES_PER_DQ 16U
#endif
#ifndef T07_READ_LFSR_LENGTH
#define T07_READ_LFSR_LENGTH 6U
#endif
#ifndef T07_READ_LFSR_DQ_GROUP_SIZE
#define T07_READ_LFSR_DQ_GROUP_SIZE 8U
#endif
#ifndef T07_MODE_READ
#define T07_MODE_READ 0x0CU
#endif
#ifndef T07_RESULT_FRAME_NUM
#define T07_RESULT_FRAME_NUM 0U
#endif
#ifndef T07_RESULT_READ_RETRY_COUNT
#define T07_RESULT_READ_RETRY_COUNT 3U
#endif

#if T07_PE_DLY_COUNT > 65535U
#error "T07_PE_DLY_COUNT must fit in u16"
#endif

typedef enum T07Status {
    T07_OK = 0,
    T07_ERROR_INVALID_ARGUMENT = -1,
    T07_ERROR_BUFFER_TOO_SMALL = -2,
    T07_ERROR_IO_NOT_CONFIGURED = -3,
    T07_ERROR_ACK = -4,
    T07_ERROR_TABLE = -5,
    T07_ERROR_SEGMENT = -6,
    T07_ERROR_READ_ENABLE = -7,
    T07_ERROR_PACKET_SEQUENCE = -8,
    T07_ERROR_LFSR_MISMATCH = -9,
    T07_ERROR_DELAY_APPLY = -10
} T07Status;

typedef enum T07Pc {
    T07_PC0 = 0,
    T07_PC1 = 1
} T07Pc;

typedef struct T07ResultEntry {
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
    u8 rx_dq[T07_RX_DQ_COUNT];
} T07ResultEntry;

typedef struct T07ValidRxEntry {
    /*
     * KO_NOTE:
     * read_en으로 표시된 valid bit들만 모아서 만든 "검사용 RX 샘플"입니다.
     * 8개의 valid bit가 모이면 rx_dq[0..63] 각각에 8-bit 값 하나가 생깁니다.
     */
    u8 rx_dq[T07_RX_DQ_COUNT];
} T07ValidRxEntry;

typedef struct T07PassZone {
    /*
     * KO_NOTE:
     * 한 DQ에서 특정 (mck_dly, bit_dly) 조합이 연속으로 pass한 pe_dly 구간입니다.
     * 예: m00 b3 pe003..063 len61 은 mck=0, bit=3에서 pe 3부터 63까지 pass했다는 뜻입니다.
     */
    u8 pc;
    u8 dq;
    u8 mck_dly;
    u8 bit_dly;
    u16 pe_start;
    u16 pe_end;
    u16 pe_count;
} T07PassZone;

typedef struct T07PassCenter {
    /*
     * KO_NOTE:
     * 각 PC/DQ별 대표 center입니다. 현재 구현은 가장 긴 pass zone의 가운데 값을 고릅니다.
     * valid=0이면 아직 해당 DQ에서 pass zone을 하나도 찾지 못했다는 뜻입니다.
     */
    u8 valid;
    u8 pc;
    u8 dq;
    u8 mck_dly;
    u8 bit_dly;
    u16 pe_dly;
    u16 pe_start;
    u16 pe_end;
    u16 pe_count;
} T07PassCenter;

typedef struct T07PassData {
    /*
     * Only pass zones are stored. This keeps memory almost fixed even if
     * T07_PE_DLY_COUNT grows from 64 to 1024.
     * best_center is still updated for every detected zone, including zones
     * that cannot be retained after T07_MAX_PASS_ZONES_PER_DQ is reached.
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
    u8 zone_count[T07_PC_COUNT][T07_DQ_PER_PC];
    T07PassZone zones[T07_PC_COUNT][T07_DQ_PER_PC][T07_MAX_PASS_ZONES_PER_DQ];
    T07PassCenter best_center[T07_PC_COUNT][T07_DQ_PER_PC];
} T07PassData;

typedef void (*T07Out64Fn)(uintptr_t addr, u64 value);
typedef u64 (*T07In64Fn)(uintptr_t addr);
typedef int (*T07ApplyDelayFn)(T07Pc pc, u8 dq, u8 mck_dly, u8 bit_dly, u16 pe_dly);
/*
 * KO_NOTE:
 * pass zone이 발견될 때마다 호출되는 디버그용 콜백입니다.
 * stored=1이면 T07PassData.zones에 저장된 zone이고,
 * stored=0이면 T07_MAX_PASS_ZONES_PER_DQ 제한 때문에 저장은 못 했지만 실제로 발견된 zone입니다.
 */
typedef void (*T07PassZoneLogFn)(const T07PassZone *zone, int stored, void *user_context);

const char *rt_status_message(RtStatus status);

/*
 * Copies line into out after removing leading and trailing ASCII whitespace.
 * If out is NULL, out_size is ignored and only the required byte count is
 * returned through written. The required count includes the trailing '\0'.
 */
RtStatus rt_trim_line(const char *line, char *out, size_t out_size, size_t *written);

void t07_set_io(T07Out64Fn out64, T07In64Fn in64);
void t07_set_delay_apply(T07ApplyDelayFn apply_delay);
void t07_set_pass_zone_log(T07PassZoneLogFn log_fn, void *user_context);

int t07_run_read_training_sweep(u16 result_len,
                                T07ResultEntry *entries,
                                T07ValidRxEntry *valid_rx,
                                size_t valid_rx_capacity,
                                T07PassData *pass_data,
                                T07PassCenter centers[T07_PC_COUNT][T07_DQ_PER_PC]);

#if defined(READ_TRAINING_EXPOSE_TEST_HELPERS)
/*
 * KO_NOTE:
 * 아래 함수들은 PC unit test와 visual log 생성용 helper입니다.
 * FW 적용 시에는 보통 t07_run_read_training_sweep() 중심으로 쓰면 되고,
 * 이 helper prototype들은 READ_TRAINING_EXPOSE_TEST_HELPERS를 켰을 때만 노출됩니다.
 */
extern const u8 expected_read_lfsr[T07_READ_LFSR_LENGTH][T07_READ_LFSR_DQ_GROUP_SIZE];

int t07_rsult_read(u8 mode,
                   u8 frame_num,
                   u16 bram_addr,
                   u8 *p_pkt_cnt,
                   u8 *p_seg_cnt,
                   u32 *p_data);

int t07_read_training_results(u16 result_len,
                              T07Pc pc,
                              T07ResultEntry *entries,
                              T07ValidRxEntry *valid_rx,
                              size_t valid_rx_capacity,
                              size_t *valid_rx_count);

int t07_check_valid_rx_lfsr(const T07ValidRxEntry *valid_rx,
                            size_t valid_rx_count,
                            size_t *failed_sample,
                            size_t *failed_dq);

int t07_check_dq_lfsr(const T07ValidRxEntry *valid_rx,
                      size_t valid_rx_count,
                      u8 dq,
                      size_t *failed_sample);

int t07_get_pass(const T07PassData *pass_data,
                 T07Pc pc,
                 u8 dq,
                 u8 mck_dly,
                 u8 bit_dly,
                 u16 pe_dly);

size_t t07_collect_pass_zones(const T07PassData *pass_data,
                              T07Pc pc,
                              u8 dq,
                              T07PassZone *zones,
                              size_t zone_capacity);
#endif

#ifdef __cplusplus
}
#endif

#endif
