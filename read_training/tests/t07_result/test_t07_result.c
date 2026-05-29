#include "read_training.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST_CMD_CODE 0x93U
#define TEST_ACK_OK 0xF0U
#define TEST_MAX_RESPONSES 200U
#define SAMPLE_RESULT_COUNT 10U
#define EXPECTED_VALID_RX_COUNT READ_LFSR_LENGTH

#ifndef RESULT_LOG_PATH
#define RESULT_LOG_PATH "t07_result_visual_log.txt"
#endif

static const u8 SAMPLE_READ_EN_PC0[SAMPLE_RESULT_COUNT] = {
    0xF8U, 0x07U, 0xFFU, 0x00U, 0xF0U,
    0x0FU, 0xFFU, 0xF8U, 0x07U, 0xFFU
};

static u64 g_responses[TEST_MAX_RESPONSES];
static size_t g_response_count;
static size_t g_response_index;
static u64 g_commands[16];
static size_t g_command_count;
static int g_generate_sweep_responses;
static int g_sweep_entries_valid;
static u8 g_sweep_pc;
static u8 g_sweep_mck_dly;
static u8 g_sweep_bit_dly;
static u16 g_sweep_pe_dly;
static T07ResultEntry g_sweep_entries[SAMPLE_RESULT_COUNT];
static u32 g_generated_segments[RESULT_SEGMENT_COUNT];
static size_t g_generated_segment_index;
static size_t g_apply_delay_call_count;
static size_t g_pass_zone_log_count;
static size_t g_pass_zone_log_overflow_count;

static u64 make_response(u8 pkt_cnt, u8 seg_cnt, u32 data);
static void start_generated_response(u16 bram_addr);

static void reset_mock_io(void)
{
    memset(g_responses, 0, sizeof(g_responses));
    memset(g_commands, 0, sizeof(g_commands));
    g_response_count = 0U;
    g_response_index = 0U;
    g_command_count = 0U;
    g_generate_sweep_responses = 0;
    g_sweep_entries_valid = 0;
    g_sweep_pc = PC0;
    g_sweep_mck_dly = 0U;
    g_sweep_bit_dly = 0U;
    g_sweep_pe_dly = 0U;
    memset(g_sweep_entries, 0, sizeof(g_sweep_entries));
    memset(g_generated_segments, 0, sizeof(g_generated_segments));
    g_generated_segment_index = 0U;
    g_apply_delay_call_count = 0U;
    g_pass_zone_log_count = 0U;
    g_pass_zone_log_overflow_count = 0U;
    t07_set_io(NULL, NULL);
    t07_set_delay_apply(NULL);
    t07_set_pass_zone_log(NULL, NULL);
}

static void mock_out64(uintptr_t addr, u64 value)
{
    (void)addr;
    if (g_command_count < (sizeof(g_commands) / sizeof(g_commands[0]))) {
        g_commands[g_command_count] = value;
    }
    ++g_command_count;

    if (g_generate_sweep_responses != 0) {
        start_generated_response((u16)((value >> 32) & 0xFFFFU));
    }
}

static u64 mock_in64(uintptr_t addr)
{
    (void)addr;
    if (g_generate_sweep_responses != 0) {
        size_t i = g_generated_segment_index;
        u8 pkt_cnt;
        u8 seg_cnt;

        assert(i < RESULT_SEGMENT_COUNT);
        pkt_cnt = i == (RESULT_SEGMENT_COUNT - 1U) ? 1U : 0U;
        seg_cnt = i == (RESULT_SEGMENT_COUNT - 1U) ? 0U : (u8)(i + 1U);
        ++g_generated_segment_index;
        return make_response(pkt_cnt, seg_cnt, g_generated_segments[i]);
    }

    assert(g_response_index < g_response_count);
    return g_responses[g_response_index++];
}

static u64 make_response(u8 pkt_cnt, u8 seg_cnt, u32 data)
{
    return ((u64)TEST_CMD_CODE << 56) |
           ((u64)TEST_ACK_OK << 48) |
           ((u64)(pkt_cnt & 0x0FU) << 40) |
           ((u64)seg_cnt << 32) |
           (u64)data;
}

static u8 expected_valid_rx_byte(size_t sample_index, size_t dq)
{
    return expected_read_lfsr[sample_index % READ_LFSR_LENGTH]
                             [dq % READ_LFSR_DQ_GROUP_SIZE];
}

