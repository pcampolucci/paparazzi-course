/*
 * Copyright (C) Roland Meertens
 *
 * This file is part of paparazzi
 *
 */
/**
 * @file "modules/orange_avoider/orange_avoider.c"
 * @author Roland Meertens
 * Example on how to use the colours detected to avoid orange pole in the cyberzoo
 * This module is an example module for the course AE4317 Autonomous Flight of Micro Air Vehicles at the TU Delft.
 * This module is used in combination with a color filter (cv_detect_color_object) and the navigation mode of the autopilot.
 * The avoidance strategy is to simply count the total number of orange pixels. When above a certain percentage threshold,
 * (given by color_count_frac) we assume that there is an obstacle and we turn.
 *
 * The color filter settings are set using the cv_detect_color_object. This module can run multiple filters simultaneously
 * so you have to define which filter to use with the ORANGE_AVOIDER_VISUAL_DETECTION_ID setting.
 */

#include "modules/orange_avoider/orange_avoider.h"
#include "modules/orange_avoider/trajectory_optimizer.h"
#include "firmwares/rotorcraft/navigation.h"
#include "generated/airframe.h"
#include "state.h"
#include "subsystems/abi.h"
#include <time.h>
#include <stdio.h>
#include <stdbool.h>

#define NAV_C // needed to get the nav functions like Inside...
#include "generated/flight_plan.h"

#define ORANGE_AVOIDER_VERBOSE TRUE

#define PRINT(string,...) fprintf(stderr, "[orange_avoider->%s()] " string,__FUNCTION__ , ##__VA_ARGS__)
#if ORANGE_AVOIDER_VERBOSE
#define VERBOSE_PRINT PRINT
#else
#define VERBOSE_PRINT(...)
#endif

// Declare functions used in the code
static void moveWaypoint(uint8_t waypoint, struct EnuCoor_i *new_coor);
static void buildOuterTrajectory(void);
static void buildInnerTrajectory(uint8_t currentOuterTrajIndex);
static void moveWaypointNext(uint8_t waypoint, struct EnuCoor_i *trajectory, uint8_t index_current_waypoint);
static void checkWaypointArrival(uint8_t waypoint_target, double *mseVar);
static bool updateTrajectory(struct Obstacle *obstacle_map_updated, struct EnuCoor_i *start_trajectory, uint8_t *size);
static bool checkObstaclePresence(struct Obstacle *obstacle_map_check, int x_position, int y_position);

// define and initialise global variables
double mse_outer;                              // mean squared error to check if we reached the outer target waypoint
double mse_inner;                              // mean squared error to check if we reached the inner target waypoint
uint8_t outer_index = 0;                       // index of the outer waypoint the drone is moving towards
uint8_t inner_index = 0;                       // index of the inner waypoint the drone is moving towards
uint8_t subtraj_index = 0;                     // index of the subtrajectory the drone is using to fly
bool trajectory_updated = false;               // checks if it is safe to use the incoming trajectory
uint8_t n_obstacles = 0;                       // indicates the number of obstacles currently present in the map   
bool obstacle_map_updated = false;             // checks if there is a new obstacle and then the trajectory should be updated  
bool initialized = false;                      // one time command for the trajectory setup 

// build variables for trajectories
struct EnuCoor_i outer_trajectory[OUTER_TRAJECTORY_LENGTH];
struct TrajectoryList full_trajectory[OUTER_TRAJECTORY_LENGTH]; 
struct Obstacle obstacle_map[MAX_OBSTACLES_IN_MAP];

/*
 * This next section defines an ABI messaging event (http://wiki.paparazziuav.org/wiki/ABI), necessary
 * any time data calculated in another module needs to be accessed. Including the file where this external
 * data is defined is not enough, since modules are executed parallel to each other, at different frequencies,
 * in different threads. The ABI event is triggered every time new data is sent out, and as such the function
 * defined in this file does not need to be explicitly called, only bound in the init function
 */
#ifndef ORANGE_AVOIDER_VISUAL_DETECTION_ID
#define ORANGE_AVOIDER_VISUAL_DETECTION_ID ABI_BROADCAST
#endif
static abi_event color_detection_ev;

