/****************************************************************************
 *
 *   Copyright (C) 2008-2012 PX4 Development Team. All rights reserved.
 *   Author: @author Ivan Ovinnikov <oivan@ethz.ch>
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

/*
 * @file fixedwing_control.c
 * Implementation of a fixed wing attitude and position controller.
 */

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <debug.h>
#include <math.h>
#include <termios.h>
#include <time.h>
#include <arch/board/up_hrt.h>
#include <arch/board/board.h>
#include <drivers/drv_pwm_output.h>
#include <nuttx/spi.h>
#include "../mix_and_link/mix_and_link.h"
#include "fixedwing_control.h"

__EXPORT int fixedwing_control_main(int argc, char *argv[]);

#define PID_DT 0.01f
#define PID_SCALER 0.1f
#define PID_DERIVMODE_CALC 0
#define HIL_MODE 32
#define RAD2DEG ((1.0/180.0)*M_PI)
#define AUTO -1000
#define MANUAL 3000
#define SERVO_MIN 1000
#define SERVO_MAX 2000

pthread_t control_thread;
pthread_t nav_thread;
pthread_t servo_thread;

/**
 * Servo channels function enumerator used for
 * the servo writing part
 */

enum SERVO_CHANNELS_FUNCTION {

	AIL_1    = 0,
	AIL_2    = 1,
	MOT      = 2,
	ACT_1    = 3,
	ACT_2    = 4,
	ACT_3    = 5,
	ACT_4    = 6,
	ACT_5    = 7
};

/**
 * The plane_data structure.
 *
 * The plane data structure is the local storage of all the flight information of the aircraft
 */
typedef struct {
	double lat;
	double lon;
	float alt;
	float vx;
	float vy;
	float vz;
	float yaw;
	float hdg;
	float pitch;
	float roll;
	float yawspeed;
	float pitchspeed;
	float rollspeed;
	float rollb;	/* body frame angles */
	float pitchb;
	float yawb;
	float p;
	float q;
	float r;	/* body angular rates */

	/* PID parameters*/

	float Kp_att;
	float Ki_att;
	float Kd_att;
	float Kp_pos;
	float Ki_pos;
	float Kd_pos;
	float intmax_att;
	float intmax_pos;

	/* Next waypoint*/

	float wp_x;
	float wp_y;
	float wp_z;

	/* Setpoints */

	float airspeed;
	float groundspeed;
	float roll_setpoint;
	float pitch_setpoint;
	float throttle_setpoint;

	/* Navigation mode*/
	int mode;

} plane_data_t;

/**
 * The control_outputs structure.
 *
 * The control outputs structure contains the control outputs
 * of the aircraft
 */
typedef struct {
	float roll_ailerons;
	float pitch_elevator;
	float yaw_rudder;
	float throttle;
	// set the aux values to 0 per default
	float aux1;
	float aux2;
	float aux3;
	float aux4;
	uint8_t mode;	// HIL_ENABLED: 32
	uint8_t nav_mode;
} control_outputs_t;

/**
 * Generic PID algorithm with PD scaling
 */
static float pid(float error, float error_deriv, uint16_t dt, float scaler, float K_p, float K_i, float K_d, float intmax);

/*
 * Output calculations
 */

static void calc_body_angular_rates(float roll, float pitch, float yaw, float rollspeed, float pitchspeed, float yawspeed);
static void calc_rotation_matrix(float roll, float pitch, float yaw, float x, float y, float z);
static void calc_bodyframe_angles(float roll, float pitch, float yaw);
static float calc_bearing(void);
static float calc_roll_ail(void);
static float calc_pitch_elev(void);
static float calc_yaw_rudder(float hdg);
static float calc_throttle(void);
static float calc_gnd_speed(void);
static void get_parameters(void);
static float calc_roll_setpoint(void);
static float calc_pitch_setpoint(void);
static float calc_throttle_setpoint(void);
static float calc_wp_distance(void);
static void set_plane_mode(void);

/*
 * The control, navigation and servo loop threads
 */

static void *control_loop(void *arg);
static void *nav_loop(void *arg);
static void *servo_loop(void *arg);

