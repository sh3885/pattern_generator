import pattern_generator
pattern_generator.DEBUG_MODE = False

ca_pins = ['R0', 'R1', 'R2', 'R3', 'R4', 'R5', 'R6', 'R7', 'R8', 'R9', 'R10', 'C0', 'C1', 'C2', 'C3', 'C4', 'C5', 'C6', 'C7']

for pin in ca_pins:
    print('=== ' + pin + ' Training with sequence "00111100" (40 frames, 10 clocks) ===')
    pattern = pattern_generator.generate_ca_training_pattern(pin, '00111100', clock_toggle=True, num_frames=40)
    print('Hex Pattern: ' + pattern)
    steps = pattern_generator.get_aword_misr_steps(pattern)
    misr_values = [format(post, '010X') for _, _, _, _, post in steps]
    print('MISR per Clock: ' + str(misr_values))
    final_misr = pattern_generator.get_aword_misr(pattern)
    print('Final MISR: ' + final_misr)
    
    # SERDES 16:1 conversion
    serdes_pattern = pattern_generator.pattern_to_serdes_16to1(pattern, clock_state=0)
    print('SERDES 16:1 Pattern: ' + serdes_pattern)
    print()