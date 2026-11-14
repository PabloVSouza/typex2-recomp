//! Build tools the launcher downloads into its own data folder (`tools/`), so nothing is
//! installed on the computer: pinned versions, checked against their published checksums,
//! removed in one go. Builds prefer these over tools found on the system.
use crate::recomp::Ctx;
use serde::Serialize;
use serde_json::json;
use sha1::Sha1;
use sha2::{Digest, Sha256};
use std::fs;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::process::Command;

type Result<T> = std::result::Result<T, String>;

#[derive(Clone, Copy)]
pub enum Sum {
    Sha256(&'static str),
    Sha1(&'static str),
}

#[derive(Clone, Copy, Serialize)]
pub struct Package {
    pub id: &'static str,
    pub name: &'static str,
    pub version: &'static str,
    pub size: u64,
    pub for_what: &'static str,
    #[serde(skip)]
    url: &'static str,
    #[serde(skip)]
    sum: Sum,
}

/// clang, ld64.lld and clang's headers, trimmed from LLVM's official macOS release by
/// bloodborne-recomp's tools/recomp/toolchain.sh (Apache-2.0 with LLVM exception).
#[cfg(all(target_os = "macos", target_arch = "aarch64"))]
const LLVM: Option<Package> = Some(Package {
    id: "llvm",
    name: "LLVM clang",
    version: "23.1.4",
    size: 0,
    for_what: "Compiling games to play on this computer",
    url: "https://github.com/bloodborne-native/bloodborne-recomp/releases/download/toolchain-llvm-23.1.4/llvm-23.1.4-macos-arm64.tar.xz",
    sum: Sum::Sha256("7e148b8692fa33ded7e6cba48dc879b8a224a935d05232b0999331d85ed8417c"),
});
/// llvm-mingw (mstorsjo/llvm-mingw, Apache-2.0 with LLVM exception and the MinGW-w64 licences):
/// clang, lld and a MinGW-w64 UCRT sysroot, the C runtime x2-player is built against (MSYS2 CLANG64).
#[cfg(all(windows, target_arch = "x86_64"))]
const LLVM: Option<Package> = Some(Package {
    id: "llvm",
    name: "LLVM clang (llvm-mingw)",
    version: "20261020",
    size: 190612575,
    for_what: "Compiling games to play on this computer",
    url: "https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/llvm-mingw-20261020-ucrt-x86_64.zip",
    sum: Sum::Sha256("fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd"),
});
#[cfg(all(windows, target_arch = "aarch64"))]
const LLVM: Option<Package> = Some(Package {
    id: "llvm",
    name: "LLVM clang (llvm-mingw)",
    version: "20261020",
    size: 185737889,
    for_what: "Compiling games to play on this computer",
    url: "https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/llvm-mingw-20261020-ucrt-aarch64.zip",
    sum: Sum::Sha256("8f24fedd0b75c674c6c83dec2d76a7ac31a690120212843a17e465ef49854f69"),
});
#[cfg(not(any(all(target_os = "macos", target_arch = "aarch64"), windows)))]
const LLVM: Option<Package> = None;

macro_rules! pkg {
    ($id:expr, $name:expr, $ver:expr, $size:expr, $for:expr, $url:expr, $sum:expr) => {
        Some(Package { id: $id, name: $name, version: $ver, size: $size, for_what: $for, url: $url, sum: $sum })
    };
}

const CMAKE_FOR: &str = "Exports (RetroArch cores, Android app)";
const NDK_FOR: &str = "Android exports";

fn cmake() -> Option<Package> {
    let (url, size, sum) = if cfg!(target_os = "macos") {
        ("https://github.com/Kitware/CMake/releases/download/v4.5.0/cmake-4.5.0-macos-universal.tar.gz", 92795915, "a645d69fd21e521759cc0c64e7c60462d43faf0c7214ac05f75c80f7c5037dde")
    } else if cfg!(all(target_os = "linux", target_arch = "x86_64")) {
        ("https://github.com/Kitware/CMake/releases/download/v4.5.0/cmake-4.5.0-linux-x86_64.tar.gz", 67002501, "81f0d5b0960fb6adc8197030ffc827fba937640d51e73e39e0e9e40d88c66f6f")
    } else if cfg!(all(target_os = "linux", target_arch = "aarch64")) {
        ("https://github.com/Kitware/CMake/releases/download/v4.5.0/cmake-4.5.0-linux-aarch64.tar.gz", 53719606, "0c8447e7bba9701467b5ac6155d157aa9e309e71af4981f78bd4b9176107f553")
    } else if cfg!(all(windows, target_arch = "x86_64")) {
        ("https://github.com/Kitware/CMake/releases/download/v4.5.0/cmake-4.5.0-windows-x86_64.zip", 56341654, "301b1728317ccd84e984b02e366a8293735bc5ac618757d3d8f58b578f062411")
    } else if cfg!(all(windows, target_arch = "aarch64")) {
        ("https://github.com/Kitware/CMake/releases/download/v4.5.0/cmake-4.5.0-windows-arm64.zip", 54402580, "7dba95db83ec1e8fa3f221c06454ebc692c8f06d93bf5fdf840113fa28018690")
    } else {
        return None;
    };
    pkg!("cmake", "CMake", "4.5.0", size, CMAKE_FOR, url, Sum::Sha256(sum))
}

fn ninja() -> Option<Package> {
    let (url, size, sum) = if cfg!(target_os = "macos") {
        ("https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-mac.zip", 314051, "c99048673aa765960a99cf10c6ddb9f1fad506099ff0a0e137ad8960a88f321b")
    } else if cfg!(all(target_os = "linux", target_arch = "x86_64")) {
        ("https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-linux.zip", 134040, "5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6")
    } else if cfg!(all(target_os = "linux", target_arch = "aarch64")) {
        ("https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-linux-aarch64.zip", 126256, "fd2cacc8050a7f12a16a2e48f9e06fca5c14fc4c2bee2babb67b58be17a607fc")
    } else if cfg!(all(windows, target_arch = "x86_64")) {
        ("https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip", 291570, "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65")
    } else if cfg!(all(windows, target_arch = "aarch64")) {
        ("https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-winarm64.zip", 270354, "e52f0bdef9dfb1003229dbd6508a508c4073fd017247002adc66e5e806cb0391")
    } else {
        return None;
    };
    pkg!("ninja", "Ninja", "1.13.2", size, CMAKE_FOR, url, Sum::Sha256(sum))
}

/// NDK r30 (30.0.16248370); SHA-1 from Google's SDK repository manifest (repository2-3.xml).
fn ndk() -> Option<Package> {
    let (url, size, sum) = if cfg!(target_os = "macos") {
        ("https://dl.google.com/android/repository/android-ndk-r30-darwin.zip", 974984488, "c060be96767eefbb8e0a27796d6f43115fc1a0c4")
    } else if cfg!(all(target_os = "linux", target_arch = "x86_64")) {
        ("https://dl.google.com/android/repository/android-ndk-r30-linux.zip", 738633529, "5107f898313790e449e87eee2183d9a20602dee9")
    } else if cfg!(all(windows, target_arch = "x86_64")) {
        ("https://dl.google.com/android/repository/android-ndk-r30-windows.zip", 727980788, "9bf167a1985fa7d4a036186b78f702eab9179408")
    } else {
        return None;
    };
    pkg!("ndk", "Android NDK", "r30", size, NDK_FOR, url, Sum::Sha1(sum))
}

pub fn packages() -> Vec<Package> {
    [LLVM, cmake(), ninja(), ndk()].into_iter().flatten().collect()
}

pub fn root(data: &Path) -> PathBuf {
    data.join("tools")
}

fn install_dir(data: &Path, p: &Package) -> PathBuf {
    root(data).join(format!("{}-{}", p.id, p.version))
}

/// The package's folder, when completely installed.
pub fn installed(data: &Path, id: &str) -> Option<PathBuf> {
    let p = packages().into_iter().find(|p| p.id == id)?;
    let dir = install_dir(data, &p);
    dir.join(".complete").is_file().then_some(dir)
}

/// A file below `dir` whose path ends with `rel` (archives nest their top folder).
pub fn find_below(dir: &Path, rel: &str, depth: usize) -> Option<PathBuf> {
    let direct = dir.join(rel);
    if direct.exists() {
        return Some(direct);
    }
    if depth == 0 {
        return None;
    }
    let mut entries: Vec<PathBuf> = fs::read_dir(dir).ok()?.flatten().map(|e| e.path()).filter(|p| p.is_dir()).collect();
    entries.sort();
    entries.into_iter().find_map(|d| find_below(&d, rel, depth - 1))
}

pub struct Llvm {
    pub clang: PathBuf,
    /// clang's own headers (lib/clang/<major>/include).
    pub resource: PathBuf,
}

pub fn llvm(data: &Path) -> Option<Llvm> {
    let dir = installed(data, "llvm")?;
    let clang = find_below(&dir, &format!("bin/clang{}", crate::kit::EXE), 2)?;
    let lib = clang.parent()?.parent()?.join("lib/clang");
    let mut versions: Vec<PathBuf> = fs::read_dir(lib).ok()?.flatten().map(|e| e.path().join("include")).filter(|p| p.is_dir()).collect();
    versions.sort();
    Some(Llvm { clang, resource: versions.pop()? })
}

pub fn cmake_bin(data: &Path) -> Option<PathBuf> {
    let dir = installed(data, "cmake")?;
    let exe = format!("bin/cmake{}", crate::kit::EXE);
    find_below(&dir, &format!("CMake.app/Contents/{exe}"), 2).or_else(|| find_below(&dir, &exe, 2))
}

pub fn ninja_bin(data: &Path) -> Option<PathBuf> {
    find_below(&installed(data, "ninja")?, &format!("ninja{}", crate::kit::EXE), 1)
}

pub fn ndk_dir(data: &Path) -> Option<PathBuf> {
    let f = find_below(&installed(data, "ndk")?, "build/cmake/android.toolchain.cmake", 3)?;
    Some(f.parent()?.parent()?.parent()?.to_path_buf())
}

enum Hasher {
    S256(Sha256),
    S1(Sha1),
}

/// Downloads, checks and unpacks the packages not installed yet.
pub fn install(ctx: &Ctx, data: &Path, ids: &[String]) -> Result<serde_json::Value> {
    let client = reqwest::blocking::Client::builder()
        .user_agent("typex2-launcher")
        .connect_timeout(std::time::Duration::from_secs(20))
        .timeout(None)
        .build()
        .map_err(|e| e.to_string())?;
    let mut done = Vec::new();
    for p in packages().into_iter().filter(|p| ids.iter().any(|i| i == p.id)) {
        if installed(data, p.id).is_some() {
            continue;
        }
        ctx.check()?;
        let dir = install_dir(data, &p);
        let staging = dir.with_extension("partial");
        let _ = fs::remove_dir_all(&staging);
        fs::create_dir_all(&staging).map_err(|e| e.to_string())?;
        let name = p.url.rsplit('/').next().unwrap_or("download");
        let archive = root(data).join(name);
        ctx.progress(json!({"step": "download", "tool": p.id, "done": 0, "total": p.size}));
        let mut resp = client.get(p.url).send().map_err(|e| format!("Downloading {}: {e}", p.name))?;
        if !resp.status().is_success() {
            return Err(format!("Downloading {}: {}", p.name, resp.status()));
        }
        let total = resp.content_length().unwrap_or(p.size);
        let mut file = fs::File::create(&archive).map_err(|e| e.to_string())?;
        let mut hasher = match p.sum {
            Sum::Sha256(_) => Hasher::S256(Sha256::new()),
            Sum::Sha1(_) => Hasher::S1(Sha1::new()),
        };
        let (mut got, mut shown) = (0u64, 0u64);
        let mut buf = vec![0u8; 1 << 16];
        loop {
            if ctx.cancelled() {
                drop(file);
                let _ = fs::remove_file(&archive);
                return Err("cancelled".into());
            }
            let n = resp.read(&mut buf).map_err(|e| format!("Downloading {}: {e}", p.name))?;
            if n == 0 {
                break;
            }
            match &mut hasher {
                Hasher::S256(h) => h.update(&buf[..n]),
                Hasher::S1(h) => h.update(&buf[..n]),
            }
            file.write_all(&buf[..n]).map_err(|e| e.to_string())?;
            got += n as u64;
            if got - shown >= 1 << 20 {
                shown = got;
                ctx.progress(json!({"step": "download", "tool": p.id, "done": got, "total": total}));
            }
        }
        drop(file);
        let (sum, want) = match (hasher, p.sum) {
            (Hasher::S256(h), Sum::Sha256(w)) => (format!("{:x}", h.finalize()), w),
            (Hasher::S1(h), Sum::Sha1(w)) => (format!("{:x}", h.finalize()), w),
            _ => unreachable!(),
        };
        if sum != want {
            let _ = fs::remove_file(&archive);
            return Err(format!("The downloaded {} is not the expected file (its checksum differs).", p.name));
        }
        ctx.progress(json!({"step": "unpack", "tool": p.id}));
        let unpack = unpack(ctx, &archive, &staging, p.name);
        let _ = fs::remove_file(&archive);
        unpack?;
        let _ = fs::remove_dir_all(&dir);
        fs::rename(&staging, &dir).map_err(|e| e.to_string())?;
        fs::write(dir.join(".complete"), p.url).map_err(|e| e.to_string())?;
        done.push(p.id);
    }
    Ok(json!({"installed": done}))
}

/// tar (bsdtar on macOS and Windows 10+ also reads zip); unzip for zips on Linux.
fn unpack(ctx: &Ctx, archive: &Path, to: &Path, name: &str) -> Result<()> {
    let zip = archive.extension().is_some_and(|e| e == "zip");
    let mut cmd = if zip && cfg!(target_os = "linux") {
        let mut c = Command::new("unzip");
        c.arg("-q").arg(archive).arg("-d").arg(to);
        c
    } else {
        let mut c = Command::new("tar");
        c.arg("-xf").arg(archive).arg("-C").arg(to);
        c
    };
    ctx.run(&mut cmd, &format!("Unpacking {name}"), &mut |_| {})
}

pub fn remove_all(data: &Path) -> Result<()> {
    let r = root(data);
    if r.exists() {
        fs::remove_dir_all(&r).map_err(|e| e.to_string())?;
    }
    Ok(())
}

/// Bytes the tools take.
pub fn disk_use(data: &Path) -> u64 {
    fn size(p: &Path) -> u64 {
        match fs::symlink_metadata(p) {
            Ok(m) if m.is_dir() => fs::read_dir(p).map(|r| r.flatten().map(|e| size(&e.path())).sum()).unwrap_or(0),
            Ok(m) => m.len(),
            Err(_) => 0,
        }
    }
    size(&root(data))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nested_archive_folders_are_searched() {
        let d = std::env::temp_dir().join(format!("x2-tools-{}", std::process::id()));
        fs::create_dir_all(d.join("cmake-4.5.0-macos-universal/CMake.app/Contents/bin")).unwrap();
        fs::write(d.join("cmake-4.5.0-macos-universal/CMake.app/Contents/bin/cmake"), "").unwrap();
        assert!(find_below(&d, "CMake.app/Contents/bin/cmake", 2).is_some());
        assert!(find_below(&d, "CMake.app/Contents/bin/cmake", 0).is_none());
        fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    fn every_package_has_a_checksum_and_a_url() {
        for p in packages() {
            assert!(p.url.starts_with("https://"));
            match p.sum {
                Sum::Sha256(s) => assert_eq!(s.len(), 64),
                Sum::Sha1(s) => assert_eq!(s.len(), 40),
            }
        }
    }
}
