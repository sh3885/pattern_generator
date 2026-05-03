import pattern_generator

def analyze_serdes_pattern(hex_value):
    """
    Analyze SERDES 16:1 hex pattern and print all frames and bit details.
    
    Args:
        hex_value (str): SERDES 16:1 hex string
    """
    # Calculate number of frames: each 16 frames = 120 hex chars (480 bits)
    hex_chars_per_block = 120  # 30 bits * 16 frames / 4 = 120
    num_blocks = len(hex_value) // hex_chars_per_block
    num_frames = num_blocks * 16
    
    # Decode all frames
    frames = pattern_generator.serdes_16to1_to_pattern(hex_value, num_frames=num_frames)
    
    print('=== SERDES 16:1 전체 프레임 디코드 ===')
    print('총 프레임 개수:', len(frames))
    
    for idx, frame_hex in enumerate(frames):
        frame_int = int(frame_hex, 16)
        frame_bin = format(frame_int, '030b')
        ck = (frame_int >> 19) & 1
        wdqs = (frame_int >> 20) & 1
        wtph = (frame_int >> 22) & 1
        rtph = (frame_int >> 24) & 1
        rden = (frame_int >> 26) & 1
        print(f'Frame {idx:2d}: {frame_hex} | {frame_bin} | CK={ck} WDQS={wdqs} WTPH={wtph} RTPH={rtph} RDEN={rden}')
    
    print()  # 기존 16비트 그룹도 그대로 출력
    print('=== SERDES 16:1 구조 (16비트씩 끊음) ===')
    print('각 16비트 = 16개 프레임의 같은 비트 위치')
    print('bit 0~15 = frame 0~15의 해당 비트 위치')
    print()
    
    serdes_int = int(hex_value, 16)
    ca_map = {
        0: 'R0', 1: 'R1', 2: 'R2', 3: 'R3', 4: 'R4', 5: 'R5', 6: 'R6',
        7: 'R7', 8: 'R8', 9: 'R9', 10: 'R10',
        11: 'C0', 12: 'C1', 13: 'C2', 14: 'C3', 15: 'C4',
        16: 'C5', 17: 'C6', 18: 'C7',
        19: 'HBM_CK', 20: 'PC0_WDQS', 21: 'PC1_WDQS', 22: 'PC0_WTPH', 23: 'PC1_WTPH',
        24: 'PC0_RTPH', 25: 'PC1_RTPH', 26: 'PC0_RD_EN', 27: 'PC1_RD_EN', 28: 'reserved0', 29: 'reserved1'
    }
    
    for block in range(num_blocks):
        print(f'--- Block {block} (Frames {block*16} to {(block+1)*16-1}) ---')
        for bit_pos in range(30):
            bit_offset = block * 480 + bit_pos * 16  # 480 bits per block
            value_16bit = (serdes_int >> bit_offset) & 0xFFFF
            binary = format(value_16bit, '016b')
            pin_name = ca_map.get(bit_pos, f'unknown{bit_pos}')
            frames_bits = [(value_16bit >> frame) & 1 for frame in range(16)]
            frames_str = ' '.join(str(bit) for bit in frames_bits)
            print(f'Bit {bit_pos:2d} ({pin_name:8s}): {binary}  [frame: {frames_str}]')
        print()

# Example usage
if __name__ == "__main__":
    # SERDES 16:1 hex 값
    hex_value = '0000000000000000000000000000000000000000CCCCFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF0000FFFF3C3C0000000000000000000000000000000000000000CCCCFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF0000FFFF3C3C0000000000000000000000000000000000000000CCCCFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF0000FFFF3C3C'
    analyze_serdes_pattern(hex_value)
