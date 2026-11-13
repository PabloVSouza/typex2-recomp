//! Building, on the user's machine, from the user's own game files. Nothing built here is
//! distributed: generated C, objects, libraries and exports stay in the data folder.
//!
//! * generate: x2recomp turns the executable into C (`work/games/<id>/gen`);
//! * library: that C compiled into a library x2-player loads (playing on this computer);
//! * export: the project's CMake build over the generated C, for a RetroArch core (Android or
//!   this computer) or the Android app, copied to `exports/<target>/`.
//!
//! One job runs at a time. Progress goes out as "job" events, tool output as "job-log" lines.
use crate::games::Game;
use crate::kit::{self, Kit};
use crate::toolchain;
use serde::Serialize;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::fs;
use std::io::{BufRead, BufReader, Read};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use tauri::{AppHandle, Emitter};

type Result<T> = std::result::Result<T, String>;

#[derive(Default)]
struct JobState {
    /// "library:<id>" or "export:<target>".
    running: Option<String>,
    cancel: Arc<AtomicBool>,
    /// Processes started by the job (process groups on Unix), killed on cancel.
    pids: Arc<Mutex<Vec<u32>>>,
}

#[derive(Default, Clone)]
pub struct Jobs(Arc<Mutex<JobState>>);

impl Jobs {
    pub fn running(&self) -> Option<String> {
        self.0.lock().ok().and_then(|j| j.running.clone())
    }

    /// Stops the job: no new step starts, running tools are killed.
    pub fn cancel(&self) {
        if let Ok(job) = self.0.lock() {
            job.cancel.store(true, Ordering::Relaxed);
            if let Ok(pids) = job.pids.lock() {
                for pid in pids.iter() {
                    kill_tree(*pid);
                }
            }
        }
    }
}

pub fn kill_tree(pid: u32) {
    #[cfg(unix)]
    {
        // the tool's process group: CMake's or Ninja's compilers too
        let _ = Command::new("kill").arg("-TERM").arg(format!("-{pid}")).status();
        let _ = Command::new("kill").arg("-TERM").arg(pid.to_string()).status();
    }
    #[cfg(windows)]
    {
        let _ = Command::new("taskkill").args(["/PID", &pid.to_string(), "/T", "/F"]).status();
    }
}

/// A running job's handle on the app: progress, log, cancellation.
#[derive(Clone)]
pub struct Ctx {
    /// None: a console run (examples/cli.rs) prints instead of emitting events.
    app: Option<AppHandle>,
    job: String,
    cancel: Arc<AtomicBool>,
    pids: Arc<Mutex<Vec<u32>>>,
}

impl Ctx {
    /// A job run from the console: progress and tool output printed.
    pub fn console(job: &str) -> Ctx {
        Ctx { app: None, job: job.into(), cancel: Default::default(), pids: Default::default() }
    }
    pub fn progress(&self, mut v: Value) {
        v["job"] = json!(self.job);
        match &self.app {
            Some(app) => drop(app.emit("job", v)),
            None if matches!(v["step"].as_str(), Some("compile" | "build" | "download")) => {}
            None => println!("[{}] {}", self.job, v),
        }
    }
    pub fn log(&self, line: &str) {
        match &self.app {
            Some(app) => drop(app.emit("job-log", json!({"job": self.job, "line": line}))),
            None => println!("  {line}"),
        }
    }
    pub fn cancelled(&self) -> bool {
        self.cancel.load(Ordering::Relaxed)
    }
    pub fn check(&self) -> Result<()> {
        if self.cancel.load(Ordering::Relaxed) {
            Err("cancelled".into())
        } else {
            Ok(())
        }
    }

