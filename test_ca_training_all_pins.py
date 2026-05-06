import pattern_generator
import analyze_serdes_16bit
pattern_generator.DEBUG_MODE = False

ca_pins = ['R0', 'R1', 'R2', 'R3', 'R4', 'R5', 'R6', 'R7', 'R8', 'R9', 'R10', 'C0', 'C1', 'C2', 'C3', 'C4', 'C5', 'C6', 'C7']

# 2 clock toggle preamble (R0~R3 default, R4~C7=1, HBM_CK toggle)
preamble = pattern_generator.generate_init_pde_pattern(num_clocks=2, clock_toggle=True)

for pin in ca_pins:
    print('=== ' + pin + ' Training with sequence "00111100" (48 frames, 12 clocks) + 2 Clock Preamble ===')
    training = pattern_generator.generate_ca_training_pattern(pin, '00111100', clock_toggle=True, num_frames=48)
    pattern = preamble + training
    print('Hex Pattern: ' + pattern)
    steps = pattern_generator.get_aword_misr_steps(pattern)
    misr_values = [format(post, '010X') for _, _, _, _, post in steps]
    print('MISR per Clock: ' + str(misr_values))
    final_misr = pattern_generator.get_aword_misr(pattern)
    print('Final MISR: ' + final_misr)
    
    # SERDES 16:1 conversion
    serdes_pattern = pattern_generator.pattern_to_serdes_16to1(pattern, padding_ck_toggle=True, padding_ck_value=0)
    print('SERDES 16:1 Pattern: ' + serdes_pattern)
    print('--- Analyze SERDES pattern for ' + pin + ' ---')
    analyze_serdes_16bit.analyze_serdes_pattern(serdes_pattern)
    print()