//! Android devices connected over USB or wireless debugging (adb), and sending an export to one:
//! the app is installed; the RetroArch core is copied to RetroArch's downloads folder, from where
//! RetroArch installs it (Load Core → Install or Restore a Core). adb is the launcher's download
//! (Build tools), or one found on the system or in the Android SDK.
use crate::{kit, toolchain};
use serde::Serialize;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

type Result<T> = std::result::Result<T, String>;

/// Where the core goes on the device: RetroArch installs cores from here.
const CORE_DIR: &str = "/sdcard/RetroArch/downloads";

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

/// Sends an export's file to the device: installs the app, or copies the core for RetroArch.
/// Returns where it went, for the message.
pub fn send(adb: &Path, serial: &str, target: &str, files: &[String]) -> Result<&'static str> {
    let find = |ext: &str| files.iter().find(|f| f.ends_with(ext)).ok_or_else(|| format!("The export has no {ext} file."));
    match target {
        "android-app" => {
            let apk = find(".apk")?;
            check(run(adb, &["-s", serial, "install", "-r", apk])?, "Installing the app")?;
            Ok("app")
        }
        "android-retroarch" => {
            let so = find(".so")?;
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
