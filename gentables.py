"""DigiCosm's constant tables (tables.h): windows, sine, exp2, filter coefficients, gain curves.
python3 gentables.py > tables.h"""
import math

FS = 48000.0
out = []


def arr(ctype, name, vals, per=12):
    out.append('static const %s %s[%d] = {' % (ctype, name, len(vals)))
    for i in range(0, len(vals), per):
        out.append('    ' + ', '.join(str(v) for v in vals[i:i + per]) + ',')
    out.append('};')


def q15(x):
    return max(-32768, min(32767, int(round(x * 32767))))


# Windows (a head's contour over a grain, or a loop's cycle), 256 entries, Q15; the Shape knob's four bands.
def flat(t):            # a gate with short fades: loops sound continuous
    return min(1.0, t / 0.04, (1.0 - t) / 0.04)


def swell(t):           # fades in over the cycle, a short fade out
    return min(t ** 1.5, (1.0 - t) / 0.06, 1.0)


def perc(t):            # a fast attack and a falling tail
    return min(t / 0.02, 1.0) * (1.0 - t) ** 2


def arch(t):            # in and out: a Hann window
    return 0.5 - 0.5 * math.cos(2 * math.pi * t)


wins = []
for f in (flat, swell, perc, arch):
    wins += [q15(max(0.0, f((i + 0.5) / 256.0))) for i in range(256)]
arr('int16_t', 'dc_win', wins, 16)

arr('int16_t', 'dc_sin', [q15(math.sin(2 * math.pi * i / 256)) for i in range(256)], 16)

# 2^(i/256), Q12 (4096..8191): a rate from an octave offset in 1/256 octaves.
arr('uint16_t', 'dc_exp2', [int(round(4096 * 2 ** (i / 256.0))) for i in range(256)], 16)

# The Filter knob: cutoff 30 Hz .. 10 kHz, exponential; Chamberlin SVF's f = 2 sin(pi fc / fs), Q14.
arr('int16_t', 'dc_svf_f', [int(round(16384 * 2 * math.sin(math.pi * (30.0 * (10000 / 30.0) ** (k / 126.0)) / FS)))
                            for k in range(128)], 16)
# Resonance: the SVF's damping q, Q14: 1.40 (none) down to 0.06.
arr('int16_t', 'dc_svf_q', [int(round(16384 * (1.40 - 1.34 * (k / 127.0) ** 0.7))) for k in range(128)], 16)

# Mix: equal power, Q11 (2048 = 1.0): dry, wet.
arr('int16_t', 'dc_mix_dry', [int(round(2048 * math.cos(math.pi / 2 * k / 127.0))) for k in range(128)], 16)
arr('int16_t', 'dc_mix_wet', [int(round(2048 * math.sin(math.pi / 2 * k / 127.0))) for k in range(128)], 16)

# Input gain (FUNC + A): 64 is 0 dB, 0.3 dB a step, 0 mutes; Q12.
arr('int32_t', 'dc_in_gain', [0] + [int(round(4096 * 10 ** ((k - 64) * 0.3 / 20))) for k in range(1, 128)], 16)

# Levels (Loop Level, Effect volume): a square-law taper, 0..127 -> 0..1.61 (100 is 1.0), Q14.
arr('int16_t', 'dc_level', [int(round(16384 * (k / 100.0) ** 2)) for k in range(128)], 16)

print('/* SPDX-License-Identifier: GPL-2.0-or-later */')
print('/* DigiCosm\'s constant tables, from gentables.py: do not edit. */')
print('\n'.join(out))
