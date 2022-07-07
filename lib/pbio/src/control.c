// SPDX-License-Identifier: MIT
// Copyright (c) 2018-2022 The Pybricks Authors

#include <stdlib.h>

#include <pbdrv/clock.h>

#include <pbio/config.h>
#include <pbio/control.h>
#include <pbio/math.h>
#include <pbio/trajectory.h>
#include <pbio/integrator.h>

/**
 * Gets the wall time in control unit time ticks (1e-4 seconds).
 *
 * @return                    Wall time in control ticks.
 */
uint32_t pbio_control_get_time_ticks(void) {
    return pbdrv_clock_get_100us();
}

/**
 * Checks if a time sample is equal to or newer than a given base time stamp.
 *
 * @param [in] sample         Sample time.
 * @param [in] base           Base time to compare to.
 * @return                    True if sample time is equal to or newer than base time, else false.
 */
bool pbio_control_time_is_later(uint32_t sample, uint32_t base) {
    return sample - base < UINT32_MAX / 2;
}

static bool pbio_control_check_completion(pbio_control_t *ctl, uint32_t time, pbio_control_state_t *state, pbio_trajectory_reference_t *end) {

    // If no control is active, then all targets are complete.
    if (!pbio_control_is_active(ctl)) {
        return true;
    }

    // Check if we are passed the nominal maneuver time.
    bool time_completed = pbio_control_time_is_later(time, end->time);

    // Timed maneuvers are done when the full duration has passed.
    if (pbio_control_type_is_time(ctl)) {
        return time_completed;
    }

    // What remains now is to deal with angle-based maneuvers. As with time
    // based trajectories, we want at least the duration to pass, so return
    // false if time has not yet completed.
    if (!time_completed) {
        return false;
    }

    // For a nonzero final speed, we're done once we're at or past
    // the target, no matter the tolerances. Equivalently, we're done
    // once the sign of the angle error differs from the speed sign.
    int32_t position_remaining = pbio_angle_diff_mdeg(&end->position, &state->position);
    if (end->speed != 0) {
        return pbio_math_sign(position_remaining) != pbio_math_sign(end->speed);
    }

    // For zero final speed, we need to at least stand still, so return false
    // when we're still moving faster than the tolerance.
    if (abs(state->speed_estimate) > ctl->settings.speed_tolerance) {
        return false;
    }

    // Once we stand still, we're complete if the distance to the
    // target is equal to or less than the allowed tolerance.
    return abs(position_remaining) <= ctl->settings.position_tolerance;
}

/**
 * Multiplies an angle (mdeg), speed (mdeg/s) or angle integral (mdeg s)
 * by a control gain and scales to uNm.
 *
 * @param [in] value         Input value (mdeg, mdeg/s, or mdeg s).
 * @param [in] gain          Gain in uNm/deg, uNm/(deg/s), or uNm/(deg s).
 * @return                   Torque in uNm.
 */
static int32_t pbio_control_mul_by_gain(int32_t value, int32_t gain) {
    return gain * value / 1000;
}

/**
 * Divides a torque (uNm) by a control gain to get an angle (mdeg),
 * speed (mdeg/s) or angle integral (mdeg s), and accounts for scaling.
 *
 * Only positive gains are allowed. If the gain is zero or less, this returns
 * zero.
 *
 * @param [in] value         Input value (uNm).
 * @param [in] gain          Positive gain in uNm/deg, uNm/(deg/s), or uNm/(deg s).
 * @return                   Result in mdeg, mdeg/s, or mdeg s.
 */
static int32_t pbio_control_div_by_gain(int32_t value, int32_t gain) {
    if (gain < 1) {
        return 0;
    }
    return value * 1000 / gain;
}

/**
 * Updates the PID controller state to calculate the next actuation step.
 *
 * @param [in]  ctl             The control instance.
 * @param [in]  time_now        The wall time (ticks).
 * @param [in]  state           The current state of the system being controlled (control units).
 * @param [out] ref             Computed reference point on the trajectory (control units).
 * @param [out] actuation       Required actuation type.
 * @param [out] control         The control output, which is the actuation payload (control units).
 */
