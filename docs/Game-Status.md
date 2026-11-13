# Game status

Progress for every known game on the Taito Type X, Type X+ and Type X2 boards.
Every game has a status: playable, planned, or not started yet.

No single source lists every game. These lists merge the following
(read November 2026):
- English Wikipedia's [Taito Type X](https://en.wikipedia.org/wiki/Taito_Type_X)
  article: the base list.
- Japanese Wikipedia's [TAITO Type X](https://ja.wikipedia.org/wiki/TAITO_Type_X)
  article: alternate names and cancelled games.
- A [community Type X2 list](https://www.arcade-projects.com/threads/taito-type-x2-launcher-replacement.30455/):
  release variants.

The board for each game is the section the sources list it under. Treat it as
a guide, not a certified hardware list. Rows marked † appear in only one
source. If you know a missing game or a wrong board, please open an issue.

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

Every supported game ships in the same RetroArch core and the same Android
app. Both identify the game from its `game.exe`.

## Planned

| Game | Board | Year | What it needs | Page |
|---|---|---|---|---|
| The King of Fighters '98 Ultimate Match | Type X | 2008 | `launcher.exe` imports D3D9, debug D3DX 36 (`d3dx9d_36.dll`), DirectSound and WinMM, all close to what the runtime already has. It is the first Type X (not X2) game. | [KOF '98 UM](Game-KOF-98-UM.md) |
| The King of Fighters: Maximum Impact Regulation A | Type X2 | 2007 | A 3D game. It also imports `MSVCR80.dll` (the C runtime as a DLL: either implement it or recompile the DLL), Direct3D 8, DirectDraw and DirectInput 8. | [KOF MI Regulation A](Game-KOF-MI-Regulation-A.md) |

## Type X2 library (2007–2015)

★ = NESiCAxLive download release. These are network-delivered builds, which
may need a different loader than disc / HDD builds.

