//! The link state machine, run by each side on its own thread every `LINK_TICK_MS`.
//! `skse/src/bridge/Link.cpp` is the C++ mirror: keep the two in step (same steps, same log wording).
//!
//! Per tick: attach (retry every 1 s) → refresh own heartbeat → read events → check the peer → maybe send a Heartbeat event.
//! - Peer alive = its state is Starting/Ready/Running and its heartbeat is at most `HEARTBEAT_TIMEOUT_MS` old.
//! - When a new peer appears (alive again, or its attach count changed), send it a Hello. Its Hello back = connected.
//! - Connected peer stops beating, faults, or says Bye → lost → this side goes idle (the fail-safe) and waits again.

use std::mem::offset_of;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, OnceLock};

use crate::proto::{
    Bye, ByeReason, HEARTBEAT_EVENT_INTERVAL_MS, HEARTBEAT_TIMEOUT_MS, Header, Heartbeat, Hello, MAGIC, MsgType, OFF_HEADER,
    OFF_RING_ER_TO_SKY, OFF_RING_ER_TO_SKY_DATA, OFF_RING_SKY_TO_ER, OFF_RING_SKY_TO_ER_DATA, REGION_NAME, REGION_SIZE,
    RING_CAPACITY, RingHeader, SideState, VERSION,
};
use crate::region::Region;
use crate::ring::{PopError, RingReader, RingWriter};
use crate::read_plain;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Side {
    Skyrim,
    EldenRing,
}

impl Side {
    pub fn name(self) -> &'static str {
        match self {
            Side::Skyrim => "Skyrim",
            Side::EldenRing => "ER",
        }
    }
    pub fn peer(self) -> Side {
        match self {
            Side::Skyrim => Side::EldenRing,
            Side::EldenRing => Side::Skyrim,
        }
    }
}

/// Byte offsets of one side's own header fields.
struct SideFields {
    pid: usize,
    state: usize,
    heartbeat: usize,
    attach: usize,
}

