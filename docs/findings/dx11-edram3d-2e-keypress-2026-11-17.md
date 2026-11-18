# Remote keypress to dismiss the GTX 570 licence screen — Phase 2e

**Date:** 2026-11-17 (afternoon CST; PC clock ~1 h behind CST)
**Game:** PID 11284, Phase 2e build, session 20261117T222813Z-p11284
**Verdict:** The remote keypress did **NOT** work. All three keys (Enter, Space, Tab+Enter) were confirmed delivered to the game window, but the game is still stuck on the licence screen (music.in_game 0, save.backup 0, 0 probe lines). **Seth needs to press a key on the PC himself.** The game was left running; the probe gate does not expire, so if he gets it into free roam the probe fires on this same running game — no relaunch needed.

---

## What was done

- Created a one-time scheduled task **"MuseSendKey"** (schtasks, `/it` = Interactive only, Run As User seths — the same session-1 configuration as "PinyonShift Test Run") that runs a PowerShell script in the interactive session. The script brings the pinyon_shift window to the foreground (`SetForegroundWindow`) and sends the key via `[System.Windows.Forms.SendKeys]::SendWait`. PsExec was NOT used (not installed on the PC; not downloaded).
- The script logs to `C:\Users\seths\AppData\Local\Temp\sendkey-log.txt`, including the foreground window handle vs the game window handle, so focus is verified, not assumed.

## The three attempts (all confirmed delivered to the game window)

| # | Key | Sent (PC time) | Focus confirmed | Entered? |
|---|-----|----------------|-----------------|----------|
| 1 | Enter | 15:44:01 | Yes — foreground handle 23070988 = game handle, window "Pinyon Shift 0.6.0 \| Windows Release" | No |
| 2 | Space | 15:46:04 | Yes | No |
| 3 | Tab then Enter | 15:47:14 | Yes | No |

SendKeys log (all three):

```
[2026-11-17 15:44:01] sendkey started, Key=ENTER
[2026-11-17 15:44:01] pinyon_shift PID=11284 MainWindowHandle=23070988 MainWindowTitle=Pinyon Shift 0.6.0 | Windows Release
[2026-11-17 15:44:01] SetForegroundWindow returned True
[2026-11-17 15:44:01] foreground window handle=23070988 (game handle=23070988)
[2026-11-17 15:44:01] key sent: ENTER
[2026-11-17 15:46:04] sendkey started, Key=SPACE
[2026-11-17 15:46:04] pinyon_shift PID=11284 MainWindowHandle=23070988 MainWindowTitle=Pinyon Shift 0.6.0 | Windows Release
[2026-11-17 15:46:04] SetForegroundWindow returned True
[2026-11-17 15:46:04] foreground window handle=23070988 (game handle=23070988)
[2026-11-17 15:46:04] key sent: SPACE
[2026-11-17 15:47:14] sendkey started, Key=TABENTER
[2026-11-17 15:47:14] pinyon_shift PID=11284 MainWindowHandle=23070988 MainWindowTitle=Pinyon Shift 0.6.0 | Windows Release
[2026-11-17 15:47:14] SetForegroundWindow returned True
[2026-11-17 15:47:14] foreground window handle=23070988 (game handle=23070988)
[2026-11-17 15:47:14] key sent: TABENTER
```

Each attempt was followed by a 60 s wait and a state check; Tab+Enter got a second 60 s wait (total ~2 min) to allow for slow D11 loading. No change in any check.

## Game state after all three attempts (final check ~15:49 PC)

- PID 11284 still running, session 1, memory ~196 MB (flat, fluctuating 146–198 MB across checks — the stuck pattern, not the entry growth to 544 MB seen in Phase 2c).
- jsonl (20261117T222813Z-p11284.jsonl): 177 lines, **music.in_game lines: 0, save.backup lines: 0**. Only `simulation.step_gate` lines (holds ~161–164, timeouts ~29–42) and `audio.native_output_mode` events — no new event types since 22:46:59Z.
- **0 `FH1 edram3d probe` lines** in all rotated logs (runtime.log through runtime.4.log).

## Why it likely didn't work (hypothesis, not proven)

SendKeys posts WM_KEYDOWN/WM_KEYUP messages to the window's message queue. Games that read the keyboard via raw input (WM_INPUT) or DirectInput/XInput read the hardware directly and ignore synthesized messages. The Pinyon game likely reads input that way, so no SendKeys keystroke — Enter, Space, or any other key — would reach the licence screen. The keypresses demonstrably reached the window (focus confirmed by matching handles each time), yet nothing happened, which is consistent with this. A mouse click would likely hit the same wall (synthesized input), so it was not tried.

## What Seth needs to do

Press a key on the PC himself (the same way he got past the licence screen before). The game (PID 11284, Phase 2e build) is still running, and the probe gate (`presented_output_frames_ >= 3000`) does not expire — once the game enters free roam, the probe fires on this same running game and the EDRAM readback is captured. **No relaunch needed.** Do NOT relaunch in a loop while it waits for input.

## Deliverables and state

- **This report:** `~/workspace/dx11-edram3d-2e-keypress-2026-11-17.md`
- **Scripts:** `~/workspace/dx11-edram3d-2e-scratch/sendkey.ps1`, `check-entered.ps1`, `check-state-2e.ps1`
- **SendKeys log (on PC):** `C:\Users\seths\AppData\Local\Temp\sendkey-log.txt` (also `sendkey-arg.txt`)
- The "MuseSendKey" scheduled task was deleted after the attempts (one-off; it can be recreated in seconds if another remote keypress is wanted).
- **Game:** still running (PID 11284, session 1), Phase 2e build. Flat-grey EDRAM question **still open** (probe never fired).
