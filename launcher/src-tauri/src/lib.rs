//! The launcher: the user's game folders, building them for this computer or another platform
//! (recomp.rs), the downloaded build tools (toolchain.rs), and playing where a player exists.
//! On Android it is the app: games found in ES-DE's ROMs folder, played from packs (android.rs).
#[cfg(target_os = "android")]
pub mod android;
pub mod art;
pub mod config;
pub mod device;
pub mod games;
pub mod kit;
pub mod recomp;
pub mod toolchain;
pub mod updates;

use config::{Config, GameEntry, GameSettings};
use serde_json::{json, Value};
#[cfg(not(target_os = "android"))]
use std::io::{BufRead, BufReader, Read};
use std::path::{Path, PathBuf};
use std::process::{Child, Command};
#[cfg(not(target_os = "android"))]
use std::process::Stdio;
use std::sync::{Arc, Mutex};
use tauri::{AppHandle, Manager, State};
#[cfg(not(target_os = "android"))]
use tauri::Emitter;

type Result<T> = std::result::Result<T, String>;

struct Running {
    game: String,
    child: Child,
}

struct AppState {
    data: PathBuf,
    kit: Option<kit::Kit>,
    config: Mutex<Config>,
    jobs: recomp::Jobs,
    running: Arc<Mutex<Option<Running>>>,
}

impl AppState {
    fn config(&self) -> Result<Config> {
        self.config.lock().map(|c| c.clone()).map_err(|e| e.to_string())
    }
    fn update(&self, f: impl FnOnce(&mut Config) -> Result<()>) -> Result<()> {
        let mut c = self.config.lock().map_err(|e| e.to_string())?;
        f(&mut c)?;
        c.save(&config::path(&self.data))
    }
    fn kit(&self) -> Result<&kit::Kit> {
        self.kit.as_ref().ok_or_else(|| "This app's build kit is missing (x2recomp and the runtime sources).".into())
    }
}

fn exe_of(entry: &GameEntry) -> Option<PathBuf> {
    games::identify(Path::new(&entry.path)).ok().map(|(_, exe)| exe)
}

/// A supported game's title, else the name of its folder.
fn title_of(id: &str, path: &str) -> String {
    games::by_id(id).map(|g| g.title.to_string()).unwrap_or_else(|| games::folder_title(path))
}

/// Whether the game is ready to play: its library built for this computer, or on Android its pack.
fn library_status(state: &AppState, id: &str, exe: Option<&Path>) -> Value {
    #[cfg(target_os = "android")]
    {
        let _ = (state, exe);
        android::pack_status(id)
    }
    #[cfg(not(target_os = "android"))]
    {
        let _ = id;
        json!(recomp::library_status(&state.data, state.kit.as_ref(), exe))
    }
}

fn android_info() -> Value {
    #[cfg(target_os = "android")]
    return android::info();
    #[cfg(not(target_os = "android"))]
    Value::Null
}

/// Android: the system screen granting "All files access" (the ROMs folder, x2.env).
#[tauri::command]
fn allow_all_files() -> Result<()> {
    #[cfg(target_os = "android")]
    return android::request_all_files_access();
    #[cfg(not(target_os = "android"))]
    Err("Only on Android.".into())
}

/// Everything the window shows, in one call.
#[tauri::command]
fn load_state(state: State<AppState>) -> Result<Value> {
    #[cfg(target_os = "android")]
    {
        let mut c = state.config.lock().map_err(|e| e.to_string())?;
        if android::sync(&mut c) {
            c.save(&config::path(&state.data))?;
        }
    }
    let config = state.config()?;
    let running = state.running.lock().ok().and_then(|r| r.as_ref().map(|r| r.game.clone()));
    let games: Vec<Value> = config
        .games
        .iter()
        .map(|entry| {
            let exe = exe_of(entry);
            json!({
                "id": entry.id,
                "path": entry.path,
                "settings": entry.settings,
                "info": games::by_id(&entry.id),
                "title": title_of(&entry.id, &entry.path),
                "supported": games::by_id(&entry.id).is_some(),
                "found": exe.is_some() || (games::by_id(&entry.id).is_none() && games::detect(Path::new(&entry.path)).is_some()),
                "library": library_status(&state, &entry.id, exe.as_deref()),
            })
        })
        .collect();
    Ok(json!({
        "version": updates::installed_version(),
        "os": std::env::consts::OS,
        "arch": std::env::consts::ARCH,
        "data": state.data,
        "kit": state.kit,
        "can_play": cfg!(target_os = "android") || state.kit.as_ref().is_some_and(|k| k.player.is_some()),
        "android": android_info(),
        "catalog": games::GAMES,
        "games": games,
        "ndk": config.ndk,
        "jobs": config.jobs,
        "language": config.language,
        "check_updates": !config.no_update_check,
        "job": state.jobs.running(),
        "running": running,
        "targets": recomp::TARGETS,
        "exports": recomp::exports(&state.data),
        "tools": kit::tools(state.kit.as_ref(), &state.data, config.ndk_setting()),
        "packages": toolchain::packages(),
        "tools_size": toolchain::disk_use(&state.data),
    }))
}