    /// Runs a tool, its output lines sent to the log and to `each`; fails with its last lines.
    pub fn run(&self, cmd: &mut Command, what: &str, each: &mut dyn FnMut(&str)) -> Result<()> {
        self.check()?;
        #[cfg(unix)]
        std::os::unix::process::CommandExt::process_group(cmd, 0);
        #[cfg(windows)]
        std::os::windows::process::CommandExt::creation_flags(cmd, 0x0800_0000); // no console window
        let mut child = cmd.stdout(Stdio::piped()).stderr(Stdio::piped()).stdin(Stdio::null()).spawn().map_err(|e| format!("{what}: {e}"))?;
        let pid = child.id();
        self.pids.lock().map_err(|e| e.to_string())?.push(pid);
        let (tx, rx) = std::sync::mpsc::channel::<String>();
        let readers: Vec<_> = [child.stdout.take().map(|s| Box::new(s) as Box<dyn Read + Send>), child.stderr.take().map(|s| Box::new(s) as Box<dyn Read + Send>)]
            .into_iter()
            .flatten()
            .map(|s| {
                let tx = tx.clone();
                std::thread::spawn(move || {
                    for line in BufReader::new(s).lines().map_while(std::result::Result::ok) {
                        let _ = tx.send(line);
                    }
                })
            })
            .collect();
        drop(tx);
        let mut tail: Vec<String> = Vec::new();
        for line in rx {
            self.log(&line);
            each(&line);
            tail.push(line);
            if tail.len() > 12 {
                tail.remove(0);
            }
        }
        for r in readers {
            let _ = r.join();
        }
        let status = child.wait().map_err(|e| e.to_string());
        if let Ok(mut pids) = self.pids.lock() {
            pids.retain(|p| *p != pid);
        }
        self.check()?;
        if !status?.success() {
            return Err(format!("{what} failed:\n{}", tail.join("\n")));
        }
        Ok(())
    }
}

/// Starts a job on a worker thread; its end is a "job" event with step "done", "error" or
/// "cancelled".
pub fn start(app: &AppHandle, jobs: &Jobs, job: String, work: impl FnOnce(&Ctx) -> Result<Value> + Send + 'static) -> Result<()> {
    let ctx = {
        let mut state = jobs.0.lock().map_err(|e| e.to_string())?;
        if let Some(other) = &state.running {
            return Err(format!("Another build is running ({other}). Wait for it or cancel it."));
        }
        state.running = Some(job.clone());
        state.cancel = Arc::new(AtomicBool::new(false));
        state.pids = Arc::new(Mutex::new(Vec::new()));
        Ctx { app: Some(app.clone()), job, cancel: state.cancel.clone(), pids: state.pids.clone() }
    };
    let jobs = jobs.clone();
    std::thread::spawn(move || {
        let result = work(&ctx);
        if let Ok(mut state) = jobs.0.lock() {
            state.running = None;
        }
        match result {
            Ok(v) => ctx.progress(json!({"step": "done", "result": v})),
            Err(_) if ctx.cancel.load(Ordering::Relaxed) => ctx.progress(json!({"step": "cancelled"})),
            Err(e) => {
                ctx.log(&e);
                ctx.progress(json!({"step": "error", "message": e}))
            }
        }
    });
    Ok(())
}

// ---- layout of the data folder ----------------------------------------------------------

pub fn games_dir(data: &Path) -> PathBuf {
    data.join("work/games")
}
fn gen_dir(data: &Path, id: &str) -> PathBuf {
    games_dir(data).join(id).join("gen")
}
fn lib_dir(data: &Path, id: &str) -> PathBuf {
    games_dir(data).join(id).join("lib")
}
pub fn exports_dir(data: &Path) -> PathBuf {
    data.join("exports")
}

pub const LIB_NAME: &str = if cfg!(target_os = "macos") {
    "game.dylib"
} else if cfg!(windows) {
    "game.dll"
} else {
    "game.so"
};

pub fn sha256_file(path: &Path) -> Result<String> {
    let mut h = Sha256::new();
    let mut f = fs::File::open(path).map_err(|e| format!("{}: {e}", path.display()))?;
    std::io::copy(&mut f, &mut h).map_err(|e| e.to_string())?;
    Ok(format!("{:x}", h.finalize()))
}

fn read_json(path: &Path) -> Option<Value> {
    serde_json::from_str(&fs::read_to_string(path).ok()?).ok()
}

