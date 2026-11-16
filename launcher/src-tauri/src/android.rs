//! The launcher as the Android app. Games live in ES-DE's ROMs folder (`ROMs/arcade/<Game>.x2`),
//! found on every refresh; their code arrives as game packs built by the launcher on a computer
//! (the Android pack export) and sent to `TypeX2/packs` on shared storage.
//! The app moves them into its private storage, the only place besides the APK Android loads code
//! from, as `x2-<size>-<entry>.so`: the name `libmain` (runtime/src/launcher.c) looks for. Play
//! writes the game's settings to its `x2.env` (ES-DE launches read it too) and starts SDL's
//! activity, which runs the game in its own process.
use crate::config::{Config, GameEntry, GameSettings};
use crate::games;
use jni::objects::{JObject, JString, JValue};
use jni::{JNIEnv, JavaVM};
use serde_json::{json, Value};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

type Result<T> = std::result::Result<T, String>;

const FLAG_ACTIVITY_NEW_TASK: i32 = 0x1000_0000;
const FLAG_ACTIVITY_CLEAR_TASK: i32 = 0x0000_8000;

/// Runs `f` with this thread's JNI environment and the app's activity.
fn jni<T>(f: impl FnOnce(&mut JNIEnv, &JObject) -> jni::errors::Result<T>) -> Result<T> {
    let ctx = ndk_context::android_context();
    let vm = unsafe { JavaVM::from_raw(ctx.vm().cast()) }.map_err(|e| e.to_string())?;
    let mut env = vm.attach_current_thread().map_err(|e| e.to_string())?;
    let activity = unsafe { JObject::from_raw(ctx.context().cast()) };
    let r = f(&mut env, &activity);
    if env.exception_check().unwrap_or(false) {
        let _ = env.exception_describe();
        let _ = env.exception_clear();
    }
    r.map_err(|e| format!("Android: {e}"))
}

fn string(env: &mut JNIEnv, o: JObject) -> jni::errors::Result<String> {
    Ok(env.get_string(&JString::from(o))?.into())
}

fn file_path(env: &mut JNIEnv, file: JObject) -> jni::errors::Result<Option<PathBuf>> {
    if file.is_null() {
        return Ok(None);
    }
    let p = env.call_method(&file, "getAbsolutePath", "()Ljava/lang/String;", &[])?.l()?;
    Ok(Some(PathBuf::from(string(env, p)?)))
}

fn package(env: &mut JNIEnv, activity: &JObject) -> jni::errors::Result<String> {
    let p = env.call_method(activity, "getPackageName", "()Ljava/lang/String;", &[])?.l()?;
    string(env, p)
}

/// Where installed packs live (the app's private files: `getFilesDir()/packs`).
fn packs_dir() -> Result<PathBuf> {
    let dir = jni(|env, a| {
        let f = env.call_method(a, "getFilesDir", "()Ljava/io/File;", &[])?.l()?;
        file_path(env, f)
    })?;
    Ok(dir.ok_or("The app has no files folder.")?.join("packs"))
}

/// Where packs arrive: `TypeX2/packs` on shared storage, where adb (Send to device) and file
/// managers can write; the app reads it with all-files access. (Not `Android/data/<app>`: file
/// managers can't open that since Android 11, and a folder adb creates there before the app's
/// first start is unreadable to the app.)
pub fn incoming_dir() -> PathBuf {
    storage().join("TypeX2/packs")
}

pub fn all_files_access() -> bool {
    jni(|env, _| env.call_static_method("android/os/Environment", "isExternalStorageManager", "()Z", &[])?.z()).unwrap_or(false)
}

/// Android's settings screen for "All files access" (reading the ROMs folder, writing `x2.env`).
pub fn request_all_files_access() -> Result<()> {
    jni(|env, a| {
        let pkg = package(env, a)?;
        let uri_s = env.new_string(format!("package:{pkg}"))?;
        let uri = env.call_static_method("android/net/Uri", "parse", "(Ljava/lang/String;)Landroid/net/Uri;", &[JValue::Object(&uri_s)])?.l()?;
        let action = env.new_string("android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION")?;
        let intent = env.new_object("android/content/Intent", "(Ljava/lang/String;Landroid/net/Uri;)V", &[JValue::Object(&action), JValue::Object(&uri)])?;
        env.call_method(a, "startActivity", "(Landroid/content/Intent;)V", &[JValue::Object(&intent)])?;
        Ok(())
    })
}

