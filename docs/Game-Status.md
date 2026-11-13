# Game status

Progress for every Taito Type X / X+ and Type X2 title. The game lists come
from Wikipedia's [Taito Type X](https://en.wikipedia.org/wiki/Taito_Type_X)
article (read November 2026). Its board for each game is just the section the
game is listed under, so treat it as a guide, not a certified hardware list.

## Status levels

| Level | Meaning |
|---|---|
| **Playable** | Attract mode, coin / start, character select and full matches at 60 fps, with no known blocker. |
| **Playable, issues** | Plays start to finish, with problems listed on the game's page (slowdown, missing feature). |
| **Boots** | Reaches attract mode or a menu, but gameplay is incomplete or broken. |
| **Dump on hand** | Not started, but a dump is available for testing. |
| — | Not started, no dump available. |

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

Every supported game ships in the same RetroArch core and the same Android
app. Both identify the game from its `game.exe`.

## Next candidates (dumps on hand)

| Game | Board | Year | What it needs | Page |
|---|---|---|---|---|
| The King of Fighters '98 Ultimate Match | Type X | 2008 | `launcher.exe` imports D3D9, debug D3DX 36 (`d3dx9d_36.dll`), DirectSound and WinMM, all close to what the runtime already has. It is the first Type X (not X2) game. | [KOF '98 UM](Game-KOF-98-UM.md) |
| The King of Fighters: Maximum Impact Regulation A | Type X2 | 2007 | A 3D game. It also imports `MSVCR80.dll` (the C runtime as a DLL: either implement it or recompile the DLL), Direct3D 8, DirectDraw and DirectInput 8. | [KOF MI Regulation A](Game-KOF-MI-Regulation-A.md) |

## Type X2 library (2007–2015)

★ = NESiCAxLive download release. These are network-delivered builds, which
may need a different loader than disc / HDD builds.

