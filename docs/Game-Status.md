# Game status

Progress for every preserved game on the Taito Type X, Type X+ and Type X2
boards, including NESiCAxLive titles. Each game has a status: playable,
planned, or not started yet.

The list includes only games known to run on PC hardware today. The
reference is the [TeknoParrot compatibility list](https://teknoparrot.com/en/Compatibility),
read November 2026, and its board for each game. Release years come from
Wikipedia's [Taito Type X](https://en.wikipedia.org/wiki/Taito_Type_X)
article. Games that only exist on the original hardware are left out. Know a
game that belongs here? Please open an issue.

## Status levels

| Level | Meaning |
|---|---|
| **Playable** | Attract mode, coin / start, character select and full matches at 60 fps, with no known blocker. |
| **Playable, issues** | Plays start to finish, with problems listed on the game's page (slowdown, missing feature). |
| **Boots** | Reaches attract mode or a menu, but gameplay is incomplete or broken. |
| **Planned** | Chosen as a next port; its requirements are researched on its page. |
| Not started | Not developed yet. |

Platforms:
- **macOS**: the standalone SDL3 executable (`build/x2-<id>`).
- **Android app**: the standalone `com.typex2.recomp` app, launched by ES-DE.
- **RetroArch**: the `typex2_libretro` core.

Results come from the two [test devices](Hardware.md#test-devices): a Mac
with an Apple M3 Pro for macOS, and a Samsung Galaxy Tab A11 (8 GB / 128 GB,
Mali-G57 MC2) for Android.

## Supported games

| Game | Board | Year | `games/` id | macOS | Android app | RetroArch (Android) | Page |
|---|---|---|---|---|---|---|---|
| The King of Fighters XII | Type X2 | 2009 | `kof12` | Playable | Playable | Playable | [KOF XII](Game-KOF-XII.md) |
| The King of Fighters XIII | Type X2 | 2010 | `kof13` | Playable | Playable | Playable | [KOF XIII](Game-KOF-XIII.md) |
| The King of Fighters XIII Climax | Type X2 | 2012 | `kof13cl` | Playable | Playable | Playable | [KOF XIII Climax](Game-KOF-XIII-Climax.md) |
| Super Street Fighter IV Arcade Edition | Type X2 | 2010 | `ssf4ae` | Playable | not tested | Playable, issues | [SSF4 AE](Game-SSF4-AE.md) |
| BlazBlue: Continuum Shift | Type X2 | 2009 | `bbcs` | Playable | not tested | Playable | [BlazBlue CS](Game-BlazBlue-CS.md) |
| BlazBlue: Calamity Trigger | Type X2 | 2008 | `bbct` | Playable | not tested | Playable, issues | [BlazBlue CT](Game-BlazBlue-CT.md) |
| The King of Fighters '98 Ultimate Match | Type X | 2008 | `kof98um` | Playable | not tested | Playable | [KOF '98 UM](Game-KOF-98-UM.md) |
| The King of Fighters: Maximum Impact Regulation A | Type X2 | 2007 | `kofmira` | Playable | not tested | not tested | [KOF MI Regulation A](Game-KOF-MI-Regulation-A.md) |

Every supported game ships in the same RetroArch core and the same Android
app. Both identify the game from its executable (`game.exe`, or `launcher.exe`
for KOF '98 UM).

## Type X2 library

| Game | Year | Status |
|---|---|---|
| Battle Fantasia | 2007 | Not started |
| **BlazBlue: Calamity Trigger** | 2008 | **Playable, issues** |
| **BlazBlue: Continuum Shift** | 2009 | **Playable** |
| Chase H.Q. 2 | 2007 | Not started |
| Cyber Diver | 2009 | Not started |
| D1GP Arcade | 2007 | Not started |
| Darius Burst: Another Chronicle | 2010 | Not started |
| Elevator Action Death Parade | 2009 | Not started |
| Gaia Attack 4 | 2010 | Not started |
| Gouketsuji Ichizoku: Senzo Kuyou | 2009 | Not started |
| Haunted Museum | 2009 | Not started |
| Haunted Museum II | 2010 | Not started |
| **The King of Fighters: Maximum Impact Regulation A** | 2007 | **Playable** |
| **The King of Fighters XII** | 2009 | **Playable** |
| **The King of Fighters XIII** | 2010 | **Playable** |
| **The King of Fighters XIII Climax** | 2012 | **Playable** |
| Music Gun Gun! 2 | 2011 | Not started |
| Samurai Spirits Sen | 2008 | Not started |
| Senko no Ronde Duo - Dis-United Order | 2009 | Not started |
| Sonic Blast Heroes | 2011 | Not started |
| Street Fighter IV | 2008 | Not started |
| Super Street Fighter IV Arcade Edition (Ver.1.0) | 2010 | Not started |
| **Super Street Fighter IV Arcade Edition (Export)** | 2010 | **Playable, issues** |
| Super Street Fighter IV Arcade Edition Ver.2012 | 2011 | Not started |
| Wacky Races | 2009 | Not started |

## NESiCAxLive library

NESiCAxLive was Taito's download service for Type X2 cabinets (2010 on).
These builds may need a different loader than the disc / HDD releases above.

| Game | Year | Status |
|---|---|---|
| 3D Cosplay Mahjong | — | Not started |
| Akai Katana Shin | 2012 | Not started |
| Aquapazza: Aquaplus Dream Match | 2011 | Not started |
| Arcana Heart 2 | 2012 | Not started |
| Arcana Heart 3 Love Max Six Stars!!!!!! | 2014 | Not started |
| BlazBlue: Central Fiction | 2015 | Not started |
| BlazBlue: Chrono Phantasma | 2012 | Not started |
| BlazBlue: Continuum Shift II | 2010 | Not started |
| Chaos Breaker | 2012 | Not started |
| Chaos Code: New Sign of Catastrophe | 2013 | Not started |
| Crimzon Clover | 2013 | Not started |
| Daemon Bride - Additional Gain | 2011 | Not started |
| Dark Awake: The King Has No Name | 2012 | Not started |
| Do Not Fall: Run For Your Drink | 2013 | Not started |
| Dragon Dance | 2011 | Not started |
| Elevator Action | 2014 | Not started |
| EN-Eins Perfektewelt | 2012 | Not started |
| Exception | 2011 | Not started |
| Gouketsuji Ichizoku: Senzo Kuyou | 2012 | Not started |
| Homura | 2012 | Not started |
| Hyper Street Fighter II: The Anniversary Edition | 2014 | Not started |
| Ikaruga | 2013 | Not started |
| The King of Fighters '98 Ultimate Match Final Edition | 2011 | Not started |
| The King of Fighters 2002 Unlimited Match | 2011 | Not started |
| The King of Fighters XIII Climax | 2013 | Not started |
| Magical Beat | 2012 | Not started |
| Nitroplus Blasterz: Heroines Infinite Duel | 2015 | Not started |
| Persona 4: The Ultimate in Mayonaka Arena | 2012 | Not started |
| Persona 4: The Ultimax Ultra Suplex Hold | 2013 | Not started |
| Psychic Force 2012 | 2012 | Not started |
| Puzzle Bobble | 2012 | Not started |
| Raiden III | 2012 | Not started |
| Raiden IV | 2012 | Not started |
| Rastan Saga | — | Not started |
| Senko no Ronde Duo - Dis-United Order | 2011 | Not started |
| Skullgirls 2nd Encore | 2015 | Not started |
| Space Invaders | 2012 | Not started |
| Spica Adventure | 2011 | Not started |
| Strania -The Stella Machina- | 2011 | Not started |
| Street Fighter III 3rd Strike: Fight for the Future | 2014 | Not started |
| Street Fighter Zero 3 | 2014 | Not started |
| Suggoi! Arcana Heart 2 | 2012 | Not started |
| The Rumble Fish 2 | 2012 | Not started |
| Tottemo E Mahjong | 2013 | Not started |
| Trouble Witches AC | 2012 | Not started |
| Ultra Street Fighter IV | 2014 | Not started |
| Vampire Savior: The Lord of Vampire | 2014 | Not started |

## Type X / Type X+ library

Type X runs the same OS family (Windows XP Embedded on 32-bit x86) on older
hardware: an AGP Radeon 9600-class GPU and an Intel 865G chipset. The
recompiler should carry over as is. The runtime may need older DirectX
interfaces (Direct3D 8, DirectDraw) and the board's I/O setup. Type X+ is
Type X with a stronger graphics card.

| Game | Board | Year | Status |
|---|---|---|---|
| Battle Gear 4 | Type X+ | 2005 | Not started |
| Battle Gear 4 Tuned | Type X+ | 2006 | Not started |
| Chaos Breaker | Type X | 2004 | Not started |
| Giga Wing Generations | Type X | 2004 | Not started |
| K-On! After School Rhythm Selection | Type X | 2013 | Not started |
| **The King of Fighters '98 Ultimate Match** | Type X | 2008 | **Playable** |
| The King of Fighters: Sky Stage | Type X | 2010 | Not started |
| New Super Mario Bros. Wii Coin World | Type X | — | Not started |
| Raiden III | Type X | 2005 | Not started |
| Raiden IV | Type X | 2007 | Not started |
| Shikigami no Shiro III | Type X | 2006 | Not started |
| Spica Adventure | Type X | 2005 | Not started |
| Taisen Hot Gimmick 5: Mirai Eigou | Type X | 2006 | Not started |
| Tetris The Grand Master 3: Terror Instinct | Type X | 2005 | Not started |
| Trouble Witches AC | Type X | 2009 | Not started |
| Valve Limit R | Type X | 2006 | Not started |

## Easiest next ports

Games built the same way as titles that already run should come first:
- Street Fighter IV and SSF4 AE Ver.1.0 / Ver.2012 share SSF4 AE's engine.
- BlazBlue: Continuum Shift II shares Continuum Shift's engine.
- KOF XIII Climax for NESiCAxLive shares Climax's engine.