/// What a build was made from: the executable and the kit.
fn stamp(exe_sha: &str, kit: &Kit) -> Value {
    json!({"exe_sha256": exe_sha, "kit": kit.stamp()})
}

fn stamp_matches(found: Option<Value>, want: &Value) -> bool {
    found.is_some_and(|f| f["exe_sha256"] == want["exe_sha256"] && f["kit"] == want["kit"])
}

// ---- generate ---------------------------------------------------------------------------

/// The game's generated C, made again when the executable or the kit changed.
fn generate(ctx: &Ctx, data: &Path, kit: &Kit, game: &Game, exe: &Path) -> Result<PathBuf> {
    let x2recomp = kit.x2recomp.as_ref().ok_or("x2recomp is missing (see Settings → Tools).")?;
    let gen = gen_dir(data, game.id);
    ctx.progress(json!({"step": "hash", "game": game.id}));
    let want = stamp(&sha256_file(exe)?, kit);
    if stamp_matches(read_json(&gen.join("stamp.json")), &want) && gen.join("sources.cmake").is_file() {
        return Ok(gen);
    }
    ctx.progress(json!({"step": "generate", "game": game.id}));
    let _ = fs::remove_dir_all(&gen);
    fs::create_dir_all(&gen).map_err(|e| e.to_string())?;
    let result = ctx.run(Command::new(x2recomp).arg(exe).arg(&gen), "x2recomp", &mut |_| {});
    if let Err(e) = result {
        let _ = fs::remove_dir_all(&gen);
        return Err(e);
    }
    fs::write(gen.join("stamp.json"), want.to_string()).map_err(|e| e.to_string())?;
    Ok(gen)
}

// ---- library (play on this computer) -----------------------------------------------------

#[derive(Serialize)]
pub struct LibraryStatus {
    /// "none", "ready" or "stale" (built from another executable or by another kit).
    pub state: &'static str,
    pub built: Option<Value>,
}

pub fn library_status(data: &Path, kit: Option<&Kit>, exe: Option<&Path>) -> LibraryStatus {
    let dir = lib_dir_from_exe(data, exe);
    let record = dir.as_ref().and_then(|d| read_json(&d.join("library.json")).filter(|_| d.join(LIB_NAME).is_file()));
    let Some(record) = record else {
        return LibraryStatus { state: "none", built: None };
    };
    // hashing a 30 MB executable on every refresh is avoided: the record keeps its size and time
    let fresh = match (kit, exe) {
        (Some(kit), Some(exe)) => record["kit"] == json!(kit.stamp()) && same_file_meta(exe, &record),
        _ => false,
    };
    LibraryStatus { state: if fresh { "ready" } else { "stale" }, built: Some(record) }
}

fn lib_dir_from_exe(data: &Path, exe: Option<&Path>) -> Option<PathBuf> {
    let (game, _) = crate::games::identify(exe?.parent()?).ok()?;
    Some(lib_dir(data, game.id))
}

fn file_meta(path: &Path) -> (u64, u64) {
    let m = fs::metadata(path).ok();
    let len = m.as_ref().map(|m| m.len()).unwrap_or(0);
    let mtime = m.and_then(|m| m.modified().ok()).and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok()).map(|d| d.as_secs()).unwrap_or(0);
    (len, mtime)
}

fn same_file_meta(exe: &Path, record: &Value) -> bool {
    let (len, mtime) = file_meta(exe);
    record["exe_len"] == json!(len) && record["exe_mtime"] == json!(mtime)
}

pub fn library_path(data: &Path, id: &str) -> PathBuf {
    lib_dir(data, id).join(LIB_NAME)
}

