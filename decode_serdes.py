from pattern_generator import serdes_16to1_to_pattern

# Provided SERDES hex pattern
serdes_hex = '01FFFF403FFFE803FFFD807FFFB01FFFF603FFFEC03FFFD007FFFA01FFFF403FFFE803FFFD807FFFB01FFFF603FFFEC03FFFD007FFFA01FFFF403FFFE803FFFD807FFFB01FFFF603FFFEC03FFFD007FFFA00FFFFE01FFFFC03FFFF807FFFF01FFFF603FFFEC03FFFD007FFFA01FFFF403FFFE803FFFD807FFFB01FFFF603FFFEC03FFFD007FFFA'

# Total bits in hex
total_bits = len(serdes_hex) * 4
frames_per_block = 16
bits_per_frame = 27
total_frames = total_bits // bits_per_frame

print(f'Hex 데이터 총 길이: {len(serdes_hex)} chars ({total_bits} bits)')
print(f'총 프레임 수: {total_frames} (27비트 단위)')
print('=' * 80)

# Reverse to all frames
frames = serdes_16to1_to_pattern(serdes_hex, num_frames=total_frames)

for frame_idx, frame_hex in enumerate(frames):
    frame_int = int(frame_hex, 16)
    bits_27 = format(frame_int & 0x7FFFFFF, '027b')  # 27 bits
    print(f'Frame {frame_idx:2d}: {bits_27} | {frame_hex}')

print('=' * 80)