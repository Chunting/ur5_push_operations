/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2010, Rice University
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Rice University nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Henning Kayser */

#include <ros/ros.h>

#include <moveit/move_group_interface/move_group_interface.h>
#include <moveit/planning_scene_interface/planning_scene_interface.h>
#include <moveit/planning_scene/planning_scene.h>
#include <moveit/planning_scene_monitor/planning_scene_monitor.h>
#include <moveit_msgs/CollisionObject.h>
#include <moveit_msgs/ExecuteTrajectoryActionResult.h>


#include <moveit/trajectory_processing/iterative_spline_parameterization.h>
#include <moveit/robot_state/robot_state.h>
#include <moveit/robot_trajectory/robot_trajectory.h>
#include <moveit/robot_state/conversions.h>

#include <tf/transform_listener.h>

#include <eigen_conversions/eigen_msg.h>

#include <visualization_msgs/Marker.h>

#include <ur5_pusher/pusher.h>
#include <ur5_pusher/safety_push_sampler.h>

#include <tams_ur5_push_execution/Push.h>
#include <tams_ur5_push_execution/PushApproach.h>
#include <std_msgs/Float64.h>

#include <ur5_pushing_object_recognition/ImageDump.h>

#pragma once



const double TIP_RADIUS = 0.004;

std::string MARKER_TOPIC = "/pushable_objects";

namespace tams_ur5_push_execution
{
    class PushExecution
    {
        private:

            ros::NodeHandle nh_;
            ros::NodeHandle pnh_;

            ros::Subscriber marker_sub_;
            ros::Publisher contact_point_pub_;

            planning_scene_monitor::PlanningSceneMonitor psm_;
            planning_scene_monitor::CurrentStateMonitorPtr csm_;

            ros::Publisher dist_pub_;
            ros::Publisher dist2_pub_;

            moveit::planning_interface::PlanningSceneInterface psi_;

            ur5_pusher::SafetyPushSampler push_sampler_;

            ros::ServiceClient snapshot_client_;
            bool take_snapshots_ = false;

            tf::TransformListener tf_listener_;

            moveit_msgs::CollisionObject obj_;

            visualization_msgs::Marker marker_;
            ros::Time marker_stamp_;

            bool first_attempt_ = true;

            double tip_radius_;

        public:
            PushExecution(bool execute_plan=false) : psm_("robot_description"){

                nh_ = (*new ros::NodeHandle());
                pnh_ = (*new ros::NodeHandle("~"));
                psi_ = (*new moveit::planning_interface::PlanningSceneInterface());
                psm_.startStateMonitor();
                csm_ = psm_.getStateMonitorNonConst();
                dist_pub_ = nh_.advertise<std_msgs::Float64>("/joint_distances/contact", 100);
                dist2_pub_ = nh_.advertise<std_msgs::Float64>("/joint_distances/post_push", 100);

                pnh_.param("tip_radius", tip_radius_, TIP_RADIUS);

                push_sampler_.setReferenceFrame("/table_top");

                marker_sub_ = nh_.subscribe(MARKER_TOPIC, 1, &PushExecution::onDetectObjects, this);
                contact_point_pub_ = nh_.advertise<visualization_msgs::Marker>("/push_approach", 0);
                marker_.id = -1;
            }

            bool performRandomPush(ur5_pusher::Pusher& pusher, bool execute_plan=false) {
                ExplorePushesFeedback fb;
                return performRandomPush(pusher, fb, execute_plan);
            }

            void enableSnapshots() {
                snapshot_client_ = nh_.serviceClient<ur5_pushing_object_recognition::ImageDump>("/image_dump_service");
                take_snapshots_ = true;
            }

            void disableSnapshots() {
                take_snapshots_ = true;
            }


            bool performRandomPush(ur5_pusher::Pusher& pusher, ExplorePushesFeedback& feedback, bool execute_plan=true) 
            {
                Push push;
                ros::Duration(0.5).sleep();
                if (isObjectClear() && createRandomPushMsg(push)) {
                    feedback.push = push;
                    tf::poseEigenToMsg(getObjectTransform(push.approach.frame_id), feedback.pre_push);
                    bool success = performPush(pusher, push, feedback.id, execute_plan);
                    // Observe Relocation
                    tf::poseEigenToMsg(getObjectTransform(push.approach.frame_id), feedback.post_push);
                    return success;
                } else {
                    return false;
                }
            }