/// The descriptor x2-player looks up: the generated tables renamed (as runtime/CMakeLists.txt
/// does for multi-game binaries) and exported as `x2_game_tables`.
fn descriptor(game_id: &str) -> String {
    let mut s = String::from("/* generated by the launcher */\n");
    for t in ["func_table", "func_count", "import_table", "import_count", "image", "tls_callbacks"] {
        s += &format!("#define x2_{t} x2g_{t}\n");
    }
    s += "#include \"table.c\"\n";
    s += &format!(
        "__attribute__((visibility(\"default\"))) const X2GameTables x2_game_tables = {{x2_func_table, x2_func_count, x2_import_table, x2_import_count, &x2_image, x2_tls_callbacks, \"{game_id}\"}};\n"
    );
    s
}

/// The compiler for game libraries and its extra flags: the downloaded clang (no system SDK:
/// clang's own headers plus the kit's stand-ins in runtime/include/sdkless, linked by lld
/// without the system's libraries), else the system's.
struct Compiler {
    cc: PathBuf,
    compile: Vec<String>,
    link: Vec<String>,
}

fn compiler(data: &Path, kit: &Kit) -> Result<Compiler> {
    let shared: Vec<String> = if cfg!(target_os = "macos") {
        // A bundle linked against x2-player: runtime symbols (thread-locals too, which lld cannot
        // leave to load time) bind to the executable, a missing one fails here; the C library's
        // functions are looked up when the player loads it.
        let player = kit.player.as_ref().ok_or("x2-player is missing.")?;
        vec!["-bundle".into(), "-bundle_loader".into(), player.display().to_string(), "-Wl,-undefined,dynamic_lookup".into()]
    } else {
        vec!["-shared".into(), "-fPIC".into()]
    };
    if let Some(llvm) = toolchain::llvm(data) {
        let mut target = Vec::new();
        if cfg!(target_os = "macos") {
            target.push(format!("--target={}-apple-macos13", std::env::consts::ARCH.replace("aarch64", "arm64")));
        }
        let mut compile = target.clone();
        compile.extend(["-nostdinc".into(), "-fno-stack-protector".into(), "-isystem".into(), llvm.resource.display().to_string(), "-isystem".into(), kit.include().join("sdkless").display().to_string()]);
        let mut link = target;
        link.extend(["-fuse-ld=lld".into(), "-nostdlib".into()]);
        link.extend(shared);
        return Ok(Compiler { cc: llvm.clang, compile, link });
    }
    let cc = kit::cc().ok_or("No C compiler: download LLVM clang in Settings → Tools.")?;
    Ok(Compiler { cc, compile: Vec::new(), link: shared })
}