/****************************************************************************
 * Public Data
 ****************************************************************************/

plane_data_t plane_data;
control_outputs_t control_outputs;
float scaler = 1; //M_PI;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 *
 * Calculates the PID control output given an error. Incorporates PD scaling and low-pass filter for the derivative component.
 *
 * @param error the input error
 * @param error_deriv the derivative of the input error
 * @param dt time constant
 * @param scaler PD scaler
 * @param K_p P gain
 * @param K_i I gain
 * @param K_d D gain
 * @param intmax Integration limit
 *
 * @return the PID control output
 */

static float pid(float error, float error_deriv, uint16_t dt, float scale, float K_p, float K_i, float K_d, float intmax)
{
	float Kp = K_p;
	float Ki = K_i;
	float Kd = K_d;
	float delta_time = dt;
	float lerror;
	float imax = intmax;
	float integrator;
	float derivative;
	float lderiv;
	int fCut = 20;		/* anything above 20 Hz is considered noise - low pass filter for the derivative */
	float output = 0;

	output += error * Kp;

	if ((fabs(Kd) > 0) && (dt > 0)) {

		if (PID_DERIVMODE_CALC) {
			derivative = (error - lerror) / delta_time;

			/*
			 * discrete low pass filter, cuts out the
			 * high frequency noise that can drive the controller crazy
			 */
			float RC = 1 / (2 * M_PI * fCut);
			derivative = lderiv +
				     (delta_time / (RC + delta_time)) * (derivative - lderiv);

			/* update state */
			lerror 	= error;
			lderiv  = derivative;

		} else {
			derivative = error_deriv;
		}

		/* add in derivative component */
		output 	+= Kd * derivative;
	}

	//printf("PID derivative %i\n", (int)(1000*derivative));

	/* scale the P and D components with the PD scaler */
	output *= scale;

	/* Compute integral component if time has elapsed */
	if ((fabs(Ki) > 0) && (dt > 0)) {
		integrator 		+= (error * Ki) * scaler * delta_time;

		if (integrator < -imax) {
			integrator = -imax;

		} else if (integrator > imax) {
			integrator = imax;
		}

		output += integrator;
	}

	//printf("PID Integrator %i\n", (int)(1000*integrator));

	return output;
}

/**
 * Load parameters from global storage.
 *
 * Fetches the current parameters from the global parameter storage and writes them
 * to the plane_data structure
 */

static void get_parameters()
{
	plane_data.Kp_att = global_data_parameter_storage->pm.param_values[PARAM_PID_ATT_P];
	plane_data.Ki_att = global_data_parameter_storage->pm.param_values[PARAM_PID_ATT_I];
	plane_data.Kd_att = global_data_parameter_storage->pm.param_values[PARAM_PID_ATT_D];
	plane_data.Kp_pos = global_data_parameter_storage->pm.param_values[PARAM_PID_POS_P];
	plane_data.Ki_pos = global_data_parameter_storage->pm.param_values[PARAM_PID_POS_I];
	plane_data.Kd_pos = global_data_parameter_storage->pm.param_values[PARAM_PID_POS_D];
	plane_data.intmax_att = global_data_parameter_storage->pm.param_values[PARAM_PID_ATT_AWU];
	plane_data.intmax_pos = global_data_parameter_storage->pm.param_values[PARAM_PID_POS_AWU];
	plane_data.airspeed =  global_data_parameter_storage->pm.param_values[PARAM_AIRSPEED];
	plane_data.wp_x =  global_data_parameter_storage->pm.param_values[PARAM_WPLON];
	plane_data.wp_y =  global_data_parameter_storage->pm.param_values[PARAM_WPLAT];
	plane_data.wp_z =  global_data_parameter_storage->pm.param_values[PARAM_WPALT];
	plane_data.mode = global_data_parameter_storage->pm.param_values[PARAM_FLIGHTMODE];
}

/**
 * Calculates the body angular rates.
 *
 * Calculates the rates of the plane using inertia matrix and
 * writes them to the plane_data structure
 *
 * @param roll
 * @param pitch
 * @param yaw
 * @param rollspeed
 * @param pitchspeed
 * @param yawspeed
 *
 */