void pbio_control_update(pbio_control_t *ctl, uint32_t time_now, pbio_control_state_t *state, pbio_trajectory_reference_t *ref, pbio_dcmotor_actuation_t *actuation, int32_t *control) {

    // Get reference signals at the reference time point in the trajectory.
    // This compensates for any time we may have spent pausing when the motor was stalled.
    pbio_trajectory_get_reference(&ctl->trajectory, pbio_control_get_ref_time(ctl, time_now), ref);

    // Get reference point we want to be at in the end, to check for completion.
    pbio_trajectory_reference_t ref_end;
    pbio_trajectory_get_endpoint(&ctl->trajectory, &ref_end);

    // Get position and speed error
    int32_t position_error = pbio_angle_diff_mdeg(&ref->position, &state->position);
    int32_t speed_error = ref->speed - state->speed_estimate;

    // Specify in which region integral control should be active. This is
    // at least the error that would still lead to maximum  proportional
    // control, with a factor of 2 so we begin integrating a bit sooner.
    int32_t integral_range = pbio_control_div_by_gain(ctl->settings.actuation_max, ctl->settings.pid_kp) * 2;

    // Get integral value that would lead to maximum actuation.
    int32_t integral_max = pbio_control_div_by_gain(ctl->settings.actuation_max, ctl->settings.pid_ki);

    // Calculate integral control errors, depending on control type.
    int32_t integral_error;
    int32_t position_error_used;
    if (pbio_control_type_is_position(ctl)) {

        // Update count integral error and get current error state
        int32_t position_remaining = pbio_angle_diff_mdeg(&ref_end.position, &ref->position);
        integral_error = pbio_position_integrator_update(&ctl->position_integrator, time_now, position_error, position_remaining, integral_range, integral_max, ctl->settings.integral_change_max);

        // For position control, the proportional term is the real position error.
        position_error_used = position_error;
    } else {
        // For time/speed based commands, the main error is speed. It integrates into a quantity with unit of position.
        // There is no count integral control, because we do not need a second order integrator for speed control.
        position_error_used = pbio_speed_integrator_get_error(&ctl->speed_integrator, position_error);
        integral_error = 0;
    }

    // Corresponding PID control signal
    int32_t torque_proportional = pbio_control_mul_by_gain(position_error_used, ctl->settings.pid_kp);
    int32_t torque_derivative = pbio_control_mul_by_gain(speed_error, ctl->settings.pid_kd);
    int32_t torque_integral = pbio_control_mul_by_gain(integral_error, ctl->settings.pid_ki);

    // Total torque signal, capped by the actuation limit
    int32_t torque = pbio_math_clamp(torque_proportional + torque_integral + torque_derivative, ctl->settings.actuation_max);

    // This completes the computation of the control signal.
    // The next steps take care of handling windup, or triggering a stop if we are on target.

    // We want to stop building up further errors if we are at the proportional torque limit. So, we pause the trajectory
    // if we get at this limit. We wait a little longer though, to make sure it does not fall back to below the limit
    // within one sample, which we can predict using the current rate times the loop time, with a factor two tolerance.
    int32_t windup_margin = pbio_integrator_times_loop_time(abs(state->speed_estimate)) * 2;
    int32_t max_windup_torque = ctl->settings.actuation_max + pbio_control_mul_by_gain(windup_margin, ctl->settings.pid_kp);

    // Position anti-windup: pause trajectory or integration if falling behind despite using maximum torque
    bool pause_integration =
        // Pause if proportional torque is beyond maximum windup torque:
        abs(torque_proportional) >= max_windup_torque &&
        // But not if we're trying to run in the other direction (else we can get unstuck by just reversing).
        pbio_math_sign(torque_proportional) != -pbio_math_sign(speed_error) &&
        // But not if we should be accelerating in the other direction (else we can get unstuck by just reversing).
        pbio_math_sign(torque_proportional) != -pbio_math_sign(ref->acceleration);

    // Position anti-windup in case of angle control (accumulated position error may not get too high)
    if (pbio_control_type_is_position(ctl)) {
        if (pause_integration) {
            // We are at the torque limit and we should prevent further position error integration.
            pbio_position_integrator_pause(&ctl->position_integrator, time_now);
        } else {
            // Not at the limit so continue integrating errors
            pbio_position_integrator_resume(&ctl->position_integrator, time_now);
        }
    }
    // Position anti-windup in case of timed speed control (speed integral may not get too high)
    else {
        if (pause_integration) {
            // We are at the torque limit and we should prevent further speed error integration.
            pbio_speed_integrator_pause(&ctl->speed_integrator, time_now, position_error);
        } else {
            // Not at the limit so continue integrating errors
            pbio_speed_integrator_resume(&ctl->speed_integrator, time_now, position_error);
        }
    }

    // Check if controller is stalled
    ctl->stalled = pbio_control_type_is_position(ctl) ?
        pbio_position_integrator_stalled(&ctl->position_integrator, time_now, state->speed_estimate, ref->speed, ctl->settings.stall_time, ctl->settings.stall_speed_limit, integral_max) :
        pbio_speed_integrator_stalled(&ctl->speed_integrator, time_now, state->speed_estimate, ref->speed, ctl->settings.stall_time, ctl->settings.stall_speed_limit);

    // Check if we are on target
    ctl->on_target = pbio_control_check_completion(ctl, ref->time, state, &ref_end);

    // Save (low-pass filtered) load for diagnostics
    ctl->load = (ctl->load * (100 - PBIO_CONFIG_CONTROL_LOOP_TIME_MS) + torque * PBIO_CONFIG_CONTROL_LOOP_TIME_MS) / 100;

    // Decide actuation based on whether control is on target.
    if (!ctl->on_target) {
        // If we're not on target yet, we keep actuating with
        // the PID torque value that has just been calculated.
        *actuation = PBIO_DCMOTOR_ACTUATION_TORQUE;
        *control = torque;
    } else {
        // If on target, decide what to do next using the on-completion type.
        switch (ctl->on_completion) {
            case PBIO_CONTROL_ON_COMPLETION_COAST:
                // Coast the motor and stop the control loop.
                *actuation = PBIO_DCMOTOR_ACTUATION_COAST;
                *control = 0;
                pbio_control_stop(ctl);
                break;
            case PBIO_CONTROL_ON_COMPLETION_BRAKE:
                // Passively brake and stop the control loop.
                *actuation = PBIO_DCMOTOR_ACTUATION_BRAKE;
                *control = 0;
                pbio_control_stop(ctl);
                break;
            case PBIO_CONTROL_ON_COMPLETION_COAST_SMART:
                // For smart coast, keep actuating (holding) briefly to enforce
                // standstill. It also gives some time for two subsequent
                // blocks to smoothly transition without going through coast.
                if (ref->time - ref_end.time < pbio_control_time_ms_to_ticks(100)) {
                    *actuation = PBIO_DCMOTOR_ACTUATION_TORQUE;
                    *control = torque;
                }
                // After that, coast the motor and stop the control loop.
                else {
                    *actuation = PBIO_DCMOTOR_ACTUATION_COAST;
                    *control = 0;
                    pbio_control_stop(ctl);
                }
                break;
            case PBIO_CONTROL_ON_COMPLETION_CONTINUE:
            // Fall through, same as hold.
            case PBIO_CONTROL_ON_COMPLETION_HOLD:
                // Holding position or continuing the trajectory just means we
                // have to keep actuating with the PID torque value that has
                // just been calculated.
                *actuation = PBIO_DCMOTOR_ACTUATION_TORQUE;
                *control = torque;

                // If we are getting here on completion of a timed command with
                // a stationary endpoint, convert it to a stationary angle
                // based command and hold it.
                if (pbio_control_type_is_time(ctl) && ref_end.speed == 0) {
                    int32_t target = pbio_control_position_ctl_to_app_long(&ctl->settings, &ref->position);
                    pbio_control_start_position_control_hold(ctl, time_now, target);
                }
                break;
        }
    }

    // Log control data.
    int32_t log_data[] = {
        ref->time - ctl->trajectory.start.time,
        pbio_control_position_ctl_to_app_long(&ctl->settings, &state->position),
        0,
        *actuation,
        *control,
        pbio_control_position_ctl_to_app_long(&ctl->settings, &ref->position),
        pbio_control_position_ctl_to_app(&ctl->settings, ref->speed),
        pbio_control_position_ctl_to_app_long(&ctl->settings, &state->position_estimate),
        pbio_control_position_ctl_to_app(&ctl->settings, state->speed_estimate),
        torque_proportional,
        torque_integral,
        torque_derivative,
    };
    pbio_logger_update(&ctl->log, log_data);
}