            bool performPush(ur5_pusher::Pusher& pusher, const Push& push, MoveObjectFeedback& feedback, bool execute_plan=true) 
            {
                ros::Duration(0.5).sleep();
                if (isObjectClear()) {
                    feedback.push = push;
                    tf::poseEigenToMsg(getObjectTransform(push.approach.frame_id), feedback.pre_push);
                    bool success = performPush(pusher, push, feedback.id, execute_plan);
                    // Observe Relocation
                    tf::poseEigenToMsg(getObjectTransform(push.approach.frame_id), feedback.post_push);
                    return success;
                } else {
                    return false;
                }
            }

            bool performPush(ur5_pusher::Pusher& pusher, const Push& push, int attempt_id, bool execute_plan=true) 
            {
                if(isObjectClear()) {

                    pusher.setPlannerId("RRTConnectkConfigDefault");
                    pusher.setPlanningTime(5.0);

                    //remove collision object in case the last attempt failed
                    removeCollisionObject();

                    // declare plan and start state
                    moveit::planning_interface::MoveGroupInterface::Plan push_plan;
                    moveit_msgs::RobotTrajectory approach_traj, push_traj, retreat_traj;

                    robot_state::RobotState rstate(*pusher.getCurrentState());

                    std::vector<std::vector<geometry_msgs::Pose>> waypoints;
                    std::vector<double> distances;

                    getPushWaypoints(push, waypoints, distances);

                    // start state to be set from IK
                    geometry_msgs::PoseStamped ps;
                    ps.pose = waypoints.front().front();
                    ps.header.frame_id = "table_top";
                    tf_listener_.transformPose(pusher.getPlanningFrame(), ps, ps);

                    if(pusher.setPusherJointValueTarget(ps)) {
                        robot_state::RobotState start_state = pusher.getJointValueTarget();

                        // apply collision object before moving to pre_push
                        applyCollisionObject();
                        if(!first_attempt_)
                            pusher.setPathConstraints(get_pusher_down_constraints());

                        // plan move to box
                        moveit::planning_interface::MoveGroupInterface::Plan move_to_box;
                        pusher.setStartStateToCurrentState();
                        if(!pusher.plan(move_to_box)) {
                            ROS_ERROR("Failed planning of movement to box!");
                            return false;
                        }

                        bool is_executing = true;
                        bool can_push = false;
                        int trajectory_execution_result_code = 0;
                        ros::Subscriber sub = nh_.subscribe<moveit_msgs::ExecuteTrajectoryActionResult>("/execute_trajectory/result", 1,
                                [&] (const moveit_msgs::ExecuteTrajectoryActionResultConstPtr& msg)
                                    {
                                        if(is_executing) {
                                            is_executing = false;
                                            trajectory_execution_result_code = msg->result.error_code.val;
                                            if(can_push) { // if already finished planning - have a short break!
                                                ros::Duration(0.5).sleep();
                                            }
                                        }
                                    });

                        if(!pusher.asyncExecute(move_to_box)) {
                            ROS_ERROR("Failed execution of movement to box!");
                            return false;
                        }

                        // remove all path constraints
                        pusher.clearPathConstraints();

                        // allow object collision
                        removeCollisionObject();

                        // plan push
                        can_push = computeCartesianPushTraj(pusher, waypoints, distances, approach_traj, push_traj, retreat_traj, start_state);

                        // wait for trajectory to finish
                        while(is_executing) {
                            ROS_ERROR_THROTTLE(1, "Moving to approach pose.");
                        }
                        pusher.setStartStateToCurrentState();
                        if(trajectory_execution_result_code != moveit_msgs::MoveItErrorCodes::SUCCESS) {
                            ROS_ERROR("Failed at trajectory execution! Aborting push attempt.");
                            return false;
                        }
                        if(!can_push) {
                            ROS_ERROR("Failed to plan push trajectory - Moving back!");
                            robot_trajectory::RobotTrajectory traj(pusher.getRobotModel(), pusher.getName());
                            moveit::planning_interface::MoveGroupInterface::Plan move_back;
                            traj.setRobotTrajectoryMsg((*pusher.getCurrentState()), move_to_box.trajectory_);
                            traj.reverse();
                            traj.getRobotTrajectoryMsg(move_back.trajectory_);
                            recomputeTimestamps(pusher, move_back.trajectory_);
                            applyCollisionObject();
                            pusher.execute(move_back);
                            return false;
                        }

                        // move to pre_push pose on first attempt
                        if(execute_plan && can_push) {

                            // create intermediate states (contact,  post push)
                            robot_state::RobotState contact_state(rstate);
                            contact_state.setJointGroupPositions(pusher.getName(), approach_traj.joint_trajectory.points.back().positions);

                            if(take_snapshots_) {

                                bool contact_shot = false;

                                // joint state callback
                                csm_->addUpdateCallback(
                                        [&] (const sensor_msgs::JointStateConstPtr& joint_state) {
                                        if(!contact_shot && joint_state->name.size()>0 && joint_state->name[0] == "ur5_shoulder_pan_joint") {
                                        rstate.setJointGroupPositions(pusher.getName(), joint_state->position);

                                        if(rstate.distance(contact_state) < 0.03) {
                                        contact_shot = true;
                                        take_snapshot(std::to_string(attempt_id) + "_2_contact");
                                        }
                                        }
                                        });
                            }

                            // concatenate trajectory parts
                            pusher.getCurrentState()->copyJointGroupPositions("arm", approach_traj.joint_trajectory.points[0].positions);
                            robot_trajectory::RobotTrajectory traj(pusher.getRobotModel(), pusher.getName());
                            robot_trajectory::RobotTrajectory append_traj(pusher.getRobotModel(), pusher.getName());
                            traj.setRobotTrajectoryMsg((*pusher.getCurrentState()), approach_traj);
                            append_traj.setRobotTrajectoryMsg(contact_state, push_traj);
                            traj.append(append_traj, 0.0);
                            // perform iterative time parameterization
                            traj.getRobotTrajectoryMsg(push_plan.trajectory_);
                            recomputeTimestamps(pusher, push_plan.trajectory_);

                            if(take_snapshots_)
                                take_snapshot(std::to_string(attempt_id) + "_1_before");

                            // EXECUTE PUSH
                            pusher.execute(push_plan);

                            if(take_snapshots_)
                                csm_->clearUpdateCallbacks();

                            if(take_snapshots_)
                                take_snapshot(std::to_string(attempt_id) + "_3_after");

                            // retreat trajectory
                            pusher.getCurrentState()->copyJointGroupPositions("arm", retreat_traj.joint_trajectory.points[0].positions);
                            recomputeTimestamps(pusher, retreat_traj);
                            push_plan.trajectory_ = retreat_traj;

                            pusher.execute(push_plan);

                            first_attempt_ = false;
                            return true;
                        }
                    } else {
                        ROS_INFO_STREAM("Failed to plan and execute push trajectory!");
                    }
                }
                return false;
            }