static u8 noise_rx_byte(size_t result_index, size_t dq)
{
    return (u8)((0x80U + (result_index * 0x11U) + (dq * 0x03U)) & 0xFFU);
}

static void bits_to_string(u8 value, char out[9])
{
    int bit;

    for (bit = 7; bit >= 0; --bit) {
        out[7 - bit] = ((value >> (unsigned int)bit) & 0x01U) != 0U ? '1' : '0';
    }
    out[8] = '\0';
}

static void pack_entry(const T07ResultEntry *entry, u32 segments[RESULT_SEGMENT_COUNT])
{
    u8 bytes[RESULT_SEGMENT_COUNT * 4U];
    size_t i;

    memset(bytes, 0, sizeof(bytes));
    bytes[0U] = entry->time_ptr;
    memcpy(&bytes[1U], entry->comp_results_pc0, sizeof(entry->comp_results_pc0));
    memcpy(&bytes[5U], entry->comp_results_pc1, sizeof(entry->comp_results_pc1));
    bytes[9U] = entry->read_en_pc0;
    bytes[10U] = entry->read_en_pc1;
    bytes[11U] = entry->read_tph_pc0;
    bytes[12U] = entry->read_tph_pc1;
    memcpy(&bytes[13U], entry->rx_dq, sizeof(entry->rx_dq));

    for (i = 0U; i < RESULT_SEGMENT_COUNT; ++i) {
        segments[i] = (u32)bytes[(i * 4U) + 0U] |
                      ((u32)bytes[(i * 4U) + 1U] << 8) |
                      ((u32)bytes[(i * 4U) + 2U] << 16) |
                      ((u32)bytes[(i * 4U) + 3U] << 24);
    }
}

static int sweep_should_pass(u8 pc, u8 dq, u8 mck_dly, u8 bit_dly, u16 pe_dly)
{
    u8 local_dq;
    u8 main_mck;
    u8 main_bit;
    u16 main_start;
    u16 main_count;
    u16 main_end;
    u8 short_mck;
    u8 short_bit;
    u16 short_start;
    u16 short_end;

    if (pc == PC0) {
        if (dq >= DQ_PER_PC) {
            return 0;
        }
        local_dq = dq;
        main_mck = (u8)(local_dq % MCK_DLY_COUNT);
        main_bit = (u8)((local_dq / 2U) % BIT_DLY_COUNT);
        main_start = (u8)(local_dq % 12U);
        main_count = (u8)(10U + (local_dq % 5U));
        short_mck = (u8)((main_mck + 1U) % MCK_DLY_COUNT);
        short_bit = (u8)((main_bit + 1U) % BIT_DLY_COUNT);
        short_start = (u8)(48U + (local_dq % 4U));
        if (local_dq == 0U) {
            main_mck = 0U;
            main_bit = 3U;
            main_start = 3U;
            main_count = 61U;
            short_mck = 1U;
            short_bit = 4U;
            short_start = 48U;
        }
    } else {
        if (dq < DQ_PER_PC || dq >= RX_DQ_COUNT) {
            return 0;
        }
        local_dq = (u8)(dq - DQ_PER_PC);
        main_mck = (u8)(15U - (local_dq % MCK_DLY_COUNT));
        main_bit = (u8)(7U - ((local_dq / 2U) % BIT_DLY_COUNT));
        main_start = (u8)(4U + (local_dq % 10U));
        main_count = (u8)(8U + (local_dq % 7U));
        short_mck = (u8)((main_mck + 15U) % MCK_DLY_COUNT);
        short_bit = (u8)((main_bit + 7U) % BIT_DLY_COUNT);
        short_start = (u8)(40U + (local_dq % 5U));
    }

    main_end = (u8)(main_start + main_count - 1U);
    short_end = (u8)(short_start + 2U);

    if (pc == PC0 && dq == 0U) {
        if (bit_dly == 0U && pe_dly == (u16)(mck_dly % 4U)) {
            return 1;
        }
        if (mck_dly == 15U &&
            bit_dly == 7U &&
            pe_dly < PE_DLY_COUNT) {
            return 1;
        }
    }

    if (mck_dly == main_mck &&
        bit_dly == main_bit &&
        pe_dly >= main_start &&
        pe_dly <= main_end) {
        return 1;
    }
    if (mck_dly == short_mck &&
        bit_dly == short_bit &&
        pe_dly >= short_start &&
        pe_dly <= short_end) {
        return 1;
    }
    return 0;
}

