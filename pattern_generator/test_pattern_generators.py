#!/usr/bin/env python3
"""Test new HBM pattern generator functions and write a readable log."""

import generate_pattern

LOG_FILE = 'pattern_function_tests.log'


def format_frame_log(frame_hex, frame_idx):
    frame_int = int(frame_hex, 16)
    bits_str = bin(frame_int)[2:].zfill(30)
    bits = [(frame_int >> i) & 1 for i in range(30)]
    
    # R0-R10
    r_bits = ''.join(str(bits[i]) for i in range(11))
    # C0-C7
    c_bits = ''.join(str(bits[i]) for i in range(11, 19))
    
    return (
        f"Frame {frame_idx:2d}: {frame_hex} | {bits_str} | "
        f"CK={bits[19]} WDQS={bits[20]} WTPH={bits[22]} RTPH={bits[24]} RDEN={bits[26]} | "
        f"R[{r_bits}] C[{c_bits}]"
    )


def log_pattern(name, pattern, **kwargs):
    lines = []
    num_frames = len(pattern) // 8
    lines.append(f"=== {name} ({num_frames} frames) ===")
    if kwargs:
        args_str = ', '.join(f"{k}={v}" for k, v in kwargs.items())
        lines.append(f"Arguments: {args_str}")
    lines.append(f"pattern length: {len(pattern)} hex chars")
    lines.append(f"pattern: {pattern}")
    for idx in range(num_frames):
        frame_hex = pattern[idx * 8:(idx + 1) * 8]
        lines.append(format_frame_log(frame_hex, idx))
    lines.append('')
    return lines


def assert_ck_sequence(pattern, expected_ck_sequence):
    num_frames = len(pattern) // 8
    actual = []
    for idx in range(num_frames):
        frame_int = int(pattern[idx * 8:(idx + 1) * 8], 16)
        actual.append((frame_int >> 19) & 1)
    if actual != expected_ck_sequence[:num_frames]:
        raise AssertionError(f"CK sequence mismatch: actual={actual}, expected={expected_ck_sequence}")


def assert_bit_sequence(pattern, bit_index, expected_sequence):
    num_frames = len(pattern) // 8
    actual = []
    for idx in range(num_frames):
        frame_int = int(pattern[idx * 8:(idx + 1) * 8], 16)
        actual.append((frame_int >> bit_index) & 1)
    if actual != expected_sequence[:num_frames]:
        raise AssertionError(f"Bit {bit_index} sequence mismatch: actual={actual}, expected={expected_sequence}")