pub fn build_library(ctx: &Ctx, data: &Path, kit: &Kit, game: &Game, exe: &Path, jobs: usize) -> Result<Value> {
    if kit.player.is_none() {
        return Err("This computer has no x2-player: games can be exported for other platforms, not played here.".into());
    }
    let cc = compiler(data, kit)?;
    let started = std::time::Instant::now();
    let gen = generate(ctx, data, kit, game, exe)?;
    let dir = lib_dir(data, game.id);
    let obj = dir.join("obj");
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&obj).map_err(|e| e.to_string())?;
    fs::write(dir.join("x2_game.c"), descriptor(game.id)).map_err(|e| e.to_string())?;

    let mut sources: Vec<PathBuf> = fs::read_dir(&gen)
        .map_err(|e| e.to_string())?
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.extension().is_some_and(|x| x == "c") && p.file_name().is_some_and(|n| n != "table.c"))
        .collect();
    // biggest first: the long files start early and the cores stay busy at the end
    sources.sort_by_key(|p| std::cmp::Reverse(fs::metadata(p).map(|m| m.len()).unwrap_or(0)));
    sources.push(dir.join("x2_game.c"));
    let total = sources.len();
    ctx.progress(json!({"step": "compile", "game": game.id, "done": 0, "total": total}));

    let next = AtomicUsize::new(0);
    let done = AtomicUsize::new(0);
    let failure: Mutex<Option<String>> = Mutex::new(None);
    let include = kit.include();
    std::thread::scope(|s| {
        for _ in 0..jobs.min(total).max(1) {
            s.spawn(|| loop {
                let i = next.fetch_add(1, Ordering::Relaxed);
                if i >= total || failure.lock().map(|f| f.is_some()).unwrap_or(true) {
                    return;
                }
                let src = &sources[i];
                let out = obj.join(src.file_stem().unwrap_or_default()).with_extension("o");
                let mut cmd = Command::new(&cc.cc);
                cmd.args(&cc.compile);
                // the flags x2_add_game uses for generated code (runtime/CMakeLists.txt)
                cmd.args(["-c", "-w", "-fno-strict-aliasing", "-fwrapv", "-fno-semantic-interposition", "-O1", "-fPIC"])
                    .arg("-I")
                    .arg(&gen)
                    .arg("-I")
                    .arg(&include)
                    .arg(src)
                    .arg("-o")
                    .arg(&out);
                let name = src.file_name().unwrap_or_default().to_string_lossy().to_string();
                if let Err(e) = ctx.run(&mut cmd, &format!("Compiling {name}"), &mut |_| {}) {
                    if let Ok(mut f) = failure.lock() {
                        f.get_or_insert(e);
                    }
                    return;
                }
                let n = done.fetch_add(1, Ordering::Relaxed) + 1;
                ctx.progress(json!({"step": "compile", "game": game.id, "done": n, "total": total}));
            });
        }
    });
    if let Some(e) = failure.into_inner().map_err(|e| e.to_string())? {
        return Err(e);
    }
    ctx.check()?;

    ctx.progress(json!({"step": "link", "game": game.id}));
    let lib = dir.join(LIB_NAME);
    let mut link = Command::new(&cc.cc);
    link.args(&cc.link);
    let mut objects: Vec<PathBuf> = fs::read_dir(&obj).map_err(|e| e.to_string())?.flatten().map(|e| e.path()).collect();
    objects.sort();
    link.args(&objects).arg("-o").arg(&lib);
    ctx.run(&mut link, "Linking", &mut |_| {})?;
    let _ = fs::remove_dir_all(&obj);

    let (exe_len, exe_mtime) = file_meta(exe);
    let gen_stamp = read_json(&gen.join("stamp.json")).unwrap_or_default();
    let record = json!({
        "game": game.id,
        "exe_sha256": gen_stamp["exe_sha256"],
        "exe_len": exe_len,
        "exe_mtime": exe_mtime,
        "kit": kit.stamp(),
        "built": std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0),
        "seconds": started.elapsed().as_secs(),
        "size": fs::metadata(&lib).map(|m| m.len()).unwrap_or(0),
        "functions": count_functions(&gen),
    });
    fs::write(dir.join("library.json"), serde_json::to_vec_pretty(&record).map_err(|e| e.to_string())?).map_err(|e| e.to_string())?;
    Ok(record)
}

/// Generated functions, from table.c's function table rows.
fn count_functions(gen: &Path) -> usize {
    fs::read_to_string(gen.join("table.c")).map(|t| t.matches("{0x").count()).unwrap_or(0)
}

pub fn delete_game_builds(data: &Path, id: &str) -> Result<()> {
    let dir = games_dir(data).join(id);
    if dir.exists() {
        fs::remove_dir_all(&dir).map_err(|e| e.to_string())?;
    }
    Ok(())
}

// ---- exports (CMake) ---------------------------------------------------------------------

#[derive(Serialize, Clone, Copy)]
pub struct Target {
    pub id: &'static str,
    pub name: &'static str,
    pub description: &'static str,
    /// Tools it needs (kit::tools ids).
    pub needs: &'static [&'static str],
    #[serde(skip)]
    android: bool,
    #[serde(skip)]
    cmake_target: &'static str,
    #[serde(skip)]
    standalone: bool,
}

const LIBRETRO_HOST: &str = if cfg!(target_os = "macos") {
    "typex2_libretro.dylib"
} else if cfg!(windows) {
    "typex2_libretro.dll"
} else {
    "typex2_libretro.so"
};