/**
 * Stops (but not resets) the update loop from updating this controller. This
 * is normally called when a motor coasts or brakes.
 *
 * @param [in]  ctl         Control status structure.
 */
void pbio_control_stop(pbio_control_t *ctl) {
    ctl->type = PBIO_CONTROL_NONE;
    ctl->on_target = true;
    ctl->stalled = false;
}

/**
 * Resets and initializes the control state. This is called when a device that
 * uses this controller is first initialized or when it is disconnected.
 *
 * @param [in]  ctl         Control status structure.
 */
void pbio_control_reset(pbio_control_t *ctl) {

    // Stop the control loop update.
    pbio_control_stop(ctl);

    // Reset the previous on-completion state.
    ctl->on_completion = PBIO_CONTROL_ON_COMPLETION_COAST;

    // The on_completion state is the only persistent setting between
    // subsequent maneuvers, so nothing else needs to be reset explicitly.
}

static pbio_error_t _pbio_control_start_position_control(pbio_control_t *ctl, uint32_t time_now, pbio_control_state_t *state, pbio_angle_t *target, int32_t speed, pbio_control_on_completion_t on_completion) {

    pbio_error_t err;

    // Set new maneuver action and stop type, and state
    ctl->on_completion = on_completion;
    ctl->on_target = false;

    // Common trajectory parameters for all cases covered here.
    pbio_trajectory_command_t command = {
        .position_end = *target,
        .speed_target = speed == 0 ? ctl->settings.speed_default : speed,
        .speed_max = ctl->settings.speed_max,
        .acceleration = ctl->settings.acceleration,
        .deceleration = ctl->settings.deceleration,
        .continue_running = on_completion == PBIO_CONTROL_ON_COMPLETION_CONTINUE,
    };


    // Given the control status, fill in remaining commands and get trajectory.
    if (!pbio_control_is_active(ctl)) {
        // If no control is ongoing, we just start from the measured state.
        command.time_start = time_now;
        command.position_start = state->position;
        command.speed_start = state->speed_estimate;

        // With the command fully populated, we can calculate the trajectory.
        err = pbio_trajectory_new_angle_command(&ctl->trajectory, &command);
        if (err != PBIO_SUCCESS) {
            return err;
        }
    } else {
        // Otherwise, If control is active, (re)start from the current
        // reference. This way the reference just branches off on a new
        // trajectory instead of falling back slightly, avoiding a speed drop.
        command.time_start = pbio_control_get_ref_time(ctl, time_now);
        pbio_trajectory_reference_t ref;
        pbio_trajectory_get_reference(&ctl->trajectory, command.time_start, &ref);
        command.position_start = ref.position;
        command.speed_start = ref.speed;

        // Before we override the trajectory to renew it, get the starting
        // point of the current speed/angle segment of the reference. We may
        // need it below.
        pbio_trajectory_reference_t ref_vertex;
        pbio_trajectory_get_last_vertex(&ctl->trajectory, command.time_start, &ref_vertex);

        // With the command fully populated, we can calculate the trajectory.
        err = pbio_trajectory_new_angle_command(&ctl->trajectory, &command);
        if (err != PBIO_SUCCESS) {
            return err;
        }

        // If the new trajectory is tangent to the current one, we can do
        // better than just branching off. Instead, we can adjust the command
        // so it starts from the same point as the previous trajectory. This
        // avoids rounding errors when restarting commands in a tight loop.
        if (ctl->trajectory.a0 == ref.acceleration) {

            // Update command with shifted starting point, equal to ongoing
            // maneuver.
            command.time_start = ref_vertex.time;
            command.position_start = ref_vertex.position;
            command.speed_start = ref_vertex.speed;

            // Recalculate the trajectory from the shifted starting point.
            err = pbio_trajectory_new_angle_command(&ctl->trajectory, &command);
            if (err != PBIO_SUCCESS) {
                return err;
            }
        }
    }

    // Reset PID control if needed
    if (!pbio_control_type_is_position(ctl)) {
        // Get (again) the reference at current time, so we get the correct
        // value regardless of the command path followed above.
        pbio_trajectory_reference_t ref_new;
        pbio_trajectory_get_reference(&ctl->trajectory, time_now, &ref_new);

        // New angle maneuver, so reset the rate integrator
        pbio_position_integrator_reset(&ctl->position_integrator, ref_new.time);

        // Reset load filter
        ctl->load = 0;
    }

    // Set the new control state
    ctl->type = PBIO_CONTROL_POSITION;

    return PBIO_SUCCESS;
}