/// The game's art for its library card (ES-DE scraped media), as a data: URL, or null.
#[tauri::command]
async fn game_art(state: State<'_, AppState>, id: String) -> Result<Option<String>> {
    let path = state.config()?.games.iter().find(|g| g.id == id).map(|g| PathBuf::from(&g.path));
    // the user's own ES-DE media first, else libretro's thumbnail (fetched once, cached)
    let local = path.and_then(|p| art::find(&p));
    let file = match local {
        Some(f) => Some(f),
        None => art::remote(&state.data, &id).await,
    };
    Ok(file.and_then(|f| art::data_url(&f)))
}

/// Adds a game folder (or points an added game at a new folder).
#[tauri::command]
fn add_game(state: State<AppState>, path: String) -> Result<Value> {
    let found = games::detect(Path::new(&path)).ok_or("No game.exe or launcher.exe in this folder. Choose the folder that holds the game's executable.")?;
    state.update(|c| {
        match c.games.iter_mut().find(|g| g.id == found.id) {
            Some(g) => g.path = path.clone(),
            None => c.games.push(GameEntry { id: found.id.clone(), path: path.clone(), settings: GameSettings::default() }),
        }
        Ok(())
    })?;
    Ok(json!({"id": found.id, "title": title_of(&found.id, &path)}))
}

/// Adds every supported game found under `path` (games::scan); a game already in the library
/// gets the folder found. Returns the games found, each with whether it was new.
#[tauri::command]
async fn scan_folder(state: State<'_, AppState>, path: String) -> Result<Value> {
    let root = PathBuf::from(&path);
    let found = tauri::async_runtime::spawn_blocking(move || games::scan(&root)).await.map_err(|e| e.to_string())?;
    let mut out = Vec::new();
    state.update(|c| {
        for (game, dir) in &found {
            let dir = dir.to_string_lossy().into_owned();
            let title = title_of(&game.id, &dir);
            let new = match c.games.iter_mut().find(|g| g.id == game.id) {
                Some(g) => {
                    g.path = dir;
                    false
                }
                None => {
                    c.games.push(GameEntry { id: game.id.clone(), path: dir, settings: GameSettings::default() });
                    true
                }
            };
            out.push(json!({"id": game.id, "title": title, "new": new}));
        }
        Ok(())
    })?;
    Ok(json!(out))
}

#[tauri::command]
fn remove_game(state: State<AppState>, id: String, delete_builds: bool) -> Result<()> {
    if delete_builds {
        recomp::delete_game_builds(&state.data, &id)?;
    }
    state.update(|c| {
        c.games.retain(|g| g.id != id);
        Ok(())
    })
}

#[tauri::command]
fn save_game_settings(state: State<AppState>, id: String, settings: GameSettings) -> Result<()> {
    state.update(|c| {
        let g = c.games.iter_mut().find(|g| g.id == id).ok_or("Unknown game.")?;
        g.settings = settings;
        // Android: the game folder's x2.env holds them, for ES-DE launches too
        #[cfg(target_os = "android")]
        android::write_env(g)?;
        Ok(())
    })
}

#[tauri::command]
fn save_build_settings(state: State<AppState>, ndk: String, jobs: u32) -> Result<()> {
    state.update(|c| {
        c.ndk = ndk;
        c.jobs = jobs;
        Ok(())
    })
}

#[tauri::command]
fn save_language(state: State<AppState>, language: String) -> Result<()> {
    state.update(|c| {
        c.language = language;
        Ok(())
    })
}

#[tauri::command]
fn save_update_check(state: State<AppState>, enabled: bool) -> Result<()> {
    state.update(|c| {
        c.no_update_check = !enabled;
        Ok(())
    })
}


/// Android devices adb sees, or `adb: false` when there is no adb (Build tools downloads it).
#[tauri::command]
async fn list_devices(state: State<'_, AppState>) -> Result<Value> {
    let Some(adb) = device::adb(&state.data) else { return Ok(json!({"adb": false, "devices": []})) };
    let list = tauri::async_runtime::spawn_blocking(move || device::devices(&adb)).await.map_err(|e| e.to_string())??;
    Ok(json!({"adb": true, "devices": list}))
}

