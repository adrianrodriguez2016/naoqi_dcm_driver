/*
 * Copyright 2016 SoftBank Robotics Europe
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#include <sensor_msgs/JointState.h>

#include <diagnostic_msgs/DiagnosticArray.h>

#include "naoqi_dcm_driver/robot.hpp"
#include "naoqi_dcm_driver/tools.hpp"

QI_REGISTER_OBJECT( Robot,
                    isConnected,
                    connect,
                    stopService);

Robot::Robot(qi::SessionPtr session):
               _session(session),
               session_name_("naoqi_dcm_driver"),
               is_connected_(false),
               nhPtr_(new ros::NodeHandle("")),
               body_type_(""),
               topic_queue_(10),
               prefix_("naoqi_dcm"),
               high_freq_(50.0),
               controller_freq_(15.0),
               joint_precision_(0.1),
               odom_frame_("odom"),
               use_dcm_(false),
               use_cmd_vel_(false)
{
}

Robot::~Robot()
{
  stopService();
}

void Robot::stopService() {
  ROS_INFO_STREAM(session_name_ << " stopping the service...");

  if (motion_)
  {
    /* reset stiffness for arms if using DCM
     * to prevent its concurrence with ALMotion */
    if(use_dcm_)
      motion_->setStiffnessArms(0.0f, 1.0f);

    //going to rest
    if (motor_groups_.size() == 1)
      if (motor_groups_[0] == "Body")
        motion_->rest();

    //set stiffness for the whole body
    setStiffness(0.0f);
  }

  is_connected_ = false;

  if(nhPtr_)
  {
    nhPtr_->shutdown();
    ros::shutdown();
  }
}

bool Robot::initializeControllers(const std::vector <std::string> &joints_names)
{
  int joints_nbr = joints_names.size();

  // Initialize Controllers' Interfaces
  joint_angles_.reserve(joints_nbr);
  joint_velocities_.reserve(joints_nbr);
  joint_efforts_.reserve(joints_nbr);
  joint_commands_.reserve(joints_nbr);

  joint_angles_.resize(joints_nbr);
  joint_velocities_.resize(joints_nbr);
  joint_efforts_.resize(joints_nbr);
  joint_commands_.resize(joints_nbr);

  try
  {
    for(int i=0; i<joints_nbr; ++i)
    {
      hardware_interface::JointStateHandle state_handle(joints_names.at(i), &joint_angles_[i],
                                                        &joint_velocities_[i], &joint_efforts_[i]);
      jnt_state_interface_.registerHandle(state_handle);

      hardware_interface::JointHandle pos_handle(jnt_state_interface_.getHandle(joints_names.at(i)),
                                                 &joint_commands_[i]);
      jnt_pos_interface_.registerHandle(pos_handle);
    }

    registerInterface(&jnt_state_interface_);
    registerInterface(&jnt_pos_interface_);
  }
  catch(const ros::Exception& e)
  {
    ROS_ERROR("Could not initialize hardware interfaces!\n\tTrace: %s", e.what());
    return false;
  }

  return true;
}


// The entry point from outside
bool Robot::connect()
{
  is_connected_ = false;

  // Load ROS Parameters
  loadParams();

  // Initialize DCM Wrapper
  if (use_dcm_)
    dcm_ = boost::shared_ptr<DCM>(new DCM(_session, controller_freq_));

  // Initialize Memory Wrapper
  memory_ = boost::shared_ptr<Memory>(new Memory(_session));

  //get the robot's name
  std::string robot = memory_->getData("RobotConfig/Body/Type");
  std::transform(robot.begin(), robot.end(), robot.begin(), ::tolower);

  // Initialize Motion Wrapper
  motion_ = boost::shared_ptr<Motion>(new Motion(_session));

  // check if the robot is waked up
  if (motor_groups_.size() == 1)
  {
    if (motor_groups_[0] == "Body")
      motion_->wakeUp();
  }
  else if (!use_dcm_)
    motion_->wakeUp();
  if (!motion_->robotIsWakeUp())
  {
    ROS_ERROR("Please, wakeUp the robot to be able to set stiffness");
    stopService();
    return false;
  }

  if (use_dcm_)
    motion_->manageConcurrence();

  //read joints names that will be controlled
  std::vector <std::string> joints_names = motion_->getBodyNamesFromGroup(motor_groups_);
  ignoreMimicJoints(&joints_names);
  ROS_INFO_STREAM("The following joints are controlled: " << print(joints_names));

  //initialise Memory, Motion, and DCM classes with controlled joints
  memory_->init(joints_names);
  motion_->init(joints_names);
  if (use_dcm_)
    dcm_->init(joints_names);

  //read joints names to initialize the joint_states topic
  joint_states_topic_.header.frame_id = "base_link";
  joint_states_topic_.name = motion_->getBodyNames("Body"); //should be Body=JointActuators+Wheels
  joint_states_topic_.position.resize(joint_states_topic_.name.size());

  //read joints names to initialize the diagnostics
  std::vector<std::string> joints_all_names = motion_->getBodyNames("JointActuators");
  diagnostics_ = boost::shared_ptr<Diagnostics>(
        new Diagnostics(_session, &diag_pub_, joints_all_names, robot));

  is_connected_ = true;

  // Subscribe/Publish ROS Topics/Services
  subscribe();

  // Turn Stiffness On
  if (!setStiffness(1.0f))
    return false;

  // Initialize Controller Manager and Controllers
  try
  {
    manager_ = new controller_manager::ControllerManager( this, *nhPtr_);
  }
  catch(const ros::Exception& e)
  {
    ROS_ERROR("Could not initialize controller manager!\n\tTrace: %s", e.what());
    return false;
  }

  if(!initializeControllers(joints_names))
    return false;

  ROS_INFO_STREAM(session_name_ << " module initialized!");
  return true;
}

