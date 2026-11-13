//! What building needs, and where it is.
//!
//! The kit is this project's own code, which the launcher carries:
//! * `x2recomp`, which turns a game's executable into C (games/<id>/gen);
//! * `x2-player`, the runtime as an executable that loads a game library (desktop play);
//! * the source tree (CMake files, runtime, headers), which export targets build from.
//!
//! In a repository checkout these are the checkout and its `build/` folder; in a release
//! they are the app's `kit/` resource folder (`bin/`, `source/`). Compilers, CMake, Ninja
//! and the Android NDK are the user's, found on the usual paths.
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::OnceLock;

pub const EXE: &str = std::env::consts::EXE_SUFFIX;

#[derive(Clone, Serialize)]
pub struct Kit {
    /// The source tree: CMakeLists.txt, runtime/, tools/.
    pub source: PathBuf,
    pub x2recomp: Option<PathBuf>,
    pub player: Option<PathBuf>,
    /// "checkout" or "bundled".
    pub kind: &'static str,
}

impl Kit {
    pub fn include(&self) -> PathBuf {
        self.source.join("runtime/include")
    }

    /// Identity of what generated code depends on: the recompiler and the runtime's
    /// recomp.h (inline helpers compiled into every function). A change rebuilds.
    pub fn stamp(&self) -> String {
        static STAMP: OnceLock<String> = OnceLock::new();
        STAMP
            .get_or_init(|| {
                let mut h = Sha256::new();
                for p in [self.x2recomp.clone(), Some(self.include().join("x2/recomp.h"))].into_iter().flatten() {
                    if let Ok(bytes) = std::fs::read(&p) {
                        h.update(&bytes);
                    }
                }
                format!("{:x}", h.finalize())[..16].to_string()
            })
            .clone()
    }
}

fn is_source_tree(dir: &Path) -> bool {
    dir.join("runtime/CMakeLists.txt").is_file() && dir.join("runtime/include/x2/recomp.h").is_file()
}

fn existing(path: PathBuf) -> Option<PathBuf> {
    path.is_file().then_some(path)
}

/// The kit: `X2_REPO`, the app's bundled `kit/`, or a checkout above the executable or the
/// working directory (`npm run tauri dev` runs from launcher/).
pub fn find(resources: Option<PathBuf>) -> Option<Kit> {
    let checkout = |root: PathBuf| {
        let build = root.join("build");
        Kit {
            x2recomp: existing(build.join("recompiler").join(format!("x2recomp{EXE}"))),
            player: existing(build.join(format!("x2-player{EXE}"))),
            source: root,
            kind: "checkout",
        }
    };
    if let Some(root) = std::env::var_os("X2_REPO").map(PathBuf::from).filter(|p| is_source_tree(p)) {
        return Some(checkout(root));
    }
    if let Some(kit) = resources.map(|r| r.join("kit")).filter(|k| is_source_tree(&k.join("source"))) {
        return Some(Kit {
            x2recomp: existing(kit.join("bin").join(format!("x2recomp{EXE}"))),
            player: existing(kit.join("bin").join(format!("x2-player{EXE}"))),
            source: kit.join("source"),
            kind: "bundled",
        });
    }
    let starts = [std::env::current_exe().ok(), std::env::current_dir().ok()];
    for start in starts.into_iter().flatten() {
        if let Some(root) = start.ancestors().find(|d| is_source_tree(d)) {
            return Some(checkout(root.to_path_buf()));
        }
    }
    None
}

/// Folders searched besides PATH: apps started from the Finder or a desktop menu do not get
/// the shell's PATH (Homebrew, MacPorts, /usr/local).
fn extra_dirs() -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = ["/opt/homebrew/bin", "/usr/local/bin", "/opt/local/bin", "/usr/bin"].iter().map(PathBuf::from).collect();
    if cfg!(windows) {
        for base in [std::env::var_os("ProgramFiles"), std::env::var_os("ProgramFiles(x86)")].into_iter().flatten() {
            let base = PathBuf::from(base);
            v.push(base.join("CMake/bin"));
            v.push(base.join("LLVM/bin"));
        }
    }
    v
}

pub fn which(name: &str) -> Option<PathBuf> {
    let file = format!("{name}{EXE}");
    let path = std::env::var_os("PATH").unwrap_or_default();
    std::env::split_paths(&path).chain(extra_dirs()).map(|d| d.join(&file)).find(|p| p.is_file())
}

/// The C compiler for game libraries: clang (on macOS from the Command Line Tools: /usr/bin/clang
/// is only a stub that offers to install them, so `xcode-select -p` must name a folder).
pub fn cc() -> Option<PathBuf> {
    if cfg!(target_os = "macos") {
        let ok = Command::new("xcode-select").arg("-p").output().map(|o| o.status.success()).unwrap_or(false);
        return ok.then(|| PathBuf::from("/usr/bin/clang"));
    }
    which("clang").or_else(|| which("cc"))
}