| Game | Year | Status |
|---|---|---|
| Akai Katana Shin ★ | 2012 | Not started |
| Aquapazza: Aquaplus Dream Match ★ | 2011 | Not started |
| Aquarian Age Alternative | 2007 | Not started |
| Arcana Heart 2 ★ | 2012 | Not started |
| Arcana Heart 3 Love Max!!!!! ★ | 2013 | Not started |
| Arcana Heart 3 Love Max Six Stars!!!!!! ★ | 2014 | Not started |
| Battle Fantasia | 2007 | Not started |
| Battle Fantasia Network Edition ★ | 2011 | Not started |
| Battle Gear 4 Tuned 2010 | 2010 | Not started |
| **BlazBlue: Calamity Trigger** | 2008 | **Playable, issues** |
| BlazBlue: Central Fiction ★ | 2015 | Not started |
| BlazBlue: Chrono Phantasma ★ | 2012 | Not started |
| **BlazBlue: Continuum Shift** | 2009 | **Playable** |
| BlazBlue: Continuum Shift II ★ | 2010 | Not started |
| Chaos Breaker ★ | 2012 | Not started |
| Chaos Code: New Sign of Catastrophe ★ | 2013 | Not started |
| Chase H.Q. 2 | 2007 | Not started |
| Cho Chabudai Gaeshi! | 2009 | Not started |
| Cho Chabudai Gaeshi! 2 | 2010 | Not started |
| Cho Chabudai Gaeshi! Kyojinnohoshi | 2011 | Not started |
| Crimzon Clover ★ | 2013 | Not started |
| Cyber Diver | 2009 | Not started |
| D1GP Arcade | 2007 | Not started |
| Daemon Bride - Additional Gain ★ | 2011 | Not started |
| Darius Burst: Another Chronicle | 2010 | Not started |
| Darius Burst: Another Chronicle EX | 2011 | Not started |
| Dark Awake ★ | 2012 | Not started |
| Do Not Fall ★ | 2013 | Not started |
| Dragon Dance ★ | 2011 | Not started |
| Dragon Quest Monster Battle Road | 2007 | Not started |
| Dragon Quest Monster Battle Road II Legends | 2009 | Not started |
| Dragon Quest Monster Battle Road Victory | 2010 | Not started |
| E-Mahjong ★ | 2013 | Not started |
| Elevator Action Death Parade | 2009 | Not started |
| Elevator Action ★ | 2014 | Not started |
| EN-Eins Perfektewelt ★ | 2012 | Not started |
| Exception ★ | 2011 | Not started |
| Gaia Attack 4 | 2010 | Not started |
| Gouketsuji Ichizoku: Matsuri Senzo Kuyou ★ | 2012 | Not started |
| Homura ★ | 2012 | Not started |
| Hopping Road | 2009 | Not started |
| Hopping Road Kids | 2010 | Not started |
| Hyper Street Fighter II: The Anniversary Edition ★ | 2014 | Not started |
| Ikaruga ★ | 2013 | Not started |
| **The King of Fighters: Maximum Impact Regulation A** | 2007 | **Planned** |
| Legendary Block King | 2012 | Not started |
| Lord of Vermilion | 2008 | Not started |
| Lord of Vermilion II | 2009 | Not started |
| Lord of Vermilion Re:2 | 2011 | Not started |
| Lupin the 3rd † (community list only; exact title unconfirmed) | — | Not started |
| Magical Beat ★ | 2012 | Not started |
| Monster Hunter Nikki Angrily Pooh Ghee Race | 2013 | Not started |
| Music Gun Gun! | 2009 | Not started |
| Music Gun Gun! 2 | 2011 | Not started |
| Music Gun Gun!: Uta ga Ippai Cho Zokaban | 2010 | Not started |
| Nitroplus Blasterz: Heroines Infinite Duel ★ | 2015 | Not started |
| Oppopo Booom | 2009 | Not started |
| Panic Museum / Haunted Museum | 2009 | Not started |
| Persona 4: The Ultimate in Mayonaka Arena ★ | 2012 | Not started |
| Persona 4: The Ultimax Ultra Suplex Hold ★ | 2013 | Not started |
| Pretty Rhythm | 2010 | Not started |
| Psychic Force 2012 ★ | 2012 | Not started |
| Puzzle Bobble ★ | 2012 | Not started |
| Raiden III ★ | 2012 | Not started |
| Raiden IV ★ | 2012 | Not started |
| Samurai Shodown: Edge of Destiny / Samurai Spirits Sen | 2008 | Not started |
| Samurai Spirits Sen ★ | 2011 | Not started |
| Senko no Ronde Duo - Dis-United Order | 2009 | Not started |
| Senko no Ronde Duo - Dis-United Order ★ | 2011 | Not started |
| Senor Nippon! | 2009 | Not started |
| Shh...! Welcome to Frightfearland / Haunted Museum II | 2010 | Not started |
| Skullgirls 2nd Encore ★ | 2015 | Not started |
| Sonic Blast Heroes | 2011 | Not started |
| Sonic Blast Heroes Dash | 2012 | Not started |
| Space Invaders ★ | 2012 | Not started |
| Spica Adventure ★ | 2011 | Not started |
| Strania -The Stella Machina- ★ | 2011 | Not started |
| Street Fighter III 3rd Strike: Fight for the Future ★ | 2014 | Not started |
| Street Fighter IV | 2008 | Not started |
| Street Fighter Zero 3 ★ | 2014 | Not started |
| Suggoi! Arcana Heart 2 ★ | 2012 | Not started |
| **Super Street Fighter IV: Arcade Edition** | 2010 | **Playable, issues** |
| Super Street Fighter IV: Arcade Edition Ver.2012 | 2011 | Not started |
| The King of Fighters '98 Ultimate Match Final Edition ★ | 2011 | Not started |
| The King of Fighters 2002 Unlimited Match ★ | 2011 | Not started |
| **The King of Fighters XII** | 2009 | **Playable** |
| **The King of Fighters XIII** | 2010 | **Playable** |
| **The King of Fighters XIII Climax** | 2012 | **Playable** |
| The King of Fighters XIII Climax ★ | 2013 | Not started |
| The pieces Naimmentanpin-Doradorara~tsu miss! ★ | 2014 | Not started |
| The Rumble Fish 2 ★ | 2012 | Not started |
| Trouble Witches AC ★ | 2012 | Not started |
| Ultra Street Fighter IV ★ | 2014 | Not started |
| Vampire Savior ★ | 2014 | Not started |
| Wacky Races | 2009 | Not started |
| Yatagarasu: Attack on Cataclysm ★ | 2015 | Not started |
| Yuukyuu no Sharin: Eternal Wheel | 2007 | Not started |