/**
 * Starts the controller to run to a given target position.
 *
 * In a servo application, this means running to a target angle.
 *
 * @param [in]  ctl            The control instance.
 * @param [in]  time_now       The wall time (ticks).
 * @param [in]  state          The current state of the system being controlled (control units).
 * @param [in]  position       The target position to run to (application units).
 * @param [in]  speed          The top speed on the way to the target (application units). The sign is ignored. If zero, default speed is used.
 * @param [in]  on_completion  What to do when reaching the target position.
 * @return                     Error code.
 */
pbio_error_t pbio_control_start_position_control(pbio_control_t *ctl, uint32_t time_now, pbio_control_state_t *state, int32_t position, int32_t speed, pbio_control_on_completion_t on_completion) {

    // Convert target position to control units.
    pbio_angle_t target;
    pbio_control_position_app_to_ctl_long(&ctl->settings, position, &target);

    // Start position control in control units.
    return _pbio_control_start_position_control(ctl, time_now, state, &target, pbio_control_position_app_to_ctl(&ctl->settings, speed), on_completion);
}

/**
 * Starts the controller to run by a given distance.
 *
 * In a servo application, this means running by the given angle.
 *
 * This function computes what the new target position will be, and then
 * calls pbio_control_start_position_control to get there.
 *
 * @param [in]  ctl             The control instance.
 * @param [in]  time_now        The wall time (ticks).
 * @param [in]  state           The current state of the system being controlled (control units).
 * @param [in]  distance        The distance to run by (application units).
 * @param [in]  speed           The top speed on the way to the target (application units). Negative speed flips the distance sign.
 * @param [in]  on_completion   What to do when reaching the target position.
 * @return                      Error code.
 */