static void calc_body_angular_rates(float roll, float pitch, float yaw, float rollspeed, float pitchspeed, float yawspeed)
{
	plane_data.p = rollspeed - sinf(pitch) * yawspeed;
	plane_data.q = cosf(roll) * pitchspeed + sinf(roll) * cos(pitch) * yawspeed;
	plane_data.r = -sinf(roll) * pitchspeed + cosf(roll) * cos(pitch) * yawspeed;
}

/**
 *
 * Calculates the attitude angles in the body reference frame.
 *
 * Writes them to the plane data structure
 *
 * @param roll
 * @param pitch
 * @param yaw
 */

static void calc_bodyframe_angles(float roll, float pitch, float yaw)
{
	plane_data.rollb = cosf(yaw) * cosf(pitch) * roll +
			   (cosf(yaw) * sinf(pitch) * sinf(roll) + sinf(yaw) * cosf(roll)) * pitch
			   + (-cosf(yaw) * sinf(pitch) * cosf(roll)  + sinf(yaw) * sinf(roll)) * yaw;
	plane_data.pitchb = -sinf(yaw) * cosf(pitch) * roll +
			    (-sinf(yaw) * sinf(pitch) * sinf(roll) + cosf(yaw) * cosf(roll)) * pitch
			    + (sinf(yaw) * sinf(pitch) * cosf(roll) + cosf(yaw) * sinf(roll)) * yaw;
	plane_data.yawb = sinf(pitch) * roll - cosf(pitch) * sinf(roll) * pitch + cosf(pitch) * cosf(roll) * yaw;
}

/**
 * calc_rotation_matrix
 *
 * Calculates the rotation matrix
 *
 * @param roll
 * @param pitch
 * @param yaw
 * @param x
 * @param y
 * @param z
 *
 */

static void calc_rotation_matrix(float roll, float pitch, float yaw, float x, float y, float z)
{
	plane_data.rollb = cosf(yaw) * cosf(pitch) * x +
			   (cosf(yaw) * sinf(pitch) * sinf(roll) + sinf(yaw) * cosf(roll)) * y
			   + (-cosf(yaw) * sinf(pitch) * cosf(roll)  + sinf(yaw) * sinf(roll)) * z;
	plane_data.pitchb = -sinf(yaw) * cosf(pitch) * x +
			    (-sinf(yaw) * sinf(pitch) * sinf(roll) + cosf(yaw) * cosf(roll)) * y
			    + (sinf(yaw) * sinf(pitch) * cosf(roll) + cosf(yaw) * sinf(roll)) * z;
	plane_data.yawb = sinf(pitch) * x - cosf(pitch) * sinf(roll) * y + cosf(pitch) * cosf(roll) * z;
}

/**
 * calc_bearing
 *
 * Calculates the bearing error of the plane compared to the waypoints
 *
 * @return bearing Bearing error
 *
 */

static float calc_bearing()
{
	float bearing = 90 + atan2(-(plane_data.wp_y - plane_data.lat), (plane_data.wp_x - plane_data.lon)) * RAD2DEG;

	if (bearing < 0)
		bearing += 360;

	return bearing;
}

/**
 * calc_roll_ail
 *
 * Calculates the roll ailerons control output
 *
 * @return Roll ailerons control output (-1,1)
 */

static float calc_roll_ail()
{
	float ret = pid((plane_data.roll_setpoint - plane_data.roll) / scaler, plane_data.rollspeed, PID_DT, PID_SCALER,
			plane_data.Kp_att, plane_data.Ki_att, plane_data.Kd_att, plane_data.intmax_att);

	if (ret < -1)
		return -1;

	if (ret > 1)
		return 1;

	return ret;
}

/**
 * calc_pitch_elev
 *
 * Calculates the pitch elevators control output
 *
 * @return Pitch elevators control output (-1,1)
 */

static float calc_pitch_elev()
{
	float ret = pid((plane_data.pitch_setpoint - plane_data.pitch) / scaler, plane_data.pitchspeed, PID_DT, PID_SCALER,
			plane_data.Kp_att, plane_data.Ki_att, plane_data.Kd_att, plane_data.intmax_att);

	if (ret < -1)
		return -1;

	if (ret > 1)
		return 1;

	return ret;
}

