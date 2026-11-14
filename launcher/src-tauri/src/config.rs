//! The launcher's settings: the user's game folders with their per-game options, and where
//! builds go. One JSON file in the app's data folder.
use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};

#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
pub struct GameSettings {
    pub fullscreen: bool,
    /// FPS overlay at start (X2_FPS=1).
    pub fps: bool,
    /// X2_PAD line; empty = the game's default layout.
    pub pad: String,
    /// Backbuffer scale (X2_BB_SCALE); 0 = automatic.
    pub scale: u32,
    /// Vsynced presentation (X2_VSYNC=1).
    pub vsync: bool,
    /// Log level 0-3 (-v, -vv, -vvv).
    pub log_level: u8,
    /// Extra environment, one KEY=VALUE per line.
    pub env: String,
}

#[derive(Serialize, Deserialize, Clone)]
pub struct GameEntry {
    pub id: String,
    pub path: String,
    #[serde(default)]
    pub settings: GameSettings,
}

#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
pub struct Config {
    pub games: Vec<GameEntry>,
    /// Android NDK folder, when not found by itself.
    pub ndk: String,
    /// Build jobs; 0 = all cores but two.
    pub jobs: u32,
    /// The launcher's language ("" = the system's).
    pub language: String,
}

impl Config {
    pub fn load(path: &Path) -> Config {
        fs::read_to_string(path).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default()
    }

    /// Written to a temporary file and renamed, so a crash never leaves half a file.
    pub fn save(&self, path: &Path) -> Result<(), String> {
        if let Some(dir) = path.parent() {
            fs::create_dir_all(dir).map_err(|e| e.to_string())?;
        }
        let tmp = path.with_extension("json.tmp");
        fs::write(&tmp, serde_json::to_vec_pretty(self).map_err(|e| e.to_string())?).map_err(|e| e.to_string())?;
        fs::rename(&tmp, path).map_err(|e| e.to_string())
    }

    pub fn game(&self, id: &str) -> Option<&GameEntry> {
        self.games.iter().find(|g| g.id == id)
    }

    pub fn ndk_setting(&self) -> Option<&str> {
        Some(self.ndk.as_str()).filter(|s| !s.is_empty())
    }

    pub fn jobs(&self) -> usize {
        if self.jobs > 0 {
            return self.jobs as usize;
        }
        std::thread::available_parallelism().map(|n| n.get().saturating_sub(2).max(1)).unwrap_or(4)
    }
}

impl GameSettings {
    /// The environment and arguments the runtime reads (README, "Debugging switches").
    pub fn launch(&self) -> (Vec<(String, String)>, Vec<String>) {
        let mut env = Vec::new();
        let mut args = Vec::new();
        if self.fps {
            env.push(("X2_FPS".into(), "1".into()));
        }
        if !self.pad.trim().is_empty() {
            env.push(("X2_PAD".into(), self.pad.trim().into()));
        }
        if self.scale > 0 {
            env.push(("X2_BB_SCALE".into(), self.scale.to_string()));
        }
        if self.vsync {
            env.push(("X2_VSYNC".into(), "1".into()));
        }
        for line in self.env.lines().map(str::trim).filter(|l| !l.is_empty() && !l.starts_with('#')) {
            if let Some((k, v)) = line.split_once('=') {
                env.push((k.trim().into(), v.trim().into()));
            }
        }
        if self.fullscreen {
            args.push("-f".into());
        }
        if self.log_level > 0 {
            args.push(format!("-{}", "v".repeat(self.log_level.min(3) as usize)));
        }
        (env, args)
    }
}

pub fn path(data: &Path) -> PathBuf {
    data.join("config.json")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn settings_become_runtime_switches() {
        let s = GameSettings { fps: true, pad: " l1=1 ".into(), scale: 2, fullscreen: true, log_level: 2, env: "# note\nX2_TOUCH=0\nbad line\n".into(), ..Default::default() };
        let (env, args) = s.launch();
        assert_eq!(env, vec![("X2_FPS".into(), "1".into()), ("X2_PAD".into(), "l1=1".into()), ("X2_BB_SCALE".into(), "2".into()), ("X2_TOUCH".into(), "0".into())]);
        assert_eq!(args, vec!["-f".to_string(), "-vv".to_string()]);
        assert!(GameSettings::default().launch().0.is_empty());
    }

    #[test]
    fn config_round_trips_and_tolerates_missing_fields() {
        let dir = std::env::temp_dir().join(format!("x2-config-{}", std::process::id()));
        let p = path(&dir);
        assert!(Config::load(&p).games.is_empty());
        let mut c = Config::default();
        c.games.push(GameEntry { id: "kof12".into(), path: "/games/kof12".into(), settings: Default::default() });
        c.save(&p).unwrap();
        assert_eq!(Config::load(&p).game("kof12").unwrap().path, "/games/kof12");
        fs::write(&p, r#"{"games":[{"id":"bbcs","path":"x"}]}"#).unwrap();
        assert!(!Config::load(&p).game("bbcs").unwrap().settings.fps);
        fs::remove_dir_all(&dir).unwrap();
    }
}
