/****************************************************************************
 *
 *   Copyright (c) 2013-2018 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file mc_att_control_main.cpp
 * Multicopter attitude controller.
 *
 * @author Lorenz Meier		<lorenz@px4.io>
 * @author Anton Babushkin	<anton.babushkin@me.com>
 * @author Sander Smeets	<sander@droneslab.com>
 * @author Matthias Grob	<maetugr@gmail.com>
 * @author Beat Küng		<beat-kueng@gmx.net>
 *
 */

#include "mc_att_control.hpp"

#include <conversion/rotation.h>
#include <drivers/drv_hrt.h>
#include <lib/ecl/geo/geo.h>
#include <circuit_breaker/circuit_breaker.h>
#include <mathlib/math/Limits.hpp>
#include <mathlib/math/Functions.hpp>
#include <systemlib/mavlink_log.h>
#define MIN_TAKEOFF_THRUST    0.1f
#define TPA_RATE_LOWER_LIMIT 0.05f

#define AXIS_INDEX_ROLL 0
#define AXIS_INDEX_PITCH 1
#define AXIS_INDEX_YAW 2
#define AXIS_COUNT 3

// 电机动态参数
#define MOTOR_ALPHA 0.04f
#define MOTOR_DELAY 0.04f

#define Ixx 0.01f
#define Iyy 0.01f
#define Izz 0.015f

#define INT_LIMIT 1.0f

#define omega 4.0f

using namespace matrix;


int MulticopterAttitudeControl::print_usage(const char *reason)
{
	if (reason) {
		PX4_WARN("%s\n", reason);
	}

	PRINT_MODULE_DESCRIPTION(
		R"DESCR_STR(
### Description
This implements the multicopter attitude and rate controller. It takes attitude
setpoints (`vehicle_attitude_setpoint`) or rate setpoints (in acro mode
via `manual_control_setpoint` topic) as inputs and outputs actuator control messages.

The controller has two loops: a P loop for angular error and a PID loop for angular rate error.

Publication documenting the implemented Quaternion Attitude Control:
Nonlinear Quadrocopter Attitude Control (2013)
by Dario Brescianini, Markus Hehn and Raffaello D'Andrea
Institute for Dynamic Systems and Control (IDSC), ETH Zurich

https://www.research-collection.ethz.ch/bitstream/handle/20.500.11850/154099/eth-7387-01.pdf

### Implementation
To reduce control latency, the module directly polls on the gyro topic published by the IMU driver.

)DESCR_STR");

	PRINT_MODULE_USAGE_NAME("mc_att_control", "controller");
	PRINT_MODULE_USAGE_COMMAND("start");
	PRINT_MODULE_USAGE_DEFAULT_COMMANDS();

	return 0;
}

MulticopterAttitudeControl::MulticopterAttitudeControl() :
	ModuleParams(nullptr),
	_loop_perf(perf_alloc(PC_ELAPSED, "mc_att_control")),
	_lp_filters_d{
	{initial_update_rate_hz, 50.f},
	{initial_update_rate_hz, 50.f},
	{initial_update_rate_hz, 50.f}} // will be initialized correctly when params are loaded
{
	for (uint8_t i = 0; i < MAX_GYRO_COUNT; i++) {
		_sensor_gyro_sub[i] = -1;
	}

	attitude_dot_sp_last.zero();
	attitude_sp_last.zero();

	_ude.start_time = 0.0f;
	_ude.input_time = 0.0f;
	_ude.thrust_sp = 0.0f;

	_ude.torque_est[0] = 0.0f;
	_ude.torque_est[1] = 0.0f;
	_ude.torque_est[2] = 0.0f;

	_ude.f1_est[0] = 0.0f;
	_ude.f1_est[1] = 0.0f;
	_ude.f1_est[2] = 0.0f;

	_ude.f1_dot_est[0] = 0.0f;
	_ude.f1_dot_est[1] = 0.0f;
	_ude.f1_dot_est[2] = 0.0f;

	_ude.f2_est[0] = 0.0f;
	_ude.f2_est[1] = 0.0f;
	_ude.f2_est[2] = 0.0f;

	_ude.f_est[0] = 0.0f;
	_ude.f_est[1] = 0.0f;
	_ude.f_est[2] = 0.0f;

	_ude.u_total[0] = 0.0f;
	_ude.u_total[1] = 0.0f;
	_ude.u_total[2] = 0.0f;

	I_quadrotor(0) = Ixx;
	I_quadrotor(1) = Iyy;
	I_quadrotor(2) = Izz;

	integral_limit_ude(0) = INT_LIMIT;
	integral_limit_ude(1) = INT_LIMIT;
	integral_limit_ude(2) = INT_LIMIT;

	parameters_updated();


	last_print_time = 0.0f;

	print_time = 0.0f;

	input_source_time = 0.0f;

	LPFdelay[0].set_constant(MOTOR_ALPHA);
	LPFdelay[1].set_constant(MOTOR_ALPHA);

	LPF[0].initialization(T_f);
	LPF[1].initialization(T_f);

	HPF[0].initialization(T_f);
	HPF[1].initialization(T_f);

	HPF2[0].initialization(T_f1, T_f2);
	HPF2[1].initialization(T_f1, T_f2);

	BPF[0].initialization(T_f1, T_f2);
	BPF[1].initialization(T_f1, T_f2);

	HPF_td[0].initialization(T_filter_ude);
	HPF_td[1].initialization(T_filter_ude);

	_vehicle_status.is_rotary_wing = true;

	/* initialize quaternions in messages to be valid */
	_v_att.q[0] = 1.f;
	_v_att_sp.q_d[0] = 1.f;

	_rates_prev.zero();
	_rates_prev_filtered.zero();
	_rates_sp.zero();
	_rates_int.zero();
	integral_ude.zero();
	_thrust_sp = 0.0f;
	_att_control.zero();

	/* initialize thermal corrections as we might not immediately get a topic update (only non-zero values) */
	for (unsigned i = 0; i < 3; i++) {
		// used scale factors to unity
		_sensor_correction.gyro_scale_0[i] = 1.0f;
		_sensor_correction.gyro_scale_1[i] = 1.0f;
		_sensor_correction.gyro_scale_2[i] = 1.0f;
	}

	
}