/**
 * calc_yaw_rudder
 *
 * Calculates the yaw rudder control output (only if yaw rudder exists on the model)
 *
 * @return Yaw rudder control output (-1,1)
 *
 */

static float calc_yaw_rudder(float hdg)
{
	float ret = pid((plane_data.yaw / RAD2DEG - abs(hdg)) / scaler, plane_data.yawspeed, PID_DT, PID_SCALER,
			plane_data.Kp_pos, plane_data.Ki_pos, plane_data.Kd_pos, plane_data.intmax_pos);

	if (ret < -1)
		return -1;

	if (ret > 1)
		return 1;

	return ret;
}

/**
 * calc_throttle
 *
 * Calculates the throttle control output
 *
 * @return Throttle control output (0,1)
 */

static float calc_throttle()
{
	float ret = pid(plane_data.throttle_setpoint - calc_gnd_speed(), 0, PID_DT, PID_SCALER,
			plane_data.Kp_pos, plane_data.Ki_pos, plane_data.Kd_pos, plane_data.intmax_pos);

	if (ret < 0.2)
		return 0.2;

	if (ret > 1)
		return 1;

	return ret;
}

/**
 * calc_gnd_speed
 *
 * Calculates the ground speed using the x and y components
 *
 * Input: none (operation on global data)
 *
 * Output: Ground speed of the plane
 *
 */

static float calc_gnd_speed()
{
	float gnd_speed = sqrtf(plane_data.vx * plane_data.vx + plane_data.vy * plane_data.vy);
	return gnd_speed;
}

/**
 * calc_wp_distance
 *
 * Calculates the distance to the next waypoint
 *
 * @return the distance to the next waypoint
 *
 */

static float calc_wp_distance()
{
	return sqrtf((plane_data.lat - plane_data.wp_y) * (plane_data.lat - plane_data.wp_y) +
		     (plane_data.lon - plane_data.wp_x) * (plane_data.lon - plane_data.wp_x));
}

/**
 * calc_roll_setpoint
 *
 * Calculates the offset angle for the roll plane,
 * saturates at +- 35 deg.
 *
 * @return setpoint on which attitude control should stabilize while changing heading
 *
 */

static float calc_roll_setpoint()
{
	float setpoint = 0;

	if (plane_data.mode == TAKEOFF) {
		setpoint = 0;

	} else {
		setpoint = calc_bearing() - plane_data.yaw;

		if (setpoint < -35)
			return -35;

		if (setpoint > 35)
			return 35;
	}

	return setpoint;
}

/**
 * calc_pitch_setpoint
 *
 * Calculates the offset angle for the pitch plane
 * saturates at +- 35 deg.
 *
 * @return setpoint on which attitude control should stabilize while changing altitude
 *
 */

static float calc_pitch_setpoint()
{
	float setpoint = 0;

	if (plane_data.mode == TAKEOFF) {
		setpoint = 35;

	} else {
		setpoint = atanf((plane_data.wp_z - plane_data.alt) / calc_wp_distance()) * RAD2DEG;

		if (setpoint < -35)
			return -35;

		if (setpoint > 35)
			return 35;
	}

	return setpoint;
}

/**
 * calc_throttle_setpoint
 *
 * Calculates the throttle setpoint for different flight modes
 *
 * @return throttle output setpoint
 *
 */

static float calc_throttle_setpoint()
{
	float setpoint = 0;

	// if TAKEOFF full throttle
	if (plane_data.mode == TAKEOFF) {
		setpoint = 60;
	}

	// if CRUISE - parameter value
	if (plane_data.mode == CRUISE) {
		setpoint = plane_data.airspeed;
	}

	// if LAND no throttle
	if (plane_data.mode == LAND) {
		setpoint = 0;
	}

	return setpoint;
}

/**
 * set_plane_mode
 *
 * Sets the plane mode
 * (TAKEOFF, CRUISE, LOITER or LAND)
 *
 */

