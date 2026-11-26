//! The games the recompiler supports, and identifying a game folder: its executable
//! (`game.exe`, or `launcher.exe` on some Type X games) is matched by its PE header's
//! image size and entry point, as the Android app does (runtime/src/launcher.c).
use serde::Serialize;
use std::fs::File;
use std::io::Read;
use std::path::{Path, PathBuf};

/// A gamepad layout offered for a game: an `X2_PAD` line (empty = the game's default).
#[derive(Serialize, Clone, Copy)]
pub struct Layout {
    pub id: &'static str,
    pub label: &'static str,
    pub pad: &'static str,
}

#[derive(Serialize, Clone, Copy)]
pub struct Game {
    /// `games/<id>` in the repository; also the build folder's name.
    pub id: &'static str,
    pub title: &'static str,
    pub board: &'static str,
    pub year: u16,
    pub publisher: &'static str,
    /// PE SizeOfImage and AddressOfEntryPoint (RVA + image base, as x2recomp records it).
    #[serde(skip)]
    pub size: u32,
    #[serde(skip)]
    pub entry: u32,
    pub layouts: &'static [Layout],
}

const DEFAULT: Layout = Layout { id: "default", label: "Game default", pad: "" };
const KOF: &[Layout] = &[
    DEFAULT,
    Layout { id: "stick-kof", label: "8-button stick (L1 / L2 rows): A C over B D", pad: "l1=a,west=c,north=cd,r1=bcd,l2=b,south=d,east=ab,r2=abc" },
];
const SF4: &[Layout] = &[
    DEFAULT,
    Layout { id: "stick-sf4", label: "8-button stick (L1 / L2 rows)", pad: "l1=1,west=2,north=3,r1=123,l2=4,south=5,east=6,r2=456" },
];
const BLAZBLUE: &[Layout] = &[
    DEFAULT,
    Layout { id: "stick-bb", label: "8-button stick, Type B panel", pad: "l1=1,west=2,north=3,r1=123,l2=4,south=23,east=12,r2=1234" },
];
const SSE: &[Layout] = &[
    DEFAULT,
    Layout { id: "stick-sse", label: "8-button stick: A B C over R", pad: "l1=1,west=2,north=3,r1=123,l2=4,south=23,east=12,r2=1234" },
];
const GOKETSUJI: &[Layout] = &[
    DEFAULT,
    Layout { id: "stick-gok", label: "8-button stick (L1 / L2 rows): punches over kicks", pad: "l1=1,west=2,north=25,r1=245,l2=4,south=5,east=14,r2=124" },
];

pub const GAMES: &[Game] = &[
    Game { id: "kof12", title: "The King of Fighters XII", board: "Type X2", year: 2009, publisher: "SNK Playmore", size: 0x2ec000, entry: 0x5f120b, layouts: KOF },
    Game { id: "kof13", title: "The King of Fighters XIII", board: "Type X2", year: 2010, publisher: "SNK Playmore", size: 0x3a7000, entry: 0x549992, layouts: KOF },
    Game { id: "kof13cl", title: "The King of Fighters XIII Climax", board: "Type X2", year: 2012, publisher: "SNK Playmore", size: 0x2442000, entry: 0x521af3, layouts: KOF },
    Game { id: "ssf4ae", title: "Super Street Fighter IV Arcade Edition", board: "Type X2", year: 2010, publisher: "Capcom", size: 0x7f4000, entry: 0x5e2719, layouts: SF4 },
    Game { id: "bbcs", title: "BlazBlue: Continuum Shift", board: "Type X2", year: 2009, publisher: "Arc System Works", size: 0xc22000, entry: 0x557219, layouts: BLAZBLUE },
    Game { id: "bbct", title: "BlazBlue: Calamity Trigger", board: "Type X2", year: 2008, publisher: "Arc System Works", size: 0x714000, entry: 0x4d703e, layouts: BLAZBLUE },
    Game { id: "kof98um", title: "The King of Fighters '98 Ultimate Match", board: "Type X", year: 2008, publisher: "SNK Playmore", size: 0x18e5000, entry: 0x5b5250, layouts: KOF },
    Game { id: "kofmira", title: "The King of Fighters: Maximum Impact Regulation A", board: "Type X2", year: 2007, publisher: "SNK Playmore", size: 0x1c3b000, entry: 0x79aba1, layouts: KOF },
    Game { id: "sse", title: "Samurai Shodown: Edge of Destiny", board: "Type X2", year: 2008, publisher: "SNK Playmore", size: 0x3344000, entry: 0x4d9fa0, layouts: SSE },
    Game { id: "goketsuji", title: "Gouketsuji Ichizoku Matsuri Senzo Kuyou", board: "Type X2", year: 2012, publisher: "Atlus", size: 0x106a6000, entry: 0x40390a, layouts: GOKETSUJI },
    Game { id: "kof2002um", title: "The King of Fighters 2002 Unlimited Match", board: "Type X2", year: 2011, publisher: "SNK Playmore", size: 0xbe3b000, entry: 0x776987, layouts: KOF },
];