/* Update Obstacle Map based on Obstacle Detector Info */
static void color_detection_cb(uint8_t __attribute__((unused)) sender_id, struct ObstacleMsg *msg)
{
  //VERBOSE_PRINT("Received a message of size %d\n", msg->size);

  for (int i=0; i < msg->size; i++){
      // if we are getting a zero value, we just ignore it
      if (msg->obstacles[i].distance == 0) {
        msg->obstacles[i].distance = 0.5;
      }
    float obstacle_mse = sqrt(pow(msg->obstacles[i].distance,2)+pow(msg->obstacles[i].left_heading,2)+pow(msg->obstacles[i].right_heading,2));
    if (obstacle_mse >= 0.0) {
      //VERBOSE_PRINT("Received valid obstacle message %f, %f, %f\n", msg->obstacles[i].distance, msg->obstacles[i].left_heading, msg->obstacles[i].right_heading);
      struct EnuCoor_i absolute_position;
      double heading = RadOfDeg((msg->obstacles[i].left_heading + msg->obstacles[i].right_heading)/2);
      absolute_position.x = POS_BFP_OF_REAL(GetPosX() + sin(heading + stateGetNedToBodyEulers_f()->psi) * msg->obstacles[i].distance);
      absolute_position.y = POS_BFP_OF_REAL(GetPosY() + cos(heading + stateGetNedToBodyEulers_f()->psi) * msg->obstacles[i].distance);
      bool obstacle_already_in = checkObstaclePresence(obstacle_map, absolute_position.x, absolute_position.y);
      //VERBOSE_PRINT("drone state (psi, x, y, z): %f %f %f %f\n", stateGetNedToBodyEulers_f()->psi, GetPosX(), GetPosY(), GetPosAlt());
      //VERBOSE_PRINT("obstacle absolute : %f %f\n", POS_FLOAT_OF_BFP(absolute_position.x), POS_FLOAT_OF_BFP(absolute_position.y));

      if (!obstacle_already_in && n_obstacles < MAX_OBSTACLES_IN_MAP) {
        VERBOSE_PRINT("Received valid obstacle message %f, %f, %f\n", msg->obstacles[i].distance, msg->obstacles[i].left_heading, msg->obstacles[i].right_heading);
        VERBOSE_PRINT("New Obstacle at: %f/%f, new size is %d\n", POS_FLOAT_OF_BFP(absolute_position.x), POS_FLOAT_OF_BFP(absolute_position.y), n_obstacles+1);
        // the obstacle is not in yet, we can add a new slot to the obstacle map
        n_obstacles += 1;
        // populate the new slot
        obstacle_map[n_obstacles-1].loc.x = absolute_position.x;
        obstacle_map[n_obstacles-1].loc.y = absolute_position.y;
        obstacle_map_updated = true;
      }
    }
  }
}

/*
 * Initialisation function, setting the colour filter, random seed and heading_increment
 */
void orange_avoider_init(void)
{

  // obstacle_map[0].loc.x = POS_BFP_OF_REAL(1.5);
  // obstacle_map[0].loc.y = POS_BFP_OF_REAL(1.5);
  // n_obstacles += 1;

  // bind our colorfilter callbacks to receive the color filter outputs
  AbiBindMsgOBSTACLE_DETECTION(ORANGE_AVOIDER_VISUAL_DETECTION_ID, &color_detection_ev, color_detection_cb);
}

/*
 * Function that checks it reached a waypoint, and then updates the new ones 
 */
