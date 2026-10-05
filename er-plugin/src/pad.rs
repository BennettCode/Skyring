//! Virtual pad input: ER reads its in-game controls from the virtual multi-device's input data, so setting a key's virtual slot
//! there looks to ER exactly like a held button, and ER's whole input pipeline (pad manipulator → action requests → its own
//! gating: stamina, recovery, tap-vs-hold) still runs. This is the game's input system, not OS keystrokes.
//!
//! Lookup is the same one `CSPad::poll_digital_input` does: UserInputKey → InputTypeGroup (mapped inputs) → CSKeyAssign's virtual
//! input index. The device's live input data is re-copied from the devices every PadStep, so writes must happen in a task group
//! after PadStep and before the characters read input (see docs/research/elden-ring-input.md, P3 step 2 probe).

use eldenring::cs::UserInputKey;
use eldenring::dluid::DLVirtualInputData;
use eldenring::fd4::{FD4PadManager, InputType};
use fromsoftware_shared::FromStatic;

/// Virtual input slots that feed `key` as a held button (AreKeysDown), with whether ER currently checks that mapped input.
///
/// # Safety
/// Main thread only (task callback).
pub unsafe fn digital_slots(key: UserInputKey) -> Vec<(i32, i32, bool)> {
    let Ok(pads) = (unsafe { FD4PadManager::instance() }) else { return Vec::new() };
    let Some(pad) = pads.get_in_game_pad() else { return Vec::new() };
    let groups = unsafe { pad.input_type_group.as_ref() };
    let Some(group) = groups.find(&key) else { return Vec::new() };
    let key_assign = unsafe { pad.key_assign.as_ref() };
    let checks = unsafe { pad.input_code_check.as_ref() };
    group
        .iter()
        .filter(|(_, ty)| *ty == InputType::AreKeysDown)
        .filter_map(|(mapped, _)| {
            let index = key_assign.get_virtual_input_index(mapped)?;
            let checked = checks.find(&mapped).is_some_and(|c| c.state_1 && !c.state_2);
            Some((mapped, index, checked))
        })
        .collect()
}

/// What ER's in-game pad reports for `key` right now (the same poll the game's controls use). `None` = no pad yet.
///
/// # Safety
/// Main thread only (task callback).
pub unsafe fn poll(key: UserInputKey) -> Option<bool> {
    let pads = unsafe { FD4PadManager::instance() }.ok()?;
    Some(pads.get_in_game_pad()?.poll_digital_input(key))
}

/// Sets every virtual slot of `key` to `down`. Returns how many slots were written (0 = not available yet).
///
/// # Safety
/// Main thread only (task callback).
pub unsafe fn set_digital(key: UserInputKey, down: bool) -> usize {
    let slots = unsafe { digital_slots(key) };
    let Ok(pads) = (unsafe { FD4PadManager::instance_mut() }) else { return 0 };
    let Some(pad) = pads.get_in_game_pad_mut() else { return 0 };
    let device = unsafe { pad.pad_device.as_mut().virtual_multi_device.as_mut() };
    for &(_, index, _) in &slots {
        device.set_virtual_digital_state(index as usize, down);
    }
    slots.len()
}

/// Virtual analog slots that feed `key` as a stick axis (IsStickMoving): the movement keys MoveForwards/Backwards/Left/Right.
///
/// # Safety
/// Main thread only (task callback).
pub unsafe fn analog_slots(key: UserInputKey) -> Vec<i32> {
    let Ok(pads) = (unsafe { FD4PadManager::instance() }) else { return Vec::new() };
    let Some(pad) = pads.get_in_game_pad() else { return Vec::new() };
    let groups = unsafe { pad.input_type_group.as_ref() };
    let Some(group) = groups.find(&key) else { return Vec::new() };
    let key_assign = unsafe { pad.key_assign.as_ref() };
    group
        .iter()
        .filter(|(_, ty)| *ty == InputType::IsStickMoving)
        .filter_map(|(mapped, _)| key_assign.get_virtual_input_index(mapped))
        .collect()
}

