//! The launcher's build jobs from the console, for scripted builds and testing:
//!   cargo run --example cli -- <data dir> tools <llvm|cmake|ninja|ndk>...
//!   cargo run --example cli -- <data dir> library <game dir>
//!   cargo run --example cli -- <data dir> export <target> <game dir>...
//! The kit is found as the app finds it (X2_REPO, or the checkout above the working directory).
use std::path::{Path, PathBuf};
use typex2_launcher_lib::{games, kit, recomp, toolchain};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.len() < 2 {
        eprintln!("usage: cli <data dir> tools <id>... | library <game dir> | export <target> <game dir>...");
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