pbio_error_t pbio_control_start_position_control_relative(pbio_control_t *ctl, uint32_t time_now, pbio_control_state_t *state, int32_t distance, int32_t speed, pbio_control_on_completion_t on_completion) {

    // Convert distance to control units.
    pbio_angle_t increment;
    pbio_control_position_app_to_ctl_long(&ctl->settings, (speed < 0 ? -distance : distance), &increment);

    // We need to decide where the relative motion starts from, and use that
    // to compute the position target by adding the increment.
    pbio_angle_t target;

    if (pbio_control_is_active(ctl)) {
        // If control is already active, restart from current reference.
        uint32_t time_ref = pbio_control_get_ref_time(ctl, time_now);
        pbio_trajectory_reference_t ref;
        pbio_trajectory_get_reference(&ctl->trajectory, time_ref, &ref);
        pbio_angle_sum(&ref.position, &increment, &target);
    } else {
        // Control is inactive. We still have two options.
        // If the previous command used smart coast and we're still close to
        // its target, we want to start from there. This avoids accumulating
        // errors in programs that use mostly relative motions like run_angle.
        pbio_trajectory_reference_t prev_end;
        pbio_trajectory_get_endpoint(&ctl->trajectory, &prev_end);
        if (ctl->on_completion == PBIO_CONTROL_ON_COMPLETION_COAST_SMART &&
            pbio_angle_diff_is_small(&prev_end.position, &state->position) &&
            abs(pbio_angle_diff_mdeg(&prev_end.position, &state->position)) < ctl->settings.position_tolerance * 2) {
            // We're close enough, so make the new target relative to the
            // endpoint of the last one.
            pbio_angle_sum(&prev_end.position, &increment, &target);
        } else {
            // No special cases apply, so the best we can do is just start from
            // the current state.
            pbio_angle_sum(&state->position, &increment, &target);
        }
    }

    return _pbio_control_start_position_control(ctl, time_now, state, &target, pbio_control_position_app_to_ctl(&ctl->settings, speed), on_completion);
}

/**
 * Starts the controller and holds at the given position.
 *
 * This is similar to starting position control, but it skips the trajectory
 * computation and just sets the reference to the target position right away.
 *
 * @param [in]  ctl             The control instance.
 * @param [in]  time_now        The wall time (ticks).
 * @param [in]  position        The target position to hold (application units).
 * @return                      Error code.
 */
pbio_error_t pbio_control_start_position_control_hold(pbio_control_t *ctl, uint32_t time_now, int32_t position) {

    // Set new maneuver action and stop type, and state
    ctl->on_completion = PBIO_CONTROL_ON_COMPLETION_HOLD;
    ctl->on_target = false;

    // Compute new maneuver based on user argument, starting from the initial state
    pbio_trajectory_command_t command = {
        .time_start = pbio_control_get_ref_time(ctl, time_now),
        .speed_target = 0,
        .continue_running = false,
    };
    pbio_control_position_app_to_ctl_long(&ctl->settings, position, &command.position_start);
    pbio_control_position_app_to_ctl_long(&ctl->settings, position, &command.position_end);

    pbio_trajectory_make_constant(&ctl->trajectory, &command);
    // If called for the first time, set state and reset PID
    if (!pbio_control_type_is_position(ctl)) {
        // Initialize or reset the PID control status for the given maneuver
        pbio_position_integrator_reset(&ctl->position_integrator, time_now);

        // Reset load filter
        ctl->load = 0;
    }

    // This is an angular control maneuver
    ctl->type = PBIO_CONTROL_POSITION;

    return PBIO_SUCCESS;
}

/**
 * Starts the controller to run for a given amount of time.
 *
 * @param [in]  ctl             The control instance.
 * @param [in]  time_now        The wall time (ticks).
 * @param [in]  state           The current state of the system being controlled (control units).
 * @param [in]  duration        For how long to run (ms).
 * @param [in]  speed           The top speed (application units). Negative speed means reverse.
 * @param [in]  on_completion   What to do when duration is over.
 * @return                      Error code.
 */
