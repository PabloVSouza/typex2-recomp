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

const DEFAULT: Layout = Layout { label: "Game default", pad: "" };
const KOF: &[Layout] = &[
    DEFAULT,
    Layout { label: "8-button stick (L1 / L2 rows): A C over B D", pad: "l1=a,west=c,north=ab,r1=bc,l2=b,south=d,east=cd,r2=abc" },
];
const SF4: &[Layout] = &[
    DEFAULT,
    Layout { label: "8-button stick (L1 / L2 rows)", pad: "l1=1,west=2,north=3,r1=123,l2=4,south=5,east=6,r2=456" },
];
const BLAZBLUE: &[Layout] = &[
    DEFAULT,
    Layout { label: "8-button stick, Type B panel", pad: "l1=1,west=2,north=3,r1=12,l2=4,south=23,east=123,r2=1234" },
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
];

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

/// The supported game in `dir` and its executable.
pub fn identify(dir: &Path) -> Result<(&'static Game, PathBuf), String> {
    let mut seen = false;
    for name in EXE_NAMES {
        let exe = dir.join(name);
        if !exe.is_file() {
            continue;
        }
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
}
