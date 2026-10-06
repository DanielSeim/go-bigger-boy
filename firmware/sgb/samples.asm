; SPDX-License-Identifier: GPL-3.0-or-later
; Authored BRR fixtures: filter zero, shift ten, looping 16-sample blocks.
; Directory begins at $0500; starts and loop points coincide.
.org $0500
.byte $00,$06,$00,$06,$09,$06,$09,$06,$12,$06,$12,$06,$1b,$06,$1b,$06
.org $0600
; Bipolar square, signed nibbles +3 and -3.
.byte $a3,$33,$33,$33,$33,$dd,$dd,$dd,$dd
; Bipolar triangle, signed nibbles 0,1,2,3,4,3,2,1,0,-1,-2,-3,-4,-3,-2,-1.
.byte $a3,$01,$23,$43,$21,$0f,$ed,$cd,$ef
; Narrow pulse: two positive and fourteen negative samples.
.byte $a3,$33,$dd,$dd,$dd,$dd,$dd,$dd,$dd
; Saw: uniformly ascending signed nibbles, one discontinuity per loop.
.byte $a3,$89,$ab,$cd,$ef,$01,$23,$45,$67