pub const TARGETS: &[Target] = &[
    Target {
        id: "android-retroarch",
        name: "RetroArch core for Android",
        description: "typex2_libretro_android.so with the chosen games. Copy it to RetroArch/downloads/ on the device, then Load Core → Install or Restore a Core.",
        needs: &["x2recomp", "cmake", "ninja", "ndk"],
        android: true,
        cmake_target: "typex2_libretro",
        standalone: false,
    },
    Target {
        id: "android-app",
        name: "Android app",
        description: "typex2.apk: the standalone app (ES-DE can launch it) with the chosen games. Needs the Android SDK's build tools next to the NDK.",
        needs: &["x2recomp", "cmake", "ninja", "ndk"],
        android: true,
        cmake_target: "apk",
        standalone: true,
    },
    Target {
        id: "host-retroarch",
        name: "RetroArch core for this computer",
        description: "The libretro core for RetroArch on this computer, with the chosen games. Copy it and typex2_libretro.info into RetroArch's cores folder.",
        needs: &["x2recomp", "cmake", "ninja", "cc"],
        android: false,
        cmake_target: "typex2_libretro",
        standalone: false,
    },
];

pub fn target(id: &str) -> Option<&'static Target> {
    TARGETS.iter().find(|t| t.id == id)
}

/// `[12/345] Building C object ...` → (12, 345).
pub fn ninja_progress(line: &str) -> Option<(u64, u64)> {
    let rest = line.strip_prefix('[')?;
    let (nums, _) = rest.split_once(']')?;
    let (a, b) = nums.split_once('/')?;
    Some((a.trim().parse().ok()?, b.trim().parse().ok()?))
}

