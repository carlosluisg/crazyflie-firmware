/**
 * Authored by Michael Hamer (http://www.mikehamer.info), November 2016.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, in version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * ============================================================================
 *
 * The controller implemented in this file is based on the paper:
 *
 * "Nonlinear Quadrocopter Attitude Control"
 * http://e-collection.library.ethz.ch/eserv/eth:7387/eth-7387-01.pdf
 *
 * Academic citation would be appreciated.
 *
 * BIBTEX ENTRIES:
      @ARTICLE{BrescianiniNonlinearController2013,
               title={Nonlinear quadrocopter attitude control},
               author={Brescianini, Dario and Hehn, Markus and D'Andrea, Raffaello},
               year={2013},
               publisher={ETH Zurich}}
 *
 * ============================================================================
 */

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "arm_math.h"
#include "stabilizer_types.h"

#include "log.h"
#include "param.h"
#include "num.h"
#include "debug.h"
#include "physical_constants.h"

#ifdef ESTIMATOR_TYPE_kalman
#include "estimator_kalman.h"
#endif

// tau is a time constant, lower -> more aggressive control (weight on position error)
// zeta is a damping factor, higher -> more damping (weight on velocity error)

static float tau_xy = 0.30;
static float zeta_xy = 0.85; // this gives good performance down to 0.4, the lower the more aggressive (less damping)

static float tau_z = 0.30;
static float zeta_z = 0.85;

// time constant of body angle (thrust direction) control
static float tau_rp = 0.15;
// what percentage is yaw control speed in terms of roll/pitch control speed \in [0, 1], 0 means yaw not controlled
static float mixing_factor = 1.0;

// time constant of rotational rate control
static float tau_rp_rate = 0.015;
static float tau_yaw_rate = 0.0075;

// minimum and maximum thrusts
static float coll_min = 1;
static float coll_max = 18;
// if too much thrust is commanded, which axis is reduced to meet maximum thrust?
// 1 -> even reduction across x, y, z
// 0 -> z gets what it wants (eg. maintain height at all costs)
static float thrust_reduction_fairness = 0.25;

// minimum and maximum body rates
static float omega_rp_max = 30;
static float omega_yaw_max = 10;
static float heuristic_rp = 12;
static float heuristic_yaw = 5;

// maximum tilt angle in radians
static float tilt_limit = 0.7854; //45 degrees
//static uint32_t lastReferenceTimestamp;

// Struct for logging position information
static positionMeasurement_t ext_pos;
static uint32_t lastControlUpdate;
static bool isInit = false;
static float des_acc[3] = {0};
static float tilt_angle_before = 0, tilt_angle_after = 0;
static int8_t tiltLimitFlag = 0;

void stateControllerInit(void)
{
  if (isInit) {
    return;
  }

  isInit = true;
}

static int16_t emergency_value;

