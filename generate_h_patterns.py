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


def pack_frames_to_u48(frames):
    """Pack 30-bit frames into a list of 48-bit values."""
    bits = 0
    bit_count = 0
    u48_values = []

    for frame in frames:
        bits |= (frame << bit_count)
        bit_count += 30

        while bit_count >= 48:
            word = bits & ((1 << 48) - 1)
            u48_values.append(word)
            bits >>= 48
            bit_count -= 48

    if bit_count > 0:
        u48_values.append(bits)

    return u48_values


def u48_array_to_hex_frames(u48_values, num_frames):
    """Decode packed 48-bit values back into 30-bit frames."""
    bits = 0
    bit_count = 0
    frame_values = []
    word_iter = iter(u48_values)
    current_word = next(word_iter, 0)

    for _ in range(num_frames):
        while bit_count < 30:
            bits |= current_word << bit_count
            bit_count += 48
            current_word = next(word_iter, 0)

        frame = bits & ((1 << 30) - 1)
        frame_values.append(frame)
        bits >>= 30
        bit_count -= 30

    return frame_values


def format_u48_value(value):
    return f"0x{value:012X}"


def decode_frame(frame_int):
    bits = [(frame_int >> i) & 1 for i in range(30)]
    fields = [f"{name}={bits[i]}" for i, name in enumerate(BIT_NAMES)]
    return ''.join(str(bit) for bit in reversed(bits)), ', '.join(fields)


def debug_pattern(name, hex_pattern, u48_values):
    frames = hex_string_to_30bit_frames(hex_pattern)
    decoded_frames = u48_array_to_hex_frames(u48_values, len(frames))
    print(f"DEBUG {name}")
    print(f"  frames: {len(frames)}")
    print(f"  bits total: {len(frames) * 30}")
    print(f"  u48 words: {len(u48_values)}")
    print(f"  first u48: {format_u48_value(u48_values[0])}")
    for idx in range(min(4, len(frames))):
        original_bits, original_fields = decode_frame(frames[idx])
        decoded_bits, decoded_fields = decode_frame(decoded_frames[idx])
        original_hex = f"0x{frames[idx]:08X}"
        decoded_hex = f"0x{decoded_frames[idx]:08X}"
        print(f"  frame[{idx}] original={original_bits} hex={original_hex} decoded={decoded_bits} hex={decoded_hex}")
        if frames[idx] != decoded_frames[idx]:
            print(f"    MISMATCH at frame {idx}: original 0x{frames[idx]:08X} decoded 0x{decoded_frames[idx]:08X}")
    if len(frames) > 4:
        idx = len(frames) - 1
        original_bits, _ = decode_frame(frames[idx])
        decoded_bits, _ = decode_frame(decoded_frames[idx])
        original_hex = f"0x{frames[idx]:08X}"
        decoded_hex = f"0x{decoded_frames[idx]:08X}"
        print(f"  frame[{idx}] original={original_bits} hex={original_hex} decoded={decoded_bits} hex={decoded_hex}")
    print("")


def hex_to_u48_values(hex_str):
    """Convert hex string to packed 48-bit integer values."""
    frames = hex_string_to_30bit_frames(hex_str)
    return pack_frames_to_u48(frames)


def hex_to_u48_array(hex_str):
    """Convert hex string to packed 48-bit values."""
    return [format_u48_value(val) for val in hex_to_u48_values(hex_str)]


def generate_ca_training_patterns():
    """
    Generate CA training patterns like in test_ca_training_all_pins.py
    """
    patterns = {}
    ca_pins = ['R0', 'R1', 'R2', 'R3', 'R4', 'R5', 'R6', 'R7', 'R8', 'R9', 'R10', 'C0', 'C1', 'C2', 'C3', 'C4', 'C5', 'C6', 'C7']

    for pin in ca_pins:
        pattern = pattern_generator.generate_ca_training_pattern(pin, '00111100', clock_toggle=True, num_frames=48)
        patterns[f"ca_training_{pin}"] = pattern

    return patterns

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

def write_h_file(patterns, filename, header_guard):
    """
    Write patterns to .h file
    """
    with open(filename, 'w') as f:
        f.write(f"#ifndef {header_guard}\n")
        f.write(f"#define {header_guard}\n\n")
        f.write("#include <stdint.h>\n\n")

        for name, hex_pattern in patterns.items():
            frames = hex_string_to_30bit_frames(hex_pattern)
            u48_array = hex_to_u48_array(hex_pattern)
            length = len(u48_array)
            f.write(f"// Original hex pattern: {hex_pattern}\n")
            f.write(f"// Packed 30-bit frames: {len(frames)}, u48 words: {length}\n")
            f.write(f"// Bit packing: frame0 LSB-first in word0\n")
            f.write(f"static const uint64_t {name}[{length}] = {{\n")
            for i, val in enumerate(u48_array):
                if i < len(u48_array) - 1:
                    f.write(f"    {val},\n")
                else:
                    f.write(f"    {val}\n")
            f.write("};\n\n")

        f.write(f"#endif // {header_guard}\n")

if __name__ == "__main__":
    print("Generating patterns...")

    # Generate CA training patterns
    ca_patterns = generate_ca_training_patterns()
    print(f"Generated {len(ca_patterns)} CA training patterns")
    for name, pattern in ca_patterns.items():
        print(f"  {name}: {pattern[:32]}... (length: {len(pattern)//8} frames)")

    # Generate init patterns
    init_patterns = generate_init_patterns()
    print(f"Generated {len(init_patterns)} init patterns")
    for name, pattern in init_patterns.items():
        print(f"  {name}: {pattern[:32]}... (length: {len(pattern)//8} frames)")

    # Write CA training patterns to separate file
    write_h_file(ca_patterns, "pattern_ca_training.h", "PATTERN_CA_TRAINING_H")
    print("Wrote CA training patterns to pattern_ca_training.h")

    # Write init patterns to separate file
    write_h_file(init_patterns, "pattern_init.h", "PATTERN_INIT_H")
    print("Wrote init patterns to pattern_init.h")

    # Debug output: verify packed patterns and decode back
    print("\nDebugging CA training patterns...")
    for name, pattern in ca_patterns.items():
        debug_pattern(name, pattern, hex_to_u48_values(pattern))

    print("Debugging init patterns...")
    for name, pattern in init_patterns.items():
        debug_pattern(name, pattern, hex_to_u64_values(pattern))