#!/usr/bin/env python3
"""Test new HBM pattern generator functions and write a readable log."""

import generate_pattern

LOG_FILE = 'pattern_function_tests.log'


def format_frame_log(frame_hex, frame_idx):
    frame_int = int(frame_hex, 16)
    bits_str = bin(frame_int)[2:].zfill(30)
    bits = [(frame_int >> i) & 1 for i in range(30)]
    return (
        f"Frame {frame_idx:2d}: {frame_hex} | {bits_str} | "
        f"CK={bits[19]} WDQS={bits[20]} WTPH={bits[22]} RTPH={bits[24]} RDEN={bits[26]} "
        f"R0={bits[0]} R1={bits[1]} R2={bits[2]} R3={bits[3]} "
        f"C0={bits[11]} C1={bits[12]} C2={bits[13]} C3={bits[14]}"
    )


def log_pattern(name, pattern):
    lines = []
    num_frames = len(pattern) // 8
    lines.append(f"=== {name} ({num_frames} frames) ===")
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


def main():
    sections = []

    mrs = generate_pattern.generate_mrs_pattern(
        op0=1, op1=1, op2=0, op3=0, op4=1, op5=0, op6=1, op7=0,
        ma0=1, ma1=0, ma2=1, ma3=0, ma4=1
    )
    assert len(mrs) == 32
    assert_ck_sequence(mrs, [1, 1, 0, 0])
    sections += log_pattern('MRS Pattern', mrs)

    nop = generate_pattern.generate_nop_pattern(2)
    assert len(nop) == 64
    assert_ck_sequence(nop, [1, 1, 0, 0] * 2)
    sections += log_pattern('NOP Pattern x2', nop)

    wr = generate_pattern.generate_write_pattern(
        pc=1,
        sid0=1, sid1=0,
        ba0=0, ba1=1, ba2=1, ba3=0,
        ca0=1, ca1=0, ca2=1, ca3=0, ca4=1
    )
    assert len(wr) == 32
    assert_ck_sequence(wr, [1, 1, 0, 0])
    sections += log_pattern('WR Pattern', wr)

    pre = generate_pattern.generate_pre_postamble_pattern(5, pc0_wdqs_toggle=True, pc1_wdqs_toggle=False)
    assert len(pre) == 40
    sections += log_pattern('Pre/Postamble Pattern', pre)

    tph = generate_pattern.generate_tph_pattern(8, pc0_wdqs_toggle=True, pc1_wdqs_toggle=False, tph_pattern='010')
    assert len(tph) == 64
    sections += log_pattern('TPH Pattern', tph)

    with open(LOG_FILE, 'w') as f:
        f.write('\n'.join(sections))

    print(f'Log written to {LOG_FILE}')


if __name__ == '__main__':
    main()
