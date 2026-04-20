import pattern_generator
pattern_generator.DEBUG_MODE = False

# Generate 8-frame pattern
pattern = pattern_generator.generate_ca_training_pattern('R0', '00111100', clock_toggle=True, num_frames=8)

# Get original frames
frames = []
for i in range(8):
    frame_hex = pattern[i*8:(i+1)*8]
    frame_int = int(frame_hex, 16) & 0x7FFFFFF
    frames.append(frame_int)
    print(f'Frame {i}: {format(frame_int, "027b")} (hex: {frame_hex})')

# Get SERDES pattern
serdes = pattern_generator.pattern_to_serdes_16to1(pattern)
serdes_int = int(serdes, 16)
serdes_binary = format(serdes_int, '0432b')

print()
print('SERDES output hex:', serdes[:50], '...')
print('SERDES output length:', len(serdes), 'chars')
print('SERDES binary length:', len(serdes_binary), 'bits')
print()
print('SERDES binary (432 bits in groups of 27):')
for i in range(16):
    start = i * 27
    end = (i + 1) * 27
    chunk = serdes_binary[start:end]
    val = int(chunk, 2)
    in_original = False
    expected = None
    if i < 8:
        expected = frames[i]
    else:
        expected = 'padding'
    match = "OK" if (i < 8 and val == frames[i]) else "check"
    print(f'  #{i:2d} [{start:3d}-{end:3d}): {chunk} (val={val:07X}, expected={expected if isinstance(expected, str) else format(expected, "07X")})')
