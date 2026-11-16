//! Android devices connected over USB or wireless debugging (adb), and sending an export to one:
//! game packs go to the Type X2 app's folder; the RetroArch core to RetroArch's downloads folder, from where
//! RetroArch installs it (Load Core → Install or Restore a Core). adb is the launcher's download
//! (Build tools), or one found on the system or in the Android SDK.
use crate::{kit, toolchain};
use serde::Serialize;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

type Result<T> = std::result::Result<T, String>;

/// Where the core goes on the device: RetroArch installs cores from here.
const CORE_DIR: &str = "/sdcard/RetroArch/downloads";

/// The Type X2 app (launcher/src-tauri/tauri.android.conf.json).
const APP_ID: &str = "com.typex2.recomp";

#[derive(Serialize, Debug, PartialEq)]
pub struct Device {
    pub serial: String,
    /// The model as adb reports it ("SM X130"), or the serial.
    pub model: String,
    /// "device" when usable; "unauthorized" until the device accepts this computer, "offline".
    pub state: String,
}

fn sdk_adb() -> Option<PathBuf> {
    let home = std::env::var_os("HOME").or_else(|| std::env::var_os("USERPROFILE")).map(PathBuf::from);
    let mut sdks: Vec<PathBuf> = Vec::new();
    for v in ["ANDROID_HOME", "ANDROID_SDK_ROOT"] {
        if let Some(p) = std::env::var_os(v) {
            sdks.push(PathBuf::from(p));
        }
    }
    if let Some(h) = &home {
        sdks.push(h.join("Library/Android/sdk"));
        sdks.push(h.join("Android/Sdk"));
    }
    if let Some(l) = std::env::var_os("LOCALAPPDATA") {
        sdks.push(PathBuf::from(l).join("Android/Sdk"));
    }
    sdks.into_iter().map(|s| s.join(format!("platform-tools/adb{}", kit::EXE))).find(|p| p.is_file())
}

/// adb found on the system (PATH) or in the Android SDK.
pub fn system_adb() -> Option<PathBuf> {
    kit::which("adb").or_else(sdk_adb)
}

/// adb: the launcher's own download first, then the system's, then the Android SDK's.
pub fn adb(data: &Path) -> Option<PathBuf> {
    toolchain::adb_bin(data).or_else(system_adb)
}

fn run(adb: &Path, args: &[&str]) -> Result<Output> {
    let mut cmd = Command::new(adb);
    cmd.args(args);
    #[cfg(windows)]
    std::os::windows::process::CommandExt::creation_flags(&mut cmd, 0x0800_0000); // no console window
    cmd.output().map_err(|e| format!("adb: {e}"))
}

/// `adb devices -l`: one line per device after the header.
fn parse_devices(out: &str) -> Vec<Device> {
    out.lines()
        .skip_while(|l| !l.starts_with("List of devices"))
        .skip(1)
        .filter_map(|l| {
            let mut words = l.split_whitespace();
            let serial = words.next()?.to_string();
            let state = words.next()?.to_string();
            let model = words
                .find_map(|w| w.strip_prefix("model:"))
                .map(|m| m.replace('_', " "))
                .unwrap_or_else(|| serial.clone());
            Some(Device { serial, model, state })
        })
        .collect()
}

pub fn devices(adb: &Path) -> Result<Vec<Device>> {
    let out = run(adb, &["devices", "-l"])?;
    if !out.status.success() {
        return Err(String::from_utf8_lossy(&out.stderr).trim().to_string());
    }
    Ok(parse_devices(&String::from_utf8_lossy(&out.stdout)))
}

fn check(out: Output, what: &str) -> Result<()> {
    let text = format!("{}{}", String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr));
    if out.status.success() && !text.contains("Failure") && !text.contains("error:") {
        return Ok(());
    }
    let last = text.lines().rev().find(|l| !l.trim().is_empty()).unwrap_or("").trim();
    Err(format!("{what}: {last}"))
}

/// The Type X2 app on the device: the release app, else a development build (`.dev`).
fn app_package(adb: &Path, serial: &str) -> Result<String> {
    let out = run(adb, &["-s", serial, "shell", "pm", "list", "packages", APP_ID])?;
    let listed: Vec<String> = String::from_utf8_lossy(&out.stdout).lines().filter_map(|l| l.trim().strip_prefix("package:").map(str::to_string)).collect();
    [APP_ID.to_string(), format!("{APP_ID}.dev")]
        .into_iter()
        .find(|p| listed.contains(p))
        .ok_or_else(|| "The Type X2 app is not on the device: install it from the releases page first.".into())
}

/// Sends an export's files to the device: the game packs into the app's folder (it installs them
/// when it next opens), or the core for RetroArch. Returns what was sent, for the message.
pub fn send(adb: &Path, serial: &str, target: &str, files: &[String]) -> Result<&'static str> {
    match target {
        "android-packs" => {
            let packs: Vec<&String> = files.iter().filter(|f| f.ends_with(".so")).collect();
            if packs.is_empty() {
                return Err("The export has no game packs.".into());
            }
            let dir = format!("/sdcard/Android/data/{}/files/packs", app_package(adb, serial)?);
            check(run(adb, &["-s", serial, "shell", "mkdir", "-p", &dir])?, "Preparing the app's packs folder")?;
            for p in packs {
                check(run(adb, &["-s", serial, "push", p, &format!("{dir}/")])?, "Copying a game pack")?;
            }
            Ok("packs")
        }
        "android-retroarch" => {
            let so = files.iter().find(|f| f.ends_with(".so")).ok_or("The export has no .so file.")?;
            check(run(adb, &["-s", serial, "shell", "mkdir", "-p", CORE_DIR])?, "Preparing RetroArch's downloads folder")?;
            check(run(adb, &["-s", serial, "push", so, &format!("{CORE_DIR}/")])?, "Copying the core")?;
            Ok("core")
        }
        _ => Err("This export is not for Android.".into()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn devices_are_parsed_with_their_model_and_state() {
        let out = "* daemon started successfully\nList of devices attached\n\
                   R8YL10WF7EZ            device usb:1-1 product:gta11wifi model:SM_X130 device:gta11wifi transport_id:3\n\
                   192.168.1.238:38975    unauthorized transport_id:4\n\n";
        assert_eq!(
            parse_devices(out),
            vec![
                Device { serial: "R8YL10WF7EZ".into(), model: "SM X130".into(), state: "device".into() },
                Device { serial: "192.168.1.238:38975".into(), model: "192.168.1.238:38975".into(), state: "unauthorized".into() },
            ]
        );
        assert!(parse_devices("List of devices attached\n\n").is_empty());
    }
}