            bool pointAtBox(ur5_pusher::Pusher& pusher) {
                if(marker_.header.frame_id.empty())
                    return false;
                geometry_msgs::Pose pose;
                pose.position.z = 0.5 * marker_.scale.z + 0.025;
                pose.orientation.w = 1.0;
                pusher.setPoseReferenceFrame(marker_.header.frame_id);
                pusher.setPusherPoseTarget(pose);
                applyCollisionObject();
                bool success = bool(pusher.move());
                removeCollisionObject();
                return success;
            }

            void reset()
            {
                first_attempt_ = true;
            }

            geometry_msgs::Pose getObjectPose(const std::string& obj_frame, const std::string& target_frame="table_top")
            {
                geometry_msgs::PoseStamped obj_pose;
                obj_pose.header.frame_id = obj_frame;
                obj_pose.pose.orientation.w = 1.0;
                tf_listener_.transformPose(target_frame, obj_pose, obj_pose);
                return obj_pose.pose;
            }

            Eigen::Affine3d getObjectTransform(const std::string& obj_frame, const std::string& target_frame="table_top")
            {
                Eigen::Affine3d obj_pose_affine;
                tf::poseMsgToEigen(getObjectPose(obj_frame, target_frame), obj_pose_affine);
                return obj_pose_affine;
            }

            std::string getCurrentObjectID() {
                return marker_.header.frame_id;
            }

        private:

            bool isObjectClear() {
                // check if we have an object
                if (marker_.header.frame_id.empty()) {
                    ROS_WARN_THROTTLE(10, "Could not find any pushable object! Skipping current push.");
                    return false;
                }
                // check if the objects frame is up to date
                if(ros::Time(0) - marker_stamp_ > ros::Duration(0.5)) {
                    ROS_WARN_THROTTLE(10, "Object frame not up to date! Skipping current push.");
                    return false;
                }
                return true;
            }

