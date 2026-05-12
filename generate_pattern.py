"""
DRAM HBM4 Pattern Generator
"""

DEBUG_MODE = True  # Global debug flag


def _parse_bit_sequence(value, length, name):
    if isinstance(value, str):
        if len(value) != length or not set(value).issubset({'0', '1'}):
            raise ValueError(f"{name} must be a {length}-bit string of '0'/'1'")
        return [int(bit) for bit in value]
    if isinstance(value, list):
        if len(value) != length:
            raise ValueError(f"{name} list must have length {length}")
        return [int(bit) for bit in value]
    raise ValueError(f"{name} must be a bit string or bit list")


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
        'HBM_CK', 'PC0_WDQS', 'PC1_WDQS', 'PC0_WTPH', 'PC1_WTPH', 'PC0_RTPH', 'PC1_RTPH', 'PC0_RD_EN', 'PC1_RD_EN', 'reserved0', 'reserved1'
    ]

    CA_INDICES = list(range(19))

    if target_ca not in BIT_NAMES[:19]:
        raise ValueError(f"Invalid target_ca: {target_ca}. Must be one of {BIT_NAMES[:19]}")

    target_index = BIT_NAMES.index(target_ca)

    training_frame_count = max(0, num_frames - 4)

    if isinstance(training_value, str):
        if set(training_value).issubset({'0', '1'}):
            if len(training_value) == training_frame_count:
                training_values = [int(b) for b in training_value]
            elif len(training_value) == 1:
                training_values = [int(training_value)] * training_frame_count
            else:
                # Cyclic pattern: repeat pattern until training_frame_count is reached
                pattern = [int(b) for b in training_value]
                training_values = []
                for i in range(training_frame_count):
                    training_values.append(pattern[i % len(pattern)])
        else:
            raise ValueError("training_value string must contain only '0' or '1'")
    elif isinstance(training_value, int):
        training_values = [int(training_value)] * training_frame_count
    elif isinstance(training_value, list):
        if len(training_value) != training_frame_count:
            raise ValueError("training_value list length must match num_frames - 4")
        training_values = [int(v) for v in training_value]
    else:
        raise ValueError("training_value must be int, str (sequence or single), or list")
    
    pattern_hex = ""
    ck_state = 0

    for frame_idx in range(num_frames):
        bits = [0] * 30

        if frame_idx < 4:
            # First 4 frames: CA default values (no training applied)
            bits[0] = 0  # R0: LOW
            bits[1] = 1  # R1: HIGH
            bits[2] = 0  # R2: LOW
            bits[3] = 1  # R3: HIGH
            for idx in range(4, 19):  # R4~R10, C0~C7
                bits[idx] = 1
        else:
            # Frames 4+: Apply training value
            training_idx = frame_idx - 4
            bits[0] = 0  # R0: LOW
            bits[1] = 1  # R1: HIGH
            bits[2] = 0  # R2: LOW
            bits[3] = 1  # R3: HIGH
            
            # Apply target value for R0..R3 if needed
            if target_index < 4:
                bits[target_index] = training_values[training_idx]

            for idx in range(4, 19):  # R4~R10, C0~C7
                if idx == target_index:
                    bits[idx] = training_values[training_idx]
                else:
                    bits[idx] = 1

        # Clock: toggle every 2 frames
        if clock_toggle:
            ck_state = (frame_idx // 2) % 2
        else:
            ck_state = 0
        
        bits[19] = 1 - ck_state
        bits[20] = 0  # PC0_WDQS fixed to 0
        bits[21] = 0  # PC1_WDQS fixed to 0
        bits[22] = 0  # PC0_WTPH fixed to 0
        bits[23] = 0  # PC1_WTPH fixed to 0
        bits[24] = 0  # PC0_RTPH fixed to 0
        bits[25] = 0  # PC1_RTPH fixed to 0
        bits[26] = 0  # PC0_RD_EN fixed to 0
        bits[27] = 0  # PC1_RD_EN fixed to 0
        bits[28] = 0  # reserved0 fixed to 0
        bits[29] = 0  # reserved1 fixed to 0

        # Convert bits to int (LSB first)
        frame_int = 0
        for i, bit in enumerate(bits):
            frame_int |= (bit << i)

        # Append to hex string (8 chars padded)
        frame_hex = f"{frame_int:08X}"
        pattern_hex += frame_hex
        
        if DEBUG_MODE:
            bits_str = bin(frame_int)[2:].zfill(30)
            target_val = bits[target_index] if target_index < 19 else 0
            print(f"Frame {frame_idx}: {frame_hex} | {bits_str} | CK={bits[19]} WDQS={bits[20]} WTPH={bits[22]} RTPH={bits[24]} RD_EN={bits[26]},{bits[27]} {target_ca}={target_val}")

    return pattern_hex


def generate_init_pde_pattern(num_clocks=1, clock_toggle=True, clock_value=0):
    """
    Generate PDE initialization pattern (hex string).
    
    Creates 16 frames per clock with R0=0, R1=1, R2=0, R3=1, other CA=1, CK toggle or fixed, rest=0.
    
    Args:
        num_clocks (int): Number of clocks (each clock = 16 frames)
        clock_toggle (bool): Toggle HBM_CK signal (starts from low)
        clock_value (int): Fixed HBM_CK value (0 or 1) if not toggling
    
    Returns:
        str: Concatenated hex string (128 chars per clock)
    """
    BIT_NAMES = [
        'R0', 'R1', 'R2', 'R3', 'R4', 'R5', 'R6', 'R7', 'R8', 'R9', 'R10',
        'C0', 'C1', 'C2', 'C3', 'C4', 'C5', 'C6', 'C7',
        'HBM_CK', 'PC0_WDQS', 'PC1_WDQS', 'PC0_WTPH', 'PC1_WTPH', 'PC0_RTPH', 'PC1_RTPH', 'PC0_RD_EN', 'PC1_RD_EN', 'reserved0', 'reserved1'
    ]
    
    pattern_hex = ""
    ck_state = 0  # Start from low
    
    for clock_idx in range(num_clocks):
        for frame_idx in range(16):
            bits = [0] * 30
            
            # CA signals: R0=0, R1=1, R2=0, R3=1, others=1
            bits[0] = 0  # R0
            bits[1] = 1  # R1
            bits[2] = 0  # R2
            bits[3] = 1  # R3
            for i in range(4, 19):  # R4~R10, C0~C7
                bits[i] = 1
            
            # CK
            if clock_toggle:
                ck_state = (frame_idx // 8) % 2  # Toggle: 0,1,0,1,...
            else:
                ck_state = clock_value
            bits[19] = 1 - ck_state
            
            # Rest are 0 (default)
            
            # Convert to int
            frame_int = 0
            for i, bit in enumerate(bits):
                frame_int |= (bit << i)
            
            frame_hex = f"{frame_int:08X}"
            pattern_hex += frame_hex
            
            if DEBUG_MODE:
                bits_str = bin(frame_int)[2:].zfill(30)
                print(f"PDE Clock {clock_idx} Frame {frame_idx}: {frame_hex} | {bits_str} | CK={bits[19]}")
    
    return pattern_hex


def generate_pde_pattern(num_clocks=1, clock_toggle=True, clock_value=0):
    """
    Generate PDE pattern with 4-frame CK toggle unit.

    Creates 4 frames per clock with R0=0, R1=1, R2=0, R3=1, other CA=1,
    CK toggles every 2 frames when clock_toggle=True, otherwise uses fixed CK.
    """
    BIT_NAMES = [
        'R0', 'R1', 'R2', 'R3', 'R4', 'R5', 'R6', 'R7', 'R8', 'R9', 'R10',
        'C0', 'C1', 'C2', 'C3', 'C4', 'C5', 'C6', 'C7',
        'HBM_CK', 'PC0_WDQS', 'PC1_WDQS', 'PC0_WTPH', 'PC1_WTPH', 'PC0_RTPH', 'PC1_RTPH', 'PC0_RD_EN', 'PC1_RD_EN', 'reserved0', 'reserved1'
    ]
    
    pattern_hex = ""
    ck_state = 0  # Start from low
    
    for clock_idx in range(num_clocks):
        for frame_idx in range(4):
            bits = [0] * 30
            
            # CA signals: R0=0, R1=1, R2=0, R3=1, others=1
            bits[0] = 0  # R0
            bits[1] = 1  # R1
            bits[2] = 0  # R2
            bits[3] = 1  # R3
            for i in range(4, 19):  # R4~R10, C0~C7
                bits[i] = 1
            
            # CK toggles every 2 frames when enabled
            if clock_toggle:
                ck_state = (frame_idx // 2) % 2
            else:
                ck_state = clock_value
            bits[19] = 1 - ck_state
            
            # Rest are 0 (default)
            
            frame_int = 0
            for i, bit in enumerate(bits):
                frame_int |= (bit << i)
            
            frame_hex = f"{frame_int:08X}"
            pattern_hex += frame_hex
            
            if DEBUG_MODE:
                bits_str = bin(frame_int)[2:].zfill(30)
                print(f"PDE4 Clock {clock_idx} Frame {frame_idx}: {frame_hex} | {bits_str} | CK={bits[19]}")
    
    return pattern_hex


def generate_init_pdx_pattern(num_clocks=1, clock_toggle=True, clock_value=0):
    """
    Generate PDX initialization pattern (hex string).
    
    Creates 16 frames per clock with all CA=1, CK toggle or fixed, rest=0.
    
    Args:
        num_clocks (int): Number of clocks (each clock = 16 frames)
        clock_toggle (bool): Toggle HBM_CK signal (starts from low)
        clock_value (int): Fixed HBM_CK value (0 or 1) if not toggling
    
    Returns:
        str: Concatenated hex string (128 chars per clock)
    """
    BIT_NAMES = [
        'R0', 'R1', 'R2', 'R3', 'R4', 'R5', 'R6', 'R7', 'R8', 'R9', 'R10',
        'C0', 'C1', 'C2', 'C3', 'C4', 'C5', 'C6', 'C7',
        'HBM_CK', 'PC0_WDQS', 'PC1_WDQS', 'PC0_WTPH', 'PC1_WTPH', 'PC0_RTPH', 'PC1_RTPH', 'PC0_RD_EN', 'PC1_RD_EN', 'reserved0', 'reserved1'
    ]
    
    pattern_hex = ""
    ck_state = 0  # Start from low
    
    for clock_idx in range(num_clocks):
        for frame_idx in range(16):
            bits = [0] * 30
            
            # All CA signals = 1
            for i in range(19):
                bits[i] = 1
            
            # CK
            if clock_toggle:
                ck_state = (frame_idx // 8) % 2  # Toggle: 0,1,0,1,...
            else:
                ck_state = clock_value
            bits[19] = 1 - ck_state
            
            # Rest are 0 (default)
            
            # Convert to int
            frame_int = 0
            for i, bit in enumerate(bits):
                frame_int |= (bit << i)
            
            frame_hex = f"{frame_int:08X}"
            pattern_hex += frame_hex
            
            if DEBUG_MODE:
                bits_str = bin(frame_int)[2:].zfill(30)
                print(f"PDX Clock {clock_idx} Frame {frame_idx}: {frame_hex} | {bits_str} | CK={bits[19]}")
    
    return pattern_hex


def generate_mrs_pattern(op0, op1, op2, op3, op4, op5, op6, op7, ma0, ma1, ma2, ma3, ma4):
    """Generate a 4-frame MRS pattern."""
    op_bits = [int(op0), int(op1), int(op2), int(op3), int(op4), int(op5), int(op6), int(op7)]
    ma_bits = [int(ma0), int(ma1), int(ma2), int(ma3), int(ma4)]

    pattern_hex = ''
    for frame_idx in range(4):
        bits = [0] * 30
        for i in range(11):
            bits[i] = 1

        if frame_idx < 2:
            bits[11] = 0
            bits[12] = 0
            bits[13] = 0
            bits[14] = ma_bits[4]
            bits[15] = op_bits[5]
            bits[16] = op_bits[6]
            bits[17] = op_bits[7]
            bits[18] = ma_bits[0]
        else:
            bits[11] = ma_bits[1]
            bits[12] = ma_bits[2]
            bits[13] = ma_bits[3]
            bits[14] = op_bits[0]
            bits[15] = op_bits[1]
            bits[16] = op_bits[2]
            bits[17] = op_bits[3]
            bits[18] = op_bits[4]

        ck_state = (frame_idx // 2) % 2
        bits[19] = 1 - ck_state

        frame_int = 0
        for i, bit in enumerate(bits):
            frame_int |= (bit << i)
        pattern_hex += f"{frame_int:08X}"

    return pattern_hex


def generate_nop_pattern(num_nops=1):
    """Generate repeated 4-frame NOP patterns."""
    if num_nops < 1:
        return ''

    pattern_hex = ''
    for _ in range(num_nops):
        for frame_idx in range(4):
            bits = [0] * 30
            for i in range(10):
                bits[i] = 1
            bits[10] = 0
            for i in range(11, 19):
                bits[i] = 1

            ck_state = (frame_idx // 2) % 2
            bits[19] = 1 - ck_state

            frame_int = 0
            for i, bit in enumerate(bits):
                frame_int |= (bit << i)
            pattern_hex += f"{frame_int:08X}"

    return pattern_hex


def generate_write_pattern(pc, sid0, sid1, ba0, ba1, ba2, ba3, ca0, ca1, ca2, ca3, ca4):
    """Generate a 4-frame WR pattern."""
    pc = int(pc) & 1
    sid0 = int(sid0)
    sid1 = int(sid1)
    ba0 = int(ba0)
    ba1 = int(ba1)
    ba2 = int(ba2)
    ba3 = int(ba3)
    ca0 = int(ca0)
    ca1 = int(ca1)
    ca2 = int(ca2)
    ca3 = int(ca3)
    ca4 = int(ca4)

    pattern_hex = ''
    for frame_idx in range(4):
        bits = [0] * 30
        for i in range(11):
            bits[i] = 1

        if frame_idx < 2:
            bits[11] = 1
            bits[12] = 0
            bits[13] = 0
            bits[14] = 0
            bits[15] = pc
            bits[16] = sid0
            bits[17] = sid1
            bits[18] = ba0
        else:
            bits[11] = ba1
            bits[12] = ba2
            bits[13] = ba3
            bits[14] = ca0
            bits[15] = ca1
            bits[16] = ca2
            bits[17] = ca3
            bits[18] = ca4

        ck_state = (frame_idx // 2) % 2
        bits[19] = 1 - ck_state

        frame_int = 0
        for i, bit in enumerate(bits):
            frame_int |= (bit << i)
        pattern_hex += f"{frame_int:08X}"

    return pattern_hex


def generate_pre_postamble_pattern(wck, pc0_wdqs_toggle=False, pc1_wdqs_toggle=False):
    """Generate a pre/postamble pattern for WCK frames."""
    if wck < 1:
        return ''

    pattern_hex = ''
    for frame_idx in range(wck):
        bits = [0] * 30
        for i in range(19):
            bits[i] = 1

        bits[19] = 0
        bits[20] = (frame_idx % 2) if pc0_wdqs_toggle else 0
        bits[21] = (frame_idx % 2) if pc1_wdqs_toggle else 0

        frame_int = 0
        for i, bit in enumerate(bits):
            frame_int |= (bit << i)
        pattern_hex += f"{frame_int:08X}"

    return pattern_hex


def generate_tph_pattern(wck, pc0_wdqs_toggle=False, pc1_wdqs_toggle=False, tph_pattern='010'):
    """Generate a TPH pattern for WCK frames."""
    if pc0_wdqs_toggle and pc1_wdqs_toggle:
        raise ValueError('pc0 and pc1 cannot both be true')
    if wck < 1:
        return ''

    tph_bits = _parse_bit_sequence(tph_pattern, len(tph_pattern), 'TPH')
    pattern_hex = ''

    for frame_idx in range(wck):
        bits = [0] * 30
        for i in range(19):
            bits[i] = 1

        bits[19] = 0
        bits[20] = (frame_idx % 2) if pc0_wdqs_toggle else 0
        bits[21] = (frame_idx % 2) if pc1_wdqs_toggle else 0

        if pc0_wdqs_toggle:
            bits[22] = tph_bits[frame_idx % len(tph_bits)]
        elif pc1_wdqs_toggle:
            bits[23] = tph_bits[frame_idx % len(tph_bits)]

        frame_int = 0
        for i, bit in enumerate(bits):
            frame_int |= (bit << i)
        pattern_hex += f"{frame_int:08X}"

    return pattern_hex


def generate_init_mrs_pattern(op0=0, op1=0, op2=0, op3=0, op4=0, op5=0, op6=0, op7=0,
                              ma0=0, ma1=0, ma2=0, ma3=0, ma4=0):
    """Generate a default MRS pattern using OP/MA bits."""
    return generate_mrs_pattern(op0, op1, op2, op3, op4, op5, op6, op7,
                                 ma0, ma1, ma2, ma3, ma4)


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

        low_bits = [(low_frame >> i) & 1 for i in range(28)]
        high_bits = [(high_frame >> i) & 1 for i in range(28)]

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


def validate_golden_ca_data():
    """
    Validate current CA pattern generation against golden reference data.
    
    Returns True if current output matches golden data, False otherwise.
    Prints validation results.
    """
    import os
    
    # Generate current R0 pattern
    training = generate_ca_training_pattern('R0', '00111100', clock_toggle=True, num_frames=48)
    current_serdes = pattern_to_serdes_16to1(training, padding_ck_toggle=True, padding_ck_value=0)
    
    # Golden SERDES from ca_log.txt
    golden_serdes = "0000000000000000000000000000000000000000CCCCFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF" + \
                   "FFFFFFFFFFFFFFFFFFFFFFFFFFFF0000FFFF3C3C0000000000000000000000000000000000000000" + \
                   "CCCCFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF0000FFFF3C3C" + \
                   "0000000000000000000000000000000000000000CCCCFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF" + \
                   "FFFFFFFFFFFFFFFFFFFFFFFFFFFF0000FFFF3C3C"
    
    # Check SERDES match
    serdes_match = current_serdes == golden_serdes
    
    # Check header first element
    try:
        # Read current header file
        header_file = os.path.join(os.path.dirname(__file__), "pattern_ca_training.h")
        with open(header_file, 'r') as f:
            content = f.read()
        
        # Find R0 first element
        import re
        r0_match = re.search(r'/\* R0 \*/ \{\s*\n\s*([^,\n]+)', content)
        if r0_match:
            current_first_element = r0_match.group(1).strip()
            header_match = current_first_element == "0x0000FFFF3C3C"
        else:
            header_match = False
            current_first_element = "NOT_FOUND"
            
    except Exception as e:
        print(f"Error reading header file: {e}")
        header_match = False
        current_first_element = "ERROR"
    
    # Print results
    print("=== CA Pattern Golden Data Validation ===")
    print(f"SERDES match: {'✓' if serdes_match else '✗'}")
    if not serdes_match:
        print(f"  Current: {current_serdes[:50]}...")
        print(f"  Golden:  {golden_serdes[:50]}...")
    
    print(f"Header R0 first element match: {'✓' if header_match else '✗'}")
    if not header_match:
        print(f"  Current: {current_first_element}")
        print(f"  Golden:  0x0000FFFF3C3C")
    
    overall_match = serdes_match and header_match
    print(f"Overall validation: {'PASS' if overall_match else 'FAIL'}")
    
    return overall_match


def pattern_to_serdes_16to1(hex_pattern, padding_ck_toggle=True, padding_ck_value=0):
    """
    Convert CA training pattern to SERDES 16:1 format.
    
    Takes 16 frames of 30-bit data and converts to SERDES 16:1 format.
    The int value layout (bit-wise):
    - bit 0-15: R0's 16-bit value (LSB position in final hex string)
    - bit 16-31: R1's 16-bit value
    - ...
    - bit 464-479: reserved1's 16-bit value (MSB position in final hex string)
    
    Args:
        hex_pattern (str): Hex pattern (8 chars per frame)
        padding_ck_toggle (bool): Whether to toggle HBM_CK for padding frames
        padding_ck_value (int): Fixed HBM_CK value (0 or 1) for padding if not toggling
    
    Returns:
        str: SERDES 16:1 hex pattern (120 hex chars per 16 frames)
    """
    # Padding frame: R0~R10, C0~C7 = HIGH, CK = toggled, rest = LOW
    def create_padding_frame(ck_val):
        bits = [0] * 30
        for i in range(11):  # R0~R10
            bits[i] = 1
        for i in range(11, 19):  # C0~C7
            bits[i] = 1
        bits[19] = 1 - ck_val  # HBM_CK
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
        frame_30bit = frame_int & 0x3FFFFFFF  # Keep only 30 bits
        frames.append(frame_30bit)
    
    # Pad to multiple of 16 frames
    if len(frames) > 0:
        last_ck = (frames[-1] >> 19) & 1
    else:
        last_ck = 0
    while len(frames) % 16 != 0:
        if padding_ck_toggle:
            ck_val = (last_ck + 1) % 2
        else:
            ck_val = padding_ck_value
        padding_frame = create_padding_frame(ck_val)
        frames.append(padding_frame)
        last_ck = ck_val
    
    # Convert to SERDES 16:1 format (16 frames -> 1 output block = 480 bits = 120 hex chars)
    serdes_blocks_list = []  # Collect blocks first
    for block_idx in range(len(frames) // 16):
        block_frames = frames[block_idx * 16:(block_idx + 1) * 16]
        
        # Build int with 480 bits (30 bits per position × 16 frames)
        # bit 0-15: R0's 16-bit value (collected from 16 frames, LSB first)
        # bit 16-31: R1's 16-bit value
        # ...
        # bit 464-479: reserved1's 16-bit value (MSB)
        combined_bits = 0
        bit_pos = 0
        
        for bit_idx in range(30):  # For each bit position (R0 ~ reserved1)
            for frame_idx in range(16):  # For each of 16 frames
                bit = (block_frames[frame_idx] >> bit_idx) & 1
                combined_bits |= (bit << bit_pos)
                bit_pos += 1
        
        # Convert to 120-char hex string
        serdes_hex = f"{combined_bits:0120X}"
        serdes_blocks_list.append(serdes_hex)
    
    # Reverse blocks so LSB block comes last in hex string
    # Block 0 (bit 0-479) should be at the end (LSB)
    # Block 2 (bit 960-1439) should be at the start (MSB)
    serdes_blocks_list.reverse()
    serdes_pattern = "".join(serdes_blocks_list)
    
    return serdes_pattern


def serdes_16to1_to_pattern(serdes_hex, num_frames=None):
    """
    Convert SERDES 16:1 hex output back into 30-bit frame hex strings.
    
    Reverses the pattern_to_serdes_16to1 encoding:
    - Hex string's start (MSB) → Block N-1 (higher frame indices)
    - Hex string's end (LSB) → Block 0 (lower frame indices, frames 0-15)
    
    Args:
        serdes_hex (str): SERDES 16:1 hex string (120 chars per 16 frames)
        num_frames (int, optional): Number of frames to reconstruct.
    
    Returns:
        list[str]: Decoded frame hex strings (8 chars per frame).
    """
    if len(serdes_hex) % 120 != 0:
        raise ValueError("serdes_hex length must be a multiple of 120 hex chars (480 bits per block)")
    
    num_blocks = len(serdes_hex) // 120
    total_frames = num_blocks * 16
    if num_frames is None:
        num_frames = total_frames
    elif num_frames > total_frames:
        raise ValueError(f"num_frames ({num_frames}) exceeds decoded frame count ({total_frames})")
    
    frames = []
    
    # Blocks are reversed in hex string (Block 0 at end = LSB)
    # So we need to process from the end
    for block_idx_in_hex in range(num_blocks):
        # Map hex position to frame block position (reversed)
        block_idx_in_frames = num_blocks - 1 - block_idx_in_hex
        
        # Extract block from hex string
        block_hex = serdes_hex[block_idx_in_hex * 120:(block_idx_in_hex + 1) * 120]
        block_int = int(block_hex, 16)
        
        # Extract 16 frames from the 480-bit int
        for frame_idx_in_block in range(16):
            frame_value = 0
            
            # Reconstruct each of 30 bits for this frame
            for bit_idx in range(30):
                # Calculate bit position in the int:
                # bit_idx 0 (R0): bits 0-15 in int (frame 0-15 values)
                # For frame_idx_in_block, the bit within its 16-bit group is at offset frame_idx_in_block
                bit_pos_in_int = bit_idx * 16 + frame_idx_in_block
                bit = (block_int >> bit_pos_in_int) & 1
                frame_value |= (bit << bit_idx)
            
            # Add frame at correct position
            frame_global_idx = block_idx_in_frames * 16 + frame_idx_in_block
            frames.append((frame_global_idx, f"{frame_value:08X}"))
    
    # Sort by frame index and extract hex strings
    frames.sort(key=lambda x: x[0])
    result = [hex_str for _, hex_str in frames]
    
    return result[:num_frames]


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
    
    print("=== Test 5: R0 Training with sequence '00111100' repeated for 12 clocks (48 frames) ===\n")
    pattern5 = generate_ca_training_pattern("R0", "0011110000111100", clock_toggle=True, num_frames=48)
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
    print("Original Pattern Frames (28-bit binary):")
    num_frames = len(pattern6) // 8
    orig_frames = []
    for i in range(num_frames):
        frame_hex = pattern6[i * 8:(i + 1) * 8]
        frame_int = int(frame_hex, 16) & 0xFFFFFFF
        orig_frames.append(frame_int)
        print(f"  Frame {i:2d}: {format(frame_int, '028b')} ({frame_hex})")
    
    # Perform SERDES conversion and collect padding frames
    serdes_pattern = pattern_to_serdes_16to1(pattern6, padding_ck_toggle=False, padding_ck_value=0)
    print(f"\nSERDES 16:1 pattern (hex):\n{serdes_pattern}")
    print(f"SERDES length: {len(serdes_pattern)} hex chars\n")
    
    # Reconstruct frames including padding for visualization
    print("Frames after padding to 16 (28-bit binary):")
    all_frames = orig_frames.copy()
    ck_state = 0
    for i in range(8, 16):
        bits = [0] * 28
        for j in range(11):  # R0~R10
            bits[j] = 1
        for j in range(11, 19):  # C0~C7
            bits[j] = 1
        bits[19] = 1 - ck_state
        frame_int = 0
        for j, bit in enumerate(bits):
            frame_int |= (bit << j)
        all_frames.append(frame_int)
        print(f"  Frame {i:2d}: {format(frame_int, '028b')} (padding, CK={ck_state})")
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