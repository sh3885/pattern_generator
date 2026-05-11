#!/usr/bin/env python3
"""
Generate CA training and initialization patterns for HBM testing.
Creates ptn_ca.txt and ptn_init.txt files.
"""

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

    for pin in ca_pins:
        training = generate_pattern.generate_ca_training_pattern(pin, '00111100', clock_toggle=True, num_frames=48)
        serdes_pattern = generate_pattern.pattern_to_serdes_16to1(training, padding_ck_toggle=True, padding_ck_value=0)
        ca_trn_pat_list.append(f"0x{serdes_pattern.upper()}")

        final_misr = generate_pattern.get_aword_misr(training[32:])  # Exclude first 4 frames for MISR
        ca_trn_misr_list.append(final_misr)

    return ca_trn_pat_list, ca_trn_misr_list

def generate_init_patterns():
    """Generate initialization patterns (PDE + PDX)."""
    # Init pull pattern
    # 1) PDE & CK Low 고정 (4nCK)
    pattern_1 = generate_pattern.generate_init_pde_pattern(num_clocks=4, clock_toggle=False, clock_value=0)
    # 2) PDE & CK toggle (20nCK)
    pattern_2 = generate_pattern.generate_init_pde_pattern(num_clocks=20, clock_toggle=True)
    # 3) PDX & CK toggle (48nCK)
    pattern_3 = generate_pattern.generate_init_pdx_pattern(num_clocks=48, clock_toggle=True)

    # Combine patterns for conversion
    combined_pattern = pattern_3 + pattern_2 + pattern_1
    serdes_pattern = generate_pattern.pattern_to_serdes_16to1(combined_pattern)

    return serdes_pattern

def create_ptn_ca_txt(ca_trn_pat_list, ca_trn_misr_list):
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

def create_ptn_init_txt(init_pattern):
    """Create ptn_init.txt file."""
    with open('ptn_init.txt', 'w') as f:
        f.write(f'init_pattern = "{init_pattern}"\n')

if __name__ == "__main__":
    print("Generating CA training patterns...")
    ca_trn_pat_list, ca_trn_misr_list = generate_ca_patterns()

    print("Generating initialization patterns...")
    init_pattern = generate_init_patterns()

    print("Creating ptn_ca.txt...")
    create_ptn_ca_txt(ca_trn_pat_list, ca_trn_misr_list)

    print("Creating ptn_init.txt...")
    create_ptn_init_txt(init_pattern)

    print("Pattern generation completed!")
    print(f"CA patterns: {len(ca_trn_pat_list)} pins")
    print(f"Init pattern length: {len(init_pattern)} hex chars")