            void recomputeTimestamps(ur5_pusher::Pusher& pusher, moveit_msgs::RobotTrajectory& trajectory_msg, double max_vel=3.14, double max_accel=1.0) {

                robot_trajectory::RobotTrajectory traj(pusher.getRobotModel(), pusher.getName());
                traj.setRobotTrajectoryMsg((*pusher.getCurrentState()), trajectory_msg);
                //trajectory_processing::IterativeSplineParameterization isp;
                trajectory_processing::IterativeParabolicTimeParameterization isp;
                isp.computeTimeStamps(traj, max_vel, max_accel);
                traj.getRobotTrajectoryMsg(trajectory_msg);
            }

            void applyCollisionObject() {
                std_msgs::ColorRGBA color;
                color.r = 0.5;
                color.a = 0.5;
                psi_.applyCollisionObject(obj_, color);
            }

            void removeCollisionObject() {
                std::vector<std::string> object_ids;
                object_ids.push_back(obj_.id);
                psi_.removeCollisionObjects(object_ids);
            }

            bool createRandomPushMsg(Push& push) {
                push.mode = Push::LINEAR;
                push_sampler_.setObject(marker_);
                push_sampler_.setObjectPose(marker_.pose);

                if(push_sampler_.sampleRandomPush(push))
                {
                    visualizePushApproach(push.approach);
                    return true;
                }
                return false;
            }

            void take_snapshot(const std::string& filename)
            {
                ur5_pushing_object_recognition::ImageDump srv;
                srv.request.filename = filename;
                snapshot_client_.call(srv);
            }

            moveit_msgs::Constraints get_pusher_down_constraints() {
                //Create orientation constraint - pusher down
                moveit_msgs::OrientationConstraint ocm;
                ocm.link_name = "s_model_tool0";
                ocm.header.frame_id = "table_top";
                ocm.orientation = tf::createQuaternionMsgFromRollPitchYaw(0, 0.5*M_PI, 0);
                ocm.absolute_x_axis_tolerance = 0.3;
                ocm.absolute_y_axis_tolerance = 0.3;
                ocm.absolute_z_axis_tolerance = 0.1;
                ocm.weight = 1.0;

                moveit_msgs::Constraints constraints;
                constraints.orientation_constraints.push_back(ocm);
                constraints.name = "pusher:down:0.3:0.1";
                return constraints;
            }

            void visualizePushApproach(const PushApproach& approach) {
                geometry_msgs::Pose pose;
                pose.position = approach.point;
                pose.orientation = approach.normal;
                visualizePushApproach(approach.frame_id, pose, approach.angle);
            }


            void visualizePushApproach(std::string frame_id, geometry_msgs::Pose pose, double angle, int id=0) {
                visualization_msgs::Marker approach;
                approach.type = visualization_msgs::Marker::ARROW;
                approach.header.frame_id = frame_id;
                approach.header.stamp = ros::Time();
                approach.id = id;
                approach.action = visualization_msgs::Marker::ADD;

                geometry_msgs::Pose push_direction;
                push_direction.orientation = tf::createQuaternionMsgFromYaw(angle);

                float arrow_len = 0.04;
                geometry_msgs::Pose arrow_offset;
                arrow_offset.orientation.w = 1;
                arrow_offset.position.x = -arrow_len;

                Eigen::Affine3d offset_affine;
                Eigen::Affine3d direction_affine;
                Eigen::Affine3d pose_affine;
                tf::poseMsgToEigen(arrow_offset, offset_affine);
                tf::poseMsgToEigen(push_direction, direction_affine);
                tf::poseMsgToEigen(pose, pose_affine);
                tf::poseEigenToMsg(pose_affine * direction_affine * offset_affine, approach.pose);

                approach.scale.x = arrow_len;
                approach.scale.y = 0.01;
                approach.scale.z = 0.01;
                approach.color.a = 1.0;
                approach.color.r = 1.0;
                approach.lifetime = ros::Duration(5);
                contact_point_pub_.publish(approach);
            }