void Robot::subscribe()
{
  // Subscribe/Publish ROS Topics/Services
  if (use_cmd_vel_)
    cmd_vel_sub_ = nhPtr_->subscribe(prefix_+"cmd_vel", topic_queue_, &Robot::commandVelocity, this);

  diag_pub_ = nhPtr_->advertise<diagnostic_msgs::DiagnosticArray>(prefix_+"diagnostics", topic_queue_);

  stiffness_pub_ = nhPtr_->advertise<std_msgs::Float32>(prefix_+"stiffnesses", topic_queue_);
  stiffness_.data = 1.0f;

  joint_states_pub_ = nhPtr_->advertise<sensor_msgs::JointState>("/joint_states", topic_queue_);
}

void Robot::loadParams()
{
  ros::NodeHandle nh("~");
  // Load Server Parameters
  nh.getParam("BodyType", body_type_);
  nh.getParam("TopicQueue", topic_queue_);
  nh.getParam("HighCommunicationFrequency", high_freq_);
  nh.getParam("ControllerFrequency", controller_freq_);
  nh.getParam("JointPrecision", joint_precision_);
  nh.getParam("OdomFrame", odom_frame_);
  nh.getParam("use_dcm", use_dcm_);
  if (use_dcm_)
    ROS_WARN_STREAM("Please, be carefull! You have chosen to control the robot based on DCM. "
                    << "This leads to concurrence between DCM and ALMotion and "
                    << "it can cause shaking the robot. If it starts shaking, stop the node, "
                    << "for example by pressing Ctrl+C");

  //set the prefix for topics
  nh.getParam("Prefix", prefix_);
  if (prefix_.length() > 0)
    if (prefix_.at(prefix_.length()-1) != '/')
      prefix_ += "/";

  //set the motors groups to control
  std::string tmp="";
  nh.getParam("motor_groups", tmp);
  boost::split (motor_groups_, tmp, boost::is_any_of(" "));
  if (motor_groups_.size() == 1)
    if (motor_groups_[0].empty())
      motor_groups_.clear();
  if (motor_groups_.size() == 0)
  {
    motor_groups_.push_back("LArm");
    motor_groups_.push_back("RArm");
  }
}

void Robot::run()
{
  controllerLoop();
}

void Robot::controllerLoop()
{
  static ros::Rate rate(controller_freq_);
  while(ros::ok())
  {
    ros::Time time = ros::Time::now();

    if(!is_connected_)
      break;

    //publishBaseFootprint(time);

    stiffness_pub_.publish(stiffness_);

    readJoints();

    if (!diagnostics_->publish())
      stopService();

    try
    {
      manager_->update(time, ros::Duration(1.0f/controller_freq_));
    }
    catch(ros::Exception& e)
    {
      ROS_ERROR("%s", e.what());
      return;
    }

    writeJoints();

    publishJointStateFromAlMotion();

    rate.sleep();
  }
  ROS_INFO_STREAM("Shutting down the main loop");
}

bool Robot::isConnected()
{
  return is_connected_;
}

