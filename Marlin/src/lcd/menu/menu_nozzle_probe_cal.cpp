/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2020 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

//
// Nozzle-Probe Calibration (BlueR custom)
//
// Measures the nozzle-to-probe Z offset using the inductive probe under the
// SAME trigger criterion that real probing (G29) uses, with a tactile manual
// bed-zero step:
//
//   1. Home, position the nozzle over the probe XY reference point.
//   2. Disable the Z stepper. The user hand-lowers the nozzle until it just
//      touches the bed (gap = 0).
//   3. The user presses "Nozzle touching bed": this sets Z = 0 (the physical
//      reference) and re-enables the Z stepper.
//   4. The procedure auto-raises until the probe RELEASES (it is triggered at
//      Z = 0), then probes back down N times with probe.probe_at_point() —
//      identical to real probing — and reports min / max / avg of the trigger
//      height. With probe.offset.z temporarily 0, that height IS the
//      nozzle-to-probe offset.
//   5. The user reviews the result and chooses Save (write probe.offset.z +
//      persist) or Discard.
//
// Gated behind BLUER_NOZZLE_PROBE_CAL.
//

#include "../../inc/MarlinConfigPre.h"

#if ENABLED(BLUER_NOZZLE_PROBE_CAL)

#include "menu_item.h"
#include "menu_addon.h"

#include "../../module/motion.h"
#include "../../module/planner.h"
#include "../../module/probe.h"
#include "../../module/stepper.h"
#include "../../gcode/queue.h"
#include "../../libs/numtostr.h"
#include "../../core/utility.h"   // safe_delay

#if HAS_LEVELING
  #include "../../feature/bedlevel/bedlevel.h"
#endif

// How many downward probes to average for the reported offset.
#ifndef NOZZLE_PROBE_CAL_SAMPLES
  #define NOZZLE_PROBE_CAL_SAMPLES 4
#endif

// Step and feedrate for the "auto-raise until probe releases" search.
#ifndef NOZZLE_PROBE_CAL_RAISE_STEP
  #define NOZZLE_PROBE_CAL_RAISE_STEP 0.2f      // (mm) per release-search step
#endif
#ifndef NOZZLE_PROBE_CAL_RAISE_MARGIN
  #define NOZZLE_PROBE_CAL_RAISE_MARGIN 3.0f    // (mm) extra clearance above release height
#endif
#ifndef NOZZLE_PROBE_CAL_RAISE_LIMIT
  #define NOZZLE_PROBE_CAL_RAISE_LIMIT 30.0f    // (mm) give up if no release within this travel
#endif

// Live state captured across the screen flow.
static float npc_offset_backup;   // probe.offset.z saved for Cancel
static float npc_result;          // last computed offset (avg)
static float npc_min, npc_max;    // spread of the samples
static bool  npc_leveling_was_active;
static bool  npc_valid;           // a measurement was completed successfully

// Forward declarations
void npc_result_menu();
void npc_run_measurement();

// Restore the saved offset / leveling state and leave the wizard.
inline void npc_restore_and_back(const_float_t z_offset) {
  probe.offset.z = z_offset;
  SET_SOFT_ENDSTOP_LOOSE(false);
  TERN_(HAS_LEVELING, set_bed_leveling_enabled(npc_leveling_was_active));
  ui.goto_previous_screen_no_defer();
}

//
// Final screen: show the measured offset and Save / Discard.
//
void npc_result_menu() {
  START_MENU();
  STATIC_ITEM_F(F("Nozzle-Probe offset"), SS_CENTER|SS_INVERT);

  if (npc_valid) {
    // Note: the value (vstr) is concatenated directly after the label with no
    // separator, so keep a trailing space in each label or a negative value
    // reads as joined to the text ("min-3.52").
    STATIC_ITEM_F(F("Z offset: "), SS_CENTER, ftostr42_52(npc_result));
    STATIC_ITEM_F(F("min: "),      SS_CENTER, ftostr42_52(npc_min));
    STATIC_ITEM_F(F("max: "),      SS_CENTER, ftostr42_52(npc_max));

    ACTION_ITEM_F(F("Save offset"), []{
      probe.offset.z = npc_result;          // Apply the measured offset
      npc_offset_backup = npc_result;       // Don't let Cancel undo a saved value
      queue.inject(F("M500"));              // Persist to EEPROM
      npc_restore_and_back(npc_result);
      do_z_clearance(Z_POST_CLEARANCE);     // Raise as if homed
    });
    ACTION_ITEM_F(F("Discard"), []{
      npc_restore_and_back(npc_offset_backup);
      do_z_clearance(Z_POST_CLEARANCE);
    });
  }
  else {
    STATIC_ITEM_F(F("Measurement failed"), SS_LEFT);
    ACTION_ITEM_F(F("Back"), []{
      npc_restore_and_back(npc_offset_backup);
      do_z_clearance(Z_POST_CLEARANCE);
    });
  }

  END_MENU();
}