            bool getPushWaypoints(const tams_ur5_push_execution::Push& push, std::vector<std::vector<geometry_msgs::Pose>>& waypoints, std::vector<double>& wp_distances) {

                // object pose
                geometry_msgs::PoseStamped obj_pose;
                obj_pose.header.frame_id = push.approach.frame_id;
                obj_pose.pose.orientation.w = 1.0;
                tf_listener_.transformPose("table_top", obj_pose, obj_pose);
                Eigen::Affine3d obj_pose_affine;
                tf::poseMsgToEigen(obj_pose.pose, obj_pose_affine);

                // push direction
                Eigen::Affine3d direction(Eigen::AngleAxis<double>(push.approach.angle, Eigen::Vector3d::UnitZ()));

                // approach point
                Eigen::Affine3d approach_affine(Eigen::Affine3d::Identity());
                geometry_msgs::Pose pose;
                pose.position = push.approach.point;

                // move approach pose outwards by tip_radius to disable early contact
                pose.position.x -= tip_radius_;
                pose.orientation = push.approach.normal;
                tf::poseMsgToEigen(pose, approach_affine);
                approach_affine = obj_pose_affine * approach_affine * direction;

                //trajectory distances
                float approach_distance = 0.05;
                float retreat_height = marker_.scale.z + 0.05;

                // fill waypoints
                geometry_msgs::Pose start_wp, approach_wp, push_wp, retreat_low_wp, retreat_high_wp;
                geometry_msgs::Quaternion orientation;
                orientation.w = 1.0;

                Eigen::Affine3d wp(Eigen::Affine3d::Identity()); // instantiate as identity transform!!!
                wp.translate(Eigen::Vector3d(-approach_distance, 0.0, 0.0));
                tf::poseEigenToMsg(approach_affine * wp, start_wp);
                start_wp.orientation = orientation;

                wp.translate(Eigen::Vector3d(approach_distance, 0.0, 0.0));
                tf::poseEigenToMsg(approach_affine * wp, approach_wp);
                approach_wp.orientation = orientation;

                wp.translate(Eigen::Vector3d(push.distance, 0.0, 0.0));
                tf::poseEigenToMsg(approach_affine * wp, push_wp);
                push_wp.orientation = orientation;

                wp.translate(Eigen::Vector3d(-0.015, 0.0, 0.0));
                tf::poseEigenToMsg(approach_affine * wp, retreat_low_wp);
                retreat_low_wp.orientation = orientation;

                wp.translate(Eigen::Vector3d(0.0, 0.0, retreat_height));
                tf::poseEigenToMsg(approach_affine * wp, retreat_high_wp);
                retreat_high_wp.orientation = orientation;

                std::vector<geometry_msgs::Pose> start_wps = {start_wp};
                std::vector<geometry_msgs::Pose> approach_wps = {approach_wp};
                std::vector<geometry_msgs::Pose> push_wps = {push_wp};
                std::vector<geometry_msgs::Pose> retreat_wps = {retreat_low_wp, retreat_high_wp};

                waypoints = {start_wps, approach_wps, push_wps, retreat_wps};

                double retreat_distance = std::sqrt(std::pow(push.distance,2) + std::pow(retreat_height, 2));
                wp_distances = {approach_distance, push.distance, retreat_distance};
            }