static int mock_apply_delay(u8 pc, u8 dq, u8 mck_dly, u8 bit_dly, u16 pe_dly)
{
    if (pc == PC0) {
        assert(dq < DQ_PER_PC);
    } else {
        assert(dq >= DQ_PER_PC && dq < RX_DQ_COUNT);
    }

    g_sweep_pc = pc;
    g_sweep_mck_dly = mck_dly;
    g_sweep_bit_dly = bit_dly;
    g_sweep_pe_dly = pe_dly;
    g_sweep_entries_valid = 0;
    ++g_apply_delay_call_count;
    return TRAINING_OK;
}

static void make_sweep_data(void)
{
    size_t result_index;
    size_t dq;
    size_t produced = 0U;
    unsigned int bits_in_run = 0U;

    memset(g_sweep_entries, 0, sizeof(g_sweep_entries));

    for (result_index = 0U; result_index < SAMPLE_RESULT_COUNT; ++result_index) {
        T07ResultEntry *entry = &g_sweep_entries[result_index];

        entry->time_ptr = (u8)(0x60U + result_index);
        entry->comp_results_pc0[0] = (u8)(0x10U + result_index);
        entry->comp_results_pc0[1] = (u8)(0x20U + result_index);
        entry->comp_results_pc0[2] = (u8)(0x30U + result_index);
        entry->comp_results_pc0[3] = (u8)(0x40U + result_index);
        entry->comp_results_pc1[0] = (u8)(0x50U + result_index);
        entry->comp_results_pc1[1] = (u8)(0x60U + result_index);
        entry->comp_results_pc1[2] = (u8)(0x70U + result_index);
        entry->comp_results_pc1[3] = (u8)(0x80U + result_index);
        entry->read_en_pc0 = g_sweep_pc == PC0 ? SAMPLE_READ_EN_PC0[result_index] : 0x00U;
        entry->read_en_pc1 = g_sweep_pc == PC1 ? SAMPLE_READ_EN_PC0[result_index] : 0x00U;
        entry->read_tph_pc0 = (u8)(0xB0U + result_index);
        entry->read_tph_pc1 = (u8)(0xC0U + result_index);

        for (dq = 0U; dq < RX_DQ_COUNT; ++dq) {
            entry->rx_dq[dq] = noise_rx_byte(result_index, dq);
        }
    }

    for (result_index = 0U; result_index < SAMPLE_RESULT_COUNT; ++result_index) {
        unsigned int bit;

        for (bit = 0U; bit < 8U; ++bit) {
            if (((SAMPLE_READ_EN_PC0[result_index] >> bit) & 0x01U) != 0U) {
                for (dq = 0U; dq < RX_DQ_COUNT; ++dq) {
                    u8 compact_value = expected_valid_rx_byte(produced, dq);
                    u8 bit_value;

                    if (sweep_should_pass(g_sweep_pc,
                                          (u8)dq,
                                          g_sweep_mck_dly,
                                          g_sweep_bit_dly,
                                          g_sweep_pe_dly) == 0) {
                        compact_value ^= 0x01U;
                    }

                    bit_value = (u8)((compact_value >> bits_in_run) & 0x01U);
                    g_sweep_entries[result_index].rx_dq[dq] =
                        (u8)(g_sweep_entries[result_index].rx_dq[dq] & (u8)~(1U << bit));
                    g_sweep_entries[result_index].rx_dq[dq] =
                        (u8)(g_sweep_entries[result_index].rx_dq[dq] | (u8)(bit_value << bit));
                }

                ++bits_in_run;
                if (bits_in_run == 8U) {
                    ++produced;
                    bits_in_run = 0U;
                }
            } else if (bits_in_run != 0U) {
                assert(0);
            }
        }
    }

    assert(produced == EXPECTED_VALID_RX_COUNT);
    assert(bits_in_run == 0U);
    g_sweep_entries_valid = 1;
}

static void start_generated_response(u16 bram_addr)
{
    assert(bram_addr < SAMPLE_RESULT_COUNT);
    if (g_sweep_entries_valid == 0) {
        make_sweep_data();
    }
    pack_entry(&g_sweep_entries[bram_addr], g_generated_segments);
    g_generated_segment_index = 0U;
}

