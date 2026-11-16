//! The launcher's build jobs from the console, for scripted builds and testing:
//!   cargo run --example cli -- <data dir> tools <llvm|cmake|ninja|ndk>...
//!   cargo run --example cli -- <data dir> library <game dir>
//!   cargo run --example cli -- <data dir> export <target> <game dir>...
//!   cargo run --example cli -- <data dir> scan <folder>   (the games the launcher would add; reads only)
//!   cargo run --example cli -- <data dir> devices          (Android devices adb sees)
//!   cargo run --example cli -- <data dir> send <android-packs|android-retroarch> <serial> <file>...
//! The kit is found as the app finds it (X2_REPO, or the checkout above the working directory).
use std::path::{Path, PathBuf};
use typex2_launcher_lib::{device, games, kit, recomp, toolchain};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.len() < 2 {
        eprintln!("usage: cli <data dir> tools <id>... | library <game dir> | export <target> <game dir>... | scan <folder> | devices | send <target> <serial> <file>...");
        std::process::exit(2);
    }
    let data = PathBuf::from(&args[0]);
    let kit = kit::find(None).expect("no kit: run inside the repository or set X2_REPO");
    let jobs = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4);
    let identify = |dir: &str| games::identify(Path::new(dir)).unwrap_or_else(|e| panic!("{dir}: {e}"));
    let result = match args[1].as_str() {
        "tools" => toolchain::install(&recomp::Ctx::console("tools"), &data, &args[2..]),
        "library" => {
            let (game, exe) = identify(&args[2]);
            recomp::build_library(&recomp::Ctx::console(&format!("library:{}", game.id)), &data, &kit, game, &exe, jobs)
        }
        "export" => {
            let target = recomp::target(&args[2]).expect("unknown target");
            let chosen: Vec<_> = args[3..].iter().map(|d| identify(d)).collect();
            recomp::export(&recomp::Ctx::console(&format!("export:{}", target.id)), &data, &kit, target, &chosen, kit::ndk(None), jobs)
        }
        "scan" => {
            for (game, dir) in games::scan(Path::new(&args[2])) {
                println!("{:24} {}", game.id, dir.display());
            }
            Ok(serde_json::Value::Null)
        }
        "devices" => device::adb(&data).ok_or_else(|| "no adb".to_string()).and_then(|adb| device::devices(&adb)).map(|d| serde_json::json!(d)),
        "send" => {
            let adb = device::adb(&data).expect("no adb");
            let files: Vec<String> = args[4..].to_vec();
            device::send(&adb, &args[3], &args[2], &files).map(|s| serde_json::json!(s))
        }
        other => Err(format!("unknown command {other}")),
    };
    match result {
        Ok(v) => println!("done: {v}"),
        Err(e) => {
            eprintln!("error: {e}");
            std::process::exit(1);
        }
    }
}
