#!/usr/bin/env python3
"""
Generate CA training and initialization patterns for HBM testing.
Creates ptn_ca.txt and ptn_init.txt files.
"""

import random
import generate_pattern
import analyze_serdes_16bit

# Disable debug mode for cleaner output
generate_pattern.DEBUG_MODE = False

def generate_ca_patterns():
    """Generate CA training patterns for all pins."""
    ca_pins = ['R0', 'R1', 'R2', 'R3', 'R4', 'R5', 'R6', 'R7', 'R8', 'R9', 'R10',
               'C0', 'C1', 'C2', 'C3', 'C4', 'C5', 'C6', 'C7']

    ca_trn_pat_list = []
    ca_trn_misr_list = []
    logs = []

    for pin in ca_pins:
        training = generate_pattern.generate_ca_training_pattern(pin, '11000011', clock_toggle=True, num_frames=48)
        pattern = training
        raw_frames = len(pattern) // 8
        
        logs.append(f'=== {pin} Training with sequence "11000011" ===')
        logs.append(f'Raw pattern: {raw_frames} frames (4 default + 44 training)')
        logs.append('Hex Pattern: ' + pattern)
        
        steps = generate_pattern.get_aword_misr_steps(pattern[32:])
        misr_values = [format(post, '010X') for _, _, _, _, post in steps]
        logs.append('MISR per Clock: ' + str(misr_values))
        
        final_misr = generate_pattern.get_aword_misr(training[32:])
        logs.append('Final MISR: ' + final_misr)
        
        serdes_pattern = generate_pattern.pattern_to_serdes_16to1(pattern, padding_ck_toggle=True, padding_ck_value=0)
        serdes_blocks = len(serdes_pattern) // 120
        padded_frames = serdes_blocks * 16
        logs.append(f'SERDES 16:1 Pattern: {serdes_pattern}')
        logs.append(f'SERDES output: {len(serdes_pattern)} hex chars, {serdes_blocks} blocks, {padded_frames} decoded frames (padded to 16-frame boundary)')
        logs.append('--- Analyze SERDES pattern for ' + pin + ' ---')
        logs.append('')
        
        ca_trn_pat_list.append(f"0x{serdes_pattern.upper()}")
        ca_trn_misr_list.append(final_misr)

    return ca_trn_pat_list, ca_trn_misr_list, logs

def generate_init_patterns():
    """Generate initialization patterns (PDE + PDX)."""
    logs = []
    logs.append("=== Testing Initialization Patterns ===\n")
    
    # Init pull pattern
    # 1) PDE & CK Low 고정 (4nCK)
    pattern_1 = generate_pattern.generate_init_pde_pattern(num_clocks=4, clock_toggle=False, clock_value=0)
    logs.append(f"1) PDE & CK Low 고정 (4nCK): {pattern_1}")
    
    # 2) PDE & CK toggle (20nCK)
    pattern_2 = generate_pattern.generate_init_pde_pattern(num_clocks=20, clock_toggle=True)
    logs.append(f"2) PDE & CK toggle (20nCK): {pattern_2}")
    
    # 3) PDX & CK toggle (48nCK)
    pattern_3 = generate_pattern.generate_init_pdx_pattern(num_clocks=48, clock_toggle=True)
    logs.append(f"3) PDX & CK toggle (48nCK): {pattern_3}")

    # Combine patterns for conversion
    combined_pattern = pattern_3 + pattern_2 + pattern_1
    serdes_pattern = generate_pattern.pattern_to_serdes_16to1(combined_pattern)
    logs.append(f"4) Combined Pattern 16:1 Serdes 변환: {serdes_pattern}\n")

    return serdes_pattern, logs

def create_ptn_ca_txt(ca_trn_pat_list, ca_trn_misr_list, logs):
    """Create ptn_ca.txt file."""
    with open('ptn_ca.txt', 'w') as f:
        f.write('ca_trn_pat_list = [\n')
        for pat in ca_trn_pat_list:
            f.write(f"    '{pat}',\n")
        f.write(']\n\n')
        f.write('ca_trn_misr_list = [\n')
        for misr in ca_trn_misr_list:
            f.write(f"    '{misr}',\n")
        f.write(']\n')
        
        f.write('\n# === Logs from test_ca_training_all_pins.py ===\n')
        for log in logs:
            f.write(log + '\n')

def create_ptn_init_txt(init_pattern, logs):
    """Create ptn_init.txt file."""
    with open('ptn_init.txt', 'w') as f:
        f.write(f'init_pattern = "{init_pattern}"\n')
        
        f.write('\n# === Logs from test_init_pattern.py ===\n')
        for log in logs:
            f.write(log + '\n')