| Game | Year | Status |
|---|---|---|
| Akai Katana Shin ★ | 2012 | — |
| Aquapazza: Aquaplus Dream Match ★ | 2011 | — |
| Aquarian Age Alternative | 2007 | — |
| Arcana Heart 2 ★ | 2012 | — |
| Arcana Heart 3 Love Max!!!!! ★ | 2013 | — |
| Arcana Heart 3 Love Max Six Stars!!!!!! ★ | 2014 | — |
| Battle Fantasia | 2007 | — |
| Battle Fantasia Network Edition ★ | 2011 | — |
| Battle Gear 4 Tuned 2010 | 2010 | — |
| **BlazBlue: Calamity Trigger** | 2008 | **Playable, issues** |
| BlazBlue: Central Fiction ★ | 2015 | — |
| BlazBlue: Chrono Phantasma ★ | 2012 | — |
| **BlazBlue: Continuum Shift** | 2009 | **Playable** |
| BlazBlue: Continuum Shift II ★ | 2010 | — |
| Chaos Breaker ★ | 2012 | — |
| Chaos Code: New Sign of Catastrophe ★ | 2013 | — |
| Chase H.Q. 2 | 2007 | — |
| Cho Chabudai Gaeshi! | 2009 | — |
| Cho Chabudai Gaeshi! 2 | 2010 | — |
| Cho Chabudai Gaeshi! Kyojinnohoshi | 2011 | — |
| Crimzon Clover ★ | 2013 | — |
| Cyber Diver | 2009 | — |
| D1GP Arcade | 2007 | — |
| Daemon Bride - Additional Gain ★ | 2011 | — |
| Darius Burst: Another Chronicle | 2010 | — |
| Darius Burst: Another Chronicle EX | 2011 | — |
| Dark Awake ★ | 2012 | — |
| Do Not Fall ★ | 2013 | — |
| Dragon Dance ★ | 2011 | — |
| Dragon Quest Monster Battle Road | 2007 | — |
| Dragon Quest Monster Battle Road II Legends | 2009 | — |
| Dragon Quest Monster Battle Road Victory | 2010 | — |
| E-Mahjong ★ | 2013 | — |
| Elevator Action Death Parade | 2009 | — |
| Elevator Action ★ | 2014 | — |
| EN-Eins Perfektewelt ★ | 2012 | — |
| Exception ★ | 2011 | — |
| Gaia Attack 4 | 2010 | — |
| Gouketsuji Ichizoku: Matsuri Senzo Kuyou ★ | 2012 | — |
| Homura ★ | 2012 | — |
| Hopping Road | 2009 | — |
| Hopping Road Kids | 2010 | — |
| Hyper Street Fighter II: The Anniversary Edition ★ | 2014 | — |
| Ikaruga ★ | 2013 | — |
| **The King of Fighters: Maximum Impact Regulation A** | 2007 | **Dump on hand** |
| Legendary Block King | 2012 | — |
| Lord of Vermilion | 2008 | — |
| Lord of Vermilion II | 2009 | — |
| Lord of Vermilion Re:2 | 2011 | — |
| Magical Beat ★ | 2012 | — |
| Monster Hunter Nikki Angrily Pooh Ghee Race | 2013 | — |
| Music Gun Gun! | 2009 | — |
| Music Gun Gun! 2 | 2011 | — |
| Music Gun Gun!: Uta ga Ippai Cho Zokaban | 2010 | — |
| Nitroplus Blasterz: Heroines Infinite Duel ★ | 2015 | — |
| Oppopo Booom | 2009 | — |
| Panic Museum / Haunted Museum | 2009 | — |
| Persona 4: The Ultimate in Mayonaka Arena ★ | 2012 | — |
| Persona 4: The Ultimax Ultra Suplex Hold ★ | 2013 | — |
| Pretty Rhythm | 2010 | — |
| Psychic Force 2012 ★ | 2012 | — |
| Puzzle Bobble ★ | 2012 | — |
| Raiden III ★ | 2012 | — |
| Raiden IV ★ | 2012 | — |
| Samurai Shodown: Edge of Destiny / Samurai Spirits Sen | 2008 | — |
| Samurai Spirits Sen ★ | 2011 | — |
| Senko no Ronde Duo - Dis-United Order | 2009 | — |
| Senko no Ronde Duo - Dis-United Order ★ | 2011 | — |
| Senor Nippon! | 2009 | — |
| Shh...! Welcome to Frightfearland / Haunted Museum II | 2010 | — |
| Skullgirls 2nd Encore ★ | 2015 | — |
| Sonic Blast Heroes | 2011 | — |
| Sonic Blast Heroes Dash | 2012 | — |
| Space Invaders ★ | 2012 | — |
| Spica Adventure ★ | 2011 | — |
| Strania -The Stella Machina- ★ | 2011 | — |
| Street Fighter III 3rd Strike: Fight for the Future ★ | 2014 | — |
| Street Fighter IV | 2008 | — |
| Street Fighter Zero 3 ★ | 2014 | — |
| Suggoi! Arcana Heart 2 ★ | 2012 | — |
| **Super Street Fighter IV: Arcade Edition** | 2010 | **Playable, issues** |
| Super Street Fighter IV: Arcade Edition Ver.2012 | 2011 | — |
| The King of Fighters '98 Ultimate Match Final Edition ★ | 2011 | — |
| The King of Fighters 2002 Unlimited Match ★ | 2011 | — |
| **The King of Fighters XII** | 2009 | **Playable** |
| **The King of Fighters XIII** | 2010 | **Playable** |
| **The King of Fighters XIII Climax** | 2012 | **Playable** |
| The King of Fighters XIII Climax ★ | 2013 | — |
| The pieces Naimmentanpin-Doradorara~tsu miss! ★ | 2014 | — |
| The Rumble Fish 2 ★ | 2012 | — |
| Trouble Witches AC ★ | 2012 | — |
| Ultra Street Fighter IV ★ | 2014 | — |
| Vampire Savior ★ | 2014 | — |
| Wacky Races | 2009 | — |
| Yatagarasu: Attack on Cataclysm ★ | 2015 | — |
| Yuukyuu no Sharin: Eternal Wheel | 2007 | — |