impl Game {
    /// Its pack for the Android app (runtime/CMakeLists.txt `packs`, runtime/src/launcher.c).
    pub fn pack_name(&self) -> String {
        format!("x2-{:08x}-{:08x}.so", self.size, self.entry)
    }
}

pub fn by_id(id: &str) -> Option<&'static Game> {
    GAMES.iter().find(|g| g.id == id)
}

/// ImageBase + AddressOfEntryPoint and SizeOfImage from a PE file's headers.
pub fn pe_identity(path: &Path) -> Option<(u32, u32)> {
    let mut head = vec![0u8; 4096];
    let n = File::open(path).ok()?.read(&mut head).ok()?;
    head.truncate(n);
    let u32_at = |o: usize| head.get(o..o + 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]));
    if head.get(0..2)? != b"MZ" {
        return None;
    }
    let pe = u32_at(0x3c)? as usize;
    if head.get(pe..pe + 4)? != b"PE\0\0" {
        return None;
    }
    let opt = pe + 24;
    let entry = u32_at(opt + 16)?;
    let base = u32_at(opt + 28)?;
    let size = u32_at(opt + 56)?;
    Some((size, base.wrapping_add(entry)))
}

/// The executables a game folder may hold, in the order the runtime tries them (game_exe.h).
pub const EXE_NAMES: &[&str] = &["game.exe", "launcher.exe"];

/// `name` in `dir`, matched without case as on Windows (Goketsuji Ichizoku's dump has `Game.exe`;
/// Android's and Linux's storage is case-sensitive).
pub fn find_exe(dir: &Path, name: &str) -> Option<PathBuf> {
    let exact = dir.join(name);
    if exact.is_file() {
        return Some(exact);
    }
    std::fs::read_dir(dir).ok()?.flatten().map(|e| e.path()).find(|p| p.is_file() && p.file_name().and_then(|n| n.to_str()).is_some_and(|n| n.eq_ignore_ascii_case(name)))
}

/// The supported game in `dir` and its executable.
pub fn identify(dir: &Path) -> Result<(&'static Game, PathBuf), String> {
    let mut seen = false;
    for name in EXE_NAMES {
        let Some(exe) = find_exe(dir, name) else {
            continue;
        };
        seen = true;
        if let Some((size, entry)) = pe_identity(&exe) {
            if let Some(game) = GAMES.iter().find(|g| g.size == size && g.entry == entry) {
                return Ok((game, exe));
            }
        }
    }
    Err(if seen {
        "This folder's game.exe is not one of the supported games (or is another version of one).".into()
    } else {
        "No game.exe or launcher.exe in this folder. Choose the folder that holds the game's executable.".into()
    })
}

/// A game folder as the library lists it: a supported game, or another game's executable (not
/// supported yet), kept as `other-<size>-<entry>` so each game is listed once.
pub struct Detected {
    pub id: String,
    pub game: Option<&'static Game>,
}

/// Any game in `dir`: a supported one first, else the first executable with a PE header.
pub fn detect(dir: &Path) -> Option<Detected> {
    if let Ok((game, _)) = identify(dir) {
        return Some(Detected { id: game.id.into(), game: Some(game) });
    }
    EXE_NAMES.iter().find_map(|name| find_exe(dir, name).and_then(|exe| pe_identity(&exe))).map(|(size, entry)| Detected { id: format!("other-{size:08x}-{entry:08x}"), game: None })
}

/// The name an unsupported game is listed under: its folder's, without ES-DE's `.x2`.
pub fn folder_title(path: &str) -> String {
    let name = Path::new(path).file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| path.into());
    name.strip_suffix(".x2").unwrap_or(&name).to_string()
}