void orange_avoider_periodic(void)
{

  // // only evaluate our state machine if we are flying
  // if(!autopilot_in_flight()){
  //   return;
  // }

  if(!initialized){

    for (int i = 0; i < OUTER_TRAJECTORY_LENGTH; i++) {
      full_trajectory[i].size = INNER_TRAJECTORY_LENGTH;
    }

    // Build outer trajectory based on few sparse waypoints
    buildOuterTrajectory();

    // For each space between outer waypoints build an editable pointwise inner trajectory
    for (int i = 0; i < OUTER_TRAJECTORY_LENGTH; i++)
    {
      buildInnerTrajectory(i);
    }

    initialized = true;
  }

  // Check how close we are from the targets
  checkWaypointArrival(WP_OUTER, &mse_outer);      // Calculate how close it is from the next outer waypoint
  checkWaypointArrival(WP_INNER, &mse_inner);      // Calculate how close it is from the next inner waypoint

  // check if a new obstacle has been added to the map
  if (obstacle_map_updated) {
    updateTrajectory(obstacle_map, full_trajectory[subtraj_index].inner_trajectory, &full_trajectory[subtraj_index].size);
    obstacle_map_updated = false;
  }

  if (mse_inner < 0.5 && trajectory_updated){

    VERBOSE_PRINT("[INNER TRAJECTORY] Setting new Waypoint at %d, going to : (%f/%f) \n", \
    inner_index, \
    POS_FLOAT_OF_BFP(full_trajectory[subtraj_index].inner_trajectory[inner_index].x), \
    POS_FLOAT_OF_BFP(full_trajectory[subtraj_index].inner_trajectory[inner_index].y));

    moveWaypointNext(WP_INNER, full_trajectory[subtraj_index].inner_trajectory, inner_index);

    if (inner_index < full_trajectory[subtraj_index].size-1) {
      inner_index += 1;
    }
  
  } if (mse_outer < 0.5) {

    if (outer_index < OUTER_TRAJECTORY_LENGTH-1) {
      outer_index += 1;
      subtraj_index = outer_index - 1;
    } else {
      outer_index = 0;
      subtraj_index = OUTER_TRAJECTORY_LENGTH-1;
    }

    inner_index = 0;

    VERBOSE_PRINT("[OUTER TRAJECTORY] Setting new Waypoint at %d, going to : (%f/%f) \n", \
    outer_index, \
    POS_FLOAT_OF_BFP(outer_trajectory[outer_index].x), \
    POS_FLOAT_OF_BFP(outer_trajectory[outer_index].y));

    // move to the next waypoint
    moveWaypointNext(WP_OUTER, outer_trajectory, outer_index);

    trajectory_updated = updateTrajectory(obstacle_map, full_trajectory[subtraj_index].inner_trajectory, &full_trajectory[subtraj_index].size);

  }

  NavGotoWaypointHeading(WP_INNER);

  return;
}

/*
 * Takes the inner trajectory and overrides a new one in the allocated slots, placing the rest to zero
 */
bool updateTrajectory(struct Obstacle *obstacle_map_ut, struct EnuCoor_i *start_trajectory, uint8_t *size) {
  clock_t t_trajectory; 
  t_trajectory = clock();
  struct OptimizedTrajectory new_inner = optimize_trajectory(obstacle_map_ut, start_trajectory, size, n_obstacles);

  // override old trajectory with new one and zeroes if not getting all the space
  for (int i=0; i < INNER_TRAJECTORY_SPACE; i++){
    if (i < *size) {
      //VERBOSE_PRINT("new inner %d is %f/%f\n", i, POS_FLOAT_OF_BFP(new_inner[i].x), POS_FLOAT_OF_BFP(new_inner[i].y));
      full_trajectory[subtraj_index].inner_trajectory[i].x = new_inner.buf[i].x;
      full_trajectory[subtraj_index].inner_trajectory[i].y = new_inner.buf[i].y;
    } else {
      full_trajectory[subtraj_index].inner_trajectory[i].x = 0;
      full_trajectory[subtraj_index].inner_trajectory[i].y = 0;
    }
  }
  t_trajectory = clock() - t_trajectory; 
  double time_taken_trajectory = 1000 * ((double)t_trajectory)/CLOCKS_PER_SEC; // in milliseconds 
  VERBOSE_PRINT("Time Taken for Trajectory Optimization : %f ms\n", time_taken_trajectory);
  return true;
}

/*
 * Calculates coordinates of distance forward and sets waypoint 'waypoint' to those coordinates
 */
void moveWaypointNext(uint8_t waypoint, struct EnuCoor_i *trajectory, uint8_t index_current_waypoint)
{ 
  moveWaypoint(waypoint, &trajectory[index_current_waypoint]);
}

/*
 * Checks if WP_GOAL is very close to WP_TARGET, then change the waypoint
 */
void checkWaypointArrival(uint8_t waypoint_target, double *mseVar)
{
  double error_x = GetPosX() - WaypointX(waypoint_target);
  double error_y = GetPosY() - WaypointY(waypoint_target);
  *mseVar = sqrt(pow(error_x,2)+pow(error_y,2));
}

/*
 * Sets waypoint 'waypoint' to the coordinates of 'new_coor'
 */