/// Sets every virtual analog slot of `key` to `value`. Returns how many slots were written (0 = not available yet).
///
/// # Safety
/// Main thread only (task callback).
pub unsafe fn set_analog(key: UserInputKey, value: f32) -> usize {
    let slots = unsafe { analog_slots(key) };
    let Ok(pads) = (unsafe { FD4PadManager::instance_mut() }) else { return 0 };
    let Some(pad) = pads.get_in_game_pad_mut() else { return 0 };
    let device = unsafe { pad.pad_device.as_mut().virtual_multi_device.as_mut() };
    for &index in &slots {
        device.set_virtual_analog_state(index as usize, value);
    }
    slots.len()
}

/// What ER's in-game pad reports for analog `key` right now. `None` = no pad yet.
///
/// # Safety
/// Main thread only (task callback).
pub unsafe fn poll_analog(key: UserInputKey) -> Option<f32> {
    let pads = unsafe { FD4PadManager::instance() }.ok()?;
    Some(pads.get_in_game_pad()?.poll_analog_input(key))
}

/// Holds the move stick: `x` right (-1..1), `y` forward (-1..1). Each direction key is one-sided: Forwards/Right take the positive
/// part, Backwards/Left the **negative** part (a positive value there is ignored: P3 step 5 self-test). Returns how many slots were written.
///
/// # Safety
/// Main thread only (task callback).
pub unsafe fn set_move(x: f32, y: f32) -> usize {
    use UserInputKey::*;
    unsafe {
        set_analog(MoveForwards, y.max(0.0))
            + set_analog(MoveBackwards, y.min(0.0))
            + set_analog(MoveRight, x.max(0.0))
            + set_analog(MoveLeft, x.min(0.0))
    }
}

/// Polled move keys as `F/B/L/R` values, for log lines.
///
/// # Safety
/// Main thread only (task callback).
pub unsafe fn move_polls() -> String {
    use UserInputKey::*;
    let p = |k| unsafe { poll_analog(k) }.unwrap_or(f32::NAN);
    format!("{:.2}/{:.2}/{:.2}/{:.2}", p(MoveForwards), p(MoveBackwards), p(MoveLeft), p(MoveRight))
}

/// Every entry of `key`'s input-type group: (mapped input, type, virtual index, checked). Unlike `digital_slots` this keeps
/// AreKeysUp / IsStickMoving entries and unmapped ones (P3 step 2 probe).
///
/// # Safety
/// Main thread only (task callback).
pub unsafe fn describe(key: UserInputKey) -> Vec<(i32, InputType, Option<i32>, bool)> {
    let Ok(pads) = (unsafe { FD4PadManager::instance() }) else { return Vec::new() };
    let Some(pad) = pads.get_in_game_pad() else { return Vec::new() };
    let groups = unsafe { pad.input_type_group.as_ref() };
    let Some(group) = groups.find(&key) else { return Vec::new() };
    let key_assign = unsafe { pad.key_assign.as_ref() };
    let checks = unsafe { pad.input_code_check.as_ref() };
    group
        .iter()
        .map(|(mapped, ty)| {
            let checked = checks.find(&mapped).is_some_and(|c| c.state_1 && !c.state_2);
            (mapped, ty, key_assign.get_virtual_input_index(mapped), checked)
        })
        .collect()
}

/// Every in-game `UserInputKey` (the enum has gaps), for `poll_mask`.
pub const ALL_KEYS: [UserInputKey; 22] = {
    use UserInputKey::*;
    [
        MouseMovementX, MouseMovementY, MovementControl, Attack, StrongAttack, Guard, Skill, EventAction, Backstep, BackstepTapped,
        Jump, UseItem, SwitchSpell, SwitchRightHandArmament, SwitchleftHandArmament, SwitchItem, ResetCamera, Crouch, SwitchSpell2,
        SwitchItem2, ResetCameraTapped, EventActionPouch,
    ]
};