void stateController(control_t *control, setpoint_t *setpoint, const sensorData_t *sensors, const state_t *state)
{
  //uint32_t ticksSinceLastCommand = (xTaskGetTickCount() - lastReferenceTimestamp);
  /*
  if (ticksSinceLastCommand > M2T(500)) { // require commands at 2Hz
    control->enable = false;
    control->thrust = 0;
    return;
  }
  */

  emergency_value = setpoint->setEmergency;
  if (setpoint->setEmergency)
  {
    control->enable = false;
  }
  else if (setpoint->resetEmergency)
  {
    control->enable = true;
  }


  if (!control->enable)
  {
    control->thrust = 0;
    return;
  }
  
  if(setpoint->z[0]<=0.02f){
      control->thrust = 0;
      control->enable = false;
      return;
  }

  // define this here, since we do body-rate control at 100Hz below the following if statement
  float omega[3] = {0};
  omega[0] = radians(sensors->gyro.x);
  omega[1] = radians(sensors->gyro.y);
  omega[2] = radians(sensors->gyro.z);

  // update at the CONTROL_RATE (100Hz)
  if ((xTaskGetTickCount()-lastControlUpdate) > configTICK_RATE_HZ/CONTROL_RATE)
  {
    lastControlUpdate = xTaskGetTickCount();

    // desired accelerations
    float accDes[3] = {0};
    // desired thrust
    float collCmd = 0;

    //float attTilt[4] = {0};
    //arm_matrix_instance_f32 attTilt_m = {4, 1, attTilt};

    // attitude error as computed by the reduced attitude controller
    float attErrorReduced[4] = {0};
    arm_matrix_instance_f32 attErrorReduced_m = {4, 1, attErrorReduced};

    // desired attitude as computed by the reduced attitude controller
    float attDesiredReduced[4] = {0};
    arm_matrix_instance_f32 attDesiredReduced_m = {4, 1, attDesiredReduced};

    // attitude error as computed by the full attitude controller
    float attErrorFull[4] = {0};
    arm_matrix_instance_f32 attErrorFull_m = {4, 1, attErrorFull};

    // desired attitude as computed by the full attitude controller
    float attDesiredFull[4] = {0};
    arm_matrix_instance_f32 attDesiredFull_m = {4, 1, attDesiredFull};

    // current attitude
    quaternion_t sq = state->attitudeQuaternion;
    float q[4] = {sq.w, sq.x, sq.y, sq.z};
    arm_matrix_instance_f32 attitude_m = {4, 1, q};

    // inverse of current attitude
    float qI[4] = {0};
    arm_matrix_instance_f32 attitudeI_m = {4, 1, qI};
    quaternion_inverse(&attitude_m, &attitudeI_m);

    // body frame -> inertial frame :  vI = R*vB
    float R[3][3] = {0};

    R[0][0] = q[0] * q[0] + q[1] * q[1] - q[2] * q[2] - q[3] * q[3];
    R[0][1] = 2 * q[1] * q[2] - 2 * q[0] * q[3];
    R[0][2] = 2 * q[1] * q[3] + 2 * q[0] * q[2];

    R[1][0] = 2 * q[1] * q[2] + 2 * q[0] * q[3];
    R[1][1] = q[0] * q[0] - q[1] * q[1] + q[2] * q[2] - q[3] * q[3];
    R[1][2] = 2 * q[2] * q[3] - 2 * q[0] * q[1];

    R[2][0] = 2 * q[1] * q[3] - 2 * q[0] * q[2];
    R[2][1] = 2 * q[2] * q[3] + 2 * q[0] * q[1];
    R[2][2] = q[0] * q[0] - q[1] * q[1] - q[2] * q[2] + q[3] * q[3];

    // a few temporary quaternions
    float temp1[4] = {0};
    arm_matrix_instance_f32 temp1_m = {4, 1, temp1};

    float temp2[4] = {0};
    arm_matrix_instance_f32 temp2_m = {4, 1, temp2};

    // compute the position and velocity errors
    float pError[] = {setpoint->x[0] - state->position.x,
                      setpoint->y[0] - state->position.y,
                      setpoint->z[0] - state->position.z};

    float vError[] = {setpoint->x[1] - state->velocity.x,
                      setpoint->y[1] - state->velocity.y,
                      setpoint->z[1] - state->velocity.z};


    // ====== LINEAR CONTROL ======

    // compute desired accelerations in X, Y and Z
    accDes[0] = 0;
    if (CONTROLMODE_POSITION(setpoint->xmode))
    { accDes[0] += 1.0f / tau_xy / tau_xy * pError[0]; }
    if (CONTROLMODE_VELOCITY(setpoint->xmode))
    { accDes[0] += 2.0f * zeta_xy / tau_xy * vError[0]; }
    if (CONTROLMODE_ACCELERATION(setpoint->xmode))
    { accDes[0] += setpoint->x[2]; }
    accDes[0] = constrain(accDes[0], -coll_max, coll_max);

    accDes[1] = 0;
    if (CONTROLMODE_POSITION(setpoint->ymode))
    { accDes[1] += 1.0f / tau_xy / tau_xy * pError[1]; }
    if (CONTROLMODE_VELOCITY(setpoint->ymode))
    { accDes[1] += 2.0f * zeta_xy / tau_xy * vError[1]; }
    if (CONTROLMODE_ACCELERATION(setpoint->ymode))
    { accDes[1] += setpoint->y[2]; }
    accDes[1] = constrain(accDes[1], -coll_max, coll_max);

    accDes[2] = GRAVITY;
    if (CONTROLMODE_POSITION(setpoint->zmode))
    { accDes[2] += 1.0f / tau_z / tau_z * pError[2]; }
    if (CONTROLMODE_VELOCITY(setpoint->zmode))
    { accDes[2] += 2.0f * zeta_z / tau_z * vError[2]; }
    if (CONTROLMODE_ACCELERATION(setpoint->zmode))
    { accDes[2] += setpoint->z[2]; }
    accDes[2] = constrain(accDes[2], -coll_max, coll_max);


    // ====== THRUST CONTROL ======

    // compute commanded thrust required to achieve the z acceleration 
    configASSERT(R[2][2] != 0);
    collCmd = accDes[2] / R[2][2];

    if (fabsf(collCmd) > coll_max) {
      // exceeding the thrust threshold
      // we compute a reduction factor r based on fairness f \in [0,1] such that:
      // collMax^2 = (r*x)^2 + (r*y)^2 + (r*f*z + (1-f)z + g)^2
      float x = accDes[0];
      float y = accDes[1];
      float z = accDes[2] - GRAVITY;
      float g = GRAVITY;
      float f = constrain(thrust_reduction_fairness, 0, 1);

      float r = 0;

      // solve as a quadratic
      float a = powf(x, 2) + powf(y, 2) + powf(z*f, 2);
      if (a<0) { a = 0; }

      float b = 2 * z*f*((1-f)*z + g);
      float c = powf(coll_max, 2) - powf((1-f)*z + g, 2);
      if (c<0) { c = 0; }

      if (fabsf(a)<1e-6f) {
        r = 0;
      } else {
        float sqrtterm = powf(b, 2) + 4.0f*a*c;
        configASSERT(sqrtterm>=0);
        r = (-b + arm_sqrt(sqrtterm))/(2.0f*a);
        r = constrain(r,0,1);
      }
      accDes[0] = r*x;
      accDes[1] = r*y;
      accDes[2] = (r*f+(1-f))*z + g;
    } 
    configASSERT(R[2][2]!=0);
    collCmd = constrain(accDes[2] / R[2][2], coll_min, coll_max);

    // FYI: this thrust will result in the accelerations
    // xdd = R02*coll
    // ydd = R12*coll

    // a unit vector pointing in the direction of the desired thrust (ie. the direction of body's z axis in the inertial frame)
    float zI_des[3] = {accDes[0], accDes[1], accDes[2]};
    arm_matrix_instance_f32 zI_des_m = {3, 1, zI_des};
    vec_normalize(&zI_des_m);

    // a unit vector pointing in the direction of the current thrust
    float zI_cur[3] = {R[0][2], R[1][2], R[2][2]};
    arm_matrix_instance_f32 zI_cur_m = {3, 1, zI_cur};
    vec_normalize(&zI_cur_m);

    // a unit vector pointing in the direction of the inertial frame z-axis
    float zI[3] = {0, 0, 1};
    arm_matrix_instance_f32 zI_m = {3, 1, zI};

    // ======= LIMIT MAX TILT ANGLE =========

    float dotProd = vec_dot(&zI_m, &zI_des_m);
    dotProd = constrain(dotProd, -1, 1);
    float alpha_tilt = acosf(dotProd);
    tilt_angle_before = degrees(alpha_tilt);

    float rotAxisI[3] = {0};
    arm_matrix_instance_f32 rotAxisI_m = {3, 1, rotAxisI};

    // if (fabsf(alpha_tilt) > tilt_limit){
    //   DEBUG_PRINT("Exceed Tilt Limit!\n");
    //   tiltLimitFlag = 1;
    //   alpha_tilt = (alpha_tilt >= 0) ? tilt_limit : -tilt_limit;
    //
    //   if (fabsf(alpha_tilt) > 1 * ARCMINUTE)
    //   {
    //     vec_cross(&zI_m, &zI_des_m, &rotAxisI_m);
    //     vec_normalize(&rotAxisI_m);
    //   }
    //   else
    //   {
    //     rotAxisI[0] = 1;
    //     rotAxisI[1] = 1;
    //     rotAxisI[2] = 0;
    //   }
    //
    //   attTilt[0] = cosf(alpha_tilt / 2.0f);
    //   attTilt[1] = sinf(alpha_tilt / 2.0f) * rotAxisI[0];
    //   attTilt[2] = sinf(alpha_tilt / 2.0f) * rotAxisI[1];
    //   attTilt[3] = sinf(alpha_tilt / 2.0f) * rotAxisI[2];
    //
    //   // mathematical formula: p(r_rotated) = q * p(r)* q_adjoint, where
    //   // q is the quaternion which represents desired rotation
    //   // r is the vector you want to rotate
    //   // p(r) means the vector expressed in quaternion form [0, r[0], r[1], r[2]]
    //   // q = [w, x, y, z] ---> q_adjoint = [w, -x, -y, -z]
    //
    //   // We want to rotate the inertial vector by the maximum tilt angle, So
    //   // p(zI_des_clamped) = quaternion_max_tilt * p(zI)  * quaternion_max_tilt_adjoint
    //   float vec_in_quat[4] = {0, 0, 0, 1}; //p(z)
    //   arm_matrix_instance_f32 vec_in_quat_m = {4, 1, vec_in_quat};
    //   float temp_product[4] = {0};
    //   arm_matrix_instance_f32 temp_product_m = {4, 1, temp_product};
    //   quaternion_multiply(&attTilt_m, &vec_in_quat_m, &temp_product_m);
    //   // compute q_adjoint
    //   attTilt[1] = -attTilt[1];
    //   attTilt[2] = -attTilt[2];
    //   attTilt[3] = -attTilt[3];
    //   quaternion_multiply(&temp_product_m, &attTilt_m, &vec_in_quat_m);
    //   // compute new clamped zI_des from its representation in quaternion
    //   zI_des[0] = vec_in_quat[1];
    //   zI_des[1] = vec_in_quat[2];
    //   zI_des[2] = vec_in_quat[3];
    // }
    des_acc[0] = zI_des[0];
    des_acc[1] = zI_des[1];
    des_acc[2] = zI_des[2];
    // ====== REDUCED ATTITUDE CONTROL ======

    // compute the error angle between the current and the desired thrust directions
    dotProd = vec_dot(&zI_cur_m, &zI_des_m);
    dotProd = constrain(dotProd, -1, 1);
    float alpha = acosf(dotProd);

    // the axis around which this rotation needs to occur in the inertial frame (ie. an axis orthogonal to the two)

    if (fabsf(alpha) > 1 * ARCMINUTE)
    {
      vec_cross(&zI_cur_m, &zI_des_m, &rotAxisI_m);
      vec_normalize(&rotAxisI_m);
    }
    else
    {
      rotAxisI[0] = 1;
      rotAxisI[1] = 1;
      rotAxisI[2] = 0;
    }

    // the attitude error quaternion
    attErrorReduced[0] = cosf(alpha / 2.0f);
    attErrorReduced[1] = sinf(alpha / 2.0f) * rotAxisI[0];
    attErrorReduced[2] = sinf(alpha / 2.0f) * rotAxisI[1];
    attErrorReduced[3] = sinf(alpha / 2.0f) * rotAxisI[2];

    // choose the shorter rotation
    if (sinf(alpha / 2.0f) < 0)
    {
      vec_negate(&rotAxisI_m);
    }
    if (cosf(alpha / 2.0f) < 0)
    {
      vec_negate(&rotAxisI_m);
      vec_negate(&attErrorReduced_m);
    }

    quaternion_multiply(&attitude_m, &attErrorReduced_m, &attDesiredReduced_m);
    quaternion_normalize(&attErrorReduced_m);
    quaternion_normalize(&attDesiredReduced_m);


    // ====== FULL ATTITUDE CONTROL ======

    // compute the error angle between the inertial and the desired thrust directions
    dotProd = vec_dot(&zI_m, &zI_des_m);
    dotProd = constrain(dotProd, -1, 1);
    alpha = acosf(dotProd);
    if (fabsf(alpha) > tilt_limit){
      tiltLimitFlag = 1;
      //DEBUG_PRINT("EXCEED TILT LIMIT!");
      alpha = (alpha >= 0) ? tilt_limit : -tilt_limit;
    }
    tilt_angle_after = degrees(alpha);

    // the axis around which this rotation needs to occur in the inertial frame (ie. an axis orthogonal to the two)
    if (fabsf(alpha) > 1 * ARCMINUTE)
    {
      vec_cross(&zI_m, &zI_des_m, &rotAxisI_m);
      vec_normalize(&rotAxisI_m);
    }
    else
    {
      rotAxisI[0] = 1;
      rotAxisI[1] = 1;
      rotAxisI[2] = 0;
    }

    // the quaternion corresponding to a roll and pitch around this axis
    float attFullReqPitchRoll[4] = {0};
    arm_matrix_instance_f32 attFullReqPitchRoll_m = {4, 1, attFullReqPitchRoll};
    attFullReqPitchRoll[0] = cosf(alpha / 2.0f);
    attFullReqPitchRoll[1] = sinf(alpha / 2.0f) * rotAxisI[0];
    attFullReqPitchRoll[2] = sinf(alpha / 2.0f) * rotAxisI[1];
    attFullReqPitchRoll[3] = sinf(alpha / 2.0f) * rotAxisI[2];

    // the quaternion corresponding to a rotation to the desired yaw
    float attFullReqYaw[4] = {0};
    arm_matrix_instance_f32 attFullReqYaw_m = {4, 1, attFullReqYaw};
    attFullReqYaw[0] = cosf(setpoint->yaw[0] / 2.0f);
    attFullReqYaw[1] = 0;
    attFullReqYaw[2] = 0;
    attFullReqYaw[3] = sinf(setpoint->yaw[0] / 2.0f);

    // the full rotation (roll & pitch, then yaw)
    quaternion_multiply(&attFullReqPitchRoll_m, &attFullReqYaw_m, &attDesiredFull_m);

    // back transform from the current attitude to get the error between rotations
    quaternion_multiply(&attitudeI_m, &attDesiredFull_m, &attErrorFull_m);

    // correct rotation
    if (attErrorFull[0] < 0)
    {
      vec_negate(&attErrorFull_m);
      quaternion_multiply(&attitude_m, &attErrorFull_m, &attDesiredFull_m);
    }

    quaternion_normalize(&attErrorFull_m);
    quaternion_normalize(&attDesiredFull_m);


    // ====== MIXING FULL & REDUCED CONTROL ======

    float attError[4] = {0};
    arm_matrix_instance_f32 attError_m = {4, 1, attError};

    if (mixing_factor <= 0)
    {
      // 100% reduced control (no yaw control)
      memcpy(attError, attErrorReduced, sizeof(attError));
    }
    else if (mixing_factor >= 1)
    {
      // 100% full control (yaw controlled with same time constant as roll & pitch)
      memcpy(attError, attErrorFull, sizeof(attError));
    }
    else
    {
      // mixture of reduced and full control

      // calculate rotation between the two errors
      quaternion_inverse(&attErrorReduced_m, &temp1_m);
      quaternion_multiply(&temp1_m, &attErrorFull_m, &temp2_m);
      quaternion_normalize(&temp2_m);

      // by defintion this rotation has the form [cos(alpha/2), 0, 0, sin(alpha/2)]
      // where the first element gives the rotation angle, and the last the direction
      alpha = 2.0f * acosf(constrain(temp2[0], -1, 1));

      // bisect the rotation from reduced to full control
      temp1[0] = cosf(alpha * mixing_factor / 2.0f);
      temp1[1] = 0;
      temp1[2] = 0;
      temp1[3] = sinf(alpha * mixing_factor / 2.0f) * (temp2[3] < 0 ? -1 : 1); // rotate in the correct direction

      quaternion_multiply(&attErrorReduced_m, &temp1_m, &attError_m);
      quaternion_normalize(&attError_m);
    }

    // ====== COMPUTE CONTROL SIGNALS ======

    // compute the commanded body rates
    control->omega[0] = 2.0f / tau_rp * attError[1];
    control->omega[1] = 2.0f / tau_rp * attError[2];
    control->omega[2] = 2.0f / tau_rp * attError[3] + setpoint->yaw[1]; // due to the mixing, this will behave with time constant tau_yaw

    // apply the rotation heuristic
    if (control->omega[0] * omega[0] < 0 && fabsf(omega[0]) > heuristic_rp) { // desired rotational rate in direction opposite to current rotational rate
      control->omega[0] = omega_rp_max * (omega[0] < 0 ? -1 : 1); // maximum rotational rate in direction of current rotation
    }

    if (control->omega[1] * omega[1] < 0 && fabsf(omega[1]) > heuristic_rp) { // desired rotational rate in direction opposite to current rotational rate
      control->omega[1] = omega_rp_max * (omega[1] < 0 ? -1 : 1); // maximum rotational rate in direction of current rotation
    }

    if (control->omega[2] * omega[2] < 0 && fabsf(omega[2]) > heuristic_yaw) { // desired rotational rate in direction opposite to current rotational rate
      control->omega[2] = omega_rp_max * (omega[2] < 0 ? -1 : 1); // maximum rotational rate in direction of current rotation
    }

    // scale the commands to satisfy rate constraints
    float scaling = 1;
    scaling = max(scaling, fabsf(control->omega[0]) / omega_rp_max);
    scaling = max(scaling, fabsf(control->omega[1]) / omega_rp_max);
    scaling = max(scaling, fabsf(control->omega[2]) / omega_yaw_max);

    control->omega[0] /= scaling;
    control->omega[1] /= scaling;
    control->omega[2] /= scaling;
    control->thrust = collCmd;



  }

  // control the body torques
  float omegaErr[3] = {(control->omega[0] - omega[0])/tau_rp_rate,
                       (control->omega[1] - omega[1])/tau_rp_rate,
                       (control->omega[2] - omega[2])/tau_yaw_rate};

  arm_matrix_instance_f32 omegaErr_m = {3, 1, omegaErr};
  arm_matrix_instance_f32 torques_m = {3, 1, control->torque};

  // update the commanded body torques based on the current error in body rates
  mat_mult(&CRAZYFLIE_INERTIA_m, &omegaErr_m, &torques_m);
}

