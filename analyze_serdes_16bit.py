import pattern_generator

# SERDES 16:1 hex 값
hex_value = '0000000000000000000000000000CCCCFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF0000FFFF3C3C0000000000000000000000000000CCCCFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF0000FFFF3C3C0000000000000000000000000000CCCCFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF0000FFFF3C3C'

# 전체 프레임 디코드
frames = pattern_generator.serdes_16to1_to_pattern(hex_value, num_frames=16)

print('=== SERDES 16:1 전체 프레임 디코드 ===')
print('총 프레임 개수:', len(frames))
print()

for idx, frame_hex in enumerate(frames):
    frame_int = int(frame_hex, 16)
    frame_bin = format(frame_int, '027b')
    ck = (frame_int >> 26) & 1
    wdqs = (frame_int >> 25) & 1
    wtph = (frame_int >> 24) & 1
    rtph = (frame_int >> 23) & 1
    rden = (frame_int >> 22) & 1
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
    19: 'HBM_CK', 20: 'WDQS', 21: 'WTPH', 22: 'RTPH', 23: 'RDEN',
    24: 'reserved1', 25: 'reserved2', 26: 'reserved3'
}

for bit_pos in range(27):
    value_16bit = (serdes_int >> (bit_pos * 16)) & 0xFFFF
    binary = format(value_16bit, '016b')
    pin_name = ca_map.get(bit_pos, f'unknown{bit_pos}')
    frames_bits = [(value_16bit >> frame) & 1 for frame in range(16)]
    frames_str = ' '.join(str(bit) for bit in frames_bits)
    print(f'Bit {bit_pos:2d} ({pin_name:8s}): {binary}  [frame: {frames_str}]')
