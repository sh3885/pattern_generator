#!/usr/bin/env python3
"""
Test script for initialization patterns: PDE, PDX, MRS
"""

import pattern_generator

# Disable debug mode for cleaner output
pattern_generator.DEBUG_MODE = False

def test_init_patterns():
    print("=== Testing Initialization Patterns ===\n")
    
    # Test PDE pattern - 1 clock
    print("1. PDE Pattern (1 clock, CK toggle):")
    pde_pattern = pattern_generator.generate_init_pde_pattern(num_clocks=1, clock_toggle=True)
    print(f"PDE Hex: {pde_pattern}")
    print(f"Length: {len(pde_pattern)} chars (should be 128 for 1 clock)")
    # SERDES conversion
    pde_serdes = pattern_generator.pattern_to_serdes_16to1(pde_pattern, padding_ck_toggle=False, padding_ck_value=0)
    print(f"PDE SERDES: {pde_serdes}")
    print(f"SERDES Length: {len(pde_serdes)} chars (should be 128, no padding added)\n")
    
    # Test PDX pattern - 1 clock
    print("2. PDX Pattern (1 clock, CK toggle):")
    pdx_pattern = pattern_generator.generate_init_pdx_pattern(num_clocks=1, clock_toggle=True)
    print(f"PDX Hex: {pdx_pattern}")
    print(f"Length: {len(pdx_pattern)} chars (should be 128 for 1 clock)")
    # SERDES conversion
    pdx_serdes = pattern_generator.pattern_to_serdes_16to1(pdx_pattern, padding_ck_toggle=False, padding_ck_value=0)
    print(f"PDX SERDES: {pdx_serdes}")
    print(f"SERDES Length: {len(pdx_serdes)} chars (should be 128, no padding added)\n")
    
    # Test PDE pattern - 2 clocks
    print("3. PDE Pattern (2 clocks, CK toggle):")
    pde_2clocks = pattern_generator.generate_init_pde_pattern(num_clocks=2, clock_toggle=True)
    print(f"PDE 2 Clocks Hex: {pde_2clocks}")
    print(f"Length: {len(pde_2clocks)} chars (should be 256 for 2 clocks)")
    # SERDES conversion
    pde_2_serdes = pattern_generator.pattern_to_serdes_16to1(pde_2clocks, padding_ck_toggle=False, padding_ck_value=0)
    print(f"PDE 2 Clocks SERDES: {pde_2_serdes}")
    print(f"SERDES Length: {len(pde_2_serdes)} chars (should be 256, no padding added)\n")
    
    # Test PDX pattern - 2 clocks
    print("4. PDX Pattern (2 clocks, CK toggle):")
    pdx_2clocks = pattern_generator.generate_init_pdx_pattern(num_clocks=2, clock_toggle=True)
    print(f"PDX 2 Clocks Hex: {pdx_2clocks}")
    print(f"Length: {len(pdx_2clocks)} chars (should be 256 for 2 clocks)")
    # SERDES conversion
    pdx_2_serdes = pattern_generator.pattern_to_serdes_16to1(pdx_2clocks, padding_ck_toggle=False, padding_ck_value=0)
    print(f"PDX 2 Clocks SERDES: {pdx_2_serdes}")
    print(f"SERDES Length: {len(pdx_2_serdes)} chars (should be 256, no padding added)\n")
    
    # Test with fixed CK - 1 clock
    print("5. PDE Pattern (1 clock, CK fixed low):")
    pde_fixed = pattern_generator.generate_init_pde_pattern(num_clocks=1, clock_toggle=False, clock_value=0)
    print(f"PDE Fixed Low: {pde_fixed[:32]}... (truncated)")
    print(f"Length: {len(pde_fixed)} chars")
    # SERDES conversion
    pde_fixed_serdes = pattern_generator.pattern_to_serdes_16to1(pde_fixed, padding_ck_toggle=False, padding_ck_value=0)
    print(f"PDE Fixed SERDES: {pde_fixed_serdes}")
    print(f"SERDES Length: {len(pde_fixed_serdes)} chars\n")
    
    print("6. PDX Pattern (1 clock, CK fixed high):")
    pdx_fixed = pattern_generator.generate_init_pdx_pattern(num_clocks=1, clock_toggle=False, clock_value=1)
    print(f"PDX Fixed High: {pdx_fixed[:32]}... (truncated)")
    print(f"Length: {len(pdx_fixed)} chars")
    # SERDES conversion
    pdx_fixed_serdes = pattern_generator.pattern_to_serdes_16to1(pdx_fixed, padding_ck_toggle=False, padding_ck_value=0)
    print(f"PDX Fixed SERDES: {pdx_fixed_serdes}")
    print(f"SERDES Length: {len(pdx_fixed_serdes)} chars\n")
    
    # MRS is TBD
    print("7. MRS Pattern: TBD\n")

if __name__ == "__main__":
    test_init_patterns()