static void set_plane_mode()
{
	if (plane_data.alt < 10) {
		plane_data.mode = TAKEOFF;

	} else {
		plane_data.mode = CRUISE;
		// TODO: if reached waypoint and no further waypoint exists, go to LOITER mode
	}

	// Debug override - don't need TAKEOFF mode for now
	plane_data.mode = CRUISE;
}

/*
 * fixedwing_control_main
 *
 * @param argc number of arguments
 * @param argv argument array
 *
 * @return 0
 *
 */

int fixedwing_control_main(int argc, char *argv[])
{
	/* print text */
	printf("Fixedwing control started\n");
	usleep(100000);

	/* default values for arguments */
	char *fixedwing_uart_name = "/dev/ttyS1";
	char *commandline_usage = "\tusage: fixedwing_control -d fixedwing-devicename\n";

	/* read arguments */
	int i;

	if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--device") == 0) {
		if (argc > i + 1) {
			fixedwing_uart_name = argv[i + 1];

		} else {
			printf(commandline_usage);
			return 0;
		}
	}

	/* Set up to publish fixed wing control messages */
	struct fixedwing_control_s control;
	int fixedwing_control_pub = orb_advertise(ORB_ID(fixedwing_control), &control);

	/* Subscribe to global position, attitude and rc */
	struct vehicle_global_position_s global_pos;
	int global_pos_sub = orb_subscribe(ORB_ID(vehicle_global_position));
	struct vehicle_attitude_s att;
	int attitude_sub = orb_subscribe(ORB_ID(vehicle_attitude));
	struct rc_channels_s rc;
	int rc_sub = orb_subscribe(ORB_ID(rc_channels));

	/* Control constants */
	control_outputs.mode = HIL_MODE;
	control_outputs.nav_mode = 0;

	/* Servo setup */

	int fd;
	servo_position_t data[PWM_OUTPUT_MAX_CHANNELS];

	fd = open("/dev/pwm_servo", O_RDWR);

	if (fd < 0) {
		printf("failed opening /dev/pwm_servo\n");
	}

	ioctl(fd, PWM_SERVO_ARM, 0);

	int16_t buffer_rc[3];
	int16_t buffer_servo[3];
	mixer_data_t mixer_buffer;
	mixer_buffer.input  = buffer_rc;
	mixer_buffer.output = buffer_servo;

	mixer_conf_t mixers[3];

	mixers[0].source = PITCH;
	mixers[0].nr_actuators = 2;
	mixers[0].dest[0] = AIL_1;
	mixers[0].dest[1] = AIL_2;
	mixers[0].dual_rate[0] = 1;
	mixers[0].dual_rate[1] = 1;

	mixers[1].source = ROLL;
	mixers[1].nr_actuators = 2;
	mixers[1].dest[0] = AIL_1;
	mixers[1].dest[1] = AIL_2;
	mixers[1].dual_rate[0] = 1;
	mixers[1].dual_rate[1] = -1;

	mixers[2].source = THROTTLE;
	mixers[2].nr_actuators = 1;
	mixers[2].dest[0] = MOT;
	mixers[2].dual_rate[0] = 1;

	/*
	 * Main control, navigation and servo routine
	 */

	while(1)
	{
		/*
		 * DATA Handling
		 * Fetch current flight data
		 */

		/* get position, attitude and rc inputs */
		// XXX add error checking
		orb_copy(ORB_ID(vehicle_global_position), global_pos_sub, &global_pos);
		orb_copy(ORB_ID(vehicle_attitude), attitude_sub, &att);
		orb_copy(ORB_ID(rc_channels), rc_sub, &rc);

		/* scaling factors are defined by the data from the APM Planner
		 * TODO: ifdef for other parameters (HIL/Real world switch)
		 */

		/* position values*/
		plane_data.lat = global_pos.lat / 10000000;
		plane_data.lon = global_pos.lon / 10000000;
		plane_data.alt = global_pos.alt / 1000;
		plane_data.vx = global_pos.vx / 100;
		plane_data.vy = global_pos.vy / 100;
		plane_data.vz = global_pos.vz / 100;

		/* attitude values*/
		plane_data.roll = att.roll;
		plane_data.pitch = att.pitch;
		plane_data.yaw = att.yaw;
		plane_data.rollspeed = att.rollspeed;
		plane_data.pitchspeed = att.pitchspeed;
		plane_data.yawspeed = att.yawspeed;

		/* parameter values */
		get_parameters();

		/* Attitude control part */

//#define MUTE
#ifndef MUTE
		/******************************** DEBUG OUTPUT ************************************************************/

		printf("Parameter: %i,%i,%i,%i,%i,%i,%i,%i,%i,%i,%i,%i \n", (int)plane_data.Kp_att, (int)plane_data.Ki_att,
				(int)plane_data.Kd_att, (int)plane_data.intmax_att, (int)plane_data.Kp_pos, (int)plane_data.Ki_pos,
				(int)plane_data.Kd_pos, (int)plane_data.intmax_pos, (int)plane_data.airspeed,
				(int)plane_data.wp_x, (int)plane_data.wp_y, (int)plane_data.wp_z);

//		printf("PITCH SETPOINT: %i\n", (int)plane_data.pitch_setpoint);
//		printf("ROLL SETPOINT: %i\n", (int)plane_data.roll_setpoint);
//		printf("THROTTLE SETPOINT: %i\n", (int)calc_throttle_setpoint());

//		printf("\n\nVx: %i\t Vy: %i\t Current speed:%i\n\n", (int)plane_data.vx, (int)plane_data.vy, (int)(calc_gnd_speed()));

//		printf("Current Altitude: %i\n\n", (int)plane_data.alt);

		printf("\nAttitude values: \n R:%i \n P: %i \n Y: %i \n\n RS: %i \n PS: %i \n YS: %i \n ",
				(int)(1000 * plane_data.roll), (int)(1000 * plane_data.pitch), (int)(1000 * plane_data.yaw),
				(int)(100 * plane_data.rollspeed), (int)(100 * plane_data.pitchspeed), (int)(100 * plane_data.yawspeed));

//		printf("\nBody Rates: \n P: %i \n Q: %i \n R: %i \n",
//				(int)(10000 * plane_data.p), (int)(10000 * plane_data.q), (int)(10000 * plane_data.r));

		printf("\nCalculated outputs: \n R: %i\n P: %i\n Y: %i\n T: %i \n",
				(int)(10000 * control_outputs.roll_ailerons), (int)(10000 * control_outputs.pitch_elevator),
				(int)(10000 * control_outputs.yaw_rudder), (int)(10000 * control_outputs.throttle));

		/************************************************************************************************************/

#endif

		/*
		 * Computation section
		 *
		 * The function calls to compute the required control values happen
		 * in this section.
		 */

		/* Set plane mode */
		set_plane_mode();

		/* Calculate the P,Q,R body rates of the aircraft */
		calc_body_angular_rates(plane_data.roll / RAD2DEG, plane_data.pitch / RAD2DEG, plane_data.yaw / RAD2DEG,
				plane_data.rollspeed, plane_data.pitchspeed, plane_data.yawspeed);

		/* Calculate the body frame angles of the aircraft */
		//calc_bodyframe_angles(plane_data.roll/RAD2DEG,plane_data.pitch/RAD2DEG,plane_data.yaw/RAD2DEG);

		/* Calculate the output values */
		control_outputs.roll_ailerons = calc_roll_ail();
		control_outputs.pitch_elevator = calc_pitch_elev();
		//control_outputs.yaw_rudder = calc_yaw_rudder();
		control_outputs.throttle = calc_throttle();

		if (rc.chan[rc.function[OVERRIDE]].scale < MANUAL) { // if we're flying in automated mode

			if (plane_data.mode == TAKEOFF) {
				control.attitude_control_output[ROLL] = 0;
				control.attitude_control_output[PITCH] = 5000;
				control.attitude_control_output[THROTTLE] = 10000;
				//global_data_fixedwing_control->attitude_control_output[YAW] = (int16_t)(control_outputs.yaw_rudder);
			}

			if (plane_data.mode == CRUISE) {
				control.attitude_control_output[ROLL] = (int16_t)(10000 * control_outputs.roll_ailerons);
				control.attitude_control_output[PITCH] = (int16_t)(10000 * control_outputs.pitch_elevator);
				control.attitude_control_output[THROTTLE] = (int16_t)(10000 * control_outputs.throttle);
				//control->attitude_control_output[YAW] = (int16_t)(control_outputs.yaw_rudder);
			}

			control.counter++;
			control.timestamp = hrt_absolute_time();
		}

		/* Navigation part */

		// Get GPS Waypoint

		//		if(global_data_wait(&global_data_position_setpoint->access_conf) == 0)
		//		{
		//			plane_data.wp_x = global_data_position_setpoint->x;
		//			plane_data.wp_y = global_data_position_setpoint->y;
		//			plane_data.wp_z = global_data_position_setpoint->z;
		//		}
		//		global_data_unlock(&global_data_position_setpoint->access_conf);

		if (rc.chan[rc.function[OVERRIDE]].scale < MANUAL) {	// AUTO mode
			// AUTO/HYBRID switch

			if (rc.chan[rc.function[OVERRIDE]].scale < AUTO) {
				plane_data.roll_setpoint = calc_roll_setpoint();
				plane_data.pitch_setpoint = calc_pitch_setpoint();
				plane_data.throttle_setpoint = calc_throttle_setpoint();

			} else {
				plane_data.roll_setpoint = rc.chan[rc.function[ROLL]].scale / 200;
				plane_data.pitch_setpoint = rc.chan[rc.function[PITCH]].scale / 200;
				plane_data.throttle_setpoint = rc.chan[rc.function[THROTTLE]].scale / 200;
			}

			//control_outputs.yaw_rudder = calc_yaw_rudder(plane_data.hdg);

			// 10 Hz loop
			usleep(100000);

		} else {
			control.attitude_control_output[ROLL] = rc.chan[rc.function[ROLL]].scale;
			control.attitude_control_output[PITCH] = rc.chan[rc.function[PITCH]].scale;
			control.attitude_control_output[THROTTLE] = rc.chan[rc.function[THROTTLE]].scale;
			// since we don't have a yaw rudder
			//control->attitude_control_output[3] = global_data_rc_channels->chan[YAW].scale;

			control.counter++;
			control.timestamp = hrt_absolute_time();
		}

		/* publish the control data */

		orb_publish(ORB_ID(fixedwing_control), fixedwing_control_pub, &control);

		/* Servo part */

		buffer_rc[ROLL] = control.attitude_control_output[ROLL];
		buffer_rc[PITCH] = control.attitude_control_output[PITCH];
		buffer_rc[THROTTLE] = control.attitude_control_output[THROTTLE];

		//mix_and_link(mixers, 3, 2, &mixer_buffer);

		// Scaling and saturation of servo outputs happens here

		data[AIL_1] = buffer_servo[AIL_1] / global_data_parameter_storage->pm.param_values[PARAM_SERVO_SCALE]
		                                  + global_data_parameter_storage->pm.param_values[PARAM_SERVO1_TRIM];

		if (data[AIL_1] > SERVO_MAX)
			data[AIL_1] = SERVO_MAX;

		if (data[AIL_1] < SERVO_MIN)
			data[AIL_1] = SERVO_MIN;

		data[AIL_2] = buffer_servo[AIL_2] / global_data_parameter_storage->pm.param_values[PARAM_SERVO_SCALE]
		                                  + global_data_parameter_storage->pm.param_values[PARAM_SERVO2_TRIM];

		if (data[AIL_2] > SERVO_MAX)
			data[AIL_2] = SERVO_MAX;

		if (data[AIL_2] < SERVO_MIN)
			data[AIL_2] = SERVO_MIN;

		data[MOT] = buffer_servo[MOT] / global_data_parameter_storage->pm.param_values[PARAM_SERVO_SCALE]
		                              + global_data_parameter_storage->pm.param_values[PARAM_SERVO3_TRIM];

		if (data[MOT] > SERVO_MAX)
			data[MOT] = SERVO_MAX;

		if (data[MOT] < SERVO_MIN)
			data[MOT] = SERVO_MIN;

		int result = write(fd, &data, sizeof(data));

		if (result != sizeof(data)) {
			printf("failed writing servo outputs\n");
		}

		/* 20Hz loop*/
		usleep(50000);
	}

	return 0;
}