/// Sends the last export of `target` to a device: the app installed, or the core copied for RetroArch.
#[tauri::command]
async fn send_export(state: State<'_, AppState>, target: String, serial: String) -> Result<String> {
    let adb = device::adb(&state.data).ok_or("adb is missing: download it in Build tools.")?;
    let record = recomp::exports(&state.data)
        .into_iter()
        .find(|e| e["target"] == target.as_str())
        .ok_or("Nothing exported for this target yet.")?;
    let files: Vec<String> = record["files"].as_array().into_iter().flatten().filter_map(|f| f.as_str().map(String::from)).collect();
    let sent = tauri::async_runtime::spawn_blocking(move || device::send(&adb, &serial, &target, &files))
        .await
        .map_err(|e| e.to_string())??;
    Ok(sent.to_string())
}
fn game_and_exe(config: &Config, id: &str) -> Result<(&'static games::Game, PathBuf)> {
    let entry = config.game(id).ok_or("This game is not in the library.")?;
    let (game, exe) = games::identify(Path::new(&entry.path))?;
    if game.id != id {
        return Err(format!("The folder now holds {}.", game.title));
    }
    Ok((game, exe))
}

#[tauri::command]
fn build_library(app: AppHandle, state: State<AppState>, id: String) -> Result<()> {
    let config = state.config()?;
    let (game, exe) = game_and_exe(&config, &id)?;
    let kit = state.kit()?.clone();
    let data = state.data.clone();
    let jobs = config.jobs();
    recomp::start(&app, &state.jobs, format!("library:{id}"), move |ctx| recomp::build_library(ctx, &data, &kit, game, &exe, jobs))
}

#[tauri::command]
fn export(app: AppHandle, state: State<AppState>, target: String, ids: Vec<String>) -> Result<()> {
    let config = state.config()?;
    let t = recomp::target(&target).ok_or("Unknown target.")?;
    let chosen = ids.iter().map(|id| game_and_exe(&config, id)).collect::<Result<Vec<_>>>()?;
    let kit = state.kit()?.clone();
    let data = state.data.clone();
    let ndk = kit::ndk(config.ndk_setting());
    let jobs = config.jobs();
    recomp::start(&app, &state.jobs, format!("export:{target}"), move |ctx| recomp::export(ctx, &data, &kit, t, &chosen, ndk, jobs))
}

#[tauri::command]
fn install_tools(app: AppHandle, state: State<AppState>, ids: Vec<String>) -> Result<()> {
    let data = state.data.clone();
    recomp::start(&app, &state.jobs, "tools".into(), move |ctx| toolchain::install(ctx, &data, &ids))
}

#[tauri::command]
fn remove_tools(state: State<AppState>) -> Result<()> {
    if state.jobs.running().is_some() {
        return Err("Wait for the running build, or cancel it.".into());
    }
    toolchain::remove_all(&state.data)
}

#[tauri::command]
fn cancel_job(state: State<AppState>) {
    state.jobs.cancel();
}

#[tauri::command]
fn delete_builds(state: State<AppState>, id: String) -> Result<()> {
    if state.jobs.running().is_some_and(|j| j.ends_with(&format!(":{id}"))) {
        return Err("Cancel the build first.".into());
    }
    #[cfg(target_os = "android")]
    return android::delete_pack(&id);
    #[cfg(not(target_os = "android"))]
    recomp::delete_game_builds(&state.data, &id)
}

/// Starts a game with x2-player and its library; output lines go out as "game-output", the
/// end as "game-state".
#[tauri::command]
fn launch(app: AppHandle, state: State<AppState>, id: String) -> Result<()> {
    let config = state.config()?;
    let entry = config.game(&id).ok_or("This game is not in the library.")?.clone();
    #[cfg(target_os = "android")]
    {
        let _ = app;
        if android::pack_status(&id)["state"] != "ready" {
            return Err("Send this game's Android pack from the launcher on your computer first.".into());
        }
        android::write_env(&entry)?;
        android::play(&entry.path)
    }
    #[cfg(not(target_os = "android"))]
    launch_desktop(app, &state, &config, entry, id)
}