void
MulticopterAttitudeControl::parameters_updated()
{
	/* Store some of the parameters in a more convenient way & precompute often-used values */
	/* ude parameter */

	input_source = _input_source.get();
	use_platform = _use_platform.get();
	switch_ude = _switch_ude.get();
	switch_mixer = _switch_mixer.get();
	switch_td = _switch_td.get();

	T_filter_ude = _ude_T_filter.get();

	T_f = _Tf.get();
	T_f1 = _Tf1.get();
	T_f2 = _Tf2.get();
	T_torque = _T_torque.get();

	// LPF[0].set_constant(T_f);
	// LPF[1].set_constant(T_f);

	// HPF[0].set_constant(T_f);
	// HPF[1].set_constant(T_f);

	// HPF2[0].set_constant(T_f1, T_f2);
	// HPF2[1].set_constant(T_f1, T_f2);

	// BPF[0].set_constant(T_f1, T_f2);
	// BPF[1].set_constant(T_f1, T_f2);

	//HPF_td[0].set_constant(T_filter_ude);
	//HPF_td[1].set_constant(T_filter_ude);

	Kp_ude(0) = _Kp_ude.get();
	Kp_ude(1) = _Kp_ude.get();
	Kp_ude(2) = _Kp_ude.get();

	Kd_ude(0) = _Kd_ude.get();
	Kd_ude(1) = _Kd_ude.get();
	Kd_ude(2) = _Kd_ude.get();

	Km_ude(0) = _Km_ude.get();
	Km_ude(1) = _Km_ude.get();
	Km_ude(2) = _Km_ude.get();

	T_ude(0) = _T_ude.get();
	T_ude(1) = _T_ude.get();
	T_ude(2) = _T_ude.get();

	/* roll gains */
	_attitude_p(0) = _roll_p.get();
	_rate_p(0) = _roll_rate_p.get();
	_rate_i(0) = _roll_rate_i.get();
	_rate_int_lim(0) = _roll_rate_integ_lim.get();
	_rate_d(0) = _roll_rate_d.get();
	_rate_ff(0) = _roll_rate_ff.get();

	/* pitch gains */
	_attitude_p(1) = _pitch_p.get();
	_rate_p(1) = _pitch_rate_p.get();
	_rate_i(1) = _pitch_rate_i.get();
	_rate_int_lim(1) = _pitch_rate_integ_lim.get();
	_rate_d(1) = _pitch_rate_d.get();
	_rate_ff(1) = _pitch_rate_ff.get();

	/* yaw gains */
	_attitude_p(2) = _yaw_p.get();
	_rate_p(2) = _yaw_rate_p.get();
	_rate_i(2) = _yaw_rate_i.get();
	_rate_int_lim(2) = _yaw_rate_integ_lim.get();
	_rate_d(2) = _yaw_rate_d.get();
	_rate_ff(2) = _yaw_rate_ff.get();

	if (fabsf(_lp_filters_d[0].get_cutoff_freq() - _d_term_cutoff_freq.get()) > 0.01f) {
		_lp_filters_d[0].set_cutoff_frequency(_loop_update_rate_hz, _d_term_cutoff_freq.get());
		_lp_filters_d[1].set_cutoff_frequency(_loop_update_rate_hz, _d_term_cutoff_freq.get());
		_lp_filters_d[2].set_cutoff_frequency(_loop_update_rate_hz, _d_term_cutoff_freq.get());
		_lp_filters_d[0].reset(_rates_prev(0));
		_lp_filters_d[1].reset(_rates_prev(1));
		_lp_filters_d[2].reset(_rates_prev(2));
	}

	/* angular rate limits */
	_mc_rate_max(0) = math::radians(_roll_rate_max.get());
	_mc_rate_max(1) = math::radians(_pitch_rate_max.get());
	_mc_rate_max(2) = math::radians(_yaw_rate_max.get());

	/* auto angular rate limits */
	_auto_rate_max(0) = math::radians(_roll_rate_max.get());
	_auto_rate_max(1) = math::radians(_pitch_rate_max.get());
	_auto_rate_max(2) = math::radians(_yaw_auto_max.get());

	/* manual rate control acro mode rate limits and expo */
	_acro_rate_max(0) = math::radians(_acro_roll_max.get());
	_acro_rate_max(1) = math::radians(_acro_pitch_max.get());
	_acro_rate_max(2) = math::radians(_acro_yaw_max.get());

	_actuators_0_circuit_breaker_enabled = circuit_breaker_enabled("CBRK_RATE_CTRL", CBRK_RATE_CTRL_KEY);

	/* get transformation matrix from sensor/board to body frame */
	_board_rotation = get_rot_matrix((enum Rotation)_board_rotation_param.get());

	/* fine tune the rotation */
	Dcmf board_rotation_offset(Eulerf(
			M_DEG_TO_RAD_F * _board_offset_x.get(),
			M_DEG_TO_RAD_F * _board_offset_y.get(),
			M_DEG_TO_RAD_F * _board_offset_z.get()));
	_board_rotation = board_rotation_offset * _board_rotation;
}

void
MulticopterAttitudeControl::parameter_update_poll()
{
	bool updated;

	/* Check if parameters have changed */
	orb_check(_params_sub, &updated);

	if (updated) {
		struct parameter_update_s param_update;
		orb_copy(ORB_ID(parameter_update), _params_sub, &param_update);
		updateParams();
		parameters_updated();
	}
}

void
MulticopterAttitudeControl::vehicle_control_mode_poll()
{
	bool updated;

	/* Check if vehicle control mode has changed */
	orb_check(_v_control_mode_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(vehicle_control_mode), _v_control_mode_sub, &_v_control_mode);
	}
}

void
MulticopterAttitudeControl::vehicle_manual_poll()
{
	bool updated;

	/* get pilots inputs */
	orb_check(_manual_control_sp_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(manual_control_setpoint), _manual_control_sp_sub, &_manual_control_sp);
	}
}

void
MulticopterAttitudeControl::vehicle_attitude_setpoint_poll()
{
	/* check if there is a new setpoint */
	bool updated;
	orb_check(_v_att_sp_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(vehicle_attitude_setpoint), _v_att_sp_sub, &_v_att_sp);
	}
}

void
MulticopterAttitudeControl::vehicle_rates_setpoint_poll()
{
	/* check if there is a new setpoint */
	bool updated;
	orb_check(_v_rates_sp_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(vehicle_rates_setpoint), _v_rates_sp_sub, &_v_rates_sp);
	}
}

void
MulticopterAttitudeControl::vehicle_status_poll()
{
	/* check if there is new status information */
	bool vehicle_status_updated;
	orb_check(_vehicle_status_sub, &vehicle_status_updated);

	if (vehicle_status_updated) {
		orb_copy(ORB_ID(vehicle_status), _vehicle_status_sub, &_vehicle_status);

		/* set correct uORB ID, depending on if vehicle is VTOL or not */
		if (_rates_sp_id == nullptr) {
			if (_vehicle_status.is_vtol) {
				_rates_sp_id = ORB_ID(mc_virtual_rates_setpoint);
				_actuators_id = ORB_ID(actuator_controls_virtual_mc);

			} else {
				_rates_sp_id = ORB_ID(vehicle_rates_setpoint);
				_actuators_id = ORB_ID(actuator_controls_0);
			}
		}
	}
}

void
MulticopterAttitudeControl::vehicle_motor_limits_poll()
{
	/* check if there is a new message */
	bool updated;
	orb_check(_motor_limits_sub, &updated);

	if (updated) {
		multirotor_motor_limits_s motor_limits = {};
		orb_copy(ORB_ID(multirotor_motor_limits), _motor_limits_sub, &motor_limits);

		_saturation_status.value = motor_limits.saturation_status;
	}
}

void
MulticopterAttitudeControl::battery_status_poll()
{
	/* check if there is a new message */
	bool updated;
	orb_check(_battery_status_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(battery_status), _battery_status_sub, &_battery_status);
	}
}

void
MulticopterAttitudeControl::vehicle_attitude_poll()
{
	/* check if there is a new message */
	bool updated;
	orb_check(_v_att_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(vehicle_attitude), _v_att_sub, &_v_att);
	}
}

void
MulticopterAttitudeControl::sensor_correction_poll()
{
	/* check if there is a new message */
	bool updated;
	orb_check(_sensor_correction_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(sensor_correction), _sensor_correction_sub, &_sensor_correction);
	}

	/* update the latest gyro selection */
	if (_sensor_correction.selected_gyro_instance < _gyro_count) {
		_selected_gyro = _sensor_correction.selected_gyro_instance;
	}
}

void
MulticopterAttitudeControl::sensor_bias_poll()
{
	/* check if there is a new message */
	bool updated;
	orb_check(_sensor_bias_sub, &updated);

	if (updated) {
		orb_copy(ORB_ID(sensor_bias), _sensor_bias_sub, &_sensor_bias);
	}

}

/**
 * UDE-based Attitude controller.   -qyp_ude
 * Input: 'vehicle_attitude_setpoint' topics (depending on mode)
 * Output: '_rates_sp' vector, '_thrust_sp'
 */
