import generate_pattern
import analyze_serdes_16bit
generate_pattern.DEBUG_MODE = False

ca_pins = ['R0', 'R1', 'R2', 'R3', 'R4', 'R5', 'R6', 'R7', 'R8', 'R9', 'R10', 'C0', 'C1', 'C2', 'C3', 'C4', 'C5', 'C6', 'C7']

for pin in ca_pins:
    training = generate_pattern.generate_ca_training_pattern(pin, '00111100', clock_toggle=True, num_frames=48)
    pattern = training
    raw_frames = len(pattern) // 8
    print(f'=== {pin} Training with sequence "00111100" ===')
    print(f'Raw pattern: {raw_frames} frames (4 default + 44 training)')
    print('Hex Pattern: ' + pattern)
    steps = generate_pattern.get_aword_misr_steps(pattern[32:])  # Exclude first 4 frames for MISR
    misr_values = [format(post, '010X') for _, _, _, _, post in steps]
    print('MISR per Clock: ' + str(misr_values))
    final_misr = generate_pattern.get_aword_misr(pattern[32:])  # Exclude first 4 frames for MISR
    print('Final MISR: ' + final_misr)
    
    # SERDES 16:1 conversion
    serdes_pattern = generate_pattern.pattern_to_serdes_16to1(pattern, padding_ck_toggle=True, padding_ck_value=0)
    serdes_blocks = len(serdes_pattern) // 120
    padded_frames = serdes_blocks * 16
    print(f'SERDES 16:1 Pattern: {serdes_pattern}')
    print(f'SERDES output: {len(serdes_pattern)} hex chars, {serdes_blocks} blocks, {padded_frames} decoded frames (padded to 16-frame boundary)')
    print('--- Analyze SERDES pattern for ' + pin + ' ---')
    analyze_serdes_16bit.analyze_serdes_pattern(serdes_pattern)
    print()
ca_trn_pat_list = []
ca_trn_misr_list = []

for pin in ca_pins:
    training = generate_pattern.generate_ca_training_pattern(pin, '00111100', clock_toggle=True, num_frames=48)
    serdes_pattern = generate_pattern.pattern_to_serdes_16to1(training, padding_ck_toggle=True, padding_ck_value=0)
    ca_trn_pat_list.append(f"0x{serdes_pattern.upper()}")
    
    final_misr = generate_pattern.get_aword_misr(training[32:])  # Exclude first 4 frames for MISR
    ca_trn_misr_list.append(final_misr)

print('ca_trn_pat_list = [')
for pat in ca_trn_pat_list:
    print(f"    '{pat}',")
print(']')

print('ca_trn_misr_list = [')
for misr in ca_trn_misr_list:
    print(f"    '{misr}',")
print(']')