fn fields(side: Side) -> SideFields {
    match side {
        Side::Skyrim => SideFields {
            pid: OFF_HEADER + offset_of!(Header, sky_pid),
            state: OFF_HEADER + offset_of!(Header, sky_state),
            heartbeat: OFF_HEADER + offset_of!(Header, sky_heartbeat_ms),
            attach: OFF_HEADER + offset_of!(Header, sky_attach_count),
        },
        Side::EldenRing => SideFields {
            pid: OFF_HEADER + offset_of!(Header, er_pid),
            state: OFF_HEADER + offset_of!(Header, er_state),
            heartbeat: OFF_HEADER + offset_of!(Header, er_heartbeat_ms),
            attach: OFF_HEADER + offset_of!(Header, er_attach_count),
        },
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Level {
    Info,
    Warn,
    Error,
}

/// Receives the link's log lines (subsystem "link"); the caller adds timestamp and side tag.
pub type LogFn = Box<dyn FnMut(Level, &str) + Send>;

#[derive(Clone, Copy, Debug)]
pub struct Identity {
    pub plugin_version: [u16; 3],
    pub game_version: [u16; 4],
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PeerStatus {
    /// No handshake yet since this side attached.
    Waiting,
    Connected,
    /// Was connected, then timed out / faulted / said Bye. Back to waiting for a new peer.
    Lost,
}

struct Attached {
    tx: RingWriter,
    rx: RingReader,
    joined: bool,
    region: Arc<Region>,
}

/// What the game threads need from the link without taking its lock: the joined region (for the `slot`s) and
/// whether the peer is connected. Get it once with [`Link::shared`]; the `Arc<Region>` keeps the mapping alive.
#[derive(Default)]
pub struct LinkShared {
    region: OnceLock<Arc<Region>>,
    connected: AtomicBool,
}

impl LinkShared {
    /// Set once this side joined a region with a matching layout; `None` before that (or forever, on a mismatch).
    pub fn region(&self) -> Option<Arc<Region>> {
        self.region.get().cloned()
    }

    /// Handshake done and the peer is alive, as of the link's last tick (≤ `LINK_TICK_MS` old).
    pub fn connected(&self) -> bool {
        self.connected.load(Ordering::Acquire)
    }
}

pub struct Link {
    side: Side,
    region_name: String,
    identity: Identity,
    log: LogFn,
    shared: Arc<LinkShared>,
    attached: Option<Attached>,
    retry_at_ms: u64,
    last_problem: String,
    joined_at_ms: u64,
    state: SideState,
    status: PeerStatus,
    peer_alive: bool,
    peer_attach: u32,
    peer_seq: u32,
    peer_seen_at_ms: u64,
    hello_missing_warned: bool,
    next_beat_event_ms: u64,
}

fn version_string(v: &[u16]) -> String {
    v.iter().map(u16::to_string).collect::<Vec<_>>().join(".")
}

impl Link {
    pub fn new(side: Side, identity: Identity, log: LogFn) -> Self {
        Self::with_region_name(side, REGION_NAME, identity, log)
    }

    /// Tests and fake peers use their own region name so they never collide with a running game.
    pub fn with_region_name(side: Side, region_name: &str, identity: Identity, log: LogFn) -> Self {
        Self {
            side,
            region_name: region_name.to_string(),
            identity,
            log,
            shared: Arc::default(),
            attached: None,
            retry_at_ms: 0,
            last_problem: String::new(),
            joined_at_ms: 0,
            state: SideState::Absent,
            status: PeerStatus::Waiting,
            peer_alive: false,
            peer_attach: 0,
            peer_seq: 0,
            peer_seen_at_ms: 0,
            hello_missing_warned: false,
            next_beat_event_ms: 0,
        }
    }

    pub fn status(&self) -> PeerStatus {
        self.status
    }

    pub fn connected(&self) -> bool {
        self.status == PeerStatus::Connected
    }

    pub fn shared(&self) -> Arc<LinkShared> {
        self.shared.clone()
    }

    /// `now_ms` = `crate::now_ms()` in the games; tests pass their own clock. `frames` = game frames run so far.
    pub fn tick(&mut self, now_ms: u64, frames: u64) {
        if self.attached.is_none() && !self.try_open(now_ms) {
            return;
        }
        let mut a = self.attached.take().expect("attached");
        if a.joined || self.try_join(&mut a, now_ms) {
            a.region.u64_at(fields(self.side).heartbeat).store(now_ms, Ordering::Release);
            // Peer check first: a new peer is greeted before its own Hello is read, so a Hello never looks like a restart.
            self.check_peer(&mut a, now_ms);
            self.pump(&mut a, now_ms);
            self.send_heartbeat_event(&mut a, now_ms, frames);
        }
        self.attached = Some(a);
        self.shared.connected.store(self.connected(), Ordering::Release);
    }

    /// Clean exit: Bye to a connected peer, then state ShuttingDown. Safe to call more than once.
    pub fn shutdown(&mut self) {
        let Some(mut a) = self.attached.take() else { return };
        if a.joined && self.state != SideState::ShuttingDown {
            let sent = self.connected() && a.tx.send(&Bye { reason: ByeReason::Quit as u32, ..Default::default() }).is_ok();
            self.set_state(&a, SideState::ShuttingDown);
            self.shared.connected.store(false, Ordering::Release);
            self.say(Level::Info, &format!("shutting down (Bye sent: {})", if sent { "yes" } else { "no peer" }));
        }
        self.attached = Some(a);
    }

    fn say(&mut self, level: Level, message: &str) {
        (self.log)(level, message);
    }

    /// Logs a recurring problem only when it changes, so a retry loop doesn't flood the log.
    fn problem(&mut self, level: Level, message: String) {
        if message != self.last_problem {
            self.say(level, &message);
            self.last_problem = message;
        }
    }

    fn set_state(&mut self, a: &Attached, state: SideState) {
        self.state = state;
        a.region.u32_at(fields(self.side).state).store(state as u32, Ordering::Release);
    }

    fn try_open(&mut self, now_ms: u64) -> bool {
        if now_ms < self.retry_at_ms {
            return false;
        }
        let region = match Region::open(&self.region_name) {
            Ok(region) => region,
            Err(e) => {
                self.retry_at_ms = now_ms + 1000;
                self.problem(Level::Error, format!("{e}; retrying every 1 s"));
                return false;
            }
        };
        if region.created {
            // Zero-filled by Windows. Fill the shared fields, then publish magic last so an opener never sees half a header.
            let h = |field| OFF_HEADER + field;
            region.u32_at(h(offset_of!(Header, version))).store(VERSION, Ordering::Relaxed);
            region.u32_at(h(offset_of!(Header, header_size))).store(size_of::<Header>() as u32, Ordering::Relaxed);
            region.u32_at(h(offset_of!(Header, region_size))).store(REGION_SIZE as u32, Ordering::Relaxed);
            for ring in [OFF_RING_SKY_TO_ER, OFF_RING_ER_TO_SKY] {
                region.u32_at(ring + offset_of!(RingHeader, capacity)).store(RING_CAPACITY, Ordering::Relaxed);
            }
            region.u32_at(h(offset_of!(Header, magic))).store(MAGIC, Ordering::Release);
        }
        let (tx_ring, rx_ring) = match self.side {
            Side::Skyrim => ((OFF_RING_SKY_TO_ER, OFF_RING_SKY_TO_ER_DATA), (OFF_RING_ER_TO_SKY, OFF_RING_ER_TO_SKY_DATA)),
            Side::EldenRing => ((OFF_RING_ER_TO_SKY, OFF_RING_ER_TO_SKY_DATA), (OFF_RING_SKY_TO_ER, OFF_RING_SKY_TO_ER_DATA)),
        };
        // SAFETY: the region maps REGION_SIZE bytes and lives in the same Attached; this side is the only writer of
        // tx and the only reader of rx.
        let tx = unsafe { RingWriter::new(region.base(), tx_ring.0, tx_ring.1) };
        let rx = unsafe { RingReader::new(region.base(), rx_ring.0, rx_ring.1) };
        let how = if region.created { "created" } else { "opened existing" };
        self.say(Level::Info, &format!("region {} {how} ({REGION_SIZE:#x} bytes, protocol v{VERSION})", self.region_name));
        self.attached = Some(Attached { tx, rx, joined: false, region: Arc::new(region) });
        true
    }

    /// Validates the header written by the creator, then claims this side's fields.
    fn try_join(&mut self, a: &mut Attached, now_ms: u64) -> bool {
        let r = &a.region;
        let h = |field| r.u32_at(OFF_HEADER + field).load(Ordering::Acquire);
        let magic = h(offset_of!(Header, magic));
        if magic == 0 {
            self.problem(Level::Info, "waiting for the region creator to finish initialising".into());
            return false;
        }
        let (version, header_size, region_size) =
            (h(offset_of!(Header, version)), h(offset_of!(Header, header_size)), h(offset_of!(Header, region_size)));
        if magic != MAGIC || version != VERSION || header_size as usize != size_of::<Header>() || region_size as usize != REGION_SIZE {
            self.state = SideState::Faulted;
            self.problem(
                Level::Error,
                format!(
                    "PROTOCOL MISMATCH: region has magic={magic:#010x} v{version} header={header_size} size={region_size:#x}, \
                     ours magic={MAGIC:#010x} v{VERSION} header={} size={REGION_SIZE:#x}. Rebuild both plugins. Staying idle.",
                    size_of::<Header>()
                ),
            );
            return false;
        }

        let skipped = a.rx.skip_all();
        a.tx.reset_seq();
        let my = fields(self.side);
        r.u32_at(my.pid).store(std::process::id(), Ordering::Relaxed);
        r.u64_at(my.heartbeat).store(now_ms, Ordering::Relaxed);
        let attach = r.u32_at(my.attach).fetch_add(1, Ordering::Relaxed) + 1;
        a.joined = true;
        // Only a validated layout is handed to the game threads, so a mismatch never gets slot writes.
        let _ = self.shared.region.set(a.region.clone());
        self.set_state(a, SideState::Ready);
        self.joined_at_ms = now_ms;
        self.last_problem.clear();
        self.say(
            Level::Info,
            &format!(
                "joined as {} pid={} attach#{attach} (skipped {skipped} stale bytes); waiting for {}",
                self.side.name(),
                std::process::id(),
                self.side.peer().name()
            ),
        );
        true
    }

    fn pump(&mut self, a: &mut Attached, now_ms: u64) {
        loop {
            let (msg_type, seq, payload) = match a.rx.pop() {
                None => break,
                Some(Err(PopError::Corrupt { skipped })) => {
                    self.say(Level::Error, &format!("ring corrupt: skipped {skipped} bytes"));
                    continue;
                }
                Some(Ok(m)) => (m.msg_type, m.seq, m.payload.to_vec()),
            };
            // Seq restarts at 1 whenever the peer (re)attaches.
            if seq != 1 && self.peer_seq != 0 && seq != self.peer_seq.wrapping_add(1) {
                self.say(Level::Warn, &format!("seq gap: expected {} got {seq}", self.peer_seq.wrapping_add(1)));
            }
            self.peer_seq = seq;
            self.handle(a, now_ms, msg_type, seq, &payload);
        }
    }

    fn handle(&mut self, a: &mut Attached, now_ms: u64, msg_type: u16, seq: u32, payload: &[u8]) {
        let peer = self.side.peer().name();
        let short = |me: &mut Self, name: &str| me.say(Level::Error, &format!("{name} seq={seq} too short ({} bytes)", payload.len()));
        match MsgType::from_raw(msg_type) {
            Some(MsgType::Hello) => {
                let Some(hello) = read_plain::<Hello>(payload) else { return short(self, "Hello") };
                if hello.protocol_version != VERSION {
                    self.say(
                        Level::Error,
                        &format!("PROTOCOL MISMATCH: {peer} speaks v{}, we speak v{VERSION}. Staying idle.", hello.protocol_version),
                    );
                    let _ = a.tx.send(&Bye { reason: ByeReason::Faulted as u32, ..Default::default() });
                    self.status = PeerStatus::Lost;
                    self.set_state(a, SideState::Faulted);
                    return;
                }
                let detail = format!(
                    "{peer} pid={} plugin v{} game {} protocol v{}",
                    hello.pid,
                    version_string(&hello.plugin_version),
                    version_string(&hello.game_version),
                    hello.protocol_version
                );
                if self.status == PeerStatus::Connected {
                    // The peer lost us (e.g. we stalled past the timeout) and is reconnecting: greet it back.
                    let sent = self.send_hello(a);
                    self.say(Level::Info, &format!("Hello again from {detail} seq={seq}; replied {sent:?}"));
                } else {
                    self.status = PeerStatus::Connected;
                    self.set_state(a, SideState::Running);
                    self.next_beat_event_ms = now_ms + HEARTBEAT_EVENT_INTERVAL_MS;
                    self.say(Level::Info, &format!("CONNECTED: handshake ok with {detail} seq={seq}"));
                }
            }
            Some(MsgType::Bye) => {
                let Some(bye) = read_plain::<Bye>(payload) else { return short(self, "Bye") };
                let reason = ByeReason::from_raw(bye.reason).map_or_else(|| format!("unknown({})", bye.reason), |r| format!("{r:?}"));
                if self.status == PeerStatus::Connected {
                    self.say(Level::Warn, &format!("LOST: {peer} said Bye (reason={reason}) seq={seq}; going idle"));
                    self.status = PeerStatus::Lost;
                    self.set_state(a, SideState::Ready);
                } else {
                    self.say(Level::Info, &format!("{peer} said Bye (reason={reason}) seq={seq}"));
                }
            }
            Some(MsgType::Heartbeat) => {
                let Some(beat) = read_plain::<Heartbeat>(payload) else { return short(self, "Heartbeat") };
                let peer_hb = a.region.u64_at(fields(self.side.peer()).heartbeat).load(Ordering::Acquire);
                self.say(
                    Level::Info,
                    &format!(
                        "{peer} heartbeat seq={seq} uptime={}ms frames={} beat_age={}ms dropped_by_us={}",
                        beat.uptime_ms,
                        beat.frames,
                        now_ms.saturating_sub(peer_hb),
                        a.tx.dropped()
                    ),
                );
            }
            None => self.say(Level::Warn, &format!("unknown message type {msg_type} seq={seq} ({} bytes), ignored", payload.len())),
        }
    }

    fn check_peer(&mut self, a: &mut Attached, now_ms: u64) {
        let peer = self.side.peer().name();
        let pf = fields(self.side.peer());
        let raw_state = a.region.u32_at(pf.state).load(Ordering::Acquire);
        let state = SideState::from_raw(raw_state);
        let beat = a.region.u64_at(pf.heartbeat).load(Ordering::Acquire);
        let pid = a.region.u32_at(pf.pid).load(Ordering::Relaxed);
        let attach = a.region.u32_at(pf.attach).load(Ordering::Relaxed);
        let age = now_ms.saturating_sub(beat);
        let alive = matches!(state, Some(SideState::Starting | SideState::Ready | SideState::Running))
            && beat != 0
            && age <= HEARTBEAT_TIMEOUT_MS;

        if alive && (!self.peer_alive || attach != self.peer_attach) {
            if self.status == PeerStatus::Connected {
                self.say(Level::Warn, &format!("LOST: {peer} restarted (attach#{} → #{attach}); reconnecting", self.peer_attach));
                self.status = PeerStatus::Lost;
            }
            if self.state != SideState::Ready {
                self.set_state(a, SideState::Ready); // also clears Faulted: a new peer gets a fresh try
            }
            self.peer_attach = attach;
            self.peer_seen_at_ms = now_ms;
            self.hello_missing_warned = false;
            let sent = self.send_hello(a);
            self.say(Level::Info, &format!("{peer} detected pid={pid} attach#{attach} state={state:?}; Hello sent {sent:?}"));
        } else if !alive && self.peer_alive && self.status == PeerStatus::Connected {
            let reason = match state {
                Some(SideState::ShuttingDown) => "it is shutting down (clean exit, Bye follows)".to_string(),
                Some(SideState::Faulted) => "it faulted".to_string(),
                _ => format!("heartbeat timeout (last beat {age} ms ago, limit {HEARTBEAT_TIMEOUT_MS} ms)"),
            };
            self.say(Level::Warn, &format!("LOST: {peer} pid={pid}: {reason}; going idle (fail-safe)"));
            self.status = PeerStatus::Lost;
            self.set_state(a, SideState::Ready);
        } else if alive && self.status != PeerStatus::Connected && !self.hello_missing_warned && now_ms - self.peer_seen_at_ms > 5000 {
            self.hello_missing_warned = true;
            self.say(Level::Warn, &format!("{peer} has been alive for 5 s but sent no Hello (state={state:?})"));
        }
        self.peer_alive = alive;
    }

    fn send_hello(&mut self, a: &mut Attached) -> Result<u32, crate::ring::PushError> {
        a.tx.send(&Hello {
            protocol_version: VERSION,
            pid: std::process::id(),
            plugin_version: self.identity.plugin_version,
            game_version: self.identity.game_version,
            ..Default::default()
        })
    }

    fn send_heartbeat_event(&mut self, a: &mut Attached, now_ms: u64, frames: u64) {
        if self.status != PeerStatus::Connected || now_ms < self.next_beat_event_ms {
            return;
        }
        self.next_beat_event_ms = now_ms + HEARTBEAT_EVENT_INTERVAL_MS;
        let beat = Heartbeat { uptime_ms: now_ms - self.joined_at_ms, frames };
        if let Err(e) = a.tx.send(&beat) {
            self.say(Level::Warn, &format!("Heartbeat event not sent: {e:?} (dropped so far: {})", a.tx.dropped()));
        }
    }
}

impl Drop for Link {
    fn drop(&mut self) {
        self.shutdown();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, Mutex};

    type Lines = Arc<Mutex<Vec<String>>>;

    fn link(side: Side, region: &str) -> (Link, Lines) {
        let lines: Lines = Arc::default();
        let sink = lines.clone();
        let ident = Identity { plugin_version: [0, 1, 0], game_version: [1, 7, 104, 0] };
        let log = Box::new(move |level: Level, msg: &str| sink.lock().unwrap().push(format!("{level:?} {msg}")));
        (Link::with_region_name(side, region, ident, log), lines)
    }

    fn has(lines: &Lines, needle: &str) -> bool {
        lines.lock().unwrap().iter().any(|l| l.contains(needle))
    }

    fn name(tag: &str) -> String {
        format!("Local\\SkyrimXER_test_link_{tag}_{}", std::process::id())
    }

    fn tick_both(a: &mut Link, b: &mut Link, now: u64) {
        a.tick(now, 0);
        b.tick(now, 0);
    }

    #[test]
    fn handshake_heartbeat_and_timeout() {
        let region = name("timeout");
        let (mut sky, sky_log) = link(Side::Skyrim, &region);
        let (mut er, er_log) = link(Side::EldenRing, &region);
        let shared = sky.shared();
        assert!(shared.region().is_none() && !shared.connected());
        for t in [1000, 1050, 1100] {
            tick_both(&mut sky, &mut er, t);
        }
        assert!(sky.connected() && er.connected(), "sky: {sky_log:?}\ner: {er_log:?}");
        assert!(shared.region().is_some() && shared.connected() && er.shared().connected());
        assert!(has(&sky_log, "CONNECTED: handshake ok with ER"));
        assert!(!has(&sky_log, "Hello again") && !has(&er_log, "Hello again"), "exactly one Hello each way");

        for t in (1150..=6250).step_by(50) {
            tick_both(&mut sky, &mut er, t);
        }
        assert!(sky.connected() && er.connected(), "no timeout while both tick: {sky_log:?}");
        assert!(has(&sky_log, "ER heartbeat seq=") && has(&er_log, "Skyrim heartbeat seq="));

        // ER stops ticking (frozen or killed): Skyrim must notice only after the timeout.
        sky.tick(6250 + HEARTBEAT_TIMEOUT_MS, 0);
        assert!(sky.connected());
        sky.tick(6251 + HEARTBEAT_TIMEOUT_MS, 0);
        assert_eq!(sky.status(), PeerStatus::Lost);
        assert!(!shared.connected(), "game threads see the loss");
        assert!(has(&sky_log, "LOST: ER") && has(&sky_log, "heartbeat timeout"));

        // ER comes back: reconnects without restarting Skyrim.
        tick_both(&mut sky, &mut er, 9000);
        for t in [9050, 9100] {
            tick_both(&mut sky, &mut er, t);
        }
        assert!(sky.connected() && er.connected(), "sky: {sky_log:?}
er: {er_log:?}");
        assert!(!has(&sky_log, "seq gap") && !has(&er_log, "seq gap"));
    }

    #[test]
    fn bye_and_restart() {
        let region = name("bye");
        let (mut sky, sky_log) = link(Side::Skyrim, &region);
        {
            let (mut er, _) = link(Side::EldenRing, &region);
            for t in [1000, 1050, 1100] {
                tick_both(&mut sky, &mut er, t);
            }
            assert!(sky.connected());
            // Dropping the link sends Bye and closes ER's handle; Skyrim's handle keeps the region alive.
        }
        sky.tick(1150, 0);
        assert_eq!(sky.status(), PeerStatus::Lost);
        assert!(has(&sky_log, "said Bye (reason=Quit)"));

        // A new ER process opens the existing region and skips nothing stale it shouldn't act on.
        let (mut er2, er2_log) = link(Side::EldenRing, &region);
        for t in [1200, 1250, 1300] {
            tick_both(&mut sky, &mut er2, t);
        }
        assert!(sky.connected() && er2.connected(), "sky: {sky_log:?}\ner2: {er2_log:?}");
        assert!(has(&er2_log, "opened existing") && has(&er2_log, "attach#2"));
    }

    #[test]
    fn mismatched_header_faults_without_writing() {
        let region_name = name("mismatch");
        let region = Region::open(&region_name).unwrap();
        region.u32_at(OFF_HEADER + offset_of!(Header, version)).store(VERSION + 1, Ordering::Relaxed);
        region.u32_at(OFF_HEADER + offset_of!(Header, magic)).store(MAGIC, Ordering::Release);
        let (mut er, er_log) = link(Side::EldenRing, &region_name);
        er.tick(1000, 0);
        er.tick(1050, 0);
        assert!(has(&er_log, "PROTOCOL MISMATCH"));
        assert_eq!(er_log.lock().unwrap().iter().filter(|l| l.contains("MISMATCH")).count(), 1, "logged once");
        assert_eq!(region.u32_at(fields(Side::EldenRing).pid).load(Ordering::Relaxed), 0, "no writes into a foreign layout");
        assert!(er.shared().region().is_none(), "a foreign layout is never handed to the game threads");
    }
}
