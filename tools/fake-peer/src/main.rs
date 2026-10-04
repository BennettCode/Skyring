//! fake-peer: plays one side of the link so the other side can be tested without its game.
//!
//! `cargo run -p fake-peer -- <skyrim|er> [--seconds N] [--no-bye] [--region NAME] [--dodge-every S]`
//! - `--seconds N`: run for N seconds (default: until killed), then exit.
//! - `--no-bye`: exit without a Bye, like a crash (the other side should log a heartbeat timeout).
//! - `--region NAME`: use another mapping name (tests use one so they never touch a running game's region).
//! - `--dodge-every S` (skyrim): hold Dodge for 250 ms, first 1 s after connecting, then every S seconds (default 4).
//!
//! Same thread shape as the plugins: the link ticks on its own thread every LINK_TICK_MS, the main thread runs ~60 "frames"
//! per second and touches only the slots (through `LinkShared`, never the link's lock).
//! - skyrim: writes InputState every frame (Dodge pulses), reads PlayerState and logs its edges.
//! - er: reads InputState; each Dodge press costs 20 stamina and gives 1 s of IFrame|Dodging (anim 27010), then stamina
//!   regenerates +10 every 250 ms. Writes PlayerState every frame.
//!
//! Logs to stdout in the shared format with side tag FAKE-SKY / FAKE-ER.

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use skyrimxer_protocol::link::{Identity, Level, Link, LinkShared, Side};
use skyrimxer_protocol::now_ms;
use skyrimxer_protocol::proto::{
    Button, InputState, LINK_TICK_MS, OFF_SLOT_INPUT, OFF_SLOT_PLAYER, PlayerFlag, PlayerState, REGION_NAME,
};
use skyrimxer_protocol::slot::{SlotReader, SlotWriter, fresh};

const FRAME_MS: u64 = 16;
const DODGE: u32 = 1 << Button::Dodge as u32;
const IFRAME: u32 = 1 << PlayerFlag::IFrame as u32;

fn usage() -> ! {
    eprintln!("usage: fake-peer <skyrim|er> [--seconds N] [--no-bye] [--region NAME] [--dodge-every S]");
    std::process::exit(2);
}

#[derive(Clone, Copy)]
struct Logger {
    tag: &'static str,
}

impl Logger {
    fn log(self, level: Level, subsystem: &str, msg: &str) {
        let ts = chrono::Utc::now().format("%Y-%m-%dT%H:%M:%S%.3fZ");
        let level = match level {
            Level::Info => "info",
            Level::Warn => "warning",
            Level::Error => "error",
        };
        println!("{ts} [{}] [{level}] [{subsystem}] {msg}", self.tag);
    }
    fn info(self, subsystem: &str, msg: &str) {
        self.log(Level::Info, subsystem, msg);
    }
}

/// Logs PlayerState edges on the Skyrim side (same wording as skse/tests/link_test.cpp): fresh/stale transitions,
/// stamina changes, IFrame on/off, anim changes.
#[derive(Default)]
struct PlayerWatch {
    fresh: bool,
    last: Option<PlayerState>,
}