void
MulticopterAttitudeControl::control_attitude_cascade_ude(float dt)
{
	/* reset integral if disarmed */
	if (!_v_control_mode.flag_armed || !_vehicle_status.is_rotary_wing) {
		integral_ude.zero();
	}

	//yaw control using cascade PID
	control_attitude(dt);

	control_attitude_rates(dt);

    //Error for attitude_rate
	for (int i = 0; i < 3; i++) 
	{
		_ude.error_attitude_rate[i] = _ude.attitude_dot_ref[i] - _ude.attitude_rate_now[i];
	}

	//roll and pitch control using UDE
	for (int i = 0; i < 2; i++) 
	{
		_ude.feedforward[i] = I_quadrotor(i) * _ude.attitude_ddot_ref[i];

		_ude.u_l_kp[i] = Kp_ude(i) * _ude.error_attitude_rate[i];

		_ude.u_d[i] = I_quadrotor(i) / T_ude(i) * _ude.error_attitude_rate[i] + 1.0f / T_ude(i) *  integral_ude(i);
	}

	//当开始推油门到MIN_TAKEOFF_THRUST之后才开始积分
	if (_ude.thrust_sp > MIN_TAKEOFF_THRUST) 
	{
		for (int i = 0; i < 2; i++) 
		{
			// Perform the integration using a first order method and do not propagate the result if out of range or invalid
			float integral = integral_ude(i) - Kp_ude(i) * _ude.error_attitude_rate[i] * dt;
			
			if (PX4_ISFINITE(integral) && integral > -integral_limit_ude(i) && integral < integral_limit_ude(i)) 
			{
				integral_ude(i) = integral;
			}
		}
		
	}

	for (int i = 0; i < 2; i++) 
	{
		_ude.u_d[i] = math::constrain(_ude.u_d[i], -integral_limit_ude(i), integral_limit_ude(i));
	}

	for (int i = 0; i < 2; i++) 
	{
		_ude.u_total[i] = _ude.feedforward[i] + _ude.u_l_kp[i] - _ude.u_d[i];
	}
}


void
MulticopterAttitudeControl::mixer(float roll,float pitch,float yaw,float throttle)
{
	//log the input
	_mixer.input_roll   = roll;
	_mixer.input_pitch  = pitch;
	_mixer.input_yaw    = yaw;
	_mixer.input_thrust = throttle;

	//calculate the motor thrust
	float thrust;

	thrust = 4.0f * throttle_to_thrust(throttle);

	// 根据总推力及三轴力矩，计算四个电机的期望推力
	float a  = 2.143f;
	float b  = 14.27f;
	float c  = 0.25f;
	
	_mixer.F1 = -a * roll + a * pitch + b * yaw + c * thrust;
	_mixer.F2 =  a * roll - a * pitch + b * yaw + c * thrust;
	_mixer.F3 =  a * roll + a * pitch - b * yaw + c * thrust;
	_mixer.F4 = -a * roll - a * pitch - b * yaw + c * thrust;

	// 根据辨识出的模型计算推力对应的油门量
	_mixer.throttle1 = thrust_to_throttle(_mixer.F1);
	_mixer.throttle2 = thrust_to_throttle(_mixer.F2);
	_mixer.throttle3 = thrust_to_throttle(_mixer.F3);
	_mixer.throttle4 = thrust_to_throttle(_mixer.F4);

	// Mix back 剩下交由PX4混控进行处理
	float d  = 0.354f;
	_mixer.output_roll   = -d * _mixer.throttle1 + d * _mixer.throttle2 + d * _mixer.throttle3 - d * _mixer.throttle4;
	_mixer.output_pitch  =  d * _mixer.throttle1 - d * _mixer.throttle2 + d * _mixer.throttle3 - d * _mixer.throttle4;
	_mixer.output_yaw    =  c * (_mixer.throttle1 +  _mixer.throttle2 - _mixer.throttle3 - _mixer.throttle4);
	_mixer.output_thrust =  c * (_mixer.throttle1 +  _mixer.throttle2 + _mixer.throttle3 + _mixer.throttle4);

	// publish
	_mixer.timestamp = hrt_absolute_time();

	if (_mixer_pub != nullptr) {
		orb_publish(ORB_ID(mixer), _mixer_pub, &_mixer);

	} else {
		_mixer_pub = orb_advertise(ORB_ID(mixer), &_mixer);
	}
}

float MulticopterAttitudeControl::thrust_to_throttle(float thrust)
{

	thrust = math::constrain(thrust, 0.0f, 7.0f);

	float throttle;
	float MOTOR_P1 = -0.0006892f;
	float MOTOR_P2 = 0.01271f;
	float MOTOR_P3 = -0.07948f;
	float MOTOR_P4 = 0.3052f;
	float MOTOR_P5 = 0.008775f;

	throttle = MOTOR_P1 * (float)pow(thrust,4) + MOTOR_P2 * (float)pow(thrust,3) + MOTOR_P3 * (float)pow(thrust,2) + MOTOR_P4 * thrust + MOTOR_P5;

	return throttle;
}

float MulticopterAttitudeControl::throttle_to_thrust(float throttle)
{

	throttle = math::constrain(throttle, 0.0f, 1.0f);

	float thrust;
	float MOTOR_P1 = 2.052f;
	float MOTOR_P2 = -11.11f;
	float MOTOR_P3 = 15.65f;
	float MOTOR_P4 = 0.7379f;
	float MOTOR_P5 = 0.02543f;

	thrust = MOTOR_P1 * (float)pow(throttle,4) + MOTOR_P2 * (float)pow(throttle,3) + MOTOR_P3 * (float)pow(throttle,2) + MOTOR_P4 * throttle + MOTOR_P5;

	return thrust;
}

/**
 * UDE with motor dynamics.   					-qyp_ude
 * Input: 'vehicle_attitude_setpoint' topics (depending on mode)
 * Output: '_ude.u_total' vector, '_ude.thrust_sp'
 */