/// The desktop: x2-player with the game's library, its output passed on as events.
#[cfg(not(target_os = "android"))]
fn launch_desktop(app: AppHandle, state: &AppState, config: &Config, entry: GameEntry, id: String) -> Result<()> {
    let (_, exe) = game_and_exe(config, &id)?;
    let player = state.kit()?.player.clone().ok_or("No x2-player for this computer.")?;
    if recomp::library_status(&state.data, state.kit.as_ref(), Some(&exe)).state != "ready" {
        return Err("Build the game for this computer first.".into());
    }
    let mut running = state.running.lock().map_err(|e| e.to_string())?;
    if running.is_some() {
        return Err("A game is already running.".into());
    }
    let (env, args) = entry.settings.launch();
    let mut cmd = Command::new(&player);
    cmd.arg(&entry.path)
        .arg("--game-lib")
        .arg(recomp::library_path(&state.data, &id))
        .args(&args)
        .envs(env)
        .current_dir(&entry.path)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    let mut child = cmd.spawn().map_err(|e| format!("Could not start the game: {e}"))?;
    let _ = app.emit("game-output", format!("Starting {} with {}", entry.path, player.display()));
    for stream in [child.stdout.take().map(|s| Box::new(s) as Box<dyn Read + Send>), child.stderr.take().map(|s| Box::new(s) as Box<dyn Read + Send>)]
        .into_iter()
        .flatten()
    {
        let app = app.clone();
        std::thread::spawn(move || {
            for line in BufReader::new(stream).lines().map_while(std::result::Result::ok) {
                let _ = app.emit("game-output", line);
            }
        });
    }
    *running = Some(Running { game: id.clone(), child });
    let _ = app.emit("game-state", json!({"running": id}));
    let shared = state.running.clone();
    std::thread::spawn(move || loop {
        std::thread::sleep(std::time::Duration::from_millis(300));
        let Ok(mut r) = shared.lock() else { return };
        let finished = match r.as_mut().map(|r| r.child.try_wait()) {
            Some(Ok(None)) => continue,
            Some(Ok(Some(status))) => status.code(),
            _ => None,
        };
        *r = None;
        let _ = app.emit("game-state", json!({"running": null, "code": finished}));
        return;
    });
    Ok(())
}

/// One controller button for the layout editor: x2-player opens a small window and prints
/// "pad <name>" (south, east, west, north, l1, r1, l2, r2, l3, r3); None when cancelled.
#[tauri::command]
async fn read_pad(state: State<'_, AppState>) -> Result<Option<String>> {
    let player = state.kit()?.player.clone().ok_or("No x2-player for this computer.")?;
    let out = tauri::async_runtime::spawn_blocking(move || Command::new(player).arg("--read-input").output())
        .await
        .map_err(|e| e.to_string())?
        .map_err(|e| e.to_string())?;
    if !out.status.success() {
        let error = String::from_utf8_lossy(&out.stderr).trim().to_string();
        return Err(if error.is_empty() { format!("the input reader exited with {}", out.status) } else { error });
    }
    let text = String::from_utf8_lossy(&out.stdout);
    Ok(text.lines().find_map(|l| l.strip_prefix("pad ")).map(|s| s.trim().to_string()))
}

#[tauri::command]
fn stop(state: State<AppState>) -> Result<()> {
    if let Some(r) = state.running.lock().map_err(|e| e.to_string())?.as_mut() {
        // the runtime quits cleanly on SIGTERM (SDL's quit event); Windows has no such signal
        #[cfg(unix)]
        let _ = Command::new("kill").arg("-TERM").arg(r.child.id().to_string()).status();
        #[cfg(not(unix))]
        let _ = r.child.kill();
    }
    Ok(())
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .setup(|app| {
            let data = app.path().app_data_dir()?;
            std::fs::create_dir_all(&data)?;
            let kit = kit::find(app.path().resource_dir().ok());
            let config = Config::load(&config::path(&data));
            app.manage(AppState { data, kit, config: Mutex::new(config), jobs: Default::default(), running: Default::default() });
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            load_state,
            game_art,
            add_game,
            scan_folder,
            remove_game,
            save_game_settings,
            save_build_settings,
            build_library,
            export,
            install_tools,
            remove_tools,
            cancel_job,
            delete_builds,
            launch,
            stop,
            read_pad,
            save_language,
            save_update_check,
            list_devices,
            send_export,
            updates::check_release,
            updates::open_release,
            allow_all_files
        ])
        .on_window_event(|window, event| {
            if let tauri::WindowEvent::Destroyed = event {
                if let Some(state) = window.app_handle().try_state::<AppState>() {
                    state.jobs.cancel();
                    if let Ok(mut r) = state.running.lock() {
                        if let Some(r) = r.as_mut() {
                            let _ = r.child.kill();
                        }
                    }
                }
            }
        })
        .run(tauri::generate_context!())
        .expect("error while running the launcher");
}