static void queue_entry_response(const T07ResultEntry *entry)
{
    u32 segments[RESULT_SEGMENT_COUNT];
    size_t i;

    pack_entry(entry, segments);
    for (i = 0U; i < RESULT_SEGMENT_COUNT; ++i) {
        u8 pkt_cnt = i == (RESULT_SEGMENT_COUNT - 1U) ? 1U : 0U;
        u8 seg_cnt = i == (RESULT_SEGMENT_COUNT - 1U) ? 0U : (u8)(i + 1U);

        assert(g_response_count < TEST_MAX_RESPONSES);
        g_responses[g_response_count] = make_response(pkt_cnt, seg_cnt, segments[i]);
        ++g_response_count;
    }
}

static void queue_bad_sequence_response(const T07ResultEntry *entry)
{
    u32 segments[RESULT_SEGMENT_COUNT];

    pack_entry(entry, segments);
    assert(g_response_count < TEST_MAX_RESPONSES);
    g_responses[g_response_count] = make_response(0U, 0U, segments[0]);
    ++g_response_count;
}

static void make_sample_data(T07ResultEntry raw_entries[SAMPLE_RESULT_COUNT],
                             T07ValidRxEntry expected_valid_rx[EXPECTED_VALID_RX_COUNT])
{
    size_t result_index;
    size_t dq;
    size_t produced = 0U;
    unsigned int bits_in_run = 0U;

    memset(raw_entries, 0, sizeof(T07ResultEntry) * SAMPLE_RESULT_COUNT);
    memset(expected_valid_rx, 0, sizeof(T07ValidRxEntry) * EXPECTED_VALID_RX_COUNT);

    for (produced = 0U; produced < EXPECTED_VALID_RX_COUNT; ++produced) {
        for (dq = 0U; dq < RX_DQ_COUNT; ++dq) {
            expected_valid_rx[produced].rx_dq[dq] = expected_valid_rx_byte(produced, dq);
        }
    }

    for (result_index = 0U; result_index < SAMPLE_RESULT_COUNT; ++result_index) {
        T07ResultEntry *entry = &raw_entries[result_index];

        entry->time_ptr = (u8)(0x40U + result_index);
        entry->comp_results_pc0[0] = (u8)(0x07U + result_index);
        entry->comp_results_pc0[1] = (u8)(0x15U + result_index);
        entry->comp_results_pc0[2] = (u8)(0x23U + result_index);
        entry->comp_results_pc0[3] = (u8)(0x31U + result_index);
        entry->comp_results_pc1[0] = (u8)(0x47U + result_index);
        entry->comp_results_pc1[1] = (u8)(0x55U + result_index);
        entry->comp_results_pc1[2] = (u8)(0x63U + result_index);
        entry->comp_results_pc1[3] = (u8)(0x71U + result_index);
        entry->read_en_pc0 = SAMPLE_READ_EN_PC0[result_index];
        entry->read_en_pc1 = 0x00U;
        entry->read_tph_pc0 = (u8)(0x90U + result_index);
        entry->read_tph_pc1 = (u8)(0xA0U + result_index);

        for (dq = 0U; dq < RX_DQ_COUNT; ++dq) {
            entry->rx_dq[dq] = noise_rx_byte(result_index, dq);
        }
    }

    produced = 0U;
    bits_in_run = 0U;
    for (result_index = 0U; result_index < SAMPLE_RESULT_COUNT; ++result_index) {
        unsigned int bit;

        for (bit = 0U; bit < 8U; ++bit) {
            if (((SAMPLE_READ_EN_PC0[result_index] >> bit) & 0x01U) != 0U) {
                for (dq = 0U; dq < RX_DQ_COUNT; ++dq) {
                    u8 bit_value = (u8)((expected_valid_rx[produced].rx_dq[dq] >> bits_in_run) & 0x01U);

                    raw_entries[result_index].rx_dq[dq] =
                        (u8)(raw_entries[result_index].rx_dq[dq] & (u8)~(1U << bit));
                    raw_entries[result_index].rx_dq[dq] =
                        (u8)(raw_entries[result_index].rx_dq[dq] | (u8)(bit_value << bit));
                }

                ++bits_in_run;
                if (bits_in_run == 8U) {
                    ++produced;
                    bits_in_run = 0U;
                }
            } else if (bits_in_run != 0U) {
                assert(0);
            }
        }
    }

    assert(produced == EXPECTED_VALID_RX_COUNT);
    assert(bits_in_run == 0U);
}

