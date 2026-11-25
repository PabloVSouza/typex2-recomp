# Controls

## Keyboard (standalone app)

| Keys | |
|---|---|
| arrows / Z X C V | move / A B C D |
| 1, 2 | start P1, P2 |
| 5, 6 | coin P1, P2 |
| F2 / F1 | test / service |
| F5 / F7 | save state / load state (one slot; see [Save states](Save-States.md)) |
| F9 | capture: log every draw of the next frame |
| F10 | FPS overlay |
| F11 / F12 / Esc | fullscreen / screenshot / quit |

## Pads

- **Players.** Pads map to players 1-2.
- **Select / Back.**
  - Pressed: coin.
  - Held for 2 s: test menu.
  - With Start (standalone app): quit.
- **L3 / R3** stay free for frontend hotkeys.
- **Touch.** On touch screens without a pad, an on-screen pad appears with
  the game's buttons, punches over kicks: KOF A C over B D (light and heavy
  punch over light and heavy kick), SSF4 LP MP HP over LK MK HK,
  BlazBlue A B C over D, Samurai Shodown EoD A B C over R. Sideways it lies
  over the picture; held upright, the picture moves to the top and the
  stick and buttons sit below it.

### Default layouts

| Game | Layout |
|---|---|
| KOF XII / XIII / XIII Climax / '98 UM / 2002 UM | FinalBurn Neo's Neo Geo "Modern" pad, punches over kicks as in SSF4: A C (light and heavy punch) on Y X, B D (light and heavy kick) on B A. R2 = A+B, R1 = C+D, L2 = A+B+C, L1 = B+C+D. |
| SSF4 AE | LP MP HP on Y X R1, LK MK HK on B A R2. L1 = three punches, L2 = three kicks. |
| BlazBlue CS / CT | A B C on Y X R1, D on B. A = B+C (throw), R2 = A+B (barrier), L1 = A+B+C, L2 = A+B+C+D. |
| Samurai Shodown EoD | As BlazBlue, with R in place of D: A B C on Y X R1, R on B. A = B+C, R2 = A+B, L1 = A+B+C, L2 = A+B+C+R. |
| Gouketsuji Ichizoku Matsuri | As KOF, with LP LK HP HK for A B C D: Y B X A = LP LK HP HK. R2 = LP+LK, R1 = HP+HK, L2 = LP+LK+HP, L1 = LK+HP+HK. |

### `X2_PAD`

`X2_PAD=<pad button>=<cabinet buttons>,...` changes the layout. It goes in the
environment or in the game folder's `x2.env`. On Android, the app's settings
screen (open it from the home screen) writes the presets below for you.
- **Pad buttons:** `south east west north l1 r1 l2 r2 l3 r3`.
- **Cabinet buttons:**

| Value | Meaning |
|---|---|
| `a b c d` | KOF's A-D |
| `1`-`6` | cabinet SHOT1-6 |
| `t` / `v` | test / service |
| `-` | nothing |

Several values together make one button press several at once
(`r2=123`).

8-button sticks whose rows are L1 Y X R1 / L2 B A R2:

```
# SSF4 AE
X2_PAD=l1=1,west=2,north=3,r1=123,l2=4,south=5,east=6,r2=456
# BlazBlue CS / CT, Samurai Shodown EoD (R in place of D)
X2_PAD=l1=1,west=2,north=3,r1=12,l2=4,south=23,east=123,r2=1234
# KOF (A C over B D)
X2_PAD=l1=a,west=c,north=ab,r1=bc,l2=b,south=d,east=cd,r2=abc
# Gouketsuji Ichizoku Matsuri (punches over kicks; north LP+HP, east LK+HK, R1 LP+LK, R2 HP+HK)
X2_PAD=l1=1,west=2,north=12,r1=14,l2=4,south=5,east=45,r2=25
```

The launcher's game page edits the layout without typing it: pick a preset,
or press Assign next to an action and then the controller button for it. It
saves the whole layout as one `X2_PAD` line.

In RetroArch, remaps apply on top, exactly as with FBNeo.

## Cabinet button numbers

| Game | SHOT1 | SHOT2 | SHOT3 | SHOT4 | SHOT5 | SHOT6 |
|---|---|---|---|---|---|---|
| KOF XII / XIII / Climax / '98 UM | A | C | — | B | D | — |
| KOF 2002 UM | A | B | C | D | — | — |
| SSF4 AE | LP | MP | HP | LK | MK | HK |
| BlazBlue CS / CT | A | B | C | D | — | — |
| Samurai Shodown EoD | A | B | C | R | — | — |
| Gouketsuji Ichizoku Matsuri | LP | HP | — | LK | HK | — |

## Scripted input (`X2_INPUT`)

`X2_INPUT="600:c,606:-,700:s"` presses keys at given frames:

| Key | Meaning |
|---|---|
| `c` | coin |
| `s` | start |
| `u d l r` | stick |
| `a b x y 5 6` | cabinet SHOT1-6 |
| `t` / `v` | test / service |
| `p` | F9 capture |
| `-` | release everything |

Upper case (`C S U D L R A B X Y`) drives player 2. It works in both front
ends and is how automated tests reach character select and fights.