def main():
    sections = []

    mrs = generate_pattern.generate_mrs_pattern(
        op0=1, op1=1, op2=0, op3=0, op4=1, op5=0, op6=1, op7=0,
        ma0=1, ma1=0, ma2=1, ma3=0, ma4=1
    )
    assert len(mrs) == 32
    assert_ck_sequence(mrs, [1, 1, 0, 0])
    sections += log_pattern('MRS Pattern', mrs,
                           op0=1, op1=1, op2=0, op3=0, op4=1, op5=0, op6=1, op7=0,
                           ma0=1, ma1=0, ma2=1, ma3=0, ma4=1)

    nop = generate_pattern.generate_nop_pattern(2)
    assert len(nop) == 64
    assert_ck_sequence(nop, [1, 1, 0, 0] * 2)
    sections += log_pattern('NOP Pattern x2', nop, num_nops=2)

    wr = generate_pattern.generate_write_pattern(
        pc=1,
        sid0=1, sid1=0,
        ba0=0, ba1=1, ba2=1, ba3=0,
        ca0=1, ca1=0, ca2=1, ca3=0, ca4=1
    )
    assert len(wr) == 32
    assert_ck_sequence(wr, [1, 1, 0, 0])
    sections += log_pattern('WR Pattern', wr,
                           pc=1, sid0=1, sid1=0,
                           ba0=0, ba1=1, ba2=1, ba3=0,
                           ca0=1, ca1=0, ca2=1, ca3=0, ca4=1)

    pre = generate_pattern.generate_pre_postamble_pattern(5, pc0_wdqs_toggle=True, pc1_wdqs_toggle=False)
    assert len(pre) == 40
    assert_ck_sequence(pre, [1, 1, 0, 0] * 2 + [1, 1])
    assert_bit_sequence(pre, 20, [1, 0, 1, 0, 1, 0, 1, 0, 1, 0])
    sections += log_pattern('Pre/Postamble Pattern PC0', pre,
                           wck=5, pc0_wdqs_toggle=True, pc1_wdqs_toggle=False)

    pre1 = generate_pattern.generate_pre_postamble_pattern(5, pc0_wdqs_toggle=False, pc1_wdqs_toggle=True)
    assert len(pre1) == 40
    assert_ck_sequence(pre1, [1, 1, 0, 0] * 2 + [1, 1])
    assert_bit_sequence(pre1, 21, [1, 0, 1, 0, 1, 0, 1, 0, 1, 0])
    sections += log_pattern('Pre/Postamble Pattern PC1', pre1,
                           wck=5, pc0_wdqs_toggle=False, pc1_wdqs_toggle=True)

    tph = generate_pattern.generate_tph_pattern(8, pc0_wdqs_toggle=True, pc1_wdqs_toggle=False, tph_pattern='010')
    assert len(tph) == 64
    assert_ck_sequence(tph, [1, 1, 0, 0] * 2)
    assert_bit_sequence(tph, 20, [1, 0, 1, 0, 1, 0, 1, 0])
    sections += log_pattern('TPH Pattern PC0', tph,
                           wck=8, pc0_wdqs_toggle=True, pc1_wdqs_toggle=False, tph_pattern='010')

    tph1 = generate_pattern.generate_tph_pattern(8, pc0_wdqs_toggle=False, pc1_wdqs_toggle=True, tph_pattern='010')
    assert len(tph1) == 64
    assert_ck_sequence(tph1, [1, 1, 0, 0] * 2)
    assert_bit_sequence(tph1, 21, [1, 0, 1, 0, 1, 0, 1, 0])
    sections += log_pattern('TPH Pattern PC1', tph1,
                           wck=8, pc0_wdqs_toggle=False, pc1_wdqs_toggle=True, tph_pattern='010')

    nop7 = generate_pattern.generate_nop_pattern(7)
    mrs_nop = mrs + nop7
    assert len(mrs_nop) == len(mrs) + len(nop7)
    assert_ck_sequence(mrs_nop, [1, 1, 0, 0] * ((len(mrs_nop) // 8 + 3) // 4))
    sections += log_pattern('MRS + NOP(7) Pattern', mrs_nop,
                           mrs='11001010/10101', num_nops=7)

    wr = generate_pattern.generate_write_pattern(
        pc=1,
        sid0=1, sid1=0,
        ba0=0, ba1=1, ba2=1, ba3=0,
        ca0=1, ca1=0, ca2=1, ca3=0, ca4=1
    )
    nop8 = generate_pattern.generate_nop_pattern(8)
    pre2 = generate_pattern.generate_pre_postamble_pattern(2, pc0_wdqs_toggle=False, pc1_wdqs_toggle=False)
    tph2 = generate_pattern.generate_tph_pattern(8, pc0_wdqs_toggle=True, pc1_wdqs_toggle=False, tph_pattern='11')
    post2 = generate_pattern.generate_pre_postamble_pattern(2, pc0_wdqs_toggle=False, pc1_wdqs_toggle=False)
    combined = wr + nop8 + pre2 + tph2 + post2
    assert len(combined) == 384
    assert_ck_sequence(wr, [1, 1, 0, 0])
    assert_ck_sequence(nop8, [1, 1, 0, 0] * 8)
    assert_ck_sequence(pre2, [1, 1])
    assert_ck_sequence(tph2, [1, 1, 0, 0] * 2)
    assert_ck_sequence(post2, [1, 1])
    sections += log_pattern('WR + NOP(8) + Pre(2) + TPH(8,11,PC0) + Post(2)', combined,
                           pc=1, sid0=1, sid1=0,
                           ba0=0, ba1=1, ba2=1, ba3=0,
                           ca0=1, ca1=0, ca2=1, ca3=0, ca4=1,
                           num_nops=8, wck_pre=2, tph_pattern='11', wck_tph=8, wck_post=2)

    with open(LOG_FILE, 'w') as f:
        f.write('\n'.join(sections))

    print(f'Log written to {LOG_FILE}')


if __name__ == '__main__':
    main()