static void assert_entries_equal(const T07ResultEntry *expected,
                                 const T07ResultEntry *actual,
                                 size_t count)
{
    size_t i;

    for (i = 0U; i < count; ++i) {
        assert(expected[i].time_ptr == actual[i].time_ptr);
        assert(memcmp(expected[i].comp_results_pc0,
                      actual[i].comp_results_pc0,
                      sizeof(expected[i].comp_results_pc0)) == 0);
        assert(memcmp(expected[i].comp_results_pc1,
                      actual[i].comp_results_pc1,
                      sizeof(expected[i].comp_results_pc1)) == 0);
        assert(expected[i].read_en_pc0 == actual[i].read_en_pc0);
        assert(expected[i].read_en_pc1 == actual[i].read_en_pc1);
        assert(expected[i].read_tph_pc0 == actual[i].read_tph_pc0);
        assert(expected[i].read_tph_pc1 == actual[i].read_tph_pc1);
        assert(memcmp(expected[i].rx_dq,
                      actual[i].rx_dq,
                      sizeof(expected[i].rx_dq)) == 0);
    }
}

static void assert_valid_rx_equal(const T07ValidRxEntry *expected,
                                  const T07ValidRxEntry *actual,
                                  size_t count)
{
    size_t i;

    for (i = 0U; i < count; ++i) {
        assert(memcmp(expected[i].rx_dq,
                      actual[i].rx_dq,
                      sizeof(expected[i].rx_dq)) == 0);
    }
}

static void write_rx_rows(FILE *log, const u8 rx_dq[RX_DQ_COUNT])
{
    size_t row;

    for (row = 0U; row < 4U; ++row) {
        size_t col;
        size_t start = row * 16U;

        fprintf(log, "  rx_dq%02u..%02u:", (unsigned int)start, (unsigned int)(start + 15U));
        for (col = 0U; col < 16U; ++col) {
            fprintf(log, " %02X", rx_dq[start + col]);
        }
        fprintf(log, "\n");
    }
}

static void write_result_entry(FILE *log, size_t index, const T07ResultEntry *entry)
{
    char read_en_bits[9];

    bits_to_string(entry->read_en_pc0, read_en_bits);

    fprintf(log, "result[%u]\n", (unsigned int)index);
    fprintf(log, "  time_ptr: %02X\n", entry->time_ptr);
    fprintf(log,
            "  comp_results_pc0(dq7_0 dq15_8 dq23_16 dq31_24): %02X %02X %02X %02X\n",
            entry->comp_results_pc0[0],
            entry->comp_results_pc0[1],
            entry->comp_results_pc0[2],
            entry->comp_results_pc0[3]);
    fprintf(log,
            "  comp_results_pc1(dq7_0 dq15_8 dq23_16 dq31_24): %02X %02X %02X %02X\n",
            entry->comp_results_pc1[0],
            entry->comp_results_pc1[1],
            entry->comp_results_pc1[2],
            entry->comp_results_pc1[3]);
    fprintf(log, "  read_en_pc0: %02X b'%s'\n", entry->read_en_pc0, read_en_bits);
    fprintf(log, "  read_en_pc1: %02X\n", entry->read_en_pc1);
    fprintf(log, "  read_tph_pc0: %02X\n", entry->read_tph_pc0);
    fprintf(log, "  read_tph_pc1: %02X\n", entry->read_tph_pc1);
    write_rx_rows(log, entry->rx_dq);
}

static void write_segments(FILE *log, size_t index, const T07ResultEntry *entry)
{
    u32 segments[RESULT_SEGMENT_COUNT];
    size_t i;

    pack_entry(entry, segments);

    fprintf(log, "result[%u] raw 20 segments:", (unsigned int)index);
    for (i = 0U; i < RESULT_SEGMENT_COUNT; ++i) {
        fprintf(log, " %08X", segments[i]);
    }
    fprintf(log, "\n");
}

static void write_valid_rx(FILE *log, size_t index, const T07ValidRxEntry *entry)
{
    fprintf(log, "valid_rx[%u]\n", (unsigned int)index);
    write_rx_rows(log, entry->rx_dq);
}