void Robot::commandVelocity(const geometry_msgs::TwistConstPtr &msg)
{
  //reset stiffness for arms if using DCM to prevent its concurrence with ALMotion
  if(use_dcm_)
    motion_->setStiffnessArms(0.0f, 1.0f);

  motion_->moveTo(msg->linear.x, msg->linear.y, msg->angular.z);
  sleep(1.0);

  //set stiffness for arms if using DCM
  if(use_dcm_)
    motion_->setStiffnessArms(1.0f, 1.0f);
}

void Robot::publishBaseFootprint(const ros::Time &ts)
{
  std::string odom_frame, base_link_frame;
  try 
  {
    odom_frame = base_footprint_listener_.resolve(odom_frame_);
    base_link_frame = base_footprint_listener_.resolve("base_link");
  }
  catch(ros::Exception& e)
  {
    ROS_ERROR("%s", e.what());
    return;
  }

  tf::StampedTransform tf_odom_to_base, tf_odom_to_left_foot, tf_odom_to_right_foot;
  double temp_freq = 1.0f/(10.0*high_freq_);
  if(!base_footprint_listener_.waitForTransform(odom_frame, base_link_frame, ros::Time(0), ros::Duration(temp_freq)))
    return;
  try 
  {
    base_footprint_listener_.lookupTransform(odom_frame, base_link_frame,  ros::Time(0), tf_odom_to_base);
  }
  catch (const tf::TransformException& ex)
  {
    ROS_ERROR("%s",ex.what());
    return ;
  }

  tf::Vector3 new_origin = (tf_odom_to_right_foot.getOrigin() + tf_odom_to_left_foot.getOrigin())/2.0;
  double height = std::min(tf_odom_to_left_foot.getOrigin().getZ(), tf_odom_to_right_foot.getOrigin().getZ());
  new_origin.setZ(height);

  double roll, pitch, yaw;
  tf_odom_to_base.getBasis().getRPY(roll, pitch, yaw);

  tf::Transform tf_odom_to_footprint(tf::createQuaternionFromYaw(yaw), new_origin);
  tf::Transform tf_base_to_footprint = tf_odom_to_base.inverse() * tf_odom_to_footprint;

  base_footprint_broadcaster_.sendTransform(tf::StampedTransform(tf_base_to_footprint, ts,
                                                                 base_link_frame, "base_footprint"));
}

void Robot::readJoints()
{
  //read memory keys
  std::vector<float> joint_positions = memory_->getListData();

  //store joints angles
  std::vector <double>::iterator it_command = joint_commands_.begin();
  std::vector <double>::iterator it_now = joint_angles_.begin();
  std::vector <float>::iterator it_sensor = joint_positions.begin();
  for(; it_command<joint_commands_.end(); ++it_command, ++it_now, ++it_sensor)
  {
    *it_now = *it_sensor;
    // Set commands to the read angles for when no command specified
    *it_command = *it_sensor;
  }
}

void Robot::publishJointStateFromAlMotion(){
  joint_states_topic_.header.stamp = ros::Time::now();

  std::vector<double> position_data = motion_->getAngles("Body");
  for(int i = 0; i<position_data.size(); ++i)
    joint_states_topic_.position[i] = position_data[i];

  joint_states_pub_.publish(joint_states_topic_);
}

void Robot::writeJoints()
{
  // Check if there is some change in joints values
  bool changed(false);
  std::vector<double>::iterator it_now = joint_angles_.begin();
  std::vector<double>::iterator it_command = joint_commands_.begin();
  for(int i=0; it_command != joint_commands_.end(); ++i, ++it_command, ++it_now)
  {
    double diff = fabs(*it_command - *it_now);
    if(diff > joint_precision_)
    {
      //ROS_INFO_STREAM(" joint " << i << " : diff=" << diff);
      changed = true;
      break;
    }
  }

  // Update joints values if there are some changes
  if(!changed)
    return;

  if (use_dcm_)
    dcm_->writeJoints(joint_commands_);
  else
    motion_->writeJoints(joint_commands_);
}

void Robot::ignoreMimicJoints(std::vector <std::string> *joints)
{
  //ignore mimic joints
  for(std::vector<std::string>::iterator it=joints->begin(); it!=joints->end(); ++it)
  {
    if( (it->find("Wheel") != std::string::npos)
         || (*it=="RHand" || *it=="LHand" || *it == "RWristYaw" || *it == "LWristYaw") && (body_type_ == "H21"))
    {
      joints->erase(it);
      it--;
    }
  }
}

//  rewrite by calling DCM rather than ALMotion
bool Robot::setStiffness(const float &stiffness)
{
  stiffness_.data = stiffness;

  if (!motion_->stiffnessInterpolation(motor_groups_, stiffness, 1.0f))
    return false;

  return true;
}