def generate_mrs_wr_patterns():
    """Generate fixed MRS patterns and randomized WR pattern with SERDES output."""
    logs = []
    mrs_patterns = {}
    mrs_serdes = {}

    mrs_definitions = [
        ("mrs_MR1_WriteLeveling_Stage8",            [0, 0, 0, 1, 0, 0, 0, 0], [1, 0, 0, 0, 0]),
        ("mrs_MR7_DWORDLoopback_Enable",            [1, 0, 0, 0, 0, 0, 0, 0], [1, 1, 1, 0, 0]),
        ("mrs_MR7_DWORDLoopback_Enable_MISR_Mode",  [1, 0, 0, 1, 1, 0, 0, 0], [1, 1, 1, 0, 0]),
        ("mrs_MR7_DWORDLoopback_Disable",           [0, 0, 0, 0, 0, 0, 0, 0], [1, 1, 1, 0, 0]),
        ("mrs_MR8_WriteLeveling_Enable",            [0, 0, 0, 1, 0, 0, 0, 0], [0, 0, 0, 1, 0]),
        ("mrs_MR8_WriteLeveling_Disable",           [0, 0, 0, 0, 0, 0, 0, 0], [0, 0, 0, 1, 0]),
    ]

    for name, op_bits, ma_bits in mrs_definitions:
        mrs_pattern = generate_pattern.generate_mrs_pattern(*op_bits, *ma_bits)
        mrs_pattern = mrs_pattern + generate_pattern.generate_nop_pattern(7)
        mrs_patterns[name] = mrs_pattern
        mrs_serdes[name] = generate_pattern.pattern_to_serdes_16to1(mrs_pattern)
        logs.append(f"{name}: OP={''.join(str(bit) for bit in op_bits)}, MA={''.join(str(bit) for bit in ma_bits)}")
        logs.append(f"{name} + NOP(7) SERDES: {mrs_serdes[name]}")

    pc = random.randint(0, 1)
    sid0 = random.randint(0, 1)
    sid1 = random.randint(0, 1)
    ba_bits = [random.randint(0, 1) for _ in range(4)]
    ca_bits = [random.randint(0, 1) for _ in range(5)]
    wr_pattern = generate_pattern.generate_write_pattern(
        pc=pc,
        sid0=sid0, sid1=sid1,
        ba0=ba_bits[0], ba1=ba_bits[1], ba2=ba_bits[2], ba3=ba_bits[3],
        ca0=ca_bits[0], ca1=ca_bits[1], ca2=ca_bits[2], ca3=ca_bits[3], ca4=ca_bits[4]
    )
    wr_pattern = (
        wr_pattern
        + generate_pattern.generate_nop_pattern(4)
        + generate_pattern.generate_pre_postamble_pattern(4)
        + generate_pattern.generate_tph_pattern(8, pc0_wdqs_toggle=True, tph_pattern='11')
        + generate_pattern.generate_pre_postamble_pattern(4)
    )
    wr_pattern_serdes = generate_pattern.pattern_to_serdes_16to1(wr_pattern)
    logs.append(f"WR bits: pc={pc}, sid0={sid0}, sid1={sid1}, BA={''.join(str(bit) for bit in ba_bits)}, CA={''.join(str(bit) for bit in ca_bits)}")
    logs.append(f"WR composite SERDES: {wr_pattern_serdes}")

    return mrs_patterns, mrs_serdes, wr_pattern, wr_pattern_serdes, logs


def create_ptn_mrs_wr_txt(mrs_patterns, mrs_serdes, wr_pattern, wr_serdes, logs):
    """Create ptn_mrs_wr.txt file."""
    with open('ptn_mrs_wr.txt', 'w') as f:
        f.write('mrs_patterns = {\n')
        for name, pattern in mrs_patterns.items():
            f.write(f'    "{name}": "{pattern}",\n')
        f.write('}\n\n')

        f.write('mrs_serdes = {\n')
        for name, serdes in mrs_serdes.items():
            f.write(f'    "{name}": "{serdes}",\n')
        f.write('}\n\n')

        f.write(f'wr_pattern = "{wr_pattern}"\n')
        f.write(f'wr_serdes = "{wr_serdes}"\n\n')
        f.write('# === Logs from MRS/WR generation ===\n')
        for log in logs:
            f.write(log + '\n')

if __name__ == "__main__":
    print("Generating CA training patterns...")
    ca_trn_pat_list, ca_trn_misr_list, ca_logs = generate_ca_patterns()

    print("Generating initialization patterns...")
    init_pattern, init_logs = generate_init_patterns()

    print("Creating ptn_ca.txt...")
    create_ptn_ca_txt(ca_trn_pat_list, ca_trn_misr_list, ca_logs)

    print("Creating ptn_init.txt...")
    create_ptn_init_txt(init_pattern, init_logs)

    print("Generating MRS/WR patterns...")
    mrs_patterns, mrs_serdes, wr_pattern, wr_serdes, mrs_wr_logs = generate_mrs_wr_patterns()

    print("Creating ptn_mrs_wr.txt...")
    create_ptn_mrs_wr_txt(mrs_patterns, mrs_serdes, wr_pattern, wr_serdes, mrs_wr_logs)

    print("Pattern generation completed!")
    print(f"CA patterns: {len(ca_trn_pat_list)} pins")
    print(f"Init pattern length: {len(init_pattern)} hex chars")
    print(f"MRS patterns: {len(mrs_patterns)} entries")
    print(f"WR pattern length: {len(wr_pattern)} hex chars")