impl PlayerWatch {
    fn update(&mut self, log: Logger, state: Option<PlayerState>, connected: bool, now: u64) {
        let is_fresh = connected && state.is_some_and(|s| fresh(s.time_ms, now));
        if is_fresh != self.fresh {
            self.fresh = is_fresh;
            match state {
                Some(s) if is_fresh => log.info(
                    "state",
                    &format!(
                        "PlayerState fresh: stamina={}/{} hp={}/{} flags={:#x} anim={} er_frame={}",
                        s.stamina, s.max_stamina, s.hp, s.max_hp, s.flags, s.anim_id, s.frame
                    ),
                ),
                _ => {
                    let why = match state {
                        _ if !connected => "link not connected".to_string(),
                        None => "never written".to_string(),
                        Some(s) => format!("last write {} ms ago", now.saturating_sub(s.time_ms)),
                    };
                    log.log(Level::Warn, "state", &format!("PlayerState stale ({why}); ignoring it"));
                    self.last = None;
                }
            }
        }
        let Some(s) = state.filter(|_| is_fresh) else { return };
        if let Some(prev) = self.last {
            if s.stamina != prev.stamina {
                log.info("state", &format!("stamina {}→{} er_frame={}", prev.stamina, s.stamina, s.frame));
            }
            if (s.flags ^ prev.flags) & IFRAME != 0 {
                log.info("state", &format!("IFrame {} er_frame={}", if s.flags & IFRAME != 0 { "on" } else { "off" }, s.frame));
            }
            if s.anim_id != prev.anim_id {
                log.info("state", &format!("anim {}→{} er_frame={}", prev.anim_id, s.anim_id, s.frame));
            }
        }
        self.last = Some(s);
    }
}

/// Fake Skyrim: Dodge pulses into InputState, PlayerState edges out.
struct FakeSky {
    every_ms: u64,
    next_pulse: Option<u64>,
    held: bool,
    writer: SlotWriter<InputState>,
    reader: SlotReader<PlayerState>,
    watch: PlayerWatch,
}

impl FakeSky {
    fn frame(&mut self, log: Logger, shared: &LinkShared, frame: u64, now: u64) {
        let connected = shared.connected();
        if connected && self.next_pulse.is_none() {
            self.next_pulse = Some(now + 1000);
        }
        let held = match self.next_pulse {
            Some(start) if now >= start + 250 => {
                self.next_pulse = Some(start + self.every_ms);
                false
            }
            Some(start) => now >= start,
            None => false,
        };
        if held != self.held {
            self.held = held;
            log.info("input", &format!("Dodge {} frame={frame}", if held { "down" } else { "up" }));
        }
        self.writer.write(&InputState { frame, time_ms: now, buttons: if held { DODGE } else { 0 }, ..Default::default() });
        self.watch.update(log, self.reader.read(), connected, now);
    }
}

/// Fake ER: a dodge costs 20 stamina and gives 1 s of i-frames.
struct FakeEr {
    reader: SlotReader<InputState>,
    writer: SlotWriter<PlayerState>,
    held: bool,
    stamina: i32,
    dodge_until: u64,
    next_regen: u64,
}

impl FakeEr {
    fn frame(&mut self, log: Logger, shared: &LinkShared, frame: u64, now: u64) {
        let input = self.reader.read().filter(|i| shared.connected() && fresh(i.time_ms, now));
        let held = input.is_some_and(|i| i.buttons & DODGE != 0);
        if held && !self.held {
            let i = input.unwrap();
            let before = self.stamina;
            self.stamina = (self.stamina - 20).max(0);
            self.dodge_until = now + 1000;
            self.next_regen = self.dodge_until;
            log.info(
                "input",
                &format!(
                    "Dodge down (InputState frame={} seq={} age={}ms) → dodge: stamina {before}→{}, IFrame for 1 s",
                    i.frame,
                    i.seq,
                    now.saturating_sub(i.time_ms),
                    self.stamina
                ),
            );
        }
        self.held = held;
        let dodging = now < self.dodge_until;
        if !dodging && self.stamina < 100 && now >= self.next_regen {
            self.stamina = (self.stamina + 10).min(100);
            self.next_regen = now + 250;
        }
        let mut flags = 1 << PlayerFlag::InWorld as u32;
        if dodging {
            flags |= IFRAME | 1 << PlayerFlag::Dodging as u32;
        }
        self.writer.write(&PlayerState {
            flags,
            frame,
            time_ms: now,
            hp: 500,
            max_hp: 500,
            fp: 80,
            max_fp: 80,
            stamina: self.stamina,
            max_stamina: 100,
            anim_id: if dodging { 27010 } else { 0 },
            block_id: 0x0A01_0000,
            ..Default::default()
        });
    }
}

