# SERDES 16:1 hex 값
hex_value = '0000000000000000000000000000CCCCFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF0000FFFF3C3C0000000000000000000000000000CCCCFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF0000FFFF3C3C0000000000000000000000000000CCCCFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF0000FFFF3C3C'

# Hex를 정수로 변환
serdes_int = int(hex_value, 16)

# 27비트 프레임이므로 27개의 16비트 그룹 분석
print('=== SERDES 16:1 구조 (16비트씩 끊음) ===')
print('각 16비트 = 16개 프레임의 같은 비트 위치')
print('bit 0~15 = frame 0~15의 해당 비트 위치')
print()

ca_map = {
    0: 'R0', 1: 'R1', 2: 'R2', 3: 'R3', 4: 'R4', 5: 'R5', 6: 'R6',
    7: 'R7', 8: 'R8', 9: 'R9', 10: 'R10',
    11: 'C0', 12: 'C1', 13: 'C2', 14: 'C3', 15: 'C4',
    16: 'C5', 17: 'C6', 18: 'C7',
    19: 'HBM_CK', 20: 'WDQS', 21: 'WTPH', 22: 'RTPH', 23: 'RDEN', 
    24: 'reserved1', 25: 'reserved2', 26: 'reserved3'
}

for bit_pos in range(27):
    # 이 비트 위치의 16비트 그룹 추출
    value_16bit = (serdes_int >> (bit_pos * 16)) & 0xFFFF
    binary = format(value_16bit, '016b')
    
    pin_name = ca_map.get(bit_pos, f'unknown{bit_pos}')
    
    # 프레임별로 분석
    frames_bits = []
    for frame in range(16):
        bit = (value_16bit >> frame) & 1
        frames_bits.append(str(bit))
    
    frames_str = ' '.join(frames_bits)
    print(f'Bit {bit_pos:2d} ({pin_name:8s}): {binary}  [frame: {frames_str}]')