void
MulticopterAttitudeControl::control_attitude_m_ude(float dt)
{
	/* reset integral if disarmed */
	if (_ude.thrust_sp < MIN_TAKEOFF_THRUST||!_v_control_mode.flag_armed || !_vehicle_status.is_rotary_wing) 
	{
		integral_ude.zero();

		_ude.torque_est[0] = 0.0f;
		_ude.torque_est[1] = 0.0f;
		_ude.torque_est[2] = 0.0f;

		_ude.f1_est[0] = 0.0f;
		_ude.f1_est[1] = 0.0f;
		_ude.f1_est[2] = 0.0f;

		_ude.f1_dot_est[0] = 0.0f;
		_ude.f1_dot_est[1] = 0.0f;
		_ude.f1_dot_est[2] = 0.0f;

		_ude.f2_est[0] = 0.0f;
		_ude.f2_est[1] = 0.0f;
		_ude.f2_est[2] = 0.0f;

		_ude.f_est[0] = 0.0f;
		_ude.f_est[1] = 0.0f;
		_ude.f_est[2] = 0.0f;
	}

	//yaw control using cascade PID
	control_attitude(dt);

	control_attitude_rates(dt);

	//Error for attitude_rate
	for (int i = 0; i < 3; i++) 
	{
		_ude.error_attitude_rate[i] = _ude.attitude_dot_ref[i] - _ude.attitude_rate_now[i];
	}

	// update the torque reference
	for (int i = 0; i < 2; i++) 
	{
		_ude.torque_ref[i] = I_quadrotor(i) * _ude.attitude_ddot_ref[i];
	}
	
	_ude.torque_est[0] = LPFdelay[0].update(_ude.u_total[0], dt);
	_ude.torque_est[1] = LPFdelay[1].update(_ude.u_total[1], dt);

	// update the disturbance estimation
	for (int i = 0; i < 2; i++) 
	{
		_ude.f1_est[i] = I_quadrotor(i) * HPF[i].update(_ude.attitude_rate_now[i],dt) - LPF[i].update(_ude.torque_est[i],dt);
		
		_ude.f1_dot_est[i] = I_quadrotor(i) * HPF2[i].update(_ude.attitude_rate_now[i],dt) - BPF[i].update(_ude.torque_est[i],dt);

		_ude.f2_est[i] = 1.0f/T_torque * _ude.torque_est[i] + 1.0f/(T_torque*MOTOR_ALPHA)*integral_ude(i);
		
		_ude.f_est[i] = MOTOR_ALPHA * _ude.f2_est[i] + _ude.f1_est[i] + MOTOR_ALPHA * _ude.f1_dot_est[i];

		_ude.f2[i] =1.0f/(MOTOR_ALPHA)* (_ude.u_total[i] - LPFdelay[i].get_delay_output());
	}

	//roll and pitch control using UDE
	for (int i = 0; i < 2; i++) 
	{
		_ude.feedforward[i] = I_quadrotor(i) * (_ude.attitude_ddot_ref[i] + MOTOR_ALPHA * _ude.attitude_dddot_ref[i]);
		
		_ude.u_l_kp[i] = Kp_ude(i) * _ude.error_attitude[i];

		_ude.u_l_kd[i] = Kd_ude(i) * _ude.error_attitude_rate[i];

		_ude.u_l_km[i] = Km_ude(i) * ( _ude.torque_ref[i] - _ude.torque_est[i]);

		_ude.u_d[i] = + Km_ude(i) * _ude.f1_est[i] + _ude.f_est[i];
	}

	//当开始推油门到MIN_TAKEOFF_THRUST之后才开始积分
	if (_ude.thrust_sp > MIN_TAKEOFF_THRUST) 
	{
		for (int i = 0; i < 2; i++) 
		{
			// Perform the integration using a first order method and do not propagate the result if out of range or invalid
			 float integral;
			 integral = integral_ude(i) + dt * (_ude.torque_est[i] - _ude.feedforward[i] - _ude.u_l_kp[i] - _ude.u_l_kd[i] - _ude.u_l_km[i] + (Km_ude(i) + 1) * _ude.f1_est[i] + MOTOR_ALPHA * _ude.f1_dot_est[i]);

			if (PX4_ISFINITE(integral) && integral > -integral_limit_ude(i) && integral < integral_limit_ude(i)) 
			{
				integral_ude(i) = integral;
			}
		}
	}

	for (int i = 0; i < 2; i++) 
	{
		_ude.u_total[i] = _ude.feedforward[i] + _ude.u_l_kp[i] + _ude.u_l_kd[i] + _ude.u_l_km[i]  - _ude.u_d[i];
		//
	}

	print_time = print_time + dt;

	if (print_time - last_print_time > 5.0f) 
	{
		last_print_time = print_time;
		//mavlink_log_info(&mavlink_log_pub, "torque_est[0]: %f, torque_est[1]: %f", &_ude.torque_est[0], &_ude.torque_est[1]);
		//mavlink_log_info(&mavlink_log_pub, "torque_des[0]: %f, torque_des[1]: %f", &_ude.u_total[0], &_ude.u_total[1]);
	}	


}



/**
 * PD+UDE Attitude and Attitude_rate controller.   					-qyp_ude
 * Input: 'vehicle_attitude_setpoint' topics (depending on mode)
 * Output: '_ude.u_total' vector, '_ude.thrust_sp'
 */
void
MulticopterAttitudeControl::control_attitude_ude(float dt)
{
	/* reset integral if disarmed */
	if (!_v_control_mode.flag_armed || !_vehicle_status.is_rotary_wing) {
		integral_ude.zero();
	}

	//yaw control using cascade PID
	control_attitude(dt);

	control_attitude_rates(dt);

	//using high-pass filter to get the attitude_dot_ref
	for (int i = 0; i < 2; i++) 
	{
		_ude.attitude_dot_ref_hpf[i] = 1.0f/(T_filter_ude + dt) * (T_filter_ude * attitude_dot_sp_last(i) + _ude.attitude_ref[i] - attitude_sp_last(i));
	}	

	/* limit rates */
	for (int i = 0; i < 2; i++) 
	{
		_ude.attitude_dot_ref_hpf[i] = math::constrain(_ude.attitude_dot_ref_hpf[i], -4.0f, 4.0f);
	}

	attitude_sp_last(0) = _ude.attitude_ref[0];
	attitude_sp_last(1) = _ude.attitude_ref[1];
	attitude_dot_sp_last = _ude.attitude_dot_ref_hpf;																																																																																																																																																																																																																																																																																							


    //Error for attitude_rate
	for (int i = 0; i < 3; i++) 
	{
		if(switch_td == 0)
		{
			_ude.error_attitude_rate[i] = _ude.attitude_dot_ref[i] - _ude.attitude_rate_now[i];
		}else if(switch_td == 1)
		{
			_ude.error_attitude_rate[i] = _ude.attitude_dot_ref_hpf[i] - _ude.attitude_rate_now[i];
		}
	}

	//roll and pitch control using UDE
	for (int i = 0; i < 2; i++) 
	{
		_ude.feedforward[i] = I_quadrotor(i) * _ude.attitude_ddot_ref[i];
		_ude.u_l_kp[i] = Kp_ude(i) * _ude.error_attitude[i];
		_ude.u_l_kd[i] = Kd_ude(i) * _ude.error_attitude_rate[i];
		_ude.u_d[i] = I_quadrotor(i) / T_ude(i) * _ude.error_attitude_rate[i] + 1.0f / T_ude(i) * integral_ude(i);
	}	

	//当开始推油门到MIN_TAKEOFF_THRUST之后才开始积分
	if (_ude.thrust_sp > MIN_TAKEOFF_THRUST) 
	{
		for (int i = 0; i < 2; i++) 
		{
			// Perform the integration using a first order method and do not propagate the result if out of range or invalid
			 float integral = integral_ude(i) - dt *( _ude.feedforward[i] + Kp_ude(i) * _ude.error_attitude[i]  + Kd_ude(i) * _ude.error_attitude_rate[i]);

			if (PX4_ISFINITE(integral) && integral > -integral_limit_ude(i) && integral < integral_limit_ude(i)) 
			{
				integral_ude(i) = integral;
			}
		}
	}

	for (int i = 0; i < 2; i++) 
	{
		_ude.u_d[i] = math::constrain(_ude.u_d[i], -integral_limit_ude(i), integral_limit_ude(i));
	}

	for (int i = 0; i < 2; i++) 
	{
		_ude.u_total[i] = _ude.feedforward[i] + _ude.u_l_kp[i] + _ude.u_l_kd[i] - _ude.u_d[i];
	}	
}

/**
 * Attitude controller.
 * Input: 'vehicle_attitude_setpoint' topics (depending on mode)
 * Output: '_rates_sp' vector, '_thrust_sp'
 */