//
// Do the actual measurement: raise until the probe releases, then probe down
// N times in place and average. Runs once, then hands off to the result menu.
//
void npc_run_measurement() {
  if (ui.should_draw()) MenuItem_static::draw(1, F("Probing..."));
  if (ui.wait_for_move) return;

  ui.wait_for_move = true;

  // Allow free Z movement: we set Z = 0 at the bed by hand, but soft endstops
  // (MIN_SOFTWARE_ENDSTOP_Z) would otherwise block probing through that zero.
  SET_SOFT_ENDSTOP_LOOSE(true);

  // Measure with a clean offset so probe_at_point returns the raw trigger
  // height (= the nozzle-to-probe distance) in our hand-set Z=0 frame.
  probe.offset.z = 0;

  // The probe is triggered at Z = 0 (bed is right there). Raise slowly until it
  // releases so the downward probe has a clean approach, then add a margin.
  bool released = false;
  for (float z = 0; z <= NOZZLE_PROBE_CAL_RAISE_LIMIT; z += NOZZLE_PROBE_CAL_RAISE_STEP) {
    do_blocking_move_to_z(z, MMM_TO_MMS(Z_PROBE_FEEDRATE_SLOW));
    if (!PROBE_TRIGGERED()) { released = true; break; }
  }

  npc_valid = false;
  if (released) {
    const float release_z = current_position.z;
    do_blocking_move_to_z(release_z + NOZZLE_PROBE_CAL_RAISE_MARGIN, MMM_TO_MMS(Z_PROBE_FEEDRATE_FAST));

    float sum = 0;
    npc_min =  99999.0f;
    npc_max = -99999.0f;
    uint8_t good = 0;
    for (uint8_t i = 0; i < NOZZLE_PROBE_CAL_SAMPLES; ++i) {
      // Probe straight down in place: nozzle-relative target == current XY, so
      // no XY drift (avoids bed-tilt error between nozzle- and probe-XY).
      const float trigger_z = probe.probe_at_point(
        current_position.x, current_position.y,
        PROBE_PT_RAISE,   // raise to clearance between samples
        0,                // verbose
        false,            // nozzle-relative: stay at this XY
        false             // no sanity check (we deliberately probe from a hand-set zero)
      );
      if (!isnan(trigger_z)) {
        // trigger_z is the nozzle height (above our hand-set Z=0 bed) at which the
        // probe fires. probe.offset.z follows Marlin's convention: a probe that
        // triggers with the nozzle above the bed has a NEGATIVE Z offset, applied
        // during homing as `current_position.z -= probe.offset.z`. So store -trigger_z.
        const float z = -trigger_z;
        sum += z;
        NOMORE(npc_min, z);
        NOLESS(npc_max, z);
        good++;
      }
    }
    if (good) {
      npc_result = sum / good;
      npc_valid = true;
    }
  }

  ui.wait_for_move = false;

  ui.goto_screen(npc_result_menu);
  ui.defer_status_screen();
}

//
// Manual-zero screen: nozzle is being hand-lowered with the Z motor disabled.
// When the user confirms the nozzle is touching the bed, set Z = 0, re-enable
// the Z stepper, and start the measurement.
//
void npc_manual_zero_menu() {
  START_MENU();
  STATIC_ITEM_F(F("Set nozzle to bed"), SS_CENTER|SS_INVERT);
  STATIC_ITEM_F(F("- Put paper on bed"), SS_LEFT);
  STATIC_ITEM_F(F("- Lower nozzle by hand"), SS_LEFT);
  STATIC_ITEM_F(F("- Slide paper, stop on drag"), SS_LEFT);

  ACTION_ITEM_F(F("Nozzle is touching bed"), []{
    // Re-engage the steppers. In TMC2209 STANDALONE mode the driver cannot read
    // back the rotor phase that drifted while we hand-turned the disabled
    // leadscrew, so enabling the coils snaps the rotor to the nearest microstep
    // (the small one-time "jerk"). Let that snap happen and settle BEFORE we
    // capture Z=0, so the reference isn't thrown off by it.
    stepper.enable_all_steppers();
    safe_delay(150);                    // Let the phase-snap settle at rest
    planner.synchronize();
    current_position.z = 0;             // This (settled) physical position is Z = 0
    sync_plan_position();
    ui.goto_screen(npc_run_measurement);
    ui.defer_status_screen();
  });

  ACTION_ITEM_F(F("Cancel"), []{
    stepper.enable_all_steppers();
    npc_restore_and_back(npc_offset_backup);
    set_axis_never_homed(Z_AXIS);       // Z was moved by hand; force a re-home
    queue.inject(F("G28Z"));
  });

  END_MENU();
}

//
// Wait for homing to finish, then move the nozzle to the probe XY reference,
// disable the Z stepper, and show the manual-zero screen.
//
void npc_prepare_after_home() {
  if (ui.should_draw()) MenuItem_static::draw(1, F("Positioning..."));
  if (ui.wait_for_move) return;

  ui.wait_for_move = true;
  // Move the nozzle to the XY reference point so the later in-place probe
  // samples the bed near where we zero the nozzle.
  #ifdef PROBE_OFFSET_WIZARD_XY_POS
    constexpr xy_pos_t ref = PROBE_OFFSET_WIZARD_XY_POS;
  #else
    constexpr xy_pos_t ref = XY_CENTER;
  #endif
  current_position.x = ref.x;
  current_position.y = ref.y;
  line_to_current_position(XY_PROBE_FEEDRATE_MM_S);
  ui.synchronize(F("Positioning..."));
  ui.wait_for_move = false;

  // Disable Z so the user can back-drive the leadscrew by hand.
  stepper.disable_axis(Z_AXIS);

  ui.goto_screen(npc_manual_zero_menu);
  ui.defer_status_screen();
}

//
// Entry point (wired into the Motion menu).
//
void goto_nozzle_probe_cal() {
  ui.defer_status_screen();

  // Save state for Cancel.
  npc_offset_backup = probe.offset.z;
  #if HAS_LEVELING
    npc_leveling_was_active = planner.leveling_active;
    set_bed_leveling_enabled(false);
  #endif

  set_all_unhomed();
  queue.inject_P(G28_STR);

  ui.goto_screen([]{
    _lcd_draw_homing();
    if (all_axes_homed()) {
      ui.goto_screen(npc_prepare_after_home);
      ui.defer_status_screen();
    }
  });
}

#endif // BLUER_NOZZLE_PROBE_CAL
