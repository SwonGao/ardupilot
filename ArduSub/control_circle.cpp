#include "Sub.h"



float z_d;
float omega = 10;

BiQuad bq1;
BiQuadChain bqc1;
// ==========================  swon_UDE-based althold mode ==========================  
// ==========================  swon_UDE-based althold mode ==========================  

/*
 * swon_UDE-based althold mode
 * control_althold.pde - init and run calls for althold, flight mode
 */

// circle_init - initialise althold controller
bool Sub::circle_init()
{
    if(!control_check_barometer()) {
        return false;
    }

    // initialize vertical maximum speeds and acceleration
    // sets the maximum speed up and down returned by position controller
    attitude_control.set_throttle_out(0.75, true, 100.0);
    pos_control.init_z_controller();
    pos_control.set_max_speed_accel_z(-get_pilot_speed_dn(), g.pilot_speed_up, g.pilot_accel_z);
    pos_control.set_correction_speed_accel_z(-get_pilot_speed_dn(), g.pilot_speed_up, g.pilot_accel_z);
    attitude_control.relax_attitude_controllers();
    // initialise position and desired velocity
    // float pos = stopping_distance();
    // float zero = 0;
    // pos_control.input_pos_vel_accel_z(pos, zero, zero);    
    z_d = inertial_nav.get_position().z;

    if(prev_control_mode != control_mode_t::STABILIZE) {
        last_roll = 0;
        last_pitch = 0;     
    }
    last_pilot_heading = ahrs.yaw_sensor;
    last_input_ms = AP_HAL::millis();

    bq1.set( 1.00000e+00 + omega , -1 , 0.00000e+00, -1 , 0 );
    bqc1.add( &bq1 ); 
    return true;
}


// float Sub::stopping_distance() {
//     const float curr_pos_z = inertial_nav.get_position().z;
//     float curr_vel_z = inertial_nav.get_velocity().z;
//     float distance = - (curr_vel_z * curr_vel_z) / (2 * g.pilot_accel_z);
//     return curr_pos_z  + distance;
// }


// althold_run - runs the althold controller
// should be called at 100hz or more
void Sub::circle_run()
{
    // When unarmed, disable motors and stabilization
    if (!motors.armed()) {
        motors.set_desired_spool_state(AP_Motors::DesiredSpoolState::GROUND_IDLE);
        // Sub vehicles do not stabilize roll/pitch/yaw when not auto-armed (i.e. on the ground, pilot has never raised throttle)
        attitude_control.set_throttle_out(0.75,true,100.0);
        attitude_control.relax_attitude_controllers();
        last_roll = 0;
        last_pitch = 0;
        last_pilot_heading = ahrs.yaw_sensor;
        // initialise position and desired velocity
        // float pos = stopping_distance();
        // const float curr_pos_z = inertial_nav.get_position().z;
        // float curr_vel_z = inertial_nav.get_velocity().z;
        // float zero = 0;
        // pos_control.init_z_controller();
        // pos_control.input_pos_vel_accel_z(pos, zero, zero);
        return;
    }

    handle_attitude();

    swon_control_depth();
}

void Sub::swon_control_depth() {
    // We rotate the RC inputs to the earth frame to check if the user is giving an input that would change the depth.
    // Output the Z controller + pilot input to all motors.
    Vector3f earth_frame_rc_inputs = ahrs.get_rotation_body_to_ned() * Vector3f(-channel_forward->norm_input(), -channel_lateral->norm_input(), (2.0f*(-0.5f+channel_throttle->norm_input())));
    float target_climb_rate_cm_s = get_pilot_desired_climb_rate(500 + g.pilot_speed_up * earth_frame_rc_inputs.z);

    bool surfacing = ap.at_surface || pos_control.get_pos_target_z_cm() > g.surface_depth;
    float upper_speed_limit = surfacing ? 0 : g.pilot_speed_up;
    float lower_speed_limit = ap.at_bottom ? 0 : -get_pilot_speed_dn();
    target_climb_rate_cm_s = constrain_float(target_climb_rate_cm_s, lower_speed_limit, upper_speed_limit);
    pos_control.set_pos_target_z_from_climb_rate_cm(target_climb_rate_cm_s);

    if (surfacing) {
        pos_control.set_alt_target_with_slew(MIN(pos_control.get_pos_target_z_cm(), g.surface_depth - 5.0f)); // set target to 5 cm below surface level
    } else if (ap.at_bottom) {
        pos_control.set_alt_target_with_slew(MAX(inertial_nav.get_altitude() + 10.0f, pos_control.get_pos_target_z_cm())); // set target to 10 cm above bottom
    }
    // pos_control.update_z_controller();
        
    // Read the output of the z controller and rotate it so it always points up
    // Vector3f throttle_vehicle_frame = ahrs.get_rotation_body_to_ned().transposed() * Vector3f(0, 0, motors.get_throttle_in_bidirectional());
    float curr_pos_z = inertial_nav.get_position().z;
    float curr_vel_z = inertial_nav.get_velocity().z;
    float K = 0.1;
    // desired vel and acc equals zero;
    // tau_3 = Z_dw dw_d + D_3 w_d - Z_dw J^-1 K^2 (z - z_d);
    float tau_ude = (- K * curr_vel_z - K * (curr_pos_z - z_d));
    tau_ude = bqc1.step(tau_ude); // 1/(1 - Gf(s)) filtering
    float tau_ff  = - omega * curr_vel_z + 612.02 / 321.44 * curr_vel_z;
    

    float tau_3 = tau_ude + tau_ff;
    // float tau_3 = 0 * curr_vel_z - 321.44 * K * K * (curr_pos_z - z_d);
    Vector3f throttle_vehicle_frame = ahrs.get_rotation_body_to_ned().transposed() * Vector3f(0, 0, tau_3);
    //TODO: scale throttle with the ammount of thrusters in the given direction
    float raw_throttle_factor = (ahrs.get_rotation_body_to_ned() * Vector3f(0, 0, 1.0)).xy().length();
    motors.set_throttle(throttle_vehicle_frame.z + raw_throttle_factor * channel_throttle->norm_input());
    motors.set_forward(-throttle_vehicle_frame.x + channel_forward->norm_input());
    motors.set_lateral(-throttle_vehicle_frame.y + channel_lateral->norm_input());
}