void
MulticopterAttitudeControl::control_attitude(float dt)
{
	vehicle_attitude_setpoint_poll();
	_thrust_sp = _v_att_sp.thrust;

	/* prepare yaw weight from the ratio between roll/pitch and yaw gains */
	Vector3f attitude_gain = _attitude_p;
	const float roll_pitch_gain = (attitude_gain(0) + attitude_gain(1)) / 2.f;
	const float yaw_w = math::constrain(attitude_gain(2) / roll_pitch_gain, 0.f, 1.f);
	attitude_gain(2) = roll_pitch_gain;

	/* get estimated and desired vehicle attitude */
	Quatf q(_v_att.q);
	Quatf qd(_v_att_sp.q_d);

	Eulerf att_ref(qd);
	Eulerf _attitude_now = q;
	float att_dot_ref[3];
	float att_ddot_ref[3];
	float att_dddot_ref[3];

	att_dot_ref[0] = 0.0f;
	att_dot_ref[1] = 0.0f;
	att_dot_ref[2] = 0.0f;

	att_ddot_ref[0] = 0.0f;
	att_ddot_ref[1] = 0.0f;
	att_ddot_ref[2] = 0.0f;

	att_dddot_ref[0] = 0.0f;
	att_dddot_ref[1] = 0.0f;
	att_dddot_ref[2] = 0.0f;

	// choose normal mode or platform mode. if in platform mode, select the input source
	if (use_platform == 1)
	{
		_thrust_sp = 0.4f;

		float Kp_att = 4.0f;

		// roll_sp = 0, pitch_sp =0, yaw_sp = yaw_now
		if (input_source == 0)
		{
			input_source_time = 0.0f;
			att_ref(0) = 0.0f;
			att_ref(1) = 0.0f;

			qd = att_ref;

			_ude.input_time = 0.0f;
		}
		// step input
		else if (input_source == 1)
		{
			_ude.input_time =  _ude.input_time + dt;

			if(input_source_time < 5)
			{
				att_ref(1) = 0.0f;
			}else if(input_source_time < 15)
			{
				att_ref(1) = 20.0f / 57.3f;
			}else if(input_source_time < 25)
			{
				att_ref(1) = -20.0f / 57.3f;
			}else
			{
				att_ref(1) = 0.0f;
			}

			att_dot_ref[1] = Kp_att*(att_ref(1) - _attitude_now(1));

			att_dot_ref[1] = math::constrain(att_dot_ref[1], -4.f, 4.f);

			att_ddot_ref[1] = HPF_td[0].update(att_dot_ref[1], dt);

			att_ddot_ref[1] = math::constrain(att_ddot_ref[1], -50.f, 50.f);

			att_dddot_ref[1] = HPF_td[1].update(att_ddot_ref[1], dt);

			att_dddot_ref[1] = math::constrain(att_dddot_ref[1], -100.f, 100.f);
			
			qd = att_ref;

			input_source_time = input_source_time + dt;
		}
		// sin input
		else if (input_source == 2)
		{
			_ude.input_time =  _ude.input_time + dt;

			float cos_angle = 30.0f / 57.3f * cos(omega * input_source_time);
    		float sin_angle = 30.0f / 57.3f * sin(omega * input_source_time);

			att_ref(1) = sin_angle;

			att_dot_ref[1] =  omega * cos_angle;

			// only in this case , attitude_ddot_ref is not zero.
			att_ddot_ref[1] = -  omega * omega * sin_angle;

			att_dddot_ref[1] = - omega * omega * omega * cos_angle;

			qd = att_ref;

			input_source_time = input_source_time + dt;
		}
		else if (input_source == 3)
		{
			_ude.input_time =  _ude.input_time + dt;


			if(input_source_time < 5)
			{
				att_ref(1) = 0.0f;
			}else if(input_source_time < 10)
			{
				att_ref(1) = 30.0f / 57.3f;
			}else if(input_source_time < 15)
			{
				att_ref(1) = -30.0f / 57.3f;
			}
			else if(input_source_time < 20)
			{
				att_ref(1) = 0.0f;
			}
			else if(input_source_time < 30)
			{
			    float cos_angle = 30.0f / 57.3f * cos(omega * (input_source_time-20.0f));
				float sin_angle = 30.0f / 57.3f * sin(omega * (input_source_time-20.0f));

				att_ref(1) = sin_angle;

				att_dot_ref[1] =  omega * cos_angle;

				// only in this case , attitude_ddot_ref is not zero.
				att_ddot_ref[1] = -  omega * omega * sin_angle;

				att_dddot_ref[1] = - omega * omega * omega * cos_angle;
			}
			else if (input_source_time < 40)
			{
				att_ref(1) = 0.0f;
			}

			qd = att_ref;

			input_source_time = input_source_time + dt;
		}
	}

	/* ensure input quaternions are exactly normalized because acosf(1.00001) == NaN */
	q.normalize();
	qd.normalize();

	/* calculate reduced desired attitude neglecting vehicle's yaw to prioritize roll and pitch */
	Vector3f e_z = q.dcm_z();
	Vector3f e_z_d = qd.dcm_z();
	Quatf qd_red(e_z, e_z_d);

	if (abs(qd_red(1)) > (1.f - 1e-5f) || abs(qd_red(2)) > (1.f - 1e-5f)) {
		/* In the infinitesimal corner case where the vehicle and thrust have the completely opposite direction,
		 * full attitude control anyways generates no yaw input and directly takes the combination of
		 * roll and pitch leading to the correct desired yaw. Ignoring this case would still be totally safe and stable. */
		qd_red = qd;

	} else {
		/* transform rotation from current to desired thrust vector into a world frame reduced desired attitude */
		qd_red *= q;
	}

	/* mix full and reduced desired attitude */
	Quatf q_mix = qd_red.inversed() * qd;
	q_mix *= math::signNoZero(q_mix(0));
	/* catch numerical problems with the domain of acosf and asinf */
	q_mix(0) = math::constrain(q_mix(0), -1.f, 1.f);
	q_mix(3) = math::constrain(q_mix(3), -1.f, 1.f);
	qd = qd_red * Quatf(cosf(yaw_w * acosf(q_mix(0))), 0, 0, sinf(yaw_w * asinf(q_mix(3))));

	/* quaternion attitude cattitude_dot_refontrol law, qe is rotation from q to qd */
	Quatf qe = q.inversed() * qd;

	/* using sin(alpha/2) scaled rotation axis as attitude error (see quaternion definition by axis angle)
	 * also taking care of the antipodal unit quaternion ambiguity */
	Vector3f eq = 2.f * math::signNoZero(qe(0)) * qe.imag();

	/* calculate angular rates setpoint */
	_rates_sp = eq.emult(attitude_gain);

	/* Feed forward the yaw setpoint rate.
	 * The yaw_feedforward_rate is a commanded rotation around the world z-axis,
	 * but we need to apply it in the body frame (because _rates_sp is expressed in the body frame).
	 * Therefore we infer the world z-axis (expressed in the body frame) by taking the last column of R.transposed (== q.inversed)
	 * and multiply it by the yaw setpoint rate (yaw_sp_move_rate) and gain (_yaw_ff).
	 * This yields a vector representing the commanded rotatation around the world z-axis expressed in the body frame
	 * such that it can be added to the rates setpoint.
	 */
	Vector3f yaw_feedforward_rate = q.inversed().dcm_z();
	yaw_feedforward_rate *= _v_att_sp.yaw_sp_move_rate * _yaw_ff.get();
	_rates_sp += yaw_feedforward_rate;


	/* limit rates */
	for (int i = 0; i < 3; i++) {
		if ((_v_control_mode.flag_control_velocity_enabled || _v_control_mode.flag_control_auto_enabled) &&
		    !_v_control_mode.flag_control_manual_enabled) {
			_rates_sp(i) = math::constrain(_rates_sp(i), -_auto_rate_max(i), _auto_rate_max(i));

		} else {
			_rates_sp(i) = math::constrain(_rates_sp(i), -_mc_rate_max(i), _mc_rate_max(i));
		}
	}

	/* VTOL weather-vane mode, dampen yaw rate */
	if (_vehicle_status.is_vtol && _v_att_sp.disable_mc_yaw_control) {
		if (_v_control_mode.flag_control_velocity_enabled || _v_control_mode.flag_control_auto_enabled) {

			const float wv_yaw_rate_max = _auto_rate_max(2) * _vtol_wv_yaw_rate_scale.get();
			_rates_sp(2) = math::constrain(_rates_sp(2), -wv_yaw_rate_max, wv_yaw_rate_max);

			// prevent integrator winding up in weathervane mode
			_rates_int(2) = 0.0f;
		}
	}


	// choose normal mode or platform mode. if in platform mode, select the input source
	if (use_platform == 1 && switch_ude != 0)
	{
		for (int i = 0; i < 3; i++) 
		{
			_rates_sp(i) = att_dot_ref[i];
		}
		
	}

	//For log

	_ude.thrust_sp = _thrust_sp;

	

	for (int i = 0; i < 3; i++)
	{
		_ude.attitude_ref[i] = att_ref(i);
		_ude.attitude_dot_ref[i] = _rates_sp(i);
		_ude.attitude_ddot_ref[i] = att_ddot_ref[i];
		_ude.attitude_dddot_ref[i] = att_dddot_ref[i];
		
		_ude.attitude_now[i] = _attitude_now(i);
		
		_ude.error_attitude[i] = _ude.attitude_ref[i] - _ude.attitude_now[i];
	}

	// _ude.attitude_rate_now[0] = _v_att.rollspeed;
	// _ude.attitude_rate_now[1] = _v_att.pitchspeed;
	// _ude.attitude_rate_now[2] = _v_att.yawspeed;

}