/// Bit `k` set = `poll_digital_input(k)` is true (k = the key's number, all < 32).
///
/// # Safety
/// Main thread only (task callback).
pub unsafe fn poll_mask() -> u32 {
    let Ok(pads) = (unsafe { FD4PadManager::instance() }) else { return 0 };
    let Some(pad) = pads.get_in_game_pad() else { return 0 };
    ALL_KEYS.iter().filter(|&&k| pad.poll_digital_input(k)).fold(0, |m, &k| m | 1 << (k as i32))
}

/// Digital state of virtual input `index` in each layer, as a compact string:
/// `M<live><initial>` for the merged VirtualMultiDevice, then `d<i><live><initial>` per source device (pad / keyboard / mouse).
/// Shows which layer a real press appears in versus ours.
///
/// # Safety
/// Main thread only (task callback).
pub unsafe fn layers(index: usize) -> String {
    let Ok(pads) = (unsafe { FD4PadManager::instance() }) else { return String::new() };
    let Some(pad) = pads.get_in_game_pad() else { return String::new() };
    let multi = unsafe { pad.pad_device.as_ref().virtual_multi_device.as_ref() };
    let mut out = format!(
        "M{}{}",
        bit(&multi.virtual_input_data, index),
        bit(&multi.initial_virtual_input_data, index)
    );
    for (i, dev) in multi.user_input_devices.iter().enumerate() {
        let dev = unsafe { dev.as_ref() };
        out.push_str(&format!(
            " d{i}{}{}",
            bit(&dev.virtual_input_data, index),
            bit(&dev.initial_virtual_input_data, index)
        ));
    }
    out
}

/// '1' / '0', or '-' when `index` is outside this device's bitset (`DynamicBitset::get` would panic; devices differ in size).
fn bit(data: &DLVirtualInputData, index: usize) -> char {
    match data.dynamic_bitset.as_slice().get(index / 32) {
        Some(row) if (row >> (index & 31)) & 1 == 1 => '1',
        Some(_) => '0',
        None => '-',
    }
}

/// Every in-game action key a pad button can press (eldenring-rs `UserInputKey`, digital ones).
const ACTION_KEYS: [UserInputKey; 21] = [
    UserInputKey::MovementControl,
    UserInputKey::Attack,
    UserInputKey::StrongAttack,
    UserInputKey::Guard,
    UserInputKey::Skill,
    UserInputKey::EventAction,
    UserInputKey::Backstep,
    UserInputKey::BackstepTapped,
    UserInputKey::Jump,
    UserInputKey::UseItem,
    UserInputKey::SwitchSpell,
    UserInputKey::SwitchRightHandArmament,
    UserInputKey::SwitchleftHandArmament,
    UserInputKey::SwitchItem,
    UserInputKey::ResetCamera,
    UserInputKey::Crouch,
    UserInputKey::SwitchSpell2,
    UserInputKey::SwitchItem2,
    UserInputKey::ResetCameraTapped,
    UserInputKey::EventActionPouch,
    UserInputKey::Map,
];

/// Releases, for this frame, every action key and every analog value (sticks, camera) of the in-game pad. PadStep copies the physical
/// devices in each frame, so while Skyrim drives ER this runs first and only our own writes after it count: the DualSense (which ER
/// reads itself through libScePad, P4 step 8) can't attack, roll or turn ER's camera behind Skyrim's back. Digital keys go through
/// the key-assign lookup (`set_digital`), never a bulk write of the bitset: its `integer_count` may be bytes, not words (the bulk
/// clear first tried reported 3968 bits for 813 analog values). The analog vector has a real length. Returns (key slots, analog
/// values) released; None = no pad yet.
///
/// # Safety
/// Main thread only (task callback), after PadStep.
pub unsafe fn clear_virtual() -> Option<(usize, usize)> {
    let mut slots = 0;
    for key in ACTION_KEYS {
        slots += unsafe { set_digital(key, false) };
    }
    let pads = unsafe { FD4PadManager::instance_mut() }.ok()?;
    let pad = pads.get_in_game_pad_mut()?;
    let device = unsafe { pad.pad_device.as_mut().virtual_multi_device.as_mut() };
    let mut analog = 0;
    for v in device.virtual_input_data.analog_key_info.vector.iter_mut() {
        *v = 0.0;
        analog += 1;
    }
    Some((slots, analog))
}