/// The games under `root` (a folder holding the game folders, like a collection on
/// another disk), each with its folder: folders are searched breadth-first, at most `SCAN_DEPTH`
/// levels down, skipping hidden folders and links; a folder holding a game is not searched further,
/// and only the first copy of a game counts. `SCAN_MAX_DIRS` keeps a whole disk from being walked.
pub fn scan(root: &Path) -> Vec<(Detected, PathBuf)> {
    const SCAN_DEPTH: usize = 4;
    const SCAN_MAX_DIRS: usize = 20_000;
    let mut found: Vec<(Detected, PathBuf)> = Vec::new();
    let mut queue = std::collections::VecDeque::from([(root.to_path_buf(), 0usize)]);
    let mut visited = 0;
    while let Some((dir, depth)) = queue.pop_front() {
        visited += 1;
        if visited > SCAN_MAX_DIRS {
            break;
        }
        if let Some(game) = detect(&dir) {
            if !found.iter().any(|(g, _)| g.id == game.id) {
                found.push((game, dir));
            }
            continue;
        }
        if depth == SCAN_DEPTH {
            continue;
        }
        let Ok(entries) = std::fs::read_dir(&dir) else { continue };
        let mut subdirs: Vec<PathBuf> = entries
            .flatten()
            .filter(|e| e.file_type().is_ok_and(|t| t.is_dir()) && !e.file_name().to_string_lossy().starts_with('.'))
            .map(|e| e.path())
            .collect();
        subdirs.sort();
        queue.extend(subdirs.into_iter().map(|d| (d, depth + 1)));
    }
    found
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A minimal PE header with the given image base, entry RVA and image size.
    fn pe(base: u32, entry: u32, size: u32) -> Vec<u8> {
        let mut b = vec![0u8; 512];
        b[0..2].copy_from_slice(b"MZ");
        b[0x3c..0x40].copy_from_slice(&0x80u32.to_le_bytes());
        b[0x80..0x84].copy_from_slice(b"PE\0\0");
        let opt = 0x80 + 24;
        b[opt + 16..opt + 20].copy_from_slice(&entry.to_le_bytes());
        b[opt + 28..opt + 32].copy_from_slice(&base.to_le_bytes());
        b[opt + 56..opt + 60].copy_from_slice(&size.to_le_bytes());
        b
    }

    #[test]
    fn games_are_identified_by_image_size_and_entry() {
        let dir = std::env::temp_dir().join(format!("x2-identify-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        assert!(identify(&dir).is_err());
        // BlazBlue CT ships its executable as launcher.exe
        std::fs::write(dir.join("launcher.exe"), pe(0x400000, 0x0d703e, 0x714000)).unwrap();
        assert_eq!(identify(&dir).unwrap().0.id, "bbct");
        std::fs::write(dir.join("game.exe"), pe(0x400000, 0x1000, 0x1000)).unwrap();
        assert_eq!(identify(&dir).unwrap().0.id, "bbct"); // unknown game.exe, known launcher.exe
        std::fs::remove_file(dir.join("launcher.exe")).unwrap();
        assert!(identify(&dir).is_err());
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn the_executable_name_ignores_case() {
        let dir = std::env::temp_dir().join(format!("x2-case-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("Game.exe"), pe(0x400000, 0x0390a, 0x106a6000)).unwrap();
        let (game, exe) = identify(&dir).unwrap();
        assert_eq!(game.id, "goketsuji");
        assert!(exe.file_name().unwrap().eq_ignore_ascii_case("game.exe")); // as stored on a case-sensitive disk
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn a_collection_folder_is_scanned_for_games() {
        let root = std::env::temp_dir().join(format!("x2-scan-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let bbct = root.join("BlazBlue Calamity Trigger");
        let kof = root.join("SNK").join("KOF 98 UM").join("data"); // deeper, under a publisher folder
        let copy = root.join("Backups").join("BlazBlue CT copy");
        let hidden = root.join(".trash").join("BlazBlue");
        let other = root.join("Raiden IV.x2"); // a game not supported yet
        for d in [&bbct, &kof, &copy, &hidden, &other] {
            std::fs::create_dir_all(d).unwrap();
        }
        std::fs::write(bbct.join("launcher.exe"), pe(0x400000, 0x0d703e, 0x714000)).unwrap();
        std::fs::write(copy.join("launcher.exe"), pe(0x400000, 0x0d703e, 0x714000)).unwrap();
        std::fs::write(hidden.join("launcher.exe"), pe(0x400000, 0x0d703e, 0x714000)).unwrap();
        std::fs::write(kof.join("game.exe"), pe(0x400000, 0x1b5250, 0x18e5000)).unwrap();
        std::fs::write(other.join("game.exe"), pe(0x400000, 0x1000, 0x2000)).unwrap();
        let found = scan(&root);
        let ids: Vec<&str> = found.iter().map(|(g, _)| g.id.as_str()).collect();
        assert_eq!(ids, ["bbct", "other-00002000-00401000", "kof98um"]); // the first copy only, nothing hidden
        assert!(found[1].0.game.is_none());
        assert_eq!(folder_title(&found[1].1.to_string_lossy()), "Raiden IV");
        assert_eq!(found[0].1, bbct);
        assert_eq!(found[2].1, kof);
        std::fs::remove_dir_all(&root).unwrap();
    }
}