static void write_visual_log(FILE *log,
                             const T07ResultEntry raw_entries[SAMPLE_RESULT_COUNT],
                             const T07ResultEntry parsed_entries[SAMPLE_RESULT_COUNT],
                             const T07ValidRxEntry valid_rx[EXPECTED_VALID_RX_COUNT])
{
    size_t i;

    fprintf(log, "T07 LSB-first result parsing visual log\n");
    fprintf(log, "\nEndian note:\n");
    fprintf(log, "  segment 0xAABBCCDD becomes table bytes DD CC BB AA.\n");
    fprintf(log, "  byte[0] receives bits [7:0], so segment 0 LSB is time_ptr.\n");
    fprintf(log, "\nExpected LFSR note:\n");
    fprintf(log, "  expected_read_lfsr has 6 rows, and each row has dq0..dq7 only.\n");
    fprintf(log, "  dq8..15, dq16..23, ..., dq56..63 repeat the same dq0..dq7 row.\n");

    fprintf(log, "\nInput sample result table, 8-bit values:\n");
    for (i = 0U; i < SAMPLE_RESULT_COUNT; ++i) {
        write_result_entry(log, i, &raw_entries[i]);
    }

    fprintf(log, "\nRaw 20 x 32-bit segments sent by mock Xil_In64:\n");
    for (i = 0U; i < SAMPLE_RESULT_COUNT; ++i) {
        write_segments(log, i, &raw_entries[i]);
    }

    fprintf(log, "\nParsed result table after t07_read_training_results():\n");
    for (i = 0U; i < SAMPLE_RESULT_COUNT; ++i) {
        write_result_entry(log, i, &parsed_entries[i]);
    }

    fprintf(log, "\nCompacted valid RX output, LSB-first read_en:\n");
    for (i = 0U; i < EXPECTED_VALID_RX_COUNT; ++i) {
        write_valid_rx(log, i, &valid_rx[i]);
    }
}

static void write_sweep_log(FILE *log,
                            const T07PassData *pass_data,
                            const T07PassCenter centers[PC_COUNT][DQ_PER_PC])
{
    size_t pc_index;

    fprintf(log, "\n\nT07 read training sweep visual log\n");
    fprintf(log, "Loop order: pc -> mck_dly -> bit_dly -> pe_dly\n");
    fprintf(log, "Each delay point applies the same delay to the 32 DQs in that PC, then reads result data once.\n");
    fprintf(log, "PC0 controls dq00..dq31. PC1 controls dq32..dq63.\n");
    fprintf(log, "A pass point means every compact valid_rx sample matched expected_read_lfsr for that DQ.\n");

    for (pc_index = 0U; pc_index < PC_COUNT; ++pc_index) {
        u8 pc = pc_index == 0U ? PC0 : PC1;
        size_t local_dq;

        fprintf(log, "\nPC%u stored pass zones and centers\n", (unsigned int)pc_index);
        for (local_dq = 0U; local_dq < DQ_PER_PC; ++local_dq) {
            u8 dq = (u8)(pc_index == 0U ? local_dq : (local_dq + DQ_PER_PC));
            T07PassZone zones[16];
            size_t zone_count = t07_collect_pass_zones(pass_data, pc, dq, zones, 16U);
            size_t zone_index;
            const T07PassCenter *center = &centers[pc_index][local_dq];

            assert(zone_count <= 16U);
            fprintf(log, "  dq%02u zones:", (unsigned int)dq);
            if (zone_count == 0U) {
                fprintf(log, " none");
            }
            for (zone_index = 0U; zone_index < zone_count; ++zone_index) {
                fprintf(log,
                        " [m%02u b%u pe%03u..%03u len%u]",
                        (unsigned int)zones[zone_index].mck_dly,
                        (unsigned int)zones[zone_index].bit_dly,
                        (unsigned int)zones[zone_index].pe_start,
                        (unsigned int)zones[zone_index].pe_end,
                        (unsigned int)zones[zone_index].pe_count);
            }
            if (center->valid != 0U) {
                fprintf(log,
                        " center=m%02u b%u pe%03u longest=pe%03u..%03u len%u",
                        (unsigned int)center->mck_dly,
                        (unsigned int)center->bit_dly,
                        (unsigned int)center->pe_dly,
                        (unsigned int)center->pe_start,
                        (unsigned int)center->pe_end,
                        (unsigned int)center->pe_count);
            } else {
                fprintf(log, " center=none");
            }
            fprintf(log, "\n");
        }
    }
}

static void log_pass_zone_event(const T07PassZone *zone, int stored, void *user_context)
{
    FILE *log = (FILE *)user_context;

    assert(zone != NULL);
    ++g_pass_zone_log_count;
    if (stored == 0) {
        ++g_pass_zone_log_overflow_count;
    }

    if (log != NULL) {
        fprintf(log,
                "  %s pc%u dq%02u m%02u b%u pe%03u..%03u len%u\n",
                stored != 0 ? "stored  " : "overflow",
                (unsigned int)zone->pc,
                (unsigned int)zone->dq,
                (unsigned int)zone->mck_dly,
                (unsigned int)zone->bit_dly,
                (unsigned int)zone->pe_start,
                (unsigned int)zone->pe_end,
                (unsigned int)zone->pe_count);
    }
}

