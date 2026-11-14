//! The launcher: the user's game folders, building them for this computer or another platform
//! (recomp.rs), the downloaded build tools (toolchain.rs), and playing where a player exists.
pub mod config;
pub mod games;
pub mod kit;
pub mod recomp;
pub mod toolchain;

use config::{Config, GameEntry, GameSettings};
use serde_json::{json, Value};
use std::io::{BufRead, BufReader, Read};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use tauri::{AppHandle, Emitter, Manager, State};

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

/// Everything the window shows, in one call.
#[tauri::command]
fn load_state(state: State<AppState>) -> Result<Value> {
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
                "found": exe.is_some(),
                "library": recomp::library_status(&state.data, state.kit.as_ref(), exe.as_deref()),
            })
        })
        .collect();
    Ok(json!({
        "version": env!("CARGO_PKG_VERSION"),
        "os": std::env::consts::OS,
        "arch": std::env::consts::ARCH,
        "data": state.data,
        "kit": state.kit,
        "can_play": state.kit.as_ref().is_some_and(|k| k.player.is_some()),
        "catalog": games::GAMES,
        "games": games,
        "ndk": config.ndk,
        "jobs": config.jobs,
        "job": state.jobs.running(),
        "running": running,
        "targets": recomp::TARGETS,
        "exports": recomp::exports(&state.data),
        "tools": kit::tools(state.kit.as_ref(), &state.data, config.ndk_setting()),
        "packages": toolchain::packages(),
        "tools_size": toolchain::disk_use(&state.data),
    }))
}

/// Adds a game folder (or points an added game at a new folder).
#[tauri::command]
fn add_game(state: State<AppState>, path: String) -> Result<Value> {
    let (game, _) = games::identify(Path::new(&path))?;
    state.update(|c| {
        match c.games.iter_mut().find(|g| g.id == game.id) {
            Some(g) => g.path = path.clone(),
            None => c.games.push(GameEntry { id: game.id.into(), path: path.clone(), settings: GameSettings::default() }),
        }
        Ok(())
    })?;
    Ok(json!({"id": game.id, "title": game.title}))
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
    recomp::delete_game_builds(&state.data, &id)
}

/// Starts a game with x2-player and its library; output lines go out as "game-output", the
/// end as "game-state".
#[tauri::command]
fn launch(app: AppHandle, state: State<AppState>, id: String) -> Result<()> {
    let config = state.config()?;
    let entry = config.game(&id).ok_or("This game is not in the library.")?.clone();
    let (_, exe) = game_and_exe(&config, &id)?;
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
            add_game,
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
            read_pad
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