pbio_error_t pbio_control_start_timed_control(pbio_control_t *ctl, uint32_t time_now, pbio_control_state_t *state, int32_t duration, int32_t speed, pbio_control_on_completion_t on_completion) {

    pbio_error_t err;

    if (on_completion == PBIO_CONTROL_ON_COMPLETION_COAST_SMART) {
        // For timed maneuvers, the end point has no meaning, so just coast.
        on_completion = PBIO_CONTROL_ON_COMPLETION_COAST;
    }

    // Set new maneuver action and stop type, and state
    ctl->on_completion = on_completion;
    ctl->on_target = false;

    // Common trajectory parameters for the cases covered here.
    pbio_trajectory_command_t command = {
        .time_start = time_now,
        .duration = pbio_control_time_ms_to_ticks(duration),
        .speed_target = pbio_control_position_app_to_ctl(&ctl->settings, speed),
        .speed_max = ctl->settings.speed_max,
        .acceleration = ctl->settings.acceleration,
        .deceleration = ctl->settings.deceleration,
        .continue_running = on_completion == PBIO_CONTROL_ON_COMPLETION_CONTINUE,
    };

    // Given the control status, fill in remaining commands and get trajectory.
    if (!pbio_control_is_active(ctl)) {
        // If no control is ongoing, we just start from the measured state.
        command.time_start = time_now;
        command.position_start = state->position;
        command.speed_start = state->speed_estimate;

        // With the command fully populated, we can calculate the trajectory.
        err = pbio_trajectory_new_time_command(&ctl->trajectory, &command);
        if (err != PBIO_SUCCESS) {
            return err;
        }
    } else {
        // Otherwise, If control is active, (re)start from the current
        // reference. This way the reference just branches off on a new
        // trajectory instead of falling back slightly, avoiding a speed drop.
        uint32_t time_ref = pbio_control_get_ref_time(ctl, time_now);
        pbio_trajectory_reference_t ref;
        pbio_trajectory_get_reference(&ctl->trajectory, time_ref, &ref);
        command.position_start = ref.position;
        command.speed_start = ref.speed;

        // Before we override the trajectory to renew it, get the starting
        // point of the current speed/angle segment of the reference. We may
        // need it below.
        pbio_trajectory_reference_t ref_vertex;
        pbio_trajectory_get_last_vertex(&ctl->trajectory, command.time_start, &ref_vertex);

        // With the command fully populated, we can calculate the trajectory.
        err = pbio_trajectory_new_time_command(&ctl->trajectory, &command);
        if (err != PBIO_SUCCESS) {
            return err;
        }

        // If the new trajectory is tangent to the current one, we can do
        // better than just branching off. Instead, we can adjust the command
        // so it starts from the same point as the previous trajectory. This
        // avoids rounding errors when restarting commands in a tight loop.
        if (pbio_control_type_is_time(ctl) && ctl->trajectory.a0 == ref.acceleration) {

            // Update command with shifted starting point, equal to ongoing
            // maneuver.
            command.time_start = ref_vertex.time;
            command.position_start = ref_vertex.position;
            command.speed_start = ref_vertex.speed;

            // We shifted the start time into the past, so we must adjust
            // duration accordingly.
            command.duration += (time_now - ref_vertex.time);

            // Recalculate the trajectory from the shifted starting point.
            err = pbio_trajectory_new_time_command(&ctl->trajectory, &command);
            if (err != PBIO_SUCCESS) {
                return err;
            }
        }
    }

    // Reset PD control if needed
    if (!pbio_control_type_is_time(ctl)) {
        // New maneuver, so reset the rate integrator
        pbio_speed_integrator_reset(&ctl->speed_integrator, time_now, 0);

        // Set the new control state
        ctl->type = PBIO_CONTROL_TIMED;

        // Reset load filter
        ctl->load = 0;
    }

    return PBIO_SUCCESS;
}

/**
 * Converts position-like control units to application-specific units.
 *
 * This should only be used if input/ouput are within known bounds.
 *
 * @param [in] s              Control settings containing the scale.
 * @param [in] input          Signal in control units.
 * @return                    Signal in application units.
 */
int32_t pbio_control_position_ctl_to_app(pbio_control_settings_t *s, int32_t input) {
    return input / s->ctl_steps_per_app_step;
}

/**
 * Converts position-like control units to application-specific units.
 *
 * This can be used with large inputs but there is more overhead.
 *
 * @param [in] s              Control settings containing the scale.
 * @param [in] input          Signal in control units.
 * @return                    Signal in application units.
 */
int32_t pbio_control_position_ctl_to_app_long(pbio_control_settings_t *s, pbio_angle_t *input) {
    return pbio_angle_to_low_res(input, s->ctl_steps_per_app_step);
}

/**
 * Converts application-specific units to position-like control units.
 *
 * This should only be used if input/ouput are within known bounds.
 *
 * @param [in] s              Control settings containing the scale.
 * @param [in] input          Signal in application units.
 * @return                    Signal in control units.
 */
int32_t pbio_control_position_app_to_ctl(pbio_control_settings_t *s, int32_t input) {
    return input * s->ctl_steps_per_app_step;
}

/**
 * Converts application-specific units to position-like control units.
 *
 * This can be used with large inputs but there is more overhead.
 *
 * @param [in]  s              Control settings containing the scale.
 * @param [in]  input          Signal in application units.
 * @param [out] output         Signal in control units.
 */
void pbio_control_position_app_to_ctl_long(pbio_control_settings_t *s, int32_t input, pbio_angle_t *output) {
    pbio_angle_from_low_res(output, input, s->ctl_steps_per_app_step);
}

/**
 * Converts actuation-like control units to application-specific units.
 *
 * @param [in] input          Actuation in control units (uNm).
 * @return                    Actuation in application units (mNm).
 */
int32_t pbio_control_actuation_ctl_to_app(int32_t input) {
    // All applications currently use this scale, but it could be generalized
    // to a appplication specific conversion constant.
    return input / 1000;
}

/**
 * Converts application-specific units to actuation-like control units.
 *
 * @param [in] input          Actuation in application units (mNm).
 * @return                    Actuation in control units (uNm).
 */
int32_t pbio_control_actuation_app_to_ctl(int32_t input) {
    // All applications currently use this scale, but it could be generalized
    // to a appplication specific conversion constant.
    return input * 1000;
}

/**
 * Gets the control limits for movement and actuation, in application units.
 *
 * @param [in]  s             Control settings structure from which to read.
 * @param [out] speed         Speed limit in application units.
 * @param [out] acceleration  Absolute rate of change of the speed during on-ramp of the maneuver.
 * @param [out] deceleration  Absolute rate of change of the speed during off-ramp of the maneuver.
 * @param [out] actuation     Upper limit on actuation.
 */
