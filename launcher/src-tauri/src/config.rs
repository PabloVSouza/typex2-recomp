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
    /// On-screen touch pad: 0 automatic (touch screens without a pad), 1 always, 2 never (X2_TOUCH).
    pub touch: u8,
    /// The touch pad's buttons (X2_TOUCH_BUTTONS): "" the game's layout (punches over kicks), "neogeo"
    /// A B C D in a row, or a custom list "1=a,2=c,5=b,..." (slots 1-4 the top row, 5-8 the bottom).
    pub touch_buttons: String,
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
    /// No automatic check for new releases (on by default).
    pub no_update_check: bool,
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
        match self.touch {
            1 => env.push(("X2_TOUCH".into(), "1".into())),
            2 => env.push(("X2_TOUCH".into(), "0".into())),
            _ => {}
        }
        if !self.touch_buttons.trim().is_empty() {
            env.push(("X2_TOUCH_BUTTONS".into(), self.touch_buttons.trim().into()));
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

impl GameSettings {
    /// The game folder's `x2.env` for these settings (Android: the app and ES-DE launches read it).
    /// The log level, an argument on the desktop, is X2_LOG there.
    pub fn env_file(&self) -> String {
        let mut env = self.launch().0;
        if self.log_level > 0 {
            env.insert(0, ("X2_LOG".into(), self.log_level.min(3).to_string()));
        }
        env.iter().map(|(k, v)| format!("{k}={v}\n")).collect()
    }

    /// Settings from an existing `x2.env`: the switches the launcher shows become its settings,
    /// every other line stays in the extra environment.
    pub fn from_env_file(text: &str) -> GameSettings {
        let mut s = GameSettings::default();
        let mut extra = Vec::new();
        for line in text.lines().map(str::trim).filter(|l| !l.is_empty()) {
            let Some((k, v)) = line.split_once('=').map(|(k, v)| (k.trim(), v.trim())) else {
                extra.push(line);
                continue;
            };
            match k {
                "X2_FPS" => s.fps = v != "0",
                "X2_PAD" => s.pad = v.into(),
                "X2_BB_SCALE" => s.scale = v.parse().unwrap_or(0),
                "X2_VSYNC" => s.vsync = v != "0",
                "X2_TOUCH" => s.touch = if v == "0" { 2 } else { 1 },
                "X2_TOUCH_BUTTONS" => s.touch_buttons = v.into(),
                "X2_LOG" => s.log_level = v.parse::<u8>().unwrap_or(0).min(3),
                _ => extra.push(line),
            }
        }
        s.env = extra.join("\n");
        s
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
    fn env_files_round_trip_through_settings() {
        let text = "X2_LOG=2\nX2_FPS=1\nX2_PAD=l1=a,west=c\n# mine\nX2_TOUCH=0\nX2_TOUCH_BUTTONS=neogeo\nX2_PACELOG=1\n";
        let s = GameSettings::from_env_file(text);
        assert!(s.fps);
        assert_eq!(s.pad, "l1=a,west=c");
        assert_eq!(s.touch, 2);
        assert_eq!(s.touch_buttons, "neogeo");
        assert_eq!(s.env, "# mine\nX2_PACELOG=1");
        assert_eq!(s.log_level, 2);
        assert_eq!(s.env_file(), "X2_LOG=2\nX2_FPS=1\nX2_PAD=l1=a,west=c\nX2_TOUCH=0\nX2_TOUCH_BUTTONS=neogeo\nX2_PACELOG=1\n");
        assert_eq!(GameSettings::default().env_file(), "");
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