/// The newest Android NDK: `ANDROID_NDK_HOME`, a configured folder, or the SDK's ndk/.
pub fn ndk(configured: Option<&str>) -> Option<PathBuf> {
    let valid = |p: &Path| p.join("build/cmake/android.toolchain.cmake").is_file();
    if let Some(p) = configured.map(PathBuf::from).filter(|p| valid(p)) {
        return Some(p);
    }
    if let Some(p) = std::env::var_os("ANDROID_NDK_HOME").map(PathBuf::from).filter(|p| valid(p)) {
        return Some(p);
    }
    let home = std::env::var_os("HOME").or_else(|| std::env::var_os("USERPROFILE")).map(PathBuf::from)?;
    let mut sdks = vec![home.join("Library/Android/sdk"), home.join("Android/Sdk")];
    if let Some(local) = std::env::var_os("LOCALAPPDATA") {
        sdks.push(PathBuf::from(local).join("Android/Sdk"));
    }
    if let Some(sdk) = std::env::var_os("ANDROID_HOME") {
        sdks.insert(0, PathBuf::from(sdk));
    }
    let mut found: Vec<PathBuf> = sdks
        .iter()
        .filter_map(|s| std::fs::read_dir(s.join("ndk")).ok())
        .flat_map(|r| r.flatten().map(|e| e.path()))
        .filter(|p| valid(p))
        .collect();
    // version folders: compare numerically (29.0.x < 30.0.x)
    found.sort_by_key(|p| {
        p.file_name().and_then(|n| n.to_str()).map(|n| n.split('.').map(|x| x.parse::<u64>().unwrap_or(0)).collect::<Vec<_>>()).unwrap_or_default()
    });
    found.pop()
}

#[derive(Serialize)]
pub struct Tool {
    pub id: &'static str,
    pub name: &'static str,
    pub path: Option<String>,
    /// "kit" (this app), "downloaded" (the launcher's tools folder), "system" or null.
    pub source: Option<&'static str>,
    /// What needs it.
    pub for_what: &'static str,
    /// A package in toolchain::packages() that provides it.
    pub download: Option<&'static str>,
    /// How to get it otherwise.
    pub hint: &'static str,
}

pub fn tools(kit: Option<&Kit>, data: &Path, ndk_setting: Option<&str>) -> Vec<Tool> {
    use crate::toolchain as tc;
    let s = |p: &PathBuf| Some(p.display().to_string());
    let pick = |own: Option<PathBuf>, sys: Option<PathBuf>| match (own, sys) {
        (Some(p), _) => (s(&p), Some("downloaded")),
        (None, Some(p)) => (s(&p), Some("system")),
        _ => (None, None),
    };
    let offered = |id: &'static str| tc::packages().iter().any(|p| p.id == id).then_some(id);
    let kit_tool = |p: Option<PathBuf>| match p {
        Some(p) => (s(&p), Some("kit")),
        None => (None, None),
    };
    let cc_hint = if cfg!(target_os = "macos") {
        "Or install Apple's Command Line Tools: xcode-select --install"
    } else if cfg!(windows) {
        "Install LLVM (clang) from llvm.org"
    } else {
        "Install clang from your distribution's packages"
    };
    let mut v = Vec::new();
    let (path, source) = kit_tool(kit.and_then(|k| k.x2recomp.clone()));
    v.push(Tool { id: "x2recomp", name: "x2recomp", path, source, for_what: "Every build", download: None, hint: "Part of this app. In a checkout: cmake --build build --target x2recomp" });
    let (path, source) = kit_tool(kit.and_then(|k| k.player.clone()));
    v.push(Tool { id: "player", name: "x2-player", path, source, for_what: "Playing on this computer", download: None, hint: "Part of this app. In a checkout: cmake --build build --target x2-player" });
    let (path, source) = pick(tc::llvm(data).map(|l| l.clang), cc());
    v.push(Tool { id: "cc", name: "C compiler (clang)", path, source, for_what: "Playing on this computer", download: offered("llvm"), hint: cc_hint });
    let (path, source) = pick(tc::cmake_bin(data), which("cmake"));
    v.push(Tool { id: "cmake", name: "CMake", path, source, for_what: "Exports", download: offered("cmake"), hint: "cmake.org" });
    let (path, source) = pick(tc::ninja_bin(data), which("ninja"));
    v.push(Tool { id: "ninja", name: "Ninja", path, source, for_what: "Exports", download: offered("ninja"), hint: "ninja-build.org" });
    let (path, source) = pick(tc::ndk_dir(data), ndk(ndk_setting));
    v.push(Tool { id: "ndk", name: "Android NDK", path, source, for_what: "Android exports", download: offered("ndk"), hint: "Android Studio → SDK Manager → NDK" });
    v
}