Likely easiest after the current set: games built the same way as titles
that already run. Street Fighter IV and SSF4 AE Ver.2012 share SSF4 AE's
engine. BlazBlue CS II shares Continuum Shift's engine. The KOF XIII Climax
NESiCAxLive build shares Climax's engine.

## Type X / Type X+ library (2003–2013)

Type X runs the same OS family (Windows XP Embedded on 32-bit x86) on older
hardware: an AGP Radeon 9600-class GPU and an Intel 865G chipset. Wikipedia
says its software doesn't run on Type X2 and vice versa, because of the
drivers. For this project, that means the recompiler should carry over as is.
The runtime may need older DirectX interfaces (Direct3D 8, DirectDraw) and
the board's I/O setup. Type X+ is Type X with a stronger graphics card.

| Game | Year | Status |
|---|---|---|
| Battle Gear 4 | 2005 | — |
| Battle Gear 4 Tuned | 2006 | — |
| Chaos Breaker | 2004 | — |
| Dance Dance!! | 2003 | — |
| Dance Dance!! Plus | 2006 | — |
| Data Carddass Dragon Ball Z | 2005 | — |
| Data Carddass Dragon Ball Z 2 | 2006 | — |
| Dinoking Battle / King of Jurassic | 2005 | — |
| Dinoking Battle II | 2006 | — |
| Dinoking Battle III | 2007 | — |
| Dinomax | 2006 | — |
| Dinomax Ver.1.5 | 2007 | — |
| Giga Wing Generations | 2004 | — |
| Goketsuji Ichizoku: Matsuri Senzo Kuyou | 2009 | — |
| Half-Life 2: Survivor | 2006 | — |
| Half-Life 2: Survivor Ver.2.0 | 2007 | — |
| Harikiri Online Professional Baseball | 2004 | — |
| Homura | 2005 | — |
| Inazuma Eleven ~ explosive football battle ~ | 2010 | — |
| Inazuma Eleven Go Battle Stadium | 2011 | — |
| Inazuma Eleven Go Batrism | 2013 | — |
| K-ON! Post-school rhythm time | 2013 | — |
| Katekyo Hitman Reborn! Southern Cross Battle | 2008 | — |
| Kirarin Revolution Happy Idol Life | 2006 | — |
| Mobile Suit Gundam: Spirits of Zeon | 2006 | — |
| Mobile Suit Gundam: Spirits of Zeon Memories | 2007 | — |
| Pokémon Battrio | 2007 | — |
| Raiden III | 2005 | — |
| Raiden IV | 2007 | — |
| Shikigami no Shiro III | 2006 | — |
| Spica Adventure | 2005 | — |
| Supreme!! Mecha Mote Chairperson Kurmote Girls Contest! | 2009 | — |
| Taisen Hot Gimmick 5 | 2006 | — |
| Taisen Hot Gimmick Mix Party | 2005 | — |
| Tetris The Grand Master 3: Terror Instinct | 2005 | — |
| Tetsudamasy | 2011 | — |
| **The King of Fighters '98 Ultimate Match** | 2008 | **Dump on hand** |
| The King of Fighters: Sky Stage | 2010 | — |
| Trouble Witches AC | 2009 | — |
| Usagi: Yasei no Toupai Online | 2005 | — |
| Valve Limit R | 2006 | — |
| War of the Grail | 2006 | — |
| Won!Tertainment Music Channel | 2006 | — |
| Zoids Card Colosseum | 2005 | — |

Many of these, such as card dispensers, network titles and medal games,
depend on peripherals beyond a joystick panel. Those are out of scope unless
someone brings the hardware protocol.