void pbio_control_settings_get_limits(pbio_control_settings_t *s, int32_t *speed, int32_t *acceleration, int32_t *deceleration, int32_t *actuation) {
    *speed = pbio_control_position_ctl_to_app(s, s->speed_max);
    *acceleration = pbio_control_position_ctl_to_app(s, s->acceleration);
    *deceleration = pbio_control_position_ctl_to_app(s, s->deceleration);
    *actuation = pbio_control_actuation_ctl_to_app(s->actuation_max);
}

/**
 * Sets the control limits for movement and actuation, in application units.
 *
 * @param [in] s              Control settings structure from which to read.
 * @param [in] speed          Speed limit in application units.
 * @param [in] acceleration   Absolute rate of change of the speed during on-ramp of the maneuver.
 * @param [in] deceleration   Absolute rate of change of the speed during off-ramp of the maneuver.
 * @param [in] actuation      Upper limit on actuation.
 * @return                    ::PBIO_SUCCESS on success
 *                            ::PBIO_ERROR_INVALID_ARG if any argument is negative.
 */
pbio_error_t pbio_control_settings_set_limits(pbio_control_settings_t *s, int32_t speed, int32_t acceleration, int32_t deceleration, int32_t actuation) {
    if (speed < 1 || acceleration < 1 || deceleration < 1 || actuation < 1) {
        return PBIO_ERROR_INVALID_ARG;
    }
    s->speed_max = pbio_control_position_app_to_ctl(s, speed);
    s->acceleration = pbio_control_position_app_to_ctl(s, acceleration);
    s->deceleration = pbio_control_position_app_to_ctl(s, deceleration);
    s->actuation_max = pbio_control_actuation_app_to_ctl(actuation);
    return PBIO_SUCCESS;
}

/**
 * Gets the PID control parameters.
 *
 * Kp, Ki, and Kd are returned given in control units. Everything else in application units.
 *
 * @param [in]  s                    Control settings structure from which to read.
 * @param [out] pid_kp               Position error feedback constant.
 * @param [out] pid_ki               Accumulated error feedback constant.
 * @param [out] pid_kd               Speed error feedback constant.
 * @param [out] integral_change_max  Absolute bound on the rate at which the integrator accumulates errors, in application units.
 */
void pbio_control_settings_get_pid(pbio_control_settings_t *s, int32_t *pid_kp, int32_t *pid_ki, int32_t *pid_kd, int32_t *integral_change_max) {
    *pid_kp = s->pid_kp;
    *pid_ki = s->pid_ki;
    *pid_kd = s->pid_kd;
    *integral_change_max = pbio_control_position_ctl_to_app(s, s->integral_change_max);
}

/**
 * Sets the PID control parameters.
 *
 * Kp, Ki, and Kd should be given in control units. Everything else in application units.
 *
 * @param [in] s                     Control settings structure to write to.
 * @param [out] pid_kp               Position error feedback constant.
 * @param [out] pid_ki               Accumulated error feedback constant.
 * @param [out] pid_kd               Speed error feedback constant.
 * @param [out] integral_change_max  Absolute bound on the rate at which the integrator accumulates errors, in application units.
 * @return                           ::PBIO_SUCCESS on success
 *                                   ::PBIO_ERROR_INVALID_ARG if any argument is negative.
 */
pbio_error_t pbio_control_settings_set_pid(pbio_control_settings_t *s, int32_t pid_kp, int32_t pid_ki, int32_t pid_kd, int32_t integral_change_max) {
    if (pid_kp < 0 || pid_ki < 0 || pid_kd < 0 || integral_change_max < 0) {
        return PBIO_ERROR_INVALID_ARG;
    }

    s->pid_kp = pid_kp;
    s->pid_ki = pid_ki;
    s->pid_kd = pid_kd;
    s->integral_change_max = pbio_control_position_app_to_ctl(s, integral_change_max);
    return PBIO_SUCCESS;
}

/**
 * Gets the tolerances associated with reaching a position target.
 * @param [in]  s           Control settings structure from which to read.
 * @param [out] speed       Speed tolerance in application units.
 * @param [out] position    Position tolerance in application units.
 */
void pbio_control_settings_get_target_tolerances(pbio_control_settings_t *s, int32_t *speed, int32_t *position) {
    *position = pbio_control_position_ctl_to_app(s, s->position_tolerance);
    *speed = pbio_control_position_ctl_to_app(s, s->speed_tolerance);
}

/**
 * Sets the tolerances associated with reaching a position target, in application units.
 *
 * @param [in] s            Control settings structure to write to.
 * @param [in] speed        Speed tolerance in application units.
 * @param [in] position     Position tolerance in application units.
 * @return                  ::PBIO_SUCCESS on success
 *                          ::PBIO_ERROR_INVALID_ARG if any argument is negative.
 */
