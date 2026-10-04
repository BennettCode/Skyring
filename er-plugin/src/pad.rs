//! Virtual pad input: ER reads its in-game controls from the virtual multi-device's input data, so setting a key's virtual slot
//! there looks to ER exactly like a held button, and ER's whole input pipeline (pad manipulator → action requests → its own
//! gating: stamina, recovery, tap-vs-hold) still runs. This is the game's input system, not OS keystrokes.
//!
//! Lookup is the same one `CSPad::poll_digital_input` does: UserInputKey → InputTypeGroup (mapped inputs) → CSKeyAssign's virtual
//! input index. The device's live input data is re-copied from the devices every PadStep, so writes must happen in a task group
//! after PadStep and before the characters read input (see docs/research/elden-ring-input.md, P3 step 2 probe).

use eldenring::cs::UserInputKey;
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