/*
 * Throttle PID attenuation
 * Function visualization available here https://www.desmos.com/calculator/gn4mfoddje
 * Input: 'tpa_breakpoint', 'tpa_rate', '_thrust_sp'
 * Output: 'pidAttenuationPerAxis' vector
 */
Vector3f
MulticopterAttitudeControl::pid_attenuations(float tpa_breakpoint, float tpa_rate)
{
	/* throttle pid attenuation factor */
	float tpa = 1.0f - tpa_rate * (fabsf(_v_rates_sp.thrust) - tpa_breakpoint) / (1.0f - tpa_breakpoint);
	tpa = fmaxf(TPA_RATE_LOWER_LIMIT, fminf(1.0f, tpa));

	Vector3f pidAttenuationPerAxis;
	pidAttenuationPerAxis(AXIS_INDEX_ROLL) = tpa;
	pidAttenuationPerAxis(AXIS_INDEX_PITCH) = tpa;
	pidAttenuationPerAxis(AXIS_INDEX_YAW) = 1.0;

	return pidAttenuationPerAxis;
}

/*
 * Attitude rates controller.
 * Input: '_rates_sp' vector, '_thrust_sp'
 * Output: '_att_control' vector
 */
void
MulticopterAttitudeControl::control_attitude_rates(float dt)
{
	/* reset integral if disarmed */
	if (!_v_control_mode.flag_armed || !_vehicle_status.is_rotary_wing) {
		_rates_int.zero();
	}

	// get the raw gyro data and correct for thermal errors
	Vector3f rates;

	if (_selected_gyro == 0) {
		rates(0) = (_sensor_gyro.x - _sensor_correction.gyro_offset_0[0]) * _sensor_correction.gyro_scale_0[0];
		rates(1) = (_sensor_gyro.y - _sensor_correction.gyro_offset_0[1]) * _sensor_correction.gyro_scale_0[1];
		rates(2) = (_sensor_gyro.z - _sensor_correction.gyro_offset_0[2]) * _sensor_correction.gyro_scale_0[2];

	} else if (_selected_gyro == 1) {
		rates(0) = (_sensor_gyro.x - _sensor_correction.gyro_offset_1[0]) * _sensor_correction.gyro_scale_1[0];
		rates(1) = (_sensor_gyro.y - _sensor_correction.gyro_offset_1[1]) * _sensor_correction.gyro_scale_1[1];
		rates(2) = (_sensor_gyro.z - _sensor_correction.gyro_offset_1[2]) * _sensor_correction.gyro_scale_1[2];

	} else if (_selected_gyro == 2) {
		rates(0) = (_sensor_gyro.x - _sensor_correction.gyro_offset_2[0]) * _sensor_correction.gyro_scale_2[0];
		rates(1) = (_sensor_gyro.y - _sensor_correction.gyro_offset_2[1]) * _sensor_correction.gyro_scale_2[1];
		rates(2) = (_sensor_gyro.z - _sensor_correction.gyro_offset_2[2]) * _sensor_correction.gyro_scale_2[2];

	} else {
		rates(0) = _sensor_gyro.x;
		rates(1) = _sensor_gyro.y;
		rates(2) = _sensor_gyro.z;
	}

	// rotate corrected measurements from sensor to body frame
	rates = _board_rotation * rates;

	// correct for in-run bias errors
	rates(0) -= _sensor_bias.gyro_x_bias;
	rates(1) -= _sensor_bias.gyro_y_bias;
	rates(2) -= _sensor_bias.gyro_z_bias;

	Vector3f rates_p_scaled = _rate_p.emult(pid_attenuations(_tpa_breakpoint_p.get(), _tpa_rate_p.get()));
	Vector3f rates_i_scaled = _rate_i.emult(pid_attenuations(_tpa_breakpoint_i.get(), _tpa_rate_i.get()));
	Vector3f rates_d_scaled = _rate_d.emult(pid_attenuations(_tpa_breakpoint_d.get(), _tpa_rate_d.get()));

	/* angular rates error */
	Vector3f rates_err = _rates_sp - rates;

	/* apply low-pass filtering to the rates for D-term */
	Vector3f rates_filtered(
		_lp_filters_d[0].apply(rates(0)),
		_lp_filters_d[1].apply(rates(1)),
		_lp_filters_d[2].apply(rates(2)));

	_att_control = rates_p_scaled.emult(rates_err) +
		       _rates_int -
		       rates_d_scaled.emult(rates_filtered - _rates_prev_filtered) / dt +
		       _rate_ff.emult(_rates_sp);

	_rates_prev = rates;
	_rates_prev_filtered = rates_filtered;

	/* update integral only if motors are providing enough thrust to be effective */
	if (_thrust_sp > MIN_TAKEOFF_THRUST) {
		for (int i = AXIS_INDEX_ROLL; i < AXIS_COUNT; i++) {
			// Check for positive control saturation
			bool positive_saturation =
				((i == AXIS_INDEX_ROLL) && _saturation_status.flags.roll_pos) ||
				((i == AXIS_INDEX_PITCH) && _saturation_status.flags.pitch_pos) ||
				((i == AXIS_INDEX_YAW) && _saturation_status.flags.yaw_pos);

			// Check for negative control saturation
			bool negative_saturation =
				((i == AXIS_INDEX_ROLL) && _saturation_status.flags.roll_neg) ||
				((i == AXIS_INDEX_PITCH) && _saturation_status.flags.pitch_neg) ||
				((i == AXIS_INDEX_YAW) && _saturation_status.flags.yaw_neg);

			// prevent further positive control saturation
			if (positive_saturation) {
				rates_err(i) = math::min(rates_err(i), 0.0f);

			}

			// prevent further negative control saturation
			if (negative_saturation) {
				rates_err(i) = math::max(rates_err(i), 0.0f);

			}

			// Perform the integration using a first order method and do not propagate the result if out of range or invalid
			float rate_i = _rates_int(i) + rates_i_scaled(i) * rates_err(i) * dt;

			if (PX4_ISFINITE(rate_i) && rate_i > -_rate_int_lim(i) && rate_i < _rate_int_lim(i)) {
				_rates_int(i) = rate_i;

			}
		}
	}

	/* explicitly limit the integrator state */
	for (int i = AXIS_INDEX_ROLL; i < AXIS_COUNT; i++) {
		_rates_int(i) = math::constrain(_rates_int(i), -_rate_int_lim(i), _rate_int_lim(i));
	}

	//Copy the attitude rate
	for (int i = 0; i < 3; i++)
	{
		_ude.attitude_rate_now[i] = rates(i);

	}

	_ude.u_total[2] = _att_control(2);

}