pbio_error_t pbio_control_settings_set_target_tolerances(pbio_control_settings_t *s, int32_t speed, int32_t position) {
    if (position < 0 || speed < 0) {
        return PBIO_ERROR_INVALID_ARG;
    }

    s->position_tolerance = pbio_control_position_app_to_ctl(s, position);
    s->speed_tolerance = pbio_control_position_app_to_ctl(s, speed);
    return PBIO_SUCCESS;
}

/**
 * Gets the tolerances associated with the controller being stalled, in application units.
 *
 * @param [in]  s           Control settings structure from which to read.
 * @param [out] speed       If this speed can't be reached with maximum actuation, it is stalled.
 * @param [out] time        Minimum consequtive stall time (ticks) before stall flag getter returns true.
 */
void pbio_control_settings_get_stall_tolerances(pbio_control_settings_t *s, int32_t *speed, uint32_t *time) {
    *speed = pbio_control_position_ctl_to_app(s, s->stall_speed_limit);
    *time = pbio_control_time_ticks_to_ms(s->stall_time);
}

/**
 * Sets the tolerances associated with the controller being stalled, in application units.
 *
 * @param [in] s            Control settings structure to write to.
 * @param [in] speed        If this speed can't be reached with maximum actuation, it is stalled.
 * @param [in] time         Minimum consequtive stall time (ticks) before stall flag getter returns true.
 * @return                  ::PBIO_SUCCESS on success
 *                          ::PBIO_ERROR_INVALID_ARG if any argument is negative.
 */
pbio_error_t pbio_control_settings_set_stall_tolerances(pbio_control_settings_t *s, int32_t speed, uint32_t time) {
    if (speed < 0) {
        return PBIO_ERROR_INVALID_ARG;
    }

    s->stall_speed_limit = pbio_control_position_app_to_ctl(s, speed);
    s->stall_time = pbio_control_time_ms_to_ticks(time);
    return PBIO_SUCCESS;
}

/**
 * Gets the time at which to evaluate the reference trajectory by compensating
 * the wall time by the amount of time spent stalling during which a position
 * based trajectory does not progress.
 *
 * @param [in]  ctl         Control status structure.
 * @param [in]  time_now    Wall time (ticks).
 * @return int32_t          Time (ticks) on the trajectory curve.
 */
uint32_t pbio_control_get_ref_time(pbio_control_t *ctl, uint32_t time_now) {
    // Angle controllers may pause the time so the reference position does not
    // keep accumulating while the controller is stuck.
    if (pbio_control_type_is_position(ctl)) {
        return pbio_position_integrator_get_ref_time(&ctl->position_integrator, time_now);
    }
    // In all other cases, it is just the current time.
    return time_now;
}

/**
 * Checks if the controller is currently active.
 *
 * @param [in]  ctl             The control instance.
 * @return                      True if active (position or time), false if not.
 */
bool pbio_control_is_active(pbio_control_t *ctl) {
    return ctl->type != PBIO_CONTROL_NONE;
}

/**
 * Checks if the controller is currently doing position control.
 *
 * @param [in]  ctl             The control instance.
 * @return                      True if position control is active, false if not.
 */
bool pbio_control_type_is_position(pbio_control_t *ctl) {
    return ctl->type == PBIO_CONTROL_POSITION;
}

/**
 * Checks if the controller is currently doing timed control.
 *
 * @param [in]  ctl             The control instance.
 * @return                      True if timed control is active, false if not.
 */
bool pbio_control_type_is_time(pbio_control_t *ctl) {
    return ctl->type == PBIO_CONTROL_TIMED;
}

/**
 * Checks if the controller is stalled and for how long.
 *
 * @param [in]  ctl             The control instance.
 * @param [out] stall_duration  For how long the controller has stalled (ticks).
 * @return                      True if controller is stalled, false if not.
 */
bool pbio_control_is_stalled(pbio_control_t *ctl, uint32_t *stall_duration) {

    // Return false if no control is active or if we're not stalled.
    if (!pbio_control_is_active(ctl) || !ctl->stalled) {
        *stall_duration = 0;
        return false;
    }

    // Get time since stalling began.
    uint32_t time_pause_begin = ctl->type == PBIO_CONTROL_POSITION ? ctl->position_integrator.time_pause_begin : ctl->speed_integrator.time_pause_begin;
    *stall_duration = pbio_control_get_time_ticks() - time_pause_begin;

    return true;
}

/**
 * Checks if the controller is done.
 *
 * For trajectories with a stationary endpoint, done means on target.
 *
 * @param [in]  ctl             The control instance.
 * @return                      True if the controller is done, false if not.
 */
bool pbio_control_is_done(pbio_control_t *ctl) {
    return ctl->type == PBIO_CONTROL_NONE || ctl->on_target;
}

/**
 * Gets the load experienced by the controller.
 *
 * It is determined as a slow moving average of the PID output, which is a
 * measure for how hard the controller must work to stay on target.
 *
 * @param [in]  ctl             The control instance.
 * @return                      The approximate load (control units).
 */
int32_t pbio_control_get_load(pbio_control_t *ctl) {
    return ctl->type == PBIO_CONTROL_NONE ? 0 : ctl->load;
}