static void run_sample_parse_test(FILE *log)
{
    T07ResultEntry raw_entries[SAMPLE_RESULT_COUNT];
    T07ResultEntry parsed_entries[SAMPLE_RESULT_COUNT];
    T07ValidRxEntry expected_valid_rx[EXPECTED_VALID_RX_COUNT];
    T07ValidRxEntry actual_valid_rx[EXPECTED_VALID_RX_COUNT];
    size_t valid_rx_count = 0U;
    size_t i;
    int status;

    make_sample_data(raw_entries, expected_valid_rx);

    reset_mock_io();
    t07_set_io(mock_out64, mock_in64);
    for (i = 0U; i < SAMPLE_RESULT_COUNT; ++i) {
        queue_entry_response(&raw_entries[i]);
    }

    status = t07_read_training_results(SAMPLE_RESULT_COUNT,
                                       PC0,
                                       parsed_entries,
                                       actual_valid_rx,
                                       EXPECTED_VALID_RX_COUNT,
                                       &valid_rx_count);

    assert(status == TRAINING_OK);
    assert(valid_rx_count == EXPECTED_VALID_RX_COUNT);
    assert(g_command_count == SAMPLE_RESULT_COUNT);
    assert(g_response_index == SAMPLE_RESULT_COUNT * RESULT_SEGMENT_COUNT);

    assert_entries_equal(raw_entries, parsed_entries, SAMPLE_RESULT_COUNT);
    assert_valid_rx_equal(expected_valid_rx, actual_valid_rx, EXPECTED_VALID_RX_COUNT);
    assert(t07_check_valid_rx_lfsr(actual_valid_rx, valid_rx_count, NULL, NULL) == TRAINING_OK);

    write_visual_log(log, raw_entries, parsed_entries, actual_valid_rx);
}

static void test_retries_bad_packet_sequence(void)
{
    T07ResultEntry raw_entries[SAMPLE_RESULT_COUNT];
    T07ValidRxEntry expected_valid_rx[EXPECTED_VALID_RX_COUNT];
    u32 raw_segments[RESULT_SEGMENT_COUNT];
    u8 pkt_cnt = 0U;
    u8 seg_cnt = 0U;
    int status;

    make_sample_data(raw_entries, expected_valid_rx);

    reset_mock_io();
    t07_set_io(mock_out64, mock_in64);
    queue_bad_sequence_response(&raw_entries[0]);
    queue_entry_response(&raw_entries[0]);

    status = t07_rsult_read(MODE_READ,
                            RESULT_FRAME_NUM,
                            0U,
                            &pkt_cnt,
                            &seg_cnt,
                            raw_segments);

    assert(status == TRAINING_OK);
    assert(g_command_count == 2U);
    assert(g_commands[0] == 0x93C0000000000000ULL);
    assert(g_commands[1] == 0x93C0000000000000ULL);
    assert(g_response_index == RESULT_SEGMENT_COUNT + 1U);
    assert(pkt_cnt == 1U);
    assert(seg_cnt == 0U);
}

static void test_lfsr_mismatch_reports_dq(void)
{
    T07ValidRxEntry rx[2];
    size_t sample;
    size_t dq;
    size_t failed_sample = 2U;
    size_t failed_dq = RX_DQ_COUNT;
    int status;

    for (sample = 0U; sample < 2U; ++sample) {
        for (dq = 0U; dq < RX_DQ_COUNT; ++dq) {
            rx[sample].rx_dq[dq] = expected_valid_rx_byte(sample, dq);
        }
    }
    rx[1].rx_dq[17] ^= 0x01U;

    status = t07_check_valid_rx_lfsr(rx, 2U, &failed_sample, &failed_dq);

    assert(status == TRAINING_ERROR_LFSR_MISMATCH);
    assert(failed_sample == 1U);
    assert(failed_dq == 17U);
}

