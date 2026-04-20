"""
DRAM HBM4 Pattern Generator
"""

DEBUG_MODE = True  # Global debug flag

def generate_ca_training_pattern(target_ca, training_value, clock_toggle=True, num_frames=1):
    """
    Generate CA training pattern (hex string).
    
    Args:
        target_ca (str): CA pin to train (e.g., 'R0', 'C7')
        training_value: Target value (0, 1, or list of values for each frame)
        clock_toggle (bool): Toggle HBM_CK signal
        num_frames (int): Number of frames to generate
    
    Returns:
        str: Concatenated hex string (8 chars per frame)
    """
    BIT_NAMES = [
        'R0', 'R1', 'R2', 'R3', 'R4', 'R5', 'R6', 'R7', 'R8', 'R9', 'R10',
        'C0', 'C1', 'C2', 'C3', 'C4', 'C5', 'C6', 'C7',
        'HBM_CK', 'PC0_WDQS', 'PC1_WDQS', 'PC0_WTPH', 'PC1_WTPH', 'PC0_RTPH', 'PC1_RTPH', 'RD_EN'
    ]

    CA_INDICES = list(range(19))

    if target_ca not in BIT_NAMES[:19]:
        raise ValueError(f"Invalid target_ca: {target_ca}. Must be one of {BIT_NAMES[:19]}")

    target_index = BIT_NAMES.index(target_ca)

    if isinstance(training_value, str):
        if set(training_value).issubset({'0', '1'}):
            if len(training_value) == num_frames:
                training_values = [int(b) for b in training_value]
            elif len(training_value) == 1:
                training_values = [int(training_value)] * num_frames
            else:
                # Cyclic pattern: repeat pattern until num_frames is reached
                pattern = [int(b) for b in training_value]
                training_values = []
                for i in range(num_frames):
                    training_values.append(pattern[i % len(pattern)])
        else:
            raise ValueError("training_value string must contain only '0' or '1'")
    elif isinstance(training_value, int):
        training_values = [int(training_value)] * num_frames
    elif isinstance(training_value, list):
        if len(training_value) != num_frames:
            raise ValueError("training_value list length must match num_frames")
        training_values = [int(v) for v in training_value]
    else:
        raise ValueError("training_value must be int, str (sequence or single), or list")
    
    pattern_hex = ""
    ck_state = 0

    for frame_idx in range(num_frames):
        bits = [0] * 27

        # Set CA pins with default values
        bits[0] = 0  # R0: LOW
        bits[1] = 1  # R1: HIGH
        bits[2] = 0  # R2: LOW
        bits[3] = 1  # R3: HIGH
        
        # Apply target value for R0..R3 if needed
        if target_index < 4:
            bits[target_index] = training_values[frame_idx]

        for idx in range(4, 19):  # R4~R10, C0~C7
            if idx == target_index:
                bits[idx] = training_values[frame_idx]
            else:
                bits[idx] = 1

        # Clock: toggle every 2 frames
        if clock_toggle:
            ck_state = (frame_idx // 2) % 2
        else:
            ck_state = 0
        
        bits[19] = ck_state
        bits[20] = 0  # WDQS fixed to 0
        bits[21] = 0
        bits[22] = 0
        bits[23] = 0
        bits[24] = 0
        bits[25] = 0
        bits[26] = 0

        # Convert bits to int (LSB first)
        frame_int = 0
        for i, bit in enumerate(bits):
            frame_int |= (bit << i)

        # Append to hex string (8 chars padded)
        frame_hex = f"{frame_int:08X}"
        pattern_hex += frame_hex
        
        if DEBUG_MODE:
            bits_str = bin(frame_int)[2:].zfill(27)
            target_val = bits[target_index] if target_index < 19 else 0
            print(f"Frame {frame_idx}: {frame_hex} | {bits_str} | CK={bits[19]} WDQS={bits[20]} WTPH={bits[22]} RTPH={bits[24]} RDEN={bits[26]} {target_ca}={target_val}")

    return pattern_hex


def extract_aword_input_words(hex_pattern):
    """
    Convert 4-frame clock blocks into 38-bit AWORD MISR input words.
    """
    if len(hex_pattern) % 32 != 0:
        raise ValueError("hex_pattern length must be a multiple of 32 hex chars (4 frames per clock)")

    words = []
    num_clocks = len(hex_pattern) // 32

    for clk in range(num_clocks):
        block = hex_pattern[clk * 32:(clk + 1) * 32]
        low_frame = int(block[0:8], 16)
        high_frame = int(block[16:24], 16)

        low_bits = [(low_frame >> i) & 1 for i in range(27)]
        high_bits = [(high_frame >> i) & 1 for i in range(27)]

        word = 0
        bit_pos = 0

        # C0..C7 low/high pairs
        for c_idx in range(8):
            word |= low_bits[11 + c_idx] << bit_pos
            word |= high_bits[11 + c_idx] << (bit_pos + 1)
            bit_pos += 2

        # reserved bits (set to 0)
        bit_pos += 2

        # R9..R4 low/high pairs (descending)
        for r in range(9, 3, -1):
            word |= low_bits[r] << bit_pos
            word |= high_bits[r] << (bit_pos + 1)
            bit_pos += 2

        # R0, R3, R2, R1 low/high pairs
        for r in [0, 3, 2, 1]:
            word |= low_bits[r] << bit_pos
            word |= high_bits[r] << (bit_pos + 1)
            bit_pos += 2

        if bit_pos != 38:
            raise RuntimeError("AWORD input word construction failed: expected 38 bits")

        words.append(word)

    return words


def get_aword_misr_steps(hex_pattern, taps=[5, 4, 0], initial=0x2AAAAAAAAA, width=38):
    """
    Return step-by-step MISR register updates per AWORD input word.

    Algorithm per clock:
      1. Right-shift the register by 1.
      2. Place the shifted-out LSB into bit 37 (MSB).
      3. If MSB is 1, XOR bits 5, 4, 0 with 1.
      4. XOR the resulting register with the AWORD input word.
      5. Use that result as the next register.

    Returns:
      list of tuples: (clock_index, word, pre, lfsr, post)
    """
    mask = (1 << width) - 1
    register = initial & mask
    steps = []
    input_words = extract_aword_input_words(hex_pattern)

    xor_mask = 0
    for tap in taps:
        xor_mask |= 1 << tap

    for idx, word in enumerate(input_words):
        pre = register
        lost_bit = register & 1
        lfsr = (register >> 1) | (lost_bit << (width - 1))

        if (lfsr >> (width - 1)) & 1:
            lfsr ^= xor_mask

        post = lfsr ^ word
        post &= mask

        register = post
        steps.append((idx + 1, word, pre, lfsr, post))

    return steps


def get_aword_misr(hex_pattern, taps=[5, 4, 0], initial=0x2AAAAAAAAA, width=38):
    """
    Calculate AWORD MISR signature from hex pattern using 38-bit input words.
    """
    steps = get_aword_misr_steps(hex_pattern, taps=taps, initial=initial, width=width)
    return f"{steps[-1][4]:0{(width + 3) // 4}X}"


def pattern_to_serdes_16to1(hex_pattern, clock_state=0):
    """
    Convert CA training pattern to SERDES 16:1 format.
    
    Takes 16 frames of 27-bit data and converts to SERDES 16:1 output.
    Pads with default values if pattern is not a multiple of 16 frames.
    
    Args:
        hex_pattern (str): Hex pattern (8 chars per frame)
        clock_state (int): Initial CK state (0 or 1)
    
    Returns:
        str: SERDES 16:1 hex pattern (128 chars per 16 frames)
    """
    BIT_NAMES = [
        'R0', 'R1', 'R2', 'R3', 'R4', 'R5', 'R6', 'R7', 'R8', 'R9', 'R10',
        'C0', 'C1', 'C2', 'C3', 'C4', 'C5', 'C6', 'C7',
        'HBM_CK', 'PC0_WDQS', 'PC1_WDQS', 'PC0_WTPH', 'PC1_WTPH', 'PC0_RTPH', 'PC1_RTPH', 'RD_EN'
    ]
    
    # Padding frame: R0~R10, C0~C7 = HIGH, CK = toggled, rest = LOW
    def create_padding_frame(ck_val):
        bits = [0] * 27
        for i in range(11):  # R0~R10
            bits[i] = 1
        for i in range(11, 19):  # C0~C7
            bits[i] = 1
        bits[19] = ck_val  # HBM_CK (already toggled from outside)
        # bits[20:27] = 0 (WDQS, WTPH, RTPH, RD_EN all LOW) - default initialized to 0
        frame_int = 0
        for i, bit in enumerate(bits):
            frame_int |= (bit << i)
        return frame_int
    
    # Parse input pattern
    num_frames = len(hex_pattern) // 8
    frames = []
    
    for i in range(num_frames):
        frame_hex = hex_pattern[i * 8:(i + 1) * 8]
        frame_int = int(frame_hex, 16)
        frame_27bit = frame_int & 0x7FFFFFF  # Keep only 27 bits
        frames.append(frame_27bit)
    
    # Pad to multiple of 16 frames
    ck_state = clock_state
    while len(frames) % 16 != 0:
        padding_frame = create_padding_frame(ck_state)
        frames.append(padding_frame)
        ck_state = (ck_state + 1) % 2  # Toggle CK every frame for consistency
    
    # Convert to SERDES 16:1 format (16 frames -> 1 output block)
    serdes_pattern = ""
    for block_idx in range(len(frames) // 16):
        block_frames = frames[block_idx * 16:(block_idx + 1) * 16]
        
        # Concatenate 16 frames of 27 bits = 432 bits = 108 hex chars (round up)
        combined_bits = 0
        bit_pos = 0
        
        for frame in block_frames:
            for i in range(27):
                bit = (frame >> i) & 1
                combined_bits |= (bit << bit_pos)
                bit_pos += 1
        
        # Convert to hex (108 hex chars for 432 bits, padded to 128 for alignment)
        num_hex_chars = (bit_pos + 3) // 4
        serdes_hex = f"{combined_bits:0{num_hex_chars}X}"
        serdes_pattern += serdes_hex
    
    return serdes_pattern


def serdes_16to1_to_pattern(serdes_hex, num_frames=16):
    """
    Convert SERDES 16:1 hex output back into 27-bit frame hex strings.
    """
    serdes_int = int(serdes_hex, 16)
    frames = []
    mask = (1 << 27) - 1
    for frame_idx in range(num_frames):
        frame_value = (serdes_int >> (frame_idx * 27)) & mask
        frames.append(f"{frame_value:08X}")
    return frames


def test_ca_training():
    global DEBUG_MODE
    
    print("=== Test 1: R5 Training with sequence '0011' (4 frames, DEBUG ON) ===\n")
    DEBUG_MODE = True
    pattern1 = generate_ca_training_pattern("R5", "0011", clock_toggle=True, num_frames=4)
    print(f"\nFinal Hex: {pattern1}\n")
    print("="*80 + "\n")
    
    print("=== Test 2: R5 Training with sequence '1010' (4 frames, DEBUG OFF) ===\n")
    DEBUG_MODE = False
    pattern2 = generate_ca_training_pattern("R5", "1010", clock_toggle=True, num_frames=4)
    print(f"Final Hex: {pattern2}\n")
    print("="*80 + "\n")
    
    print("=== Test 3: C3 Training with sequence '00111100' (8 frames, DEBUG ON) ===\n")
    DEBUG_MODE = True
    pattern3 = generate_ca_training_pattern("C3", "00111100", clock_toggle=False, num_frames=8)
    print(f"\nFinal Hex: {pattern3}\n")
    print("="*80 + "\n")
    
    print("=== Test 5: R0 Training with sequence '00111100' repeated for 4 clocks (16 frames) ===\n")
    pattern5 = generate_ca_training_pattern("R0", "0011110000111100", clock_toggle=True, num_frames=16)
    print(f"Final Hex: {pattern5}\n")
    steps5 = get_aword_misr_steps(pattern5)
    for clock, word, pre, lfsr, post in steps5:
        print(
            f"Clock {clock}: MISR in = {pre:038b} | LFSR = {lfsr:038b} | "
            f"AWORD input = {word:038b} | MISR out = {post:038b}"
        )
    misr_sig5 = get_aword_misr(pattern5)
    print(f"MISR Signature: {misr_sig5} (hex)")
    print(f"MISR Signature: {int(misr_sig5, 16):038b} (38-bit)\n")
    print("="*80 + "\n")
    
    print("=== Test 6: SERDES 16:1 Conversion Test ===\n")
    DEBUG_MODE = False
    pattern6 = generate_ca_training_pattern("R0", "00111100", clock_toggle=True, num_frames=8)
    print(f"Original pattern (8 frames): {pattern6}")
    print(f"Original length: {len(pattern6)} hex chars\n")
    
    # Display original pattern frames in binary
    print("Original Pattern Frames (27-bit binary):")
    num_frames = len(pattern6) // 8
    orig_frames = []
    for i in range(num_frames):
        frame_hex = pattern6[i * 8:(i + 1) * 8]
        frame_int = int(frame_hex, 16) & 0x7FFFFFF
        orig_frames.append(frame_int)
        print(f"  Frame {i:2d}: {format(frame_int, '027b')} ({frame_hex})")
    
    # Perform SERDES conversion and collect padding frames
    serdes_pattern = pattern_to_serdes_16to1(pattern6, clock_state=0)
    print(f"\nSERDES 16:1 pattern (hex):\n{serdes_pattern}")
    print(f"SERDES length: {len(serdes_pattern)} hex chars\n")
    
    # Reconstruct frames including padding for visualization
    print("Frames after padding to 16 (27-bit binary):")
    all_frames = orig_frames.copy()
    ck_state = 0
    for i in range(8, 16):
        bits = [0] * 27
        for j in range(11):  # R0~R10
            bits[j] = 1
        for j in range(11, 19):  # C0~C7
            bits[j] = 1
        bits[19] = ck_state
        frame_int = 0
        for j, bit in enumerate(bits):
            frame_int |= (bit << j)
        all_frames.append(frame_int)
        print(f"  Frame {i:2d}: {format(frame_int, '027b')} (padding, CK={ck_state})")
        ck_state = (ck_state + 1) % 2
    
    # Verify SERDES round-trip conversion
    print("\nSERDES reverse-conversion verification:")
    reversed_frames = serdes_16to1_to_pattern(serdes_pattern, num_frames=16)
    for frame_idx in range(16):
        expected_hex = f"{all_frames[frame_idx]:08X}"
        actual_hex = reversed_frames[frame_idx]
        expected_bin = format(all_frames[frame_idx], '027b')
        actual_bin = format(int(actual_hex, 16), '027b')
        match = "OK" if actual_bin == expected_bin else "FAIL"
        print(f"  Frame {frame_idx:2d}: actual={actual_bin} expected={expected_bin} [{match}]")
    
    print("="*80 + "\n")


if __name__ == "__main__":
    test_ca_training()