bool stateControllerTest(void) { return true; }

LOG_GROUP_START(ext_pos)
  LOG_ADD(LOG_FLOAT, X, &ext_pos.x)
  LOG_ADD(LOG_FLOAT, Y, &ext_pos.y)
  LOG_ADD(LOG_FLOAT, Z, &ext_pos.z)
LOG_GROUP_STOP(ext_pos)

LOG_GROUP_START(emergency)
  LOG_ADD(LOG_INT16, setEmergency, &emergency_value)
LOG_GROUP_STOP(emergency)

LOG_GROUP_START(des_accG)
  LOG_ADD(LOG_FLOAT, angle, &tilt_angle_before)
  LOG_ADD(LOG_FLOAT, angleB, &tilt_angle_after)
  LOG_ADD(LOG_FLOAT, X, &des_acc[0])
  LOG_ADD(LOG_FLOAT, Y, &des_acc[1])
  LOG_ADD(LOG_FLOAT, Z, &des_acc[2])
LOG_GROUP_STOP(des_accG)

LOG_GROUP_START(tilt_flag)
  LOG_ADD(LOG_INT8, flag, &tiltLimitFlag)
LOG_GROUP_STOP(tilt_flag)


PARAM_GROUP_START(ctrlr)
PARAM_ADD(PARAM_FLOAT, tau_xy, &tau_xy)
PARAM_ADD(PARAM_FLOAT, zeta_xy, &zeta_xy)
PARAM_ADD(PARAM_FLOAT, tau_z, &tau_z)
PARAM_ADD(PARAM_FLOAT, zeta_z, &zeta_z)
PARAM_ADD(PARAM_FLOAT, tau_rp, &tau_rp)
PARAM_ADD(PARAM_FLOAT, mixing_factor, &mixing_factor)
PARAM_ADD(PARAM_FLOAT, coll_fairness, &thrust_reduction_fairness)
PARAM_ADD(PARAM_FLOAT, heuristic_rp, &heuristic_rp)
PARAM_ADD(PARAM_FLOAT, heuristic_yaw, &heuristic_yaw)
PARAM_ADD(PARAM_FLOAT, tau_rp_rate, &tau_rp_rate)
PARAM_ADD(PARAM_FLOAT, tau_yaw_rate, &tau_yaw_rate)
PARAM_ADD(PARAM_FLOAT, coll_min, &coll_min)
PARAM_ADD(PARAM_FLOAT, coll_max, &coll_max)
PARAM_ADD(PARAM_FLOAT, omega_rp_max, &omega_rp_max)
PARAM_ADD(PARAM_FLOAT, omega_yaw_max, &omega_yaw_max)
PARAM_ADD(PARAM_FLOAT, tilt_limit, &tilt_limit)
PARAM_GROUP_STOP(ctrlr)