static void test_rejects_incomplete_window(void)
{
    T07ResultEntry raw_entries[SAMPLE_RESULT_COUNT];
    T07ResultEntry parsed_entries[SAMPLE_RESULT_COUNT];
    T07ValidRxEntry expected_valid_rx[EXPECTED_VALID_RX_COUNT];
    T07ValidRxEntry actual_valid_rx[EXPECTED_VALID_RX_COUNT];
    size_t valid_rx_count = 0U;
    int status;

    make_sample_data(raw_entries, expected_valid_rx);
    raw_entries[1].read_en_pc0 = 0x03U;

    reset_mock_io();
    t07_set_io(mock_out64, mock_in64);
    queue_entry_response(&raw_entries[0]);
    queue_entry_response(&raw_entries[1]);

    status = t07_read_training_results(2U,
                                       PC0,
                                       parsed_entries,
                                       actual_valid_rx,
                                       EXPECTED_VALID_RX_COUNT,
                                       &valid_rx_count);

    assert(status == TRAINING_ERROR_READ_ENABLE);
    assert(valid_rx_count == 0U);
}

static void run_sweep_test(FILE *log)
{
    static T07PassData pass_data;
    static T07PassCenter centers[PC_COUNT][DQ_PER_PC];
    T07ResultEntry parsed_entries[SAMPLE_RESULT_COUNT];
    T07ValidRxEntry valid_rx[EXPECTED_VALID_RX_COUNT];
    int status;

    reset_mock_io();
    g_generate_sweep_responses = 1;
    t07_set_io(mock_out64, mock_in64);
    t07_set_delay_apply(mock_apply_delay);
    fprintf(log, "\n\nT07 pass zone event log\n");
    fprintf(log, "Events are emitted before the fixed per-DQ storage cap can hide overflow zones.\n");
    t07_set_pass_zone_log(log_pass_zone_event, log);

    status = t07_run_read_training_sweep(SAMPLE_RESULT_COUNT,
                                         parsed_entries,
                                         valid_rx,
                                         EXPECTED_VALID_RX_COUNT,
                                         &pass_data,
                                         centers);

    assert(status == TRAINING_OK);
    t07_set_pass_zone_log(NULL, NULL);
    assert(g_pass_zone_log_count ==
           ((size_t)PC_COUNT * (size_t)DQ_PER_PC * 2U) + 17U);
    assert(g_pass_zone_log_overflow_count == 3U);
    assert(g_apply_delay_call_count ==
           (size_t)PC_COUNT *
           (size_t)MCK_DLY_COUNT *
           (size_t)BIT_DLY_COUNT *
           (size_t)PE_DLY_COUNT *
           (size_t)DQ_PER_PC);

    assert(t07_get_pass(&pass_data, PC0, 0U, 0U, 3U, 3U) != 0);
    assert(t07_get_pass(&pass_data, PC0, 0U, 0U, 3U, 63U) != 0);
    assert(t07_get_pass(&pass_data, PC0, 0U, 0U, 3U, 2U) == 0);
    assert(t07_get_pass(&pass_data, PC0, 31U, 15U, 7U, 17U) != 0);
    assert(t07_get_pass(&pass_data, PC1, 32U, 15U, 7U, 4U) != 0);
    assert(t07_get_pass(&pass_data, PC1, 63U, 0U, 0U, 15U) != 0);
    assert(t07_get_pass(&pass_data, PC0, 32U, 0U, 0U, 0U) == 0);
    assert(t07_get_pass(&pass_data, PC1, 31U, 0U, 0U, 0U) == 0);

    assert(centers[0][0].valid != 0U);
    assert(centers[0][0].dq == 0U);
    assert(centers[0][0].mck_dly == 15U);
    assert(centers[0][0].bit_dly == 7U);
    assert(centers[0][0].pe_start == 0U);
    assert(centers[0][0].pe_end == 63U);
    assert(centers[0][0].pe_dly == 32U);

    assert(centers[1][0].valid != 0U);
    assert(centers[1][0].dq == 32U);
    assert(centers[1][0].mck_dly == 15U);
    assert(centers[1][0].bit_dly == 7U);
    assert(centers[1][0].pe_start == 4U);
    assert(centers[1][0].pe_end == 11U);
    assert(centers[1][0].pe_dly == 8U);

    write_sweep_log(log, &pass_data, centers);
}

int main(void)
{
    FILE *log = fopen(RESULT_LOG_PATH, "w");

    assert(log != NULL);
    run_sample_parse_test(log);
    test_retries_bad_packet_sequence();
    test_lfsr_mismatch_reports_dq();
    test_rejects_incomplete_window();
    run_sweep_test(log);
    fclose(log);
    reset_mock_io();

    return 0;
}
