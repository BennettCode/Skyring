//! fake-peer: plays one side of the link so the other side can be tested without its game.
//!
//! `cargo run -p fake-peer -- <skyrim|er> [--seconds N] [--no-bye] [--region NAME]`
//! - `--seconds N`: run for N seconds (default: until killed), then exit.
//! - `--no-bye`: exit without a Bye, like a crash (the other side should log a heartbeat timeout).
//! - `--region NAME`: use another mapping name (tests use one so they never touch a running game's region).
//!
//! Logs to stdout in the shared format with side tag FAKE-SKY / FAKE-ER.

use std::time::{Duration, Instant};

use skyrimxer_protocol::link::{Identity, Level, Link, Side};
use skyrimxer_protocol::now_ms;
use skyrimxer_protocol::proto::{LINK_TICK_MS, REGION_NAME};

fn usage() -> ! {
    eprintln!("usage: fake-peer <skyrim|er> [--seconds N] [--no-bye] [--region NAME]");
    std::process::exit(2);
}

fn main() {
    let mut args = std::env::args().skip(1);
    let side = match args.next().as_deref() {
        Some("skyrim") => Side::Skyrim,
        Some("er") => Side::EldenRing,
        _ => usage(),
    };
    let (mut seconds, mut bye, mut region) = (None, true, REGION_NAME.to_string());
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--seconds" => seconds = args.next().and_then(|s| s.parse::<f64>().ok()).or_else(|| usage()),
            "--no-bye" => bye = false,
            "--region" => region = args.next().unwrap_or_else(|| usage()),
            _ => usage(),
        }
    }

    let tag = match side {
        Side::Skyrim => "FAKE-SKY",
        Side::EldenRing => "FAKE-ER",
    };
    let log = move |level: Level, msg: &str| {
        let ts = chrono::Utc::now().format("%Y-%m-%dT%H:%M:%S%.3fZ");
        let level = match level {
            Level::Info => "info",
            Level::Warn => "warning",
            Level::Error => "error",
        };
        println!("{ts} [{tag}] [{level}] [link] {msg}");
    };
    log(Level::Info, &format!("fake {} starting on {region} (bye on exit: {bye})", side.name()));
    let game_version = match side {
        Side::Skyrim => [1, 7, 104, 0],
        Side::EldenRing => [2, 7, 1, 0],
    };
    let identity = Identity { plugin_version: [0, 1, 0], game_version };
    let mut link = Link::with_region_name(side, &region, identity, Box::new(log));

    let start = Instant::now();
    let mut frames = 0u64;
    while seconds.is_none_or(|s| start.elapsed().as_secs_f64() < s) {
        frames += 1; // one fake "frame" per tick
        link.tick(now_ms(), frames);
        std::thread::sleep(Duration::from_millis(LINK_TICK_MS));
    }
    if bye {
        link.shutdown();
    } else {
        log(Level::Info, "exiting without Bye (simulated crash)");
        std::process::exit(0); // skip Drop, which would send Bye
    }
}