enum Role {
    Sky(FakeSky),
    Er(FakeEr),
}

fn main() {
    let mut args = std::env::args().skip(1);
    let side = match args.next().as_deref() {
        Some("skyrim") => Side::Skyrim,
        Some("er") => Side::EldenRing,
        _ => usage(),
    };
    let (mut seconds, mut bye, mut region, mut dodge_every) = (None, true, REGION_NAME.to_string(), 4.0);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--seconds" => seconds = args.next().and_then(|s| s.parse::<f64>().ok()).or_else(|| usage()),
            "--no-bye" => bye = false,
            "--region" => region = args.next().unwrap_or_else(|| usage()),
            "--dodge-every" => dodge_every = args.next().and_then(|s| s.parse::<f64>().ok()).unwrap_or_else(|| usage()),
            _ => usage(),
        }
    }

    let log = Logger {
        tag: match side {
            Side::Skyrim => "FAKE-SKY",
            Side::EldenRing => "FAKE-ER",
        },
    };
    log.info("link", &format!("fake {} starting on {region} (bye on exit: {bye})", side.name()));
    let game_version = match side {
        Side::Skyrim => [1, 7, 104, 0],
        Side::EldenRing => [2, 7, 1, 0],
    };
    let identity = Identity { plugin_version: [0, 1, 0], game_version };
    let link = Link::with_region_name(side, &region, identity, Box::new(move |level, msg: &str| log.log(level, "link", msg)));
    let shared = link.shared();
    let link = Arc::new(Mutex::new(link));

    // Link thread, like the plugins: ticks every LINK_TICK_MS with the frame count the "game" reports.
    let frames = Arc::new(AtomicU64::new(0));
    let stop = Arc::new(AtomicBool::new(false));
    let link_thread = {
        let (link, frames, stop) = (link.clone(), frames.clone(), stop.clone());
        std::thread::spawn(move || {
            while !stop.load(Ordering::Relaxed) {
                link.lock().unwrap().tick(now_ms(), frames.load(Ordering::Relaxed));
                std::thread::sleep(Duration::from_millis(LINK_TICK_MS));
            }
        })
    };

    // "Game" thread: slots only.
    let start = Instant::now();
    let mut role = None;
    while seconds.is_none_or(|s| start.elapsed().as_secs_f64() < s) {
        let frame = frames.fetch_add(1, Ordering::Relaxed) + 1;
        let now = now_ms();
        if role.is_none() {
            role = shared.region().map(|r| match side {
                Side::Skyrim => Role::Sky(FakeSky {
                    every_ms: (dodge_every * 1000.0) as u64,
                    next_pulse: None,
                    held: false,
                    writer: SlotWriter::new(r.clone(), OFF_SLOT_INPUT),
                    reader: SlotReader::new(r, OFF_SLOT_PLAYER),
                    watch: PlayerWatch::default(),
                }),
                Side::EldenRing => Role::Er(FakeEr {
                    reader: SlotReader::new(r.clone(), OFF_SLOT_INPUT),
                    writer: SlotWriter::new(r, OFF_SLOT_PLAYER),
                    held: false,
                    stamina: 100,
                    dodge_until: 0,
                    next_regen: 0,
                }),
            });
        }
        match role.as_mut() {
            Some(Role::Sky(sky)) => sky.frame(log, &shared, frame, now),
            Some(Role::Er(er)) => er.frame(log, &shared, frame, now),
            None => {}
        }
        std::thread::sleep(Duration::from_millis(FRAME_MS));
    }
    stop.store(true, Ordering::Relaxed);
    let _ = link_thread.join();
    if bye {
        link.lock().unwrap().shutdown();
    } else {
        log.info("link", "exiting without Bye (simulated crash)");
        std::process::exit(0); // skip Drop, which would send Bye
    }
}