### Release variants

Some games shipped in several builds. A port is per `game.exe`, so each build
may need its own check:

| Game | Builds |
|---|---|
| Super Street Fighter IV Arcade Edition | Ver.1.0 (NESYS network); EXP stand-alone Ver.1.0, **the build our port uses**; Ver.2012 (Ver.1.3, NESYS) |
| Elevator Action Death Parade | Japanese and export (EXP) |
| The King of Fighters XIII Climax | Disc / HDD release (supported); NESiCAxLive release (2013) |

### Cancelled Type X2 games

Japanese Wikipedia names two Type X2 games that were never released:
*Minna de Watto!* (みんなでワッと!) and *Top Speed* (トップスピード).

### Easiest next ports

Games built the same way as titles that already run should come first. Street Fighter IV and SSF4 AE Ver.2012 share SSF4 AE's
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
| Battle Gear 4 | 2005 | Not started |
| Battle Gear 4 Tuned | 2006 | Not started |
| Chaos Breaker | 2004 | Not started |
| Dance Dance!! | 2003 | Not started |
| Dance Dance!! Plus | 2006 | Not started |
| Data Carddass Dragon Ball Z | 2005 | Not started |
| Data Carddass Dragon Ball Z 2 | 2006 | Not started |
| Dinoking Battle / King of Jurassic | 2005 | Not started |
| Dinoking Battle II | 2006 | Not started |
| Dinoking Battle III | 2007 | Not started |
| Dinomax | 2006 | Not started |
| Dinomax Ver.1.5 | 2007 | Not started |
| Giga Wing Generations | 2004 | Not started |
| Goketsuji Ichizoku: Matsuri Senzo Kuyou | 2009 | Not started |
| Half-Life 2: Survivor | 2006 | Not started |
| Half-Life 2: Survivor Ver.2.0 | 2007 | Not started |
| Harikiri Online Professional Baseball | 2004 | Not started |
| Homura | 2005 | Not started |
| Inazuma Eleven ~ explosive football battle ~ | 2010 | Not started |
| Inazuma Eleven Go Battle Stadium | 2011 | Not started |
| Inazuma Eleven Go Batrism | 2013 | Not started |
| K-ON! Post-school rhythm time | 2013 | Not started |
| Katekyo Hitman Reborn! Southern Cross Battle | 2008 | Not started |
| Kirarin Revolution Happy Idol Life | 2006 | Not started |
| Mobile Suit Gundam: Spirits of Zeon | 2006 | Not started |
| Mobile Suit Gundam: Spirits of Zeon Memories | 2007 | Not started |
| Pokémon Battrio | 2007 | Not started |
| Raiden III | 2005 | Not started |
| Raiden IV | 2007 | Not started |
| Shikigami no Shiro III | 2006 | Not started |
| Spica Adventure | 2005 | Not started |
| Supreme!! Mecha Mote Chairperson Kurmote Girls Contest! | 2009 | Not started |
| Taisen Hot Gimmick 5 | 2006 | Not started |
| Taisen Hot Gimmick: Mirai Eigou † (Japanese Wikipedia; possibly the same game as Hot Gimmick 5) | 2006 | Not started |
| Taisen Hot Gimmick Mix Party | 2005 | Not started |
| Tetris The Grand Master 3: Terror Instinct | 2005 | Not started |
| Tetsudamasy | 2011 | Not started |
| **The King of Fighters '98 Ultimate Match** | 2008 | **Planned** |
| The King of Fighters: Sky Stage | 2010 | Not started |
| Trouble Witches AC | 2009 | Not started |
| Usagi: Yasei no Toupai Online | 2005 | Not started |
| Valve Limit R | 2006 | Not started |
| War of the Grail | 2006 | Not started |
| Won!Tertainment Music Channel | 2006 | Not started |
| Zoids Card Colosseum | 2005 | Not started |

Many of these, such as card dispensers, network titles and medal games,
depend on peripherals beyond a joystick panel. Those are out of scope unless
someone brings the hardware protocol.
