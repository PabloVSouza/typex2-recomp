//! Game art for the library cards: the user's own ES-DE scraped media (fanart, screenshots,
//! title screens, miximages, covers) for the game's folder, found in ES-DE's media folder and
//! only read. Without any, `remote` fetches the console port's libretro thumbnail once.
use std::fs;
use std::path::{Path, PathBuf};

/// Biggest image handed to the window (fanart is a few hundred KB to a few MB).
const MAX_BYTES: u64 = 8 << 20;
const KINDS: [&str; 5] = ["fanart", "screenshots", "titlescreens", "miximages", "covers"];
const EXTS: [&str; 3] = ["jpg", "png", "jpeg"];

/// ES-DE's home folders: ~/ES-DE (desktop), the shared storage's ES-DE folder (Android).
fn esde_homes() -> Vec<PathBuf> {
    let mut v = Vec::new();
    if cfg!(target_os = "android") {
        v.push(PathBuf::from("/storage/emulated/0/ES-DE"));
    }
    for var in ["HOME", "USERPROFILE"] {
        if let Ok(h) = std::env::var(var) {
            v.push(Path::new(&h).join("ES-DE"));
            v.push(Path::new(&h).join(".emulationstation"));
        }
    }
    v
}

/// The media folder: es_settings.xml's MediaDirectory when set, else <home>/downloaded_media.
fn media_dirs() -> Vec<PathBuf> {
    let mut v = Vec::new();
    for home in esde_homes() {
        let settings = fs::read_to_string(home.join("settings/es_settings.xml")).unwrap_or_default();
        if let Some(custom) = setting(&settings, "MediaDirectory").filter(|s| !s.is_empty()) {
            v.push(PathBuf::from(custom));
        }
        v.push(home.join("downloaded_media"));
    }
    v.retain(|d| d.is_dir());
    v
}

/// `<string name="key" value="..." />` in ES-DE's settings file
fn setting(xml: &str, key: &str) -> Option<String> {
    let at = xml.find(&format!("name=\"{key}\""))?;
    let rest = &xml[at..];
    let v = rest.find("value=\"")? + 7;
    let end = rest[v..].find('"')?;
    Some(rest[v..v + end].to_string())
}

/// The names ES-DE gives the game: its folder's name, with and without the .x2 suffix.
fn names(game: &Path) -> Vec<String> {
    let Some(base) = game.file_name().and_then(|n| n.to_str()) else { return Vec::new() };
    let mut v = vec![base.to_string()];
    match base.strip_suffix(".x2") {
        Some(stem) => v.push(stem.to_string()),
        None => v.push(format!("{base}.x2")),
    }
    v
}

/// The first image found for a game folder.
pub fn find(game: &Path) -> Option<PathBuf> {
    let names = names(game);
    let dirs = media_dirs();
    for kind in KINDS {
        for media in &dirs {
            let Ok(systems) = fs::read_dir(media) else { continue };
            for system in systems.flatten() {
                let dir = system.path().join(kind);
                for name in &names {
                    for ext in EXTS {
                        let p = dir.join(format!("{name}.{ext}"));
                        if p.is_file() {
                            return Some(p);
                        }
                    }
                }
            }
        }
    }
    None
}

