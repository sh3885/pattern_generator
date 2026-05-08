#!/usr/bin/env python3
"""
Script to generate .h file with patterns from test scripts
"""

import pattern_generator
import analyze_serdes_16bit

# Disable debug mode for cleaner output
pattern_generator.DEBUG_MODE = False

BIT_NAMES = [
    'R0', 'R1', 'R2', 'R3', 'R4', 'R5', 'R6', 'R7', 'R8', 'R9', 'R10',
    'C0', 'C1', 'C2', 'C3', 'C4', 'C5', 'C6', 'C7',
    'HBM_CK', 'PC0_WDQS', 'PC1_WDQS', 'PC0_WTPH', 'PC1_WTPH', 'PC0_RTPH', 'PC1_RTPH',
    'PC0_RD_EN', 'PC1_RD_EN', 'reserved0', 'reserved1'
]


def hex_string_to_30bit_frames(hex_str):
    """Convert hex string with 8 chars per frame to a list of 30-bit frame ints."""
    frames = []
    for i in range(0, len(hex_str), 8):
        chunk = hex_str[i:i+8]
        if len(chunk) < 8:
            chunk = chunk.ljust(8, '0')
        frame_int = int(chunk, 16) & 0x3FFFFFFF
        frames.append(frame_int)
    return frames


def u48_array_to_hex_string(u48_values, original_length):
    """Decode packed 48-bit values back into a raw hex string."""
    hex_str = ''.join(f"{value:012X}" for value in u48_values)
    return hex_str[:original_length]


def format_u48_value(value):
    return f"0x{value:012X}"


def decode_frame(frame_int):
    bits = [(frame_int >> i) & 1 for i in range(30)]
    fields = [f"{name}={bits[i]}" for i, name in enumerate(BIT_NAMES)]
    return ''.join(str(bit) for bit in reversed(bits)), ', '.join(fields)


def debug_pattern(name, serdes_hex, u48_values):
    decoded_hex = u48_array_to_hex_string(u48_values, len(serdes_hex))
    print(f"DEBUG {name}")
    print(f"  original serdes hex length: {len(serdes_hex)}")
    print(f"  packed 48-bit words: {len(u48_values)}")
    print(f"  first u48: {format_u48_value(u48_values[0])}")
    print(f"  original == decoded: {serdes_hex == decoded_hex}")
    if serdes_hex != decoded_hex:
        print(f"  MISMATCH: decoded hex differs")
        print(f"  original prefix: {serdes_hex[:48]}")
        print(f"  decoded  prefix: {decoded_hex[:48]}")
    print("")


