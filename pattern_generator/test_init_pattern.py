#!/usr/bin/env python3
"""
Test script for initialization patterns: PDE, PDX, MRS
"""

import generate_pattern
import analyze_serdes_16bit

# Disable debug mode for cleaner output
generate_pattern.DEBUG_MODE = False

def test_init_patterns():
    print("=== Testing Initialization Patterns ===\n")
    
    # Init pull pattern
    # 1) PDE & CK Low 고정 (4nCK)
    pattern_1 = generate_pattern.generate_init_pde_pattern(num_clocks=4, clock_toggle=False, clock_value=0)
    print(f"1) PDE & CK Low 고정 (4nCK): {pattern_1}")
    # 2) PDE & CK toggle (20nCK)
    pattern_2 = generate_pattern.generate_init_pde_pattern(num_clocks=20, clock_toggle=True)
    print(f"2) PDE & CK toggle (20nCK): {pattern_2}")
    # 3) PDX & CK toggle (48nCK)
    pattern_3 = generate_pattern.generate_init_pdx_pattern(num_clocks=48, clock_toggle=True)
    print(f"3) PDX & CK toggle (48nCK): {pattern_3}")
    # 4) Pattern 16:1 Serdes 변환
    pattern = pattern_3 + pattern_2 + pattern_1  # Combine patterns for conversion
    serdes_pattern = generate_pattern.pattern_to_serdes_16to1(pattern)
    print(f"4) Combined Pattern 16:1 Serdes 변환: {serdes_pattern}\n\n")
    # 5) Serdes 패턴 분석
    analyze_serdes_16bit.analyze_serdes_pattern(serdes_pattern)

if __name__ == "__main__":
    test_init_patterns()