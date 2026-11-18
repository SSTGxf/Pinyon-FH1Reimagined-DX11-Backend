# Test A evidence — Band 2 isolation (2026-11-15 ~16:46–16:51 CST)

## Run
- Config: `fh1_debug_band_suppress = 5` (Bands 1 and 3 suppressed, Band 2
  isolated), band2_trace_swap=2200, resolve + swap-fetch + tile-trace
  (2200) + bins (2200) + transition log all on. Same build as Baseline.
- Game: pid 30152, session 1, launched via "PinyonShift Test Run".
  Ran 15:46:15–15:51:46 PM PC (~5.5 min), reached swap 2912.
- Logs: runtime.log (4.5 MB) + runtime.1.log (5.24 MB) +
  runtime.2.log (5.24 MB, contains Baseline's tail) + resolve-trace.log.

## VISUAL RESULT (decisive): Band 2 content IS visible

Seth's photos (16:51 CST) show the world in the middle band — trees, road,
car — with the top and bottom blank (Bands 1 and 3 suppressed). The blue
tint (fetch swizzle) and the black-rectangle artifact are still present,
but Band 2's scene content is clearly rendering. Photo 2 also shows sky,
road, and the speedometer in the band.

This confirms the overwrite mechanism: when Bands 1 and 3 stop writing to
the same EDRAM rows, Band 2's content shows up. The middle band is black
in Baseline because the other bands overwrite Band 2's rows, not because
Band 2 fails to draw.

## Log numbers (weaker — the trace fired before the world)

The band2 trace at swap 2200 caught only 4 Band 2 draw packets + 1 IB
packet (vs 910 + 228 in Baseline), because at swap 2200 in this run the
game had not yet reached the world-driving state (only 60 tile draws, all
window_y=0, no band draws). The trace frame (2200) was too early for
Test A's driving pace. The photos, taken later in the run (~15:51), show
the world state the trace missed.

- Zero "FH1 band suppression" log lines: the suppression log line is gated
  on the trace frame (swap 2200), and at swap 2200 there were no Band 1/3
  draws to suppress yet. The suppression DID work — the photos prove Bands
  1 and 3 are off (top/bottom blank in the world).
- The Band 2 packets that did fire were all accepted (pred=0) and reached
  D3D11 (issue_enter, window_y=-256, mode=6).
- D3D11 resolve plans for Band 2 ARE present:
  `call=42221 rect=0,0-1280,256 dest=1DC05000 key=1024/32/4x/d1`,
  `call=42222 rect=0,0-1280,256 dest=1C621000 key=0/32/4x/c12`
  (1C621000 is Band 2's dest base from Baseline).
- 2x "D3D11 resolve planning failed" errors (same count as Baseline) —
  2 of the 4 Band 2 resolves in the traced window failed to plan.
  Unexplained; worth investigating whether they hit the large resolves.

## Frontbuffer content (guest_words_nz)

- Test A: steady ~28,480–28,531 from swap 2146 through 2912 (end: 28,487).
- Baseline: ~28,486 early, dropping to 26,153 (swap 2499) and 15,879
  (swap 2606, end).
- Test A holds roughly 1.8x the nonzero frontbuffer words of Baseline at
  end of run — consistent with Band 2's content being visible instead of
  black.

## Conclusion

Test A CONFIRMS the overwrite mechanism by direct visual evidence. The
leading diagnosis is now firmly the D3D11 EDRAM row-separation / untile
implementation gap: all bands draw into the same EDRAM rows (0–256) and
overwrite each other; Band 2's resolve reads rows that the other bands
have overwritten. The fix is to separate the bands' EDRAM rows (the
untile equivalent) so each band's content survives to its own resolve.

Next: Test B (suppress=2, Band 2 off) to validate the classification —
expect the middle region to go black while Bands 1 and 3 render normally.