void moveWaypoint(uint8_t waypoint, struct EnuCoor_i *new_coor)
{
  waypoint_move_xy_i(waypoint, new_coor->x, new_coor->y);
}

/*
 * Builds the trajectory in the contour by random values
 */
void buildOuterTrajectory(void) {

  VERBOSE_PRINT("------------------------------------------------------------------------------------ \n");

  double start_x = GetPosX();
  double start_y = GetPosY();
  VERBOSE_PRINT("[OUTER TRAJECTORY] Starting position (%f/%f)\n", start_x, start_y);

  // set outer trajectory points
  double rx_list[5] = {start_x, 2, 2};
  double ry_list[5] = {start_y, 2, -2};

  // populate outer_tajectory struct
  for (int i = 0; i < OUTER_TRAJECTORY_LENGTH; i++) {
      outer_trajectory[i].x = POS_BFP_OF_REAL(rx_list[i]);
      outer_trajectory[i].y = POS_BFP_OF_REAL(ry_list[i]);
      VERBOSE_PRINT("[OUTER TRAJECTORY] Point added: (%f/%f) \n", POS_FLOAT_OF_BFP(outer_trajectory[i].x), POS_FLOAT_OF_BFP(outer_trajectory[i].y));
  }

  VERBOSE_PRINT("------------------------------------------------------------------------------------ \n");
}

/*
 * Creates an 'inner' trajectory between the trajectory[i] and trajectory[i+1]
 */
void buildInnerTrajectory(uint8_t outer_index_bt){

  uint8_t actual_index = outer_index_bt + 1;

  if (outer_index_bt == OUTER_TRAJECTORY_LENGTH-1) {
    actual_index = 0;
  }

  float x_diff = POS_FLOAT_OF_BFP(outer_trajectory[actual_index].x) - POS_FLOAT_OF_BFP(outer_trajectory[outer_index_bt].x);
  float y_diff = POS_FLOAT_OF_BFP(outer_trajectory[actual_index].y) - POS_FLOAT_OF_BFP(outer_trajectory[outer_index_bt].y);
  float increment_x =  x_diff/(INNER_TRAJECTORY_LENGTH);
  float increment_y = y_diff/(INNER_TRAJECTORY_LENGTH);

  VERBOSE_PRINT("------------------------------------------------------------------------------------ \n");

  for (int i = 0; i < INNER_TRAJECTORY_LENGTH; i++){

    // Create set of points between current position and the desired waypoint (initialized as straight line)
    full_trajectory[outer_index_bt].inner_trajectory[i].x = POS_BFP_OF_REAL((i+1)*increment_x + POS_FLOAT_OF_BFP(outer_trajectory[outer_index_bt].x));
    full_trajectory[outer_index_bt].inner_trajectory[i].y = POS_BFP_OF_REAL((i+1)*increment_y + POS_FLOAT_OF_BFP(outer_trajectory[outer_index_bt].y));

    VERBOSE_PRINT("[INNER TRAJECTORY] Point added: (%f/%f) \n", POS_FLOAT_OF_BFP(full_trajectory[outer_index_bt].inner_trajectory[i].x), POS_FLOAT_OF_BFP(full_trajectory[outer_index].inner_trajectory[i].y));
  }

  VERBOSE_PRINT("------------------------------------------------------------------------------------ \n");
}

/*
 * Checks if the obstacle being sent is already present in the map
 */
bool checkObstaclePresence(struct Obstacle *obstacle_map_check, int x_position, int y_position) {
  
  for (int i=0; i < n_obstacles; i++) {
    // is the current obstacle close enough to an already existing one?
    double error_x = POS_FLOAT_OF_BFP(obstacle_map_check[i].loc.x - x_position);
    double error_y = POS_FLOAT_OF_BFP(obstacle_map_check[i].loc.y - y_position);
    double obstacle_mse = sqrt(pow(error_x,2)+pow(error_y,2));
    //VERBOSE_PRINT("Obstacle MSE is %f with %d/%d/%d/%d\n", obstacle_mse, obstacle_map[i].loc.x, obstacle_map[i].loc.y, x_position, y_position);
    if (obstacle_mse < 1 || obstacle_mse != obstacle_mse) {
      // then we most likely have already detected this obstacle or we have no useful information
      return true;
    }
  }

  // if we get here, the obstacle can be added to the map as not yet detected
  return false;
}