def hex_to_u48_values(hex_str):
    """Convert any hex string to packed 48-bit integer values.

    The returned list is ordered so the least-significant 48-bit chunk comes
    first, which matches the firmware array ordering used for packed SERDES
    output.
    """
    padded = hex_str
    if len(padded) % 12 != 0:
        padded = padded.ljust(((len(padded) + 11) // 12) * 12, '0')

    u48_values = []
    for i in range(0, len(padded), 12):
        chunk = padded[i:i+12]
        u48_values.append(int(chunk, 16))

    return u48_values[::-1]


def hex_to_u48_array(hex_str):
    """Convert any hex string to packed 48-bit values formatted as hex strings."""
    return [format_u48_value(val) for val in hex_to_u48_values(hex_str)]


def generate_ca_training_patterns():
    """
    Generate CA training patterns like in test_ca_training_all_pins.py
    Use 48 frames (12 clocks) of CA training.
    Returns patterns dict and misr list.
    """
    patterns = {}
    misr_list = []
    ca_pins = ['R0', 'R1', 'R2', 'R3', 'R4', 'R5', 'R6', 'R7', 'R8', 'R9', 'R10', 'C0', 'C1', 'C2', 'C3', 'C4', 'C5', 'C6', 'C7']

    for pin in ca_pins:
        training = pattern_generator.generate_ca_training_pattern(pin, '00111100', clock_toggle=True, num_frames=48)
        patterns[f"ca_training_{pin}"] = training
        
        # Calculate MISR values
        misr_full = pattern_generator.get_aword_misr(training)
        misr_list.append(misr_full)

    return patterns, misr_list

def generate_init_patterns():
    """
    Generate init patterns like in test_init_pattern.py
    """
    patterns = {}

    # PDE & CK Low 고정 (4nCK)
    pattern_1 = pattern_generator.generate_init_pde_pattern(num_clocks=4, clock_toggle=False, clock_value=0)
    patterns["init_pde_4nCK_low"] = pattern_1

    # PDE & CK toggle (20nCK)
    pattern_2 = pattern_generator.generate_init_pde_pattern(num_clocks=20, clock_toggle=True)
    patterns["init_pde_20nCK_toggle"] = pattern_2

    # PDX & CK toggle (48nCK)
    pattern_3 = pattern_generator.generate_init_pdx_pattern(num_clocks=48, clock_toggle=True)
    patterns["init_pdx_48nCK_toggle"] = pattern_3

    # Combined pattern
    combined_pattern = pattern_3 + pattern_2 + pattern_1
    patterns["init_combined"] = combined_pattern

    return patterns

def write_h_file(patterns, filename, header_guard, padding_ck_toggle=True, padding_ck_value=0):
    """
    Write patterns to .h file
    """
    with open(filename, 'w') as f:
        f.write(f"#ifndef {header_guard}\n")
        f.write(f"#define {header_guard}\n\n")
        f.write("#include <stdint.h>\n\n")

        for name, hex_pattern in patterns.items():
            serdes_pattern = pattern_generator.pattern_to_serdes_16to1(hex_pattern, padding_ck_toggle=padding_ck_toggle, padding_ck_value=padding_ck_value)
            u48_values = hex_to_u48_values(serdes_pattern)
            length = len(u48_values)
            f.write(f"// Original hex pattern: {hex_pattern}\n")
            f.write(f"// SERDES 16:1 hex length: {len(serdes_pattern)}\n")
            f.write(f"// Packed 48-bit words: {length}\n")
            f.write(f"static const uint64_t {name}[{length}] = {{\n")
            for i, val in enumerate(u48_values):
                formatted = format_u48_value(val)
                if i < len(u48_values) - 1:
                    f.write(f"    {formatted},\n")
                else:
                    f.write(f"    {formatted}\n")
            f.write("};\n\n")

        f.write(f"#endif // {header_guard}\n")


def write_ca_training_h_file(patterns, misr_list, filename, header_guard, padding_ck_toggle=False, padding_ck_value=0):
    """
    Write CA training patterns to a 2D array header file, including MISR values.
    """
    names = list(patterns.keys())
    row_count = len(names)
    with open(filename, 'w') as f:
        f.write(f"#ifndef {header_guard}\n")
        f.write(f"#define {header_guard}\n\n")
        f.write("#include <stdint.h>\n\n")

        pin_names = ", ".join('"{}"'.format(name.replace("ca_training_", "")) for name in names)
        f.write("// CA training pattern order: " + ", ".join(name.replace('ca_training_', '') for name in names) + "\n")
        f.write(f"static const char* ca_training_pin_names[{row_count}] = {{{pin_names}}};\n\n")

        # Convert all patterns to the same length
        all_u48_values = []
        lengths = []
        for name in names:
            hex_pattern = patterns[name]
            serdes_pattern = pattern_generator.pattern_to_serdes_16to1(hex_pattern, padding_ck_toggle=padding_ck_toggle, padding_ck_value=padding_ck_value)
            u48_values = hex_to_u48_values(serdes_pattern)
            all_u48_values.append(u48_values)
            lengths.append(len(u48_values))

        if len(set(lengths)) != 1:
            raise ValueError("All CA training patterns must have the same packed u48 length")

        array_length = lengths[0]
        f.write(f"// SERDES 16:1 hex length: {len(pattern_generator.pattern_to_serdes_16to1(patterns[names[0]], padding_ck_toggle=padding_ck_toggle, padding_ck_value=padding_ck_value))}\n")
        f.write(f"// Packed 48-bit words per CA pattern: {array_length}\n")
        f.write(f"static const uint64_t ca_training[{row_count}][{array_length}] = {{\n")

        for name, u48_values in zip(names, all_u48_values):
            f.write(f"    /* {name.replace('ca_training_', '')} */ {{\n")
            for i, val in enumerate(u48_values):
                formatted = format_u48_value(val)
                if i < len(u48_values) - 1:
                    f.write(f"        {formatted},\n")
                else:
                    f.write(f"        {formatted}\n")
            f.write("    }")
            if name != names[-1]:
                f.write(",\n")
            else:
                f.write("\n")
        f.write("};\n\n")

        # Add MISR array
        f.write(f"// MISR signatures for CA training patterns (12 clocks)\n")
        f.write(f"static const char* ca_training_misr[{row_count}] = {{\n")
        for i, misr in enumerate(misr_list):
            if i < len(misr_list) - 1:
                f.write(f"    \"{misr}\",\n")
            else:
                f.write(f"    \"{misr}\"\n")
        f.write("};\n\n")

        f.write(f"#endif // {header_guard}\n")

if __name__ == "__main__":
    print("Generating patterns...")

    # Generate CA training patterns
    ca_patterns, ca_misr_list = generate_ca_training_patterns()
    print(f"Generated {len(ca_patterns)} CA training patterns")
    for name, pattern in ca_patterns.items():
        print(f"  {name}: {pattern[:32]}... (length: {len(pattern)//8} frames)")

    # Generate init patterns
    init_patterns = generate_init_patterns()
    print(f"Generated {len(init_patterns)} init patterns")
    for name, pattern in init_patterns.items():
        print(f"  {name}: {pattern[:32]}... (length: {len(pattern)//8} frames)")

    # Write CA training patterns to separate file as a 2D array
    write_ca_training_h_file(ca_patterns, ca_misr_list, "pattern_ca_training.h", "PATTERN_CA_TRAINING_H", padding_ck_toggle=False, padding_ck_value=0)
    print("Wrote CA training patterns to pattern_ca_training.h")

    # Write init patterns to separate file
    write_h_file(init_patterns, "pattern_init.h", "PATTERN_INIT_H", padding_ck_toggle=True, padding_ck_value=0)
    print("Wrote init patterns to pattern_init.h")

    # Debug output: verify packed patterns and decode back
    print("\nDebugging CA training patterns...")
    for name, pattern in ca_patterns.items():
        serdes_pattern = pattern_generator.pattern_to_serdes_16to1(pattern, padding_ck_toggle=False, padding_ck_value=0)
        debug_pattern(name, serdes_pattern, hex_to_u48_values(serdes_pattern))

    print("Debugging init patterns...")
    for name, pattern in init_patterns.items():
        serdes_pattern = pattern_generator.pattern_to_serdes_16to1(pattern, padding_ck_toggle=True, padding_ck_value=0)
        debug_pattern(name, serdes_pattern, hex_to_u48_values(serdes_pattern))