/// Starts the game in SDL's activity (its own process), as ES-DE does.
pub fn play(dir: &str) -> Result<()> {
    jni(|env, a| {
        let pkg = package(env, a)?;
        let intent = env.new_object("android/content/Intent", "()V", &[])?;
        let (p, c) = (env.new_string(pkg)?, env.new_string("org.libsdl.app.SDLActivity")?);
        env.call_method(&intent, "setClassName", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;", &[JValue::Object(&p), JValue::Object(&c)])?;
        let (k, v) = (env.new_string("ROM")?, env.new_string(dir)?);
        env.call_method(&intent, "putExtra", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;", &[JValue::Object(&k), JValue::Object(&v)])?;
        env.call_method(&intent, "addFlags", "(I)Landroid/content/Intent;", &[JValue::Int(FLAG_ACTIVITY_NEW_TASK | FLAG_ACTIVITY_CLEAR_TASK)])?;
        env.call_method(a, "startActivity", "(Landroid/content/Intent;)V", &[JValue::Object(&intent)])?;
        Ok(())
    })
}

fn storage() -> PathBuf {
    PathBuf::from(std::env::var("EXTERNAL_STORAGE").unwrap_or_else(|_| "/storage/emulated/0".into()))
}

/// ES-DE's ROMs folder: its ROMDirectory setting, else `ROMs` on shared storage.
pub fn rom_dir() -> PathBuf {
    let root = storage();
    let set = fs::read_to_string(root.join("ES-DE/settings/es_settings.xml")).ok().and_then(|xml| {
        let at = xml.find("name=\"ROMDirectory\"")?;
        let rest = &xml[at..];
        let v = rest.find("value=\"")? + 7;
        let end = rest[v..].find('"')?;
        Some(rest[v..v + end].replace('~', &root.to_string_lossy())).filter(|s| !s.is_empty())
    });
    set.map(PathBuf::from).unwrap_or_else(|| root.join("ROMs"))
}

/// Moves packs sent to the shared folder into private storage; returns how many arrived.
fn import_packs() -> Result<usize> {
    let (from, to) = (incoming_dir(), packs_dir()?);
    let list = match fs::read_dir(&from) {
        Ok(list) => list,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(0),
        Err(e) => return Err(format!("Reading {}: {e}", from.display())),
    };
    fs::create_dir_all(&to).map_err(|e| e.to_string())?;
    let mut n = 0;
    for f in list.flatten() {
        let name = f.file_name().to_string_lossy().to_string();
        if !(name.starts_with("x2-") && name.ends_with(".so")) {
            continue;
        }
        let tmp = to.join(format!("{name}.part"));
        fs::copy(f.path(), &tmp).and_then(|_| fs::rename(&tmp, to.join(&name))).map_err(|e| format!("Installing {name}: {e}"))?;
        let _ = fs::remove_file(f.path());
        n += 1;
    }
    Ok(n)
}

/// Adds the games found in the ROMs folder to the library (their `x2.env` becomes their
/// settings) and installs packs that arrived. Returns whether the library changed.
pub fn sync(config: &mut Config) -> bool {
    if let Err(e) = import_packs() {
        eprintln!("{e}");
    }
    let mut changed = false;
    let Ok(list) = fs::read_dir(rom_dir().join("arcade")) else { return false };
    let mut dirs: Vec<PathBuf> = list.flatten().map(|e| e.path()).filter(|p| p.is_dir()).collect();
    dirs.sort();
    for dir in dirs {
        let Some(game) = games::detect(&dir) else { continue };
        let path = dir.to_string_lossy().to_string();
        match config.games.iter_mut().find(|g| g.id == game.id) {
            Some(g) if Path::new(&g.path).is_dir() => {}
            Some(g) => {
                g.path = path;
                changed = true;
            }
            None => {
                let settings = fs::read_to_string(dir.join("x2.env")).map(|t| GameSettings::from_env_file(&t)).unwrap_or_default();
                config.games.push(GameEntry { id: game.id, path, settings });
                changed = true;
            }
        }
    }
    changed
}

/// The game's pack, in the shape of the desktop's library status: "ready" when installed.
pub fn pack_status(id: &str) -> Value {
    let Some(game) = games::by_id(id) else { return json!({"state": "none", "built": null}) };
    let meta = packs_dir().ok().and_then(|d| fs::metadata(d.join(game.pack_name())).ok());
    match meta {
        Some(m) => {
            let built = m.modified().ok().and_then(|t| t.duration_since(UNIX_EPOCH).ok()).map(|d| d.as_secs()).unwrap_or(0);
            json!({"state": "ready", "built": {"built": built, "size": m.len(), "functions": 0, "seconds": 0}})
        }
        None => json!({"state": "none", "built": null}),
    }
}

pub fn delete_pack(id: &str) -> Result<()> {
    let game = games::by_id(id).ok_or("Unknown game.")?;
    let p = packs_dir()?.join(game.pack_name());
    if p.exists() {
        fs::remove_file(&p).map_err(|e| e.to_string())?;
    }
    Ok(())
}

/// Writes the game's settings to its folder's `x2.env`.
pub fn write_env(entry: &GameEntry) -> Result<()> {
    let dir = Path::new(&entry.path);
    let (file, tmp) = (dir.join("x2.env"), dir.join("x2.env.tmp"));
    fs::write(&tmp, entry.settings.env_file())
        .and_then(|_| fs::rename(&tmp, &file))
        .map_err(|e| format!("Could not save {}: {e}. Allow \"All files access\" for this app.", file.display()))
}

/// What the Android pages show besides the library.
pub fn info() -> Value {
    json!({
        "all_files": all_files_access(),
        "roms": rom_dir().join("arcade"),
        "incoming": incoming_dir(),
    })
}