void
MulticopterAttitudeControl::run()
{

	/*
	 * do subscriptions
	 */
	_v_att_sub = orb_subscribe(ORB_ID(vehicle_attitude));
	_v_att_sp_sub = orb_subscribe(ORB_ID(vehicle_attitude_setpoint));
	_v_rates_sp_sub = orb_subscribe(ORB_ID(vehicle_rates_setpoint));
	_v_control_mode_sub = orb_subscribe(ORB_ID(vehicle_control_mode));
	_params_sub = orb_subscribe(ORB_ID(parameter_update));
	_manual_control_sp_sub = orb_subscribe(ORB_ID(manual_control_setpoint));
	_vehicle_status_sub = orb_subscribe(ORB_ID(vehicle_status));
	_motor_limits_sub = orb_subscribe(ORB_ID(multirotor_motor_limits));
	_battery_status_sub = orb_subscribe(ORB_ID(battery_status));
	_outputs_sub = orb_subscribe(ORB_ID(actuator_outputs));

	_gyro_count = math::min(orb_group_count(ORB_ID(sensor_gyro)), MAX_GYRO_COUNT);

	if (_gyro_count == 0) {
		_gyro_count = 1;
	}

	for (unsigned s = 0; s < _gyro_count; s++) {
		_sensor_gyro_sub[s] = orb_subscribe_multi(ORB_ID(sensor_gyro), s);
	}

	_sensor_correction_sub = orb_subscribe(ORB_ID(sensor_correction));

	// sensor correction topic is not being published regularly and we might have missed the first update.
	// so copy it once initially so that we have the latest data. In future this will not be needed anymore as the
	// behavior of the orb_check function will change
	if (_sensor_correction_sub > 0) {
		orb_copy(ORB_ID(sensor_correction), _sensor_correction_sub, &_sensor_correction);
	}

	_sensor_bias_sub = orb_subscribe(ORB_ID(sensor_bias));

	/* wakeup source: gyro data from sensor selected by the sensor app */
	px4_pollfd_struct_t poll_fds = {};
	poll_fds.events = POLLIN;

	const hrt_abstime task_start = hrt_absolute_time();
	hrt_abstime last_run = task_start;
	float dt_accumulator = 0.f;
	int loop_counter = 0;

	while (!should_exit()) {

		poll_fds.fd = _sensor_gyro_sub[_selected_gyro];

		/* wait for up to 100ms for data */
		int pret = px4_poll(&poll_fds, 1, 100);

		/* timed out - periodic check for should_exit() */
		if (pret == 0) {
			continue;
		}

		/* this is undesirable but not much we can do - might want to flag unhappy status */
		if (pret < 0) {
			PX4_ERR("poll error %d, %d", pret, errno);
			/* sleep a bit before next try */
			usleep(100000);
			continue;
		}

		perf_begin(_loop_perf);

		/* run controller on gyro changes */
		if (poll_fds.revents & POLLIN) {
			const hrt_abstime now = hrt_absolute_time();
			float dt = (now - last_run) / 1e6f;
			last_run = now;

			/* guard against too small (< 0.2ms) and too large (> 20ms) dt's */
			if (dt < 0.0002f) {
				dt = 0.0002f;

			} else if (dt > 0.02f) {
				dt = 0.02f;
			}

			/* copy gyro data */
			orb_copy(ORB_ID(sensor_gyro), _sensor_gyro_sub[_selected_gyro], &_sensor_gyro);

			/* check for updates in other topics */
			parameter_update_poll();
			vehicle_control_mode_poll();
			vehicle_manual_poll();
			vehicle_status_poll();
			vehicle_motor_limits_poll();
			battery_status_poll();
			vehicle_attitude_poll();
			sensor_correction_poll();
			sensor_bias_poll();

			/* Check if we are in rattitude mode and the pilot is above the threshold on pitch
			 * or roll (yaw can rotate 360 in normal att control).  If both are true don't
			 * even bother running the attitude controllers */
			if (_v_control_mode.flag_control_rattitude_enabled) {
				if (fabsf(_manual_control_sp.y) > _rattitude_thres.get() ||
				    fabsf(_manual_control_sp.x) > _rattitude_thres.get()) {
					_v_control_mode.flag_control_attitude_enabled = false;
				}
			}


			// start ude control - qyp
			if (switch_ude != 0 )
			{
				if (switch_ude == 1)
				{
					control_attitude_ude(dt);
				}else if(switch_ude == 2)
				{
					control_attitude_cascade_ude(dt);
				}else if(switch_ude == 3)
				{
					control_attitude_m_ude(dt);
				}

				if (switch_mixer == 0)
				{

					// choose normal mode or platform mode. if in platform mode, select the input source
					if (use_platform == 1)
					{
						_ude.u_total[0] = 0.0f;
						_ude.u_total[2] = 0.0f;
					}


					_actuators.control[0] = (PX4_ISFINITE(_ude.u_total[0])) ? _ude.u_total[0] : 0.0f;
					_actuators.control[1] = (PX4_ISFINITE(_ude.u_total[1])) ? _ude.u_total[1] : 0.0f;
					_actuators.control[2] = (PX4_ISFINITE(_ude.u_total[2])) ? _ude.u_total[2] : 0.0f;
					_actuators.control[3] = (PX4_ISFINITE(_ude.thrust_sp)) ? _ude.thrust_sp : 0.0f;
				}else
				{


					if (use_platform == 1)
					{
						_ude.u_total[0] = 0.0f;
						_ude.u_total[2] = 0.0f;
					}

					mixer(_ude.u_total[0],_ude.u_total[1],_ude.u_total[2],_ude.thrust_sp);

					// choose normal mode or platform mode. if in platform mode, select the input source
					if (use_platform == 1)
					{
						_mixer.output_roll = 0.0f;
						_mixer.output_yaw = 0.0f;
					}

					_actuators.control[0] = (PX4_ISFINITE(_mixer.output_roll)) ? _mixer.output_roll : 0.0f;
					_actuators.control[1] = (PX4_ISFINITE(_mixer.output_pitch)) ? _mixer.output_pitch : 0.0f;
					_actuators.control[2] = (PX4_ISFINITE(_mixer.output_yaw)) ? _mixer.output_yaw : 0.0f;
					_actuators.control[3] = (PX4_ISFINITE(_mixer.output_thrust)) ? _mixer.output_thrust : 0.0f;
				}
				_actuators.control[7] = _v_att_sp.landing_gear;
				_actuators.timestamp = hrt_absolute_time();
				_actuators.timestamp_sample = _sensor_gyro.timestamp;


				//Publish the _actuators first
				/* scale effort by battery status */
				if (_bat_scale_en.get() && _battery_status.scale > 0.0f) {
					for (int i = 0; i < 4; i++) {
						_actuators.control[i] *= _battery_status.scale;
					}
				}

				if (!_actuators_0_circuit_breaker_enabled) {
					if (_actuators_0_pub != nullptr) {

						orb_publish(_actuators_id, _actuators_0_pub, &_actuators);

					} else if (_actuators_id) {
						_actuators_0_pub = orb_advertise(_actuators_id, &_actuators);
					}

				}
			}
			// default pid control
			else
			{
				if (_v_control_mode.flag_control_attitude_enabled) {

					control_attitude(dt);

					/* publish attitude rates setpoint */
					_v_rates_sp.roll = _rates_sp(0);
					_v_rates_sp.pitch = _rates_sp(1);
					_v_rates_sp.yaw = _rates_sp(2);
					_v_rates_sp.thrust = _thrust_sp;
					_v_rates_sp.timestamp = hrt_absolute_time();

					if (_v_rates_sp_pub != nullptr) {
						orb_publish(_rates_sp_id, _v_rates_sp_pub, &_v_rates_sp);

					} else if (_rates_sp_id) {
						_v_rates_sp_pub = orb_advertise(_rates_sp_id, &_v_rates_sp);
					}

				} else {
					/* attitude controller disabled, poll rates setpoint topic */
					if (_v_control_mode.flag_control_manual_enabled) {
						/* manual rates control - ACRO mode */
						Vector3f man_rate_sp(
								math::superexpo(_manual_control_sp.y, _acro_expo_rp.get(), _acro_superexpo_rp.get()),
								math::superexpo(-_manual_control_sp.x, _acro_expo_rp.get(), _acro_superexpo_rp.get()),
								math::superexpo(_manual_control_sp.r, _acro_expo_y.get(), _acro_superexpo_y.get()));
						_rates_sp = man_rate_sp.emult(_acro_rate_max);
						_thrust_sp = _manual_control_sp.z;

						/* publish attitude rates setpoint */
						_v_rates_sp.roll = _rates_sp(0);
						_v_rates_sp.pitch = _rates_sp(1);
						_v_rates_sp.yaw = _rates_sp(2);
						_v_rates_sp.thrust = _thrust_sp;
						_v_rates_sp.timestamp = hrt_absolute_time();

						if (_v_rates_sp_pub != nullptr) {
							orb_publish(_rates_sp_id, _v_rates_sp_pub, &_v_rates_sp);

						} else if (_rates_sp_id) {
							_v_rates_sp_pub = orb_advertise(_rates_sp_id, &_v_rates_sp);
						}

					} else {
						/* attitude controller disabled, poll rates setpoint topic */
						vehicle_rates_setpoint_poll();
						_rates_sp(0) = _v_rates_sp.roll;
						_rates_sp(1) = _v_rates_sp.pitch;
						_rates_sp(2) = _v_rates_sp.yaw;
						_thrust_sp = _v_rates_sp.thrust;
					}
				}

				if (_v_control_mode.flag_control_rates_enabled) {
					control_attitude_rates(dt);

					// choose normal mode or platform mode. if in platform mode, select the input source
					if (use_platform == 1)
					{
						_att_control(0) = 0.0f;
						_att_control(2) = 0.0f;
					}

					/* publish actuator controls */
					_actuators.control[0] = (PX4_ISFINITE(_att_control(0))) ? _att_control(0) : 0.0f;
					_actuators.control[1] = (PX4_ISFINITE(_att_control(1))) ? _att_control(1) : 0.0f;
					_actuators.control[2] = (PX4_ISFINITE(_att_control(2))) ? _att_control(2) : 0.0f;
					_actuators.control[3] = (PX4_ISFINITE(_thrust_sp)) ? _thrust_sp : 0.0f;

					// _actuators.control[0] = 0.0f;
					// _actuators.control[1] = 0.0f;
					// _actuators.control[2] = 0.2f;
					// _actuators.control[3] = 0.5f;

					_actuators.control[7] = _v_att_sp.landing_gear;
					_actuators.timestamp = hrt_absolute_time();
					_actuators.timestamp_sample = _sensor_gyro.timestamp;

					/* scale effort by battery status */
					if (_bat_scale_en.get() && _battery_status.scale > 0.0f) {
						for (int i = 0; i < 4; i++) {
							_actuators.control[i] *= _battery_status.scale;
						}
					}

					if (!_actuators_0_circuit_breaker_enabled) {
						if (_actuators_0_pub != nullptr) {

							orb_publish(_actuators_id, _actuators_0_pub, &_actuators);

						} else if (_actuators_id) {
							_actuators_0_pub = orb_advertise(_actuators_id, &_actuators);
						}

					}

					/* publish controller status */
					rate_ctrl_status_s rate_ctrl_status;
					rate_ctrl_status.timestamp = hrt_absolute_time();
					rate_ctrl_status.rollspeed = _rates_prev(0);
					rate_ctrl_status.pitchspeed = _rates_prev(1);
					rate_ctrl_status.yawspeed = _rates_prev(2);
					rate_ctrl_status.rollspeed_integ = _rates_int(0);
					rate_ctrl_status.pitchspeed_integ = _rates_int(1);
					rate_ctrl_status.yawspeed_integ = _rates_int(2);

					int instance;
					orb_publish_auto(ORB_ID(rate_ctrl_status), &_controller_status_pub, &rate_ctrl_status, &instance, ORB_PRIO_DEFAULT);
				}
			}

	


			/* publish ude controller status */
			_ude.timestamp = hrt_absolute_time();

			_ude.start_time =  _ude.start_time + dt;

			if (_ude_pub != nullptr) {
				orb_publish(ORB_ID(ude), _ude_pub, &_ude);

			} else {
				_ude_pub = orb_advertise(ORB_ID(ude), &_ude);
			}

			if (_v_control_mode.flag_control_termination_enabled) {
				if (!_vehicle_status.is_vtol) {

					_rates_sp.zero();
					_rates_int.zero();
					integral_ude.zero();
					_thrust_sp = 0.0f;
					_att_control.zero();

					/* publish actuator controls */
					_actuators.control[0] = 0.0f;
					_actuators.control[1] = 0.0f;
					_actuators.control[2] = 0.0f;
					_actuators.control[3] = 0.0f;
					_actuators.timestamp = hrt_absolute_time();
					_actuators.timestamp_sample = _sensor_gyro.timestamp;

					if (!_actuators_0_circuit_breaker_enabled) {
						if (_actuators_0_pub != nullptr) {

							orb_publish(_actuators_id, _actuators_0_pub, &_actuators);

						} else if (_actuators_id) {
							_actuators_0_pub = orb_advertise(_actuators_id, &_actuators);
						}
					}
				}
			}

			/* calculate loop update rate while disarmed or at least a few times (updating the filter is expensive) */
			if (!_v_control_mode.flag_armed || (now - task_start) < 3300000) {
				dt_accumulator += dt;
				++loop_counter;

				if (dt_accumulator > 1.f) {
					const float loop_update_rate = (float)loop_counter / dt_accumulator;
					_loop_update_rate_hz = _loop_update_rate_hz * 0.5f + loop_update_rate * 0.5f;
					dt_accumulator = 0;
					loop_counter = 0;
					_lp_filters_d[0].set_cutoff_frequency(_loop_update_rate_hz, _d_term_cutoff_freq.get());
					_lp_filters_d[1].set_cutoff_frequency(_loop_update_rate_hz, _d_term_cutoff_freq.get());
					_lp_filters_d[2].set_cutoff_frequency(_loop_update_rate_hz, _d_term_cutoff_freq.get());
				}
			}

		}

		perf_end(_loop_perf);
	}

	orb_unsubscribe(_v_att_sub);
	orb_unsubscribe(_v_att_sp_sub);
	orb_unsubscribe(_v_rates_sp_sub);
	orb_unsubscribe(_v_control_mode_sub);
	orb_unsubscribe(_params_sub);
	orb_unsubscribe(_manual_control_sp_sub);
	orb_unsubscribe(_vehicle_status_sub);
	orb_unsubscribe(_motor_limits_sub);
	orb_unsubscribe(_battery_status_sub);

	for (unsigned s = 0; s < _gyro_count; s++) {
		orb_unsubscribe(_sensor_gyro_sub[s]);
	}

	orb_unsubscribe(_sensor_correction_sub);
	orb_unsubscribe(_sensor_bias_sub);
}

int MulticopterAttitudeControl::task_spawn(int argc, char *argv[])
{
	_task_id = px4_task_spawn_cmd("mc_att_control",
					   SCHED_DEFAULT,
					   SCHED_PRIORITY_ATTITUDE_CONTROL,
					   1700,
					   (px4_main_t)&run_trampoline,
					   (char *const *)argv);

	if (_task_id < 0) {
		_task_id = -1;
		return -errno;
	}

	return 0;
}

MulticopterAttitudeControl *MulticopterAttitudeControl::instantiate(int argc, char *argv[])
{
	return new MulticopterAttitudeControl();
}

int MulticopterAttitudeControl::custom_command(int argc, char *argv[])
{
	return print_usage("unknown command");
}

int mc_att_control_main(int argc, char *argv[])
{
	return MulticopterAttitudeControl::main(argc, argv);
}