            bool computeCartesianPushTraj(ur5_pusher::Pusher& pusher, std::vector<std::vector<geometry_msgs::Pose>> waypoints, const std::vector<double> distances, moveit_msgs::RobotTrajectory& approach_traj, moveit_msgs::RobotTrajectory& push_traj, moveit_msgs::RobotTrajectory& retreat_traj, const robot_state::RobotState start_state)
            {
                ros::Time start_time = ros::Time::now();
                bool success = false;
                if(waypoints.size() == 4 && distances.size() == 3) {
                    pusher.setPoseReferenceFrame("table_top");

                    robot_state::RobotStatePtr next_state = std::make_shared<robot_state::RobotState>(start_state);
                    const moveit::core::JointModelGroup* jmg = start_state.getJointModelGroup(pusher.getName());

                    pusher.setStartState(start_state);
                    std::vector<geometry_msgs::Pose> wp_target;
                    std::vector<moveit_msgs::RobotTrajectory> trajectories;

                    success = true;
                    int i;
                    double success_fraction;
                    for(i=1; i < waypoints.size(); i++) {
                        moveit_msgs::RobotTrajectory traj;
                        success_fraction = pusher.computeCartesianPushPath(waypoints[i], 0.1 * distances[i-1], 3, traj);
                        if(success_fraction == 1.0) {
                            trajectories.push_back(traj);
                            next_state->setJointGroupPositions(jmg, traj.joint_trajectory.points.back().positions);
                            pusher.setStartState((*next_state));
                        } else {
                            ROS_ERROR_STREAM("Failed planning push trajectory step " << i+1 << " with success percentage " << success_fraction * 100 << "%");
                            success = false;
                            break;
                        }
                    }
                    if(success) {
                        ros::Duration planning_time = ros::Time::now() - start_time;
                        ROS_ERROR_STREAM("ComputePushTrajectory finished after " << planning_time.toSec() << " seconds.");
                        approach_traj = trajectories[0];
                        push_traj = trajectories[1];
                        retreat_traj = trajectories[2];
                    }
                    /*
                    //compute cartesian path
                    pusher.setStartState(start_state);
                    std::vector<geometry_msgs::Pose> wp_target = {waypoints[1]};
                    double approach_success = pusher.computeCartesianPushPath(wp_target, 0.1 * approach_distance, 3, approach_traj);
                    if(approach_success == 1.0) {
                    robot_state::RobotStatePtr rstate = pusher.getCurrentState();
                    moveit_msgs::RobotState wp_state;
                    const moveit::core::JointModelGroup* jmg = rstate->getJointModelGroup(pusher.getName());
                    rstate->setJointGroupPositions(jmg, approach_traj.joint_trajectory.points.back().positions);
                    moveit::core::robotStateToRobotStateMsg(*rstate, wp_state);
                    pusher.setStartState(wp_state);
                    wp_target = {waypoints[2]};
                    double push_success = pusher.computeCartesianPushPath(wp_target, 0.1 * push.distance, 3, push_traj);
                    if(push_success == 1.0) {
                    rstate->setJointGroupPositions(jmg, push_traj.joint_trajectory.points.back().positions);
                    moveit::core::robotStateToRobotStateMsg(*rstate, wp_state);
                    pusher.setStartState(wp_state);
                    wp_target = {waypoints[3]};
                    double retreat_success = pusher.computeCartesianPushPath(wp_target, 0.1*retreat_distance, 3, retreat_traj);
                    if(retreat_success == 1.0) {
                    success = true;
                    ros::Duration planning_time = ros::Time::now() - start_time;
                    ROS_ERROR_STREAM("ComputePushTrajectory finished after " << planning_time.toSec() << " seconds.");
                    } else {
                    ROS_ERROR_STREAM("Failed planning cartesian retreat path: " << retreat_success * 100 << "%");
                    }
                    } else {
                    ROS_ERROR_STREAM("Failed planning cartesian push path: " << push_success * 100 << "%");
                    }
                    } else {
                    ROS_ERROR_STREAM("Failed planning cartesian approach path: " << approach_success * 100 << "%");
                    }
                     */
                } else {
                    ROS_ERROR_STREAM("Failed to plan push trajectory. Number of waypoints or distances is invalid!");
                }
                return success;
            }

            void onDetectObjects(visualization_msgs::Marker marker) {
                if(marker_.id != marker.id && createCollisionObject(marker)) {
                    marker_ = marker;
                }
                marker_stamp_ = marker.header.stamp;
            }

            bool createCollisionObject(visualization_msgs::Marker& marker, float padding=0.015) {
                if(marker.type == visualization_msgs::Marker::CUBE) {
                    obj_.id = marker.header.frame_id + "_collision";
                    obj_.header.stamp = ros::Time(0);
                    obj_.header.frame_id = marker.header.frame_id;
                    obj_.primitive_poses.resize(1);
                    obj_.primitive_poses[0].orientation.w = 1;
                    obj_.primitive_poses[0].position.z = 0.5 * padding;
                    obj_.primitives.resize(1);
                    obj_.operation = moveit_msgs::CollisionObject::ADD;
                    obj_.primitives[0].type = shape_msgs::SolidPrimitive::BOX;
                    obj_.primitives[0].dimensions.resize(3);
                    obj_.primitives[0].dimensions[shape_msgs::SolidPrimitive::BOX_X] = marker.scale.x + 2 * padding;
                    obj_.primitives[0].dimensions[shape_msgs::SolidPrimitive::BOX_Y] = marker.scale.y + 2 * padding;
                    obj_.primitives[0].dimensions[shape_msgs::SolidPrimitive::BOX_Z] = marker.scale.z + padding;
                    return true;
                }
                return false;
            }
    };
}