pub fn export(ctx: &Ctx, data: &Path, kit: &Kit, target: &Target, games: &[(&'static Game, PathBuf)], ndk: Option<PathBuf>, jobs: usize) -> Result<Value> {
    if games.is_empty() {
        return Err("Choose at least one game.".into());
    }
    let cmake = toolchain::cmake_bin(data).or_else(|| kit::which("cmake")).ok_or("CMake is missing: download it in Settings → Tools.")?;
    let ninja = toolchain::ninja_bin(data).or_else(|| kit::which("ninja")).ok_or("Ninja is missing: download it in Settings → Tools.")?;
    for (game, exe) in games {
        generate(ctx, data, kit, game, exe)?;
    }
    let ids: Vec<&str> = games.iter().map(|(g, _)| g.id).collect();
    let build = data.join("work/build").join(target.id);
    fs::create_dir_all(&build).map_err(|e| e.to_string())?;

    ctx.progress(json!({"step": "configure"}));
    let mut cfg = Command::new(&cmake);
    cfg.arg("-S")
        .arg(&kit.source)
        .arg("-B")
        .arg(&build)
        .args(["-G", "Ninja"])
        .arg(format!("-DCMAKE_MAKE_PROGRAM={}", ninja.display()))
        .arg("-DCMAKE_BUILD_TYPE=Release")
        .arg("-DX2_BUILD_TOOLS=OFF")
        .arg(format!("-DX2_GAMES_DIR={}", games_dir(data).display()))
        .arg(format!("-DX2_GAMES={}", ids.join(";")))
        .arg(format!("-DX2_STANDALONE={}", if target.standalone { "ON" } else { "OFF" }))
        .arg(format!("-DX2_LIBRETRO={}", if target.standalone { "OFF" } else { "ON" }));
    if target.android {
        let ndk = toolchain::ndk_dir(data).or(ndk).ok_or("The Android NDK is missing: download it in Settings → Tools.")?;
        cfg.arg(format!("-DCMAKE_TOOLCHAIN_FILE={}", ndk.join("build/cmake/android.toolchain.cmake").display()))
            .arg("-DANDROID_ABI=arm64-v8a")
            .arg("-DANDROID_PLATFORM=android-33");
    }
    // With the downloaded tools, the build sees them and the OS's base folders only (no user
    // PATH: nothing else installed on the computer is picked up). With system tools, the
    // usual places too, which apps started from the Finder do not get.
    let own = cmake.starts_with(toolchain::root(data)) && ninja.starts_with(toolchain::root(data));
    let base: Vec<PathBuf> = if cfg!(windows) {
        let sys = std::env::var_os("SystemRoot").map(PathBuf::from).unwrap_or_else(|| PathBuf::from(r"C:\Windows"));
        vec![sys.join("System32"), sys.clone()]
    } else {
        ["/usr/bin", "/bin", "/usr/sbin", "/sbin"].iter().map(PathBuf::from).collect()
    };
    let user: Vec<PathBuf> = if own {
        Vec::new()
    } else {
        std::env::split_paths(&std::env::var_os("PATH").unwrap_or_default())
            .chain(["/opt/homebrew/bin", "/usr/local/bin"].iter().map(PathBuf::from))
            .collect()
    };
    let path = std::env::join_paths(
        [ninja.parent().map(Path::to_path_buf), cmake.parent().map(Path::to_path_buf)].into_iter().flatten().chain(user).chain(base),
    )
    .map_err(|e| e.to_string())?;
    cfg.env("PATH", &path);
    ctx.run(&mut cfg, "Configuring", &mut |_| {})?;

    ctx.progress(json!({"step": "build", "done": 0, "total": 0}));
    let mut b = Command::new(&cmake);
    b.arg("--build").arg(&build).args(["--target", target.cmake_target, "--parallel", &jobs.to_string()]).env("PATH", &path);
    let progress = ctx.clone();
    ctx.run(&mut b, "Building", &mut |line| {
        if let Some((done, total)) = ninja_progress(line) {
            progress.progress(json!({"step": "build", "done": done, "total": total}));
        }
    })?;

    let outputs: Vec<(PathBuf, String)> = match target.id {
        "android-retroarch" => vec![(build.join("typex2_libretro_android.so"), "typex2_libretro_android.so".into())],
        "android-app" => vec![(build.join("typex2.apk"), "typex2.apk".into())],
        _ => vec![
            (build.join(LIBRETRO_HOST), LIBRETRO_HOST.into()),
            (kit.source.join("tools/libretro/typex2_libretro.info"), "typex2_libretro.info".into()),
        ],
    };
    let out_dir = exports_dir(data).join(target.id);
    fs::create_dir_all(&out_dir).map_err(|e| e.to_string())?;
    let mut files = Vec::new();
    for (from, name) in outputs {
        if !from.is_file() {
            return Err(format!("The build did not produce {name}."));
        }
        let to = out_dir.join(&name);
        fs::copy(&from, &to).map_err(|e| format!("Copying {name}: {e}"))?;
        files.push(to.display().to_string());
    }
    let record = json!({"target": target.id, "games": ids, "files": files,
        "built": std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)});
    fs::write(out_dir.join("export.json"), serde_json::to_vec_pretty(&record).map_err(|e| e.to_string())?).map_err(|e| e.to_string())?;
    Ok(record)
}

/// The last export of each target (exports/<target>/export.json).
pub fn exports(data: &Path) -> Vec<Value> {
    TARGETS.iter().filter_map(|t| read_json(&exports_dir(data).join(t.id).join("export.json"))).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ninja_lines_give_progress() {
        assert_eq!(ninja_progress("[12/345] Building C object x.o"), Some((12, 345)));
        assert_eq!(ninja_progress("-- Configuring done"), None);
        assert_eq!(ninja_progress("[a/b] x"), None);
    }

    #[test]
    fn descriptor_renames_the_tables_and_exports_one_symbol() {
        let d = descriptor("kof12");
        assert!(d.contains("#define x2_image x2g_image"));
        assert!(d.contains("const X2GameTables x2_game_tables"));
        assert!(d.contains("\"kof12\""));
    }

    #[test]
    fn stamps_compare_executable_and_kit() {
        let a = json!({"exe_sha256": "x", "kit": "k"});
        assert!(stamp_matches(Some(a.clone()), &a));
        assert!(!stamp_matches(Some(json!({"exe_sha256": "y", "kit": "k"})), &a));
        assert!(!stamp_matches(None, &a));
    }
}