/// libretro's thumbnail server (RetroArch's): the games' console ports, as Type X has no
/// system there. (system, kind, name) per game id; Samurai Shodown: Edge of Destiny has none.
const REMOTE: [(&str, &str, &str, &str); 8] = [
    ("kof12", "Sony - PlayStation 3", "Named_Snaps", "King of Fighters XII, The (USA) (En,Ja,Fr,De,Es,It)"),
    ("kof13", "Sony - PlayStation 3", "Named_Snaps", "King of Fighters XIII, The (USA) (En,Ja,Fr,De,Es,It,Zh,Ko)"),
    ("kof13cl", "Sony - PlayStation 3", "Named_Snaps", "King of Fighters XIII, The (USA) (En,Ja,Fr,De,Es,It,Zh,Ko)"),
    ("bbct", "Sony - PlayStation 3", "Named_Snaps", "BlazBlue - Calamity Trigger (USA) (En,Ja,Zh,Ko)"),
    ("bbcs", "Sony - PlayStation 3", "Named_Snaps", "BlazBlue - Continuum Shift Extend (USA) (En,Ja,Zh,Ko)"),
    ("ssf4ae", "Sony - PlayStation 3", "Named_Boxarts", "Super Street Fighter IV Arcade Edition (USA)"),
    ("kof98um", "Sony - PlayStation 2", "Named_Snaps", "King of Fighters '98, The - Ultimate Match (USA)"),
    ("kofmira", "Sony - PlayStation 2", "Named_Titles", "KOF - Maximum Impact Regulation A (Japan)"),
];

fn url_part(s: &str) -> String {
    s.bytes()
        .map(|b| if b.is_ascii_alphanumeric() || b"-_.~".contains(&b) { (b as char).to_string() } else { format!("%{b:02X}") })
        .collect()
}

/// The game's libretro thumbnail, fetched once into <data>/art (None offline or for a game with none).
pub async fn remote(data: &Path, id: &str) -> Option<PathBuf> {
    let (_, system, kind, name) = REMOTE.iter().find(|r| r.0 == id)?;
    let cache = data.join("art").join(format!("{id}.png"));
    if cache.is_file() {
        return Some(cache);
    }
    let url = format!("https://thumbnails.libretro.com/{}/{}/{}.png", url_part(system), kind, url_part(name));
    let client = crate::updates::root_certs(reqwest::Client::builder())
        .https_only(true)
        .connect_timeout(std::time::Duration::from_secs(5))
        .timeout(std::time::Duration::from_secs(20))
        .user_agent(concat!("typex2-launcher/", env!("CARGO_PKG_VERSION")))
        .build()
        .ok()?;
    let response = client.get(&url).send().await.ok()?;
    if !response.status().is_success() {
        return None;
    }
    let bytes = response.bytes().await.ok()?;
    if bytes.len() as u64 > MAX_BYTES || !bytes.starts_with(&[0x89, b'P', b'N', b'G']) {
        return None;
    }
    fs::create_dir_all(cache.parent()?).ok()?;
    fs::write(&cache, &bytes).ok()?;
    Some(cache)
}

/// The image as a data: URL for the window.
pub fn data_url(path: &Path) -> Option<String> {
    if fs::metadata(path).ok()?.len() > MAX_BYTES {
        return None;
    }
    let bytes = fs::read(path).ok()?;
    let mime = if bytes.starts_with(&[0x89, b'P', b'N', b'G']) { "image/png" } else { "image/jpeg" };
    Some(format!("data:{mime};base64,{}", base64(&bytes)))
}

fn base64(data: &[u8]) -> String {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(data.len().div_ceil(3) * 4);
    for c in data.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        out.push(T[(n >> 18) as usize & 63] as char);
        out.push(T[(n >> 12) as usize & 63] as char);
        out.push(if c.len() > 1 { T[(n >> 6) as usize & 63] as char } else { '=' });
        out.push(if c.len() > 2 { T[n as usize & 63] as char } else { '=' });
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn encodes_base64() {
        assert_eq!(base64(b"Man"), "TWFu");
        assert_eq!(base64(b"Ma"), "TWE=");
        assert_eq!(base64(b"M"), "TQ==");
    }

    #[test]
    fn reads_media_directory() {
        let xml = r#"<string name="MediaDirectory" value="/sdcard/media" />"#;
        assert_eq!(setting(xml, "MediaDirectory").as_deref(), Some("/sdcard/media"));
    }

    #[test]
    fn names_with_and_without_x2() {
        assert_eq!(names(Path::new("/r/KOF.x2")), vec!["KOF.x2", "KOF"]);
        assert_eq!(names(Path::new("/g/KOF")), vec!["KOF", "KOF.x2"]);
    }
}
