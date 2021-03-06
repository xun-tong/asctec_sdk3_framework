/*
 * AciRemote.cpp
 *
 *  Created on: 14 Feb 2014
 *      Author: Murilo F. M.
 *      Email:  muhrix@gmail.com
 *
 */

#include "asctec_hlp_interface/AciRemote.h"
#include "asctec_hlp_interface/Helper.h"

#include <geometry_msgs/Vector3.h>
#include <geometry_msgs/Quaternion.h>
#include <tf/transform_datatypes.h>

#include <sensor_msgs/Imu.h>
#include <sensor_msgs/NavSatFix.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/Vector3Stamped.h>

#include "asctec_hlp_comm/mav_imu.h"
#include "asctec_hlp_comm/mav_rcdata.h"
#include "asctec_hlp_comm/mav_hlp_status.h"
#include "asctec_hlp_comm/MotorSpeed.h"
#include "asctec_hlp_comm/GpsCustom.h"

// by Xun
#include "asctec_hlp_comm/mav_laser.h"

#include <sstream>

namespace AciRemote {

void* aci_obj_ptr = NULL;

AciRemote::AciRemote(ros::NodeHandle& nh):
		SerialComm(), n_(nh), bytes_recv_(0),
		versions_match_(false), var_list_recv_(false),
		cmd_list_recv_(false), par_list_recv_(false),
		must_stop_engine_(false), must_stop_pub_(false) {

	// assign *this pointer of this instanced object to global pointer for use with callbacks
	aci_obj_ptr = static_cast<void*>(this);

	// fetch values from ROS parameter server
    n_.param<std::string>("serial_port", port_name_, std::string("/dev/ttyS2"));
    n_.param<int>("baudrate", baud_rate_, 57600);
    n_.param<std::string>("frame_id", frame_id_, std::string(n_.getNamespace() + "_base_link"));
	// parameters below will (that is, should) be moving to dynamic reconfigure in a later release
    n_.param<int>("packet_rate_imu_mag", imu_rate_, 50);
    n_.param<int>("packet_rate_gps", gps_rate_, 5);
    n_.param<int>("packet_rate_rcdata_status_motors", rc_status_rate_, 10);
    n_.param<int>("aci_engine_throttle", aci_rate_, 100);
    n_.param<int>("aci_heartbeat", aci_heartbeat_, 10);
    n_.param<double>("stddev_angular_velocity", ang_vel_variance_, 0.013); // taken from experiments
    n_.param<double>("stddev_linear_acceleration", lin_acc_variance_, 0.083); // taken from experiments
    n_.param<bool>("externalise_robot_state", externalise_state_, bool(true));
	ang_vel_variance_ *= ang_vel_variance_;
	lin_acc_variance_ *= lin_acc_variance_;

    n_.param<int>("packet_rate_laser_mag", laser_rate_, 50);    // by Xun

	// fetch topic names from ROS parameter server
    n_.param<std::string>("imu_topic", imu_topic_, std::string("imu"));
    n_.param<std::string>("imu_custom_topic", imu_custom_topic_, std::string("imu_custom"));
    n_.param<std::string>("mag_topic", mag_topic_, std::string("mag"));
    n_.param<std::string>("gps_topic", gps_topic_, std::string("gps"));
    n_.param<std::string>("gps_custom_topic", gps_custom_topic_, std::string("gps_custom"));
    n_.param<std::string>("rcdata_topic", rcdata_topic_, std::string("rcdata"));
    n_.param<std::string>("status_topic", status_topic_, std::string("status"));
    n_.param<std::string>("estate_externalisation_topic", extern_topic_, std::string("/espeak_node/speak_line"));
    n_.param<std::string>("motor_speed_topic", motor_topic_, std::string("motor_speed"));
    n_.param<std::string>("cmd_vel_topic", ctrl_topic_, std::string("cmd_vel"));
    n_.param<std::string>("ctrl_service", ctrl_srv_name_, std::string("set_uav_control"));

    n_.param<std::string>("laser_topic", laser_topic_, std::string("laser"));   // by Xun


	// TODO: Initialise Asctec SDK3 Command data structures before enabling RC serial switch
	//WO_SDK_.ctrl_mode = 0x02;
	//WO_SDK_.ctrl_enabled = 0x00;
	//WO_SDK_.disable_motor_onoff_by_stick = 0x00;
}

AciRemote::~AciRemote() {
	// first of all, close serial port, otherwise pure virtual method would be called
	closePort();
	// interrupt all running threads and wait for them to return
	{
		boost::upgrade_lock<boost::shared_mutex> up_lock(shared_mtx_);
		boost::upgrade_to_unique_lock<boost::shared_mutex> un_lock(up_lock);
		must_stop_pub_ = true;
	}
	if (imu_mag_thread_.get() != NULL)
		imu_mag_thread_->join();
	if (gps_thread_.get() != NULL)
		gps_thread_->join();
	if (rc_status_thread_.get() != NULL)
		rc_status_thread_->join();

  // by Xun
  if (laser_thread_.get() != NULL)
    laser_thread_->join();



	boost::unique_lock<boost::mutex> u_lock(buf_mtx_);
	must_stop_engine_ = true;
	u_lock.unlock();
	if (aci_throttle_thread_.get() != NULL)
		aci_throttle_thread_->join();

	// at this point, ros::spin() will not be called anymore and thus ROS_INFO will not work
	//ROS_INFO("All threads have shutdown");

	// assign NULL to global object pointer
	aci_obj_ptr = static_cast<void*>(NULL);
}



//-------------------------------------------------------
//	Public member functions
//-------------------------------------------------------

int AciRemote::init() {
	// open and configure serial port
	if (openPort() < 0) {
		return -1;
	}
	// initialise ACI Remote
	aciInit();
	ROS_INFO("Asctec ACI initialised");

	// set callbacks
	aciSetSendDataCallback(AciRemote::transmit);
	aciInfoPacketReceivedCallback(AciRemote::versions);
	aciSetVarListUpdateFinishedCallback(AciRemote::varListUpdateFinished);
	aciSetCmdListUpdateFinishedCallback(AciRemote::cmdListUpdateFinished);
	aciSetParamListUpdateFinishedCallback(AciRemote::paramListUpdateFinished);
	aciSetEngineRate(aci_rate_, aci_heartbeat_);

	try {
		aci_throttle_thread_ = boost::shared_ptr<boost::thread>
			(new boost::thread(boost::bind(&AciRemote::throttleEngine, this)));
	}
	catch (boost::system::system_error::exception& e) {
		ROS_ERROR_STREAM("Could not create ACI Engine thread. " << e.what());
	}

	cond_.notify_one();

	// request version info and lists of commands, parameters and variables to HLP
	aciCheckVerConf();
	aciGetDeviceCommandsList();
	aciGetDeviceParametersList();
	aciGetDeviceVariablesList();

	return 0;
}

int AciRemote::initRosLayer() {
	int c = 0;
	boost::asio::io_service io;
	while (++c < 4) {
		boost::unique_lock<boost::mutex> u_lock(mtx_);
		if (versions_match_ && var_list_recv_ && cmd_list_recv_ && par_list_recv_) {
			// advertise ROS topics
			imu_pub_ = n_.advertise<sensor_msgs::Imu>(imu_topic_, 1);
			imu_custom_pub_ = n_.advertise<asctec_hlp_comm::mav_imu>(imu_custom_topic_, 1);
			mag_pub_ = n_.advertise<geometry_msgs::Vector3Stamped>(mag_topic_, 1);
			gps_pub_ = n_.advertise<sensor_msgs::NavSatFix>(gps_topic_, 1);
			gps_custom_pub_ = n_.advertise<asctec_hlp_comm::GpsCustom>(gps_custom_topic_, 1);
			rcdata_pub_ = n_.advertise<asctec_hlp_comm::mav_rcdata>(rcdata_topic_, 1);
            status_pub_ = n_.advertise<asctec_hlp_comm::mav_hlp_status>(status_topic_, 1);
			motor_pub_ = n_.advertise<asctec_hlp_comm::MotorSpeed>(motor_topic_, 1);

      laser_pub_ = n_.advertise<asctec_hlp_comm::mav_laser>(laser_topic_, 1);   // by Xun

            // only advertise topic if parameter is set to true
            if (externalise_state_) {
                extern_pub_ = n_.advertise<std_msgs::String>(extern_topic_, 10);
            }

			ctrl_sub_ = n_.subscribe(ctrl_topic_, 1, &AciRemote::ctrlTopicCallback, this);

			ctrl_srv_ = n_.advertiseService(ctrl_srv_name_, &AciRemote::ctrlServiceCallback, this);
			//motor_srv_ = n_.advertiseService(motors_srv_name_,
			// &AciRemote::ctrlMotorsCallback, this);

			// spawn publisher threads
			try {
				imu_mag_thread_ = boost::shared_ptr<boost::thread>
					(new boost::thread(boost::bind(&AciRemote::publishImuMagData, this)));
			}
			catch (boost::system::system_error::exception& e) {
				ROS_ERROR_STREAM("Could not create IMU publisher thread. " << e.what());
			}
			try {
				gps_thread_ = boost::shared_ptr<boost::thread>
					(new boost::thread(boost::bind(&AciRemote::publishGpsData, this)));
			}
			catch (boost::system::system_error::exception& e) {
				ROS_ERROR_STREAM("Could not create GPS publisher thread. " << e.what());
			}
			try {
				rc_status_thread_ = boost::shared_ptr<boost::thread>
					(new boost::thread(boost::bind(&AciRemote::publishStatusMotorsRcData, this)));
			}
			catch (boost::system::system_error::exception& e) {
				ROS_ERROR_STREAM("Could not create Status publisher thread. " << e.what());
			}

      // by Xun
      try {
        laser_thread_ = boost::shared_ptr<boost::thread>
          (new boost::thread(boost::bind(&AciRemote::publishLaserData, this)));
      }
      catch (boost::system::system_error::exception& e) {
        ROS_ERROR_STREAM("Could not create laser publisher thread. " << e.what());
      }


			cond_any_.notify_all();

			return 0;
		}
		else {
			// release mutex, wait 5 seconds and try again (no more than 3 times)
			u_lock.unlock();
			boost::asio::deadline_timer t(io, boost::posix_time::seconds(5));
			t.wait();
		}
	}
	return -1;
}

int AciRemote::setGpsWaypoint(const asctec_hlp_comm::WaypointGPSGoalConstPtr& pose) {
	short uav_status, waypt_state;
	double roll, pitch, yaw;
	boost::shared_lock<boost::shared_mutex> s_lock(shared_mtx_);
	uav_status = RO_ALL_Data_.UAV_status;
	waypt_state = wayptStatus_;
	s_lock.unlock();

	// check flight mode
	if ((uav_status & 0x000F) != HLP_FLIGHTMODE_GPS) {
		return -1;
	}
	// always make sure that the condition (4 at the moment) is in accordance with the
	// state of the state machine implemented on the HLP firmware (sdk.c)
	else if (waypt_state < 4) {
		return waypt_state;
	}
	// GPS mode is enabled and state machine on HLP is in states 4 or 5
	// hence, waypoint may be sent over to HLP (and then to LLP from within HLP)
	boost::mutex::scoped_lock lock(ctrl_mtx_);

	// convert pose waypoint into HLP-format waypoint
	WO_wpToLL_.X = static_cast<int>(pose->geo_pose.position.latitude * 1.0e7);
	WO_wpToLL_.Y = static_cast<int>(pose->geo_pose.position.longitude * 1.0e7);
	WO_wpToLL_.height = static_cast<int>(pose->geo_pose.position.altitude * 1.0e3);

	tf::Quaternion q;
	tf::quaternionMsgToTF(pose->geo_pose.orientation, q);
	tf::Matrix3x3(q).getRPY(roll, pitch, yaw);
	// yaw: 0...360000 => 1000 = 1 degree
	WO_wpToLL_.yaw = static_cast<int>(yaw * 180000.0/M_PI);
	// max speed in GPS mode is approx. 10 m/s
	// see http://wiki.asctec.de/xwiki/bin/view/AscTec+UAVs/First+steps
	WO_wpToLL_.max_speed =
			static_cast<unsigned char>(std::min(100.0, pose->max_speed * 10.0));
	WO_wpToLL_.time =
			static_cast<unsigned short>(pose->timeout * 100.0);
	WO_wpToLL_.pos_acc =
			static_cast<unsigned short>(pose->position_accuracy * 1.0e3);

	WO_wpToLL_.wp_activated = 1;
	WO_wpToLL_.properties = WPPROP_ABSCOORDS | WPPROP_AUTOMATICGOTO
			| WPPROP_HEIGHTENABLED | WPPROP_YAWENABLED;

	WO_wpToLL_.chksum = 0xAAAA
			+ WO_wpToLL_.yaw
			+ WO_wpToLL_.height
			+ WO_wpToLL_.time
			+ WO_wpToLL_.X
			+ WO_wpToLL_.Y
			+ WO_wpToLL_.max_speed
			+ WO_wpToLL_.pos_acc
			+ WO_wpToLL_.properties
			+ WO_wpToLL_.wp_activated;

	//wpCtrlWpCmd_ = WP_CMD_SINGLE_WP;
	wpCtrlWpCmd_ = static_cast<unsigned char>(pose->command);

	// update control commands packet
    aciUpdateCmdPacket(0);
	// update waypoint command packet
	aciUpdateCmdPacket(2);

	return waypt_state;
}

void AciRemote::getGpsWayptNavStatus(unsigned short& waypt_nav_status,
		double& dist_to_goal) {
	boost::shared_lock<boost::shared_mutex> s_lock(shared_mtx_);
	waypt_nav_status = wpCtrlNavStatus_;
	// current distance to waypoint is in dm (=10cm)
	dist_to_goal = double(wpCtrlDistToWp_) * 0.1;
}

void AciRemote::getGpsWayptState(unsigned short& waypt_state) {
	boost::shared_lock<boost::shared_mutex> s_lock(shared_mtx_);
	waypt_state = wayptStatus_;
}

void AciRemote::getGpsWayptResultPose(asctec_hlp_comm::WaypointGPSResult& result) {
	boost::shared_lock<boost::shared_mutex> s_lock(shared_mtx_);

	result.geo_pose.position.latitude =
			static_cast<double>(RO_ALL_Data_.fusion_latitude) * 1.0e-7;
	result.geo_pose.position.longitude =
			static_cast<double>(RO_ALL_Data_.fusion_longitude) * 1.0e-7;
	// TODO: check whether altitude should be GPS_height * 1.0e-3 instead
	result.geo_pose.position.altitude =
			static_cast<double>(RO_ALL_Data_.fusion_height * 1.0e-3);

	// TODO: use data from IMU for more accurate estimate
	tf::Quaternion q;
	q.setRPY(0.0, 0.0, static_cast<double>(RO_ALL_Data_.GPS_heading)*M_PI/180000.0);
	tf::quaternionTFToMsg(q, result.geo_pose.orientation);

	result.status = wpCtrlNavStatus_;
}



//-------------------------------------------------------
//	Protected member functions
//-------------------------------------------------------

void AciRemote::checkVersions(struct ACI_INFO aciInfo) {
    ROS_INFO("Received versions list from HLP");
	bool match = true;

	ROS_INFO("Type\t\t\tDevice\t\tRemote");
	ROS_INFO("Major version\t\t%d\t=\t\%d",aciInfo.verMajor,ACI_VER_MAJOR);
	ROS_INFO("Minor version\t\t%d\t=\t\%d",aciInfo.verMinor,ACI_VER_MINOR);
	ROS_INFO("MAX_DESC_LENGTH\t\t%d\t=\t\%d",aciInfo.maxDescLength,MAX_DESC_LENGTH);
	ROS_INFO("MAX_NAME_LENGTH\t\t%d\t=\t\%d",aciInfo.maxNameLength,MAX_NAME_LENGTH);
	ROS_INFO("MAX_UNIT_LENGTH\t\t%d\t=\t\%d",aciInfo.maxUnitLength,MAX_UNIT_LENGTH);
	ROS_INFO("MAX_VAR_PACKETS\t\t%d\t=\t\%d",aciInfo.maxVarPackets,MAX_VAR_PACKETS);

	if (aciInfo.verMajor != ACI_VER_MAJOR ||
			aciInfo.verMinor != ACI_VER_MINOR ||
			aciInfo.maxDescLength != MAX_DESC_LENGTH ||
			aciInfo.maxNameLength != MAX_NAME_LENGTH ||
			aciInfo.maxUnitLength != MAX_UNIT_LENGTH ||
			aciInfo.maxVarPackets != MAX_VAR_PACKETS) {
		match = false;
	}

	if (match) {
		boost::mutex::scoped_lock lock(mtx_);
		versions_match_ = true;
	}
	else {
		ROS_ERROR_STREAM("ACI versions do not match. Must abort now.");
	}
}

void AciRemote::setupVarPackets() {
	ROS_INFO("Received variables list from HLP");
	// setup variables packets to be received
	// along with reception rate (not more than ACI Engine rate)

	// packet ID 0 containing: status, motor speed and RC data
	// status data
	aciAddContentToVarPacket(0, 0x0001, &RO_ALL_Data_.UAV_status);
	aciAddContentToVarPacket(0, 0x0002, &RO_ALL_Data_.flight_time);
	aciAddContentToVarPacket(0, 0x0003, &RO_ALL_Data_.battery_voltage);
	aciAddContentToVarPacket(0, 0x0004, &RO_ALL_Data_.HL_cpu_load);
	aciAddContentToVarPacket(0, 0x0005, &RO_ALL_Data_.HL_up_time);
	// motor speed data
	aciAddContentToVarPacket(0, 0x0100, &RO_ALL_Data_.motor_rpm[0]);
	aciAddContentToVarPacket(0, 0x0101, &RO_ALL_Data_.motor_rpm[1]);
	aciAddContentToVarPacket(0, 0x0102, &RO_ALL_Data_.motor_rpm[2]);
	aciAddContentToVarPacket(0, 0x0103, &RO_ALL_Data_.motor_rpm[3]);
	// Rc data
	aciAddContentToVarPacket(0, 0x0600, &RO_ALL_Data_.channel[0]);
	aciAddContentToVarPacket(0, 0x0601, &RO_ALL_Data_.channel[1]);
	aciAddContentToVarPacket(0, 0x0602, &RO_ALL_Data_.channel[2]);
	aciAddContentToVarPacket(0, 0x0603, &RO_ALL_Data_.channel[3]);
	aciAddContentToVarPacket(0, 0x0604, &RO_ALL_Data_.channel[4]);
	aciAddContentToVarPacket(0, 0x0605, &RO_ALL_Data_.channel[5]);
	aciAddContentToVarPacket(0, 0x0606, &RO_ALL_Data_.channel[6]);
	aciAddContentToVarPacket(0, 0x0607, &RO_ALL_Data_.channel[7]);
	// data after sensor fusion
	aciAddContentToVarPacket(0, 0x0303, &RO_ALL_Data_.fusion_latitude);
	aciAddContentToVarPacket(0, 0x0304, &RO_ALL_Data_.fusion_longitude);
	aciAddContentToVarPacket(0, 0x0305, &RO_ALL_Data_.fusion_dheight);
	aciAddContentToVarPacket(0, 0x0306, &RO_ALL_Data_.fusion_height);
	aciAddContentToVarPacket(0, 0x0307, &RO_ALL_Data_.fusion_speed_x);
	aciAddContentToVarPacket(0, 0x0308, &RO_ALL_Data_.fusion_speed_y);
	aciAddContentToVarPacket(0, 0x100C, &wpCtrlNavStatus_);
	aciAddContentToVarPacket(0, 0x100D, &wpCtrlDistToWp_);
	// TODO the line below sets up the address I should be using, but
	// I found a typo in the firmware (main.c) which I do not have time
	// change at the moment (need to re-compile and re-flash the firmware to the HLP)
	//aciAddContentToVarPacket(0, 0x100E, &wayptStatus_);
	// TODO the line below is currently setting the address according to main.c
	// as of 30 Apr 2014
	aciAddContentToVarPacket(0, 0x101E, &wayptStatus_);
	// debug variables
	aciAddContentToVarPacket(0, 0x100F, &RO_SDK_.ctrl_mode);
	aciAddContentToVarPacket(0, 0x1010, &RO_SDK_.ctrl_enabled);
	aciAddContentToVarPacket(0, 0x1011, &RO_SDK_.disable_motor_onoff_by_stick);
	//aciAddContentToVarPacket(0, 0x1015, &debug1_);
	//aciAddContentToVarPacket(0, 0x1016, &debug2_);
	//aciAddContentToVarPacket(0, 0x1017, &debug3_);

	// packet ID 1 containing: GPS data
	aciAddContentToVarPacket(1, 0x0106, &RO_ALL_Data_.GPS_latitude);
	aciAddContentToVarPacket(1, 0x0107, &RO_ALL_Data_.GPS_longitude);
	aciAddContentToVarPacket(1, 0x0108, &RO_ALL_Data_.GPS_height);
	aciAddContentToVarPacket(1, 0x0109, &RO_ALL_Data_.GPS_speed_x);
	aciAddContentToVarPacket(1, 0x010A, &RO_ALL_Data_.GPS_speed_y);
	aciAddContentToVarPacket(1, 0x010B, &RO_ALL_Data_.GPS_heading);
	aciAddContentToVarPacket(1, 0x010C, &RO_ALL_Data_.GPS_position_accuracy);
	aciAddContentToVarPacket(1, 0x010D, &RO_ALL_Data_.GPS_height_accuracy);
	aciAddContentToVarPacket(1, 0x010E, &RO_ALL_Data_.GPS_speed_accuracy);
	aciAddContentToVarPacket(1, 0x010F, &RO_ALL_Data_.GPS_sat_num);
	aciAddContentToVarPacket(1, 0x0110, &RO_ALL_Data_.GPS_status);
	//aciAddContentToVarPacket(1, 0x0111, &RO_ALL_Data_.GPS_time_of_week);
	//aciAddContentToVarPacket(1, 0x0112, &RO_ALL_Data_.GPS_week);

	// packet ID 2 containing: IMU + magnetometer
	aciAddContentToVarPacket(2, 0x0200, &RO_ALL_Data_.angvel_pitch);
	aciAddContentToVarPacket(2, 0x0201, &RO_ALL_Data_.angvel_roll);
	aciAddContentToVarPacket(2, 0x0202, &RO_ALL_Data_.angvel_yaw);
	aciAddContentToVarPacket(2, 0x0203, &RO_ALL_Data_.acc_x);
	aciAddContentToVarPacket(2, 0x0204, &RO_ALL_Data_.acc_y);
	aciAddContentToVarPacket(2, 0x0205, &RO_ALL_Data_.acc_z);
	aciAddContentToVarPacket(2, 0x0206, &RO_ALL_Data_.Hx);
	aciAddContentToVarPacket(2, 0x0207, &RO_ALL_Data_.Hy);
	aciAddContentToVarPacket(2, 0x0208, &RO_ALL_Data_.Hz);
	aciAddContentToVarPacket(2, 0x0300, &RO_ALL_Data_.angle_pitch);
	aciAddContentToVarPacket(2, 0x0301, &RO_ALL_Data_.angle_roll);
	aciAddContentToVarPacket(2, 0x0302, &RO_ALL_Data_.angle_yaw);

  // packet ID 3 containing: laser, by Xun
  aciAddContentToVarPacket(3, 0x1001, &laser_distance_);

	// set transmission rate for packets, update and send configuration
	aciSetVarPacketTransmissionRate(0, rc_status_rate_);
	aciSetVarPacketTransmissionRate(1, gps_rate_);
	aciSetVarPacketTransmissionRate(2, imu_rate_);

  aciSetVarPacketTransmissionRate(3, laser_rate_);    // by Xun

	aciVarPacketUpdateTransmissionRates();
	aciSendVariablePacketConfiguration(0);
	aciSendVariablePacketConfiguration(1);
	aciSendVariablePacketConfiguration(2);

  aciSendVariablePacketConfiguration(3);    // by Xun

	ROS_INFO_STREAM("Variables packets configured");

	boost::mutex::scoped_lock lock(mtx_);
	var_list_recv_ = true;
}

void AciRemote::setupCmdPackets() {
	ROS_INFO("Received commands list from HLP");
	// setup commands packets to be sent over to the HLP
	// along with configuration to whether or not receive ACK

	// packet ID 0 containing: control mode
	aciAddContentToCmdPacket(0, 0x0600, &WO_SDK_.ctrl_mode);
	aciAddContentToCmdPacket(0, 0x0601, &WO_SDK_.ctrl_enabled);
	aciAddContentToCmdPacket(0, 0x0602, &WO_SDK_.disable_motor_onoff_by_stick);

	aciAddContentToCmdPacket(0, 0x0500, &WO_DIMC_.motor[0]);
	aciAddContentToCmdPacket(0, 0x0501, &WO_DIMC_.motor[1]);
	aciAddContentToCmdPacket(0, 0x0502, &WO_DIMC_.motor[2]);
	aciAddContentToCmdPacket(0, 0x0503, &WO_DIMC_.motor[3]);

	// packet ID 1 containing: CTRL -- DMC not used here
	aciAddContentToCmdPacket(1, 0x050A, &WO_CTRL_.pitch);
	aciAddContentToCmdPacket(1, 0x050B, &WO_CTRL_.roll);
	aciAddContentToCmdPacket(1, 0x050C, &WO_CTRL_.yaw);
	aciAddContentToCmdPacket(1, 0x050D, &WO_CTRL_.thrust);
	aciAddContentToCmdPacket(1, 0x050E, &WO_CTRL_.ctrl);

	// packet ID 2 containing: single waypoint data structure
	aciAddContentToCmdPacket(2, 0x1001, &WO_wpToLL_.wp_activated);
	aciAddContentToCmdPacket(2, 0x1002, &WO_wpToLL_.properties);
	aciAddContentToCmdPacket(2, 0x1003, &WO_wpToLL_.max_speed);
	aciAddContentToCmdPacket(2, 0x1004, &WO_wpToLL_.time);
	aciAddContentToCmdPacket(2, 0x1005, &WO_wpToLL_.pos_acc);
	aciAddContentToCmdPacket(2, 0x1006, &WO_wpToLL_.chksum);
	aciAddContentToCmdPacket(2, 0x1007, &WO_wpToLL_.X);
	aciAddContentToCmdPacket(2, 0x1008, &WO_wpToLL_.Y);
	aciAddContentToCmdPacket(2, 0x1009, &WO_wpToLL_.yaw);
	aciAddContentToCmdPacket(2, 0x100A, &WO_wpToLL_.height);
	aciAddContentToCmdPacket(2, 0x100B, &wpCtrlWpCmd_);

	// set whether or not should receive ACK, and send configuration
	aciSendCommandPacketConfiguration(0, 1); // control mode must be set with ACK
	aciSendCommandPacketConfiguration(1, 0);
	aciSendCommandPacketConfiguration(2, 1); // waypoint should be set with ACK as well

	// send commands to HLP (DANGER: make sure data structures were properly initialised)
	//aciUpdateCmdPacket(0);
	//aciUpdateCmdPacket(1);
	//aciUpdateCmdPacket(2);

	ROS_INFO_STREAM("Commands packets configured");

	boost::mutex::scoped_lock lock(mtx_);
	cmd_list_recv_ = true;
}

void AciRemote::setupParPackets() {
	ROS_INFO("Received parameters list from HLP");
	boost::mutex::scoped_lock lock(mtx_);
	par_list_recv_ = true;
}



//-------------------------------------------------------
//	Private member functions
//-------------------------------------------------------

void AciRemote::transmit(void* bytes, unsigned short len) {
	AciRemote* this_obj = static_cast<AciRemote*>(aci_obj_ptr);
	this_obj->doWrite(bytes, len);
}

void AciRemote::versions(struct ACI_INFO aciInfo) {
	AciRemote* this_obj = static_cast<AciRemote*>(aci_obj_ptr);
	this_obj->checkVersions(aciInfo);
}

void AciRemote::varListUpdateFinished() {
	AciRemote* this_obj = static_cast<AciRemote*>(aci_obj_ptr);
	this_obj->setupVarPackets();
}

void AciRemote::cmdListUpdateFinished() {
	AciRemote* this_obj = static_cast<AciRemote*>(aci_obj_ptr);
	this_obj->setupCmdPackets();
}

void AciRemote::paramListUpdateFinished() {
	AciRemote* this_obj = static_cast<AciRemote*>(aci_obj_ptr);
	this_obj->setupParPackets();
}

void AciRemote::readHandler(const boost::system::error_code& error,
		size_t bytes_transferred) {
	if (!error) {
		// the read_buffer_ should have some data from initial/previous call to
		// async_read_some ( doRead() ), thus feed ACI Engine with received data
		boost::unique_lock<boost::mutex> lock(buf_mtx_);
		for (std::vector<unsigned char>::iterator it = read_buffer_.begin();
				it != (read_buffer_.begin() + bytes_transferred); ++it) {
			aciReceiveHandler(*it);
		}
		lock.unlock();
		// carry on reading more data into the buffer...
		doRead();
	}
	else {
		ROS_ERROR_STREAM("Async read to serial port " << port_name_ << ". " << error.message());
		if (isOpen()) {
			doClose();
		}
	}
}

void AciRemote::throttleEngine() {
	ROS_INFO_STREAM("ACI Engine thread throttling at " << aci_rate_ << " Hz");
	// throttle ACI Engine at aci_rate_ Hz
	int aci_throttle = 1000 / aci_rate_;

	try {
		for (;;) {
			boost::system_time const throttle_timeout =
					boost::get_system_time() + boost::posix_time::milliseconds(aci_throttle);

			boost::unique_lock<boost::mutex> u_lock(buf_mtx_);

			if (cond_.timed_wait(u_lock, throttle_timeout) == false) {
				// check whether thread should terminate (::interrupt() appears to have no effect)
				if (must_stop_engine_)
					return;

				boost::unique_lock<boost::mutex> ctrl_lock(ctrl_mtx_);
				// throttle ACI Engine
				aciEngine();
				ctrl_lock.unlock();

				// lock shared mutex: get upgradable then exclusive access
				boost::upgrade_lock<boost::shared_mutex> up_lock(shared_mtx_);
				boost::upgrade_to_unique_lock<boost::shared_mutex> un_lock(up_lock);
				// synchronise variables
				aciSynchronizeVars();
			}
		}
	}
	catch (boost::thread_interrupted const&) {
		ROS_INFO("throttleEngine() thread interrupted");
	}
}

void AciRemote::publishImuMagData() {
	sensor_msgs::ImuPtr imu_msg(new sensor_msgs::Imu);
	asctec_hlp_comm::mav_imuPtr imu_custom_msg(new asctec_hlp_comm::mav_imu);
	geometry_msgs::Vector3StampedPtr mag_msg(new geometry_msgs::Vector3Stamped);
	imu_msg->header.frame_id = frame_id_;
	imu_custom_msg->header.frame_id = frame_id_;
	mag_msg->header.frame_id = frame_id_;
	int imu_throttle = 1000 / imu_rate_;
	static int seq[] = {0, 0, 0};
	try {
		for (;;) {
			boost::system_time const imu_timeout =
					boost::get_system_time() + boost::posix_time::milliseconds(imu_throttle);
			// acquire multiple reader shared lock
			boost::shared_lock<boost::shared_mutex> s_lock(shared_mtx_);

			if (cond_any_.timed_wait(s_lock, imu_timeout) == false) {
				// check whether thread should terminate (::interrupt() appears to have no effect)
				if (must_stop_pub_)
					return;
				// TODO: implement flag to put thread into idle mode to save computational resources

				ros::Time time_stamp(ros::Time::now());
				double roll = helper::asctecAttitudeToSI(RO_ALL_Data_.angle_roll);
				double pitch = helper::asctecAttitudeToSI(RO_ALL_Data_.angle_pitch);
				double yaw = helper::asctecAttitudeToSI(RO_ALL_Data_.angle_yaw);
				if (yaw> M_PI) {
					yaw -= 2.0 * M_PI;
				}
				geometry_msgs::Quaternion q;
				helper::angle2quaternion(roll, pitch, yaw, &q.w, &q.x, &q.y, &q.z);

				// only publish if someone has already subscribed to topics
				if (imu_pub_.getNumSubscribers() > 0) {
					imu_msg->header.stamp = time_stamp;
					imu_msg->header.seq = seq[0];
					seq[0]++;
					imu_msg->linear_acceleration.x = helper::asctecAccToSI(RO_ALL_Data_.acc_x);
					imu_msg->linear_acceleration.y = helper::asctecAccToSI(RO_ALL_Data_.acc_y);
					imu_msg->linear_acceleration.z = helper::asctecAccToSI(RO_ALL_Data_.acc_z);
					imu_msg->angular_velocity.x = helper::asctecAccToSI(RO_ALL_Data_.angle_roll);
					imu_msg->angular_velocity.y = helper::asctecAccToSI(RO_ALL_Data_.angle_pitch);
					imu_msg->angular_velocity.z = helper::asctecAccToSI(RO_ALL_Data_.angle_yaw);
					imu_msg->orientation = q;
					helper::setDiagonalCovariance(imu_msg->angular_velocity_covariance,
							ang_vel_variance_);
					helper::setDiagonalCovariance(imu_msg->linear_acceleration_covariance,
							lin_acc_variance_);
					imu_pub_.publish(imu_msg);
				}
				if (imu_custom_pub_.getNumSubscribers() > 0) {
					double height = static_cast<double>(RO_ALL_Data_.fusion_height) * 0.001;
					double dheight = static_cast<double>(RO_ALL_Data_.fusion_dheight) * 0.001;
					imu_custom_msg->header.stamp = time_stamp;
					imu_custom_msg->header.seq = seq[1];
					seq[1]++;
					imu_custom_msg->acceleration.x = helper::asctecAccToSI(RO_ALL_Data_.acc_x);
					imu_custom_msg->acceleration.y = helper::asctecAccToSI(RO_ALL_Data_.acc_y);
					imu_custom_msg->acceleration.z = helper::asctecAccToSI(RO_ALL_Data_.acc_z);
					imu_custom_msg->angular_velocity.x =
							helper::asctecAccToSI(RO_ALL_Data_.angle_roll);
					imu_custom_msg->angular_velocity.y =
							helper::asctecAccToSI(RO_ALL_Data_.angle_pitch);
					imu_custom_msg->angular_velocity.z =
							helper::asctecAccToSI(RO_ALL_Data_.angle_yaw);
					imu_custom_msg->height = height;
					imu_custom_msg->differential_height = dheight;
					imu_custom_msg->orientation = q;
					imu_custom_pub_.publish(imu_custom_msg);
				}
				if (mag_pub_.getNumSubscribers() > 0) {
					mag_msg->header.stamp = time_stamp;
					mag_msg->header.seq = seq[2];
					seq[2]++;
					mag_msg->vector.x = static_cast<double>(RO_ALL_Data_.Hx);
					mag_msg->vector.y = static_cast<double>(RO_ALL_Data_.Hy);
					mag_msg->vector.z = static_cast<double>(RO_ALL_Data_.Hz);
					mag_pub_.publish(mag_msg);
				}
			}
		}
	}
	catch (boost::thread_interrupted const&) {
		ROS_INFO("publishImuMagData() thread interrupted");
	}
}

void AciRemote::publishGpsData() {
	sensor_msgs::NavSatFixPtr gps_msg(new sensor_msgs::NavSatFix);
	asctec_hlp_comm::GpsCustomPtr gps_custom_msg(new asctec_hlp_comm::GpsCustom);
	gps_msg->header.frame_id = frame_id_;
	gps_custom_msg->header.frame_id = frame_id_;
	int gps_throttle = 1000 / gps_rate_;
	static int seq[] = {0, 0};
	try {
		for (;;) {
			boost::system_time const gps_timeout =
					boost::get_system_time() + boost::posix_time::milliseconds(gps_throttle);
			// acquire multiple reader shared lock
			boost::shared_lock<boost::shared_mutex> s_lock(shared_mtx_);

			if (cond_any_.timed_wait(s_lock, gps_timeout) == false) {
				// check whether thread should terminate (::interrupt() appears to have no effect)
				if (must_stop_pub_)
					return;
				// TODO: implement flag to put thread into idle mode to save computational resources

				ros::Time time_stamp(ros::Time::now());
				// TODO: check covariance
				double var_h, var_v;
				var_h = static_cast<double>(RO_ALL_Data_.GPS_position_accuracy) * 1.0e-3 / 3.0;
				var_v = static_cast<double>(RO_ALL_Data_.GPS_height_accuracy) * 1.0e-3 / 3.0;
				var_h *= var_h;
				var_v *= var_v;
				// only publish if someone has already subscribed to topics
				if (gps_pub_.getNumSubscribers() > 0) {
					gps_msg->header.stamp = time_stamp;
					gps_msg->header.seq = seq[0];
					seq[0]++;
					gps_msg->latitude = static_cast<double>(RO_ALL_Data_.GPS_latitude) * 1.0e-7;
					gps_msg->longitude = static_cast<double>(RO_ALL_Data_.GPS_longitude) * 1.0e-7;
					gps_msg->altitude = static_cast<double>(RO_ALL_Data_.GPS_height) * 1.0e-3;
					gps_msg->position_covariance[0] = var_h;
					gps_msg->position_covariance[4] = var_h;
					gps_msg->position_covariance[8] = var_v;
					gps_msg->position_covariance_type =
							sensor_msgs::NavSatFix::COVARIANCE_TYPE_APPROXIMATED;

					gps_msg->status.service = sensor_msgs::NavSatStatus::SERVICE_GPS;
					// bit 0: GPS lock
					if (RO_ALL_Data_.GPS_status & 0x01)
						gps_msg->status.status =
								sensor_msgs::NavSatStatus::STATUS_FIX;
					else
						gps_msg->status.status =
								sensor_msgs::NavSatStatus::STATUS_NO_FIX;
					gps_pub_.publish(gps_msg);
				}
				if (gps_custom_pub_.getNumSubscribers() > 0) {
					gps_custom_msg->header.stamp = time_stamp;
					gps_custom_msg->header.seq = seq[1];
					seq[1]++;
					gps_custom_msg->latitude =
							static_cast<double>(RO_ALL_Data_.fusion_latitude) * 1.0e-7;
					gps_custom_msg->longitude =
							static_cast<double>(RO_ALL_Data_.fusion_longitude) * 1.0e-7;
					gps_custom_msg->altitude =
							static_cast<double>(RO_ALL_Data_.GPS_height) * 1.0e-3;
					gps_custom_msg->position_covariance[0] = var_h;
					gps_custom_msg->position_covariance[4] = var_h;
					gps_custom_msg->position_covariance[8] = var_v;
					gps_custom_msg->position_covariance_type =
							sensor_msgs::NavSatFix::COVARIANCE_TYPE_APPROXIMATED;
					gps_custom_msg->velocity_x =
							static_cast<double>(RO_ALL_Data_.GPS_speed_x) * 1.0e-3;
					gps_custom_msg->velocity_y =
							static_cast<double>(RO_ALL_Data_.GPS_speed_y) * 1.0e-3;
					gps_custom_msg->pressure_height =
							static_cast<double>(RO_ALL_Data_.fusion_height) * 1.0e-3;
					// TODO: check covariance
					double var_vel =
							static_cast<double>(RO_ALL_Data_.GPS_speed_accuracy) * 1.0e-3 / 3.0;
					var_vel *= var_vel;
					gps_custom_msg->velocity_covariance[0] = var_vel;
					gps_custom_msg->velocity_covariance[3] = var_vel;

					gps_custom_msg->status.service = sensor_msgs::NavSatStatus::SERVICE_GPS;
					// bit 0: GPS lock
					if (RO_ALL_Data_.GPS_status & 0x01)
						gps_custom_msg->status.status =
								sensor_msgs::NavSatStatus::STATUS_FIX;
					else
						gps_custom_msg->status.status =
								sensor_msgs::NavSatStatus::STATUS_NO_FIX;
					gps_custom_pub_.publish(gps_custom_msg);
				}
			}
		}
	}
	catch (boost::thread_interrupted const&) {
		ROS_INFO("publishGpsData() thread interrupted");
	}
}

void AciRemote::publishStatusMotorsRcData() {
	asctec_hlp_comm::mav_rcdataPtr rcdata_msg(new asctec_hlp_comm::mav_rcdata);
	asctec_hlp_comm::mav_hlp_statusPtr status_msg(new asctec_hlp_comm::mav_hlp_status);
	asctec_hlp_comm::MotorSpeedPtr motor_msg(new asctec_hlp_comm::MotorSpeed);
	rcdata_msg->header.frame_id = frame_id_;
	status_msg->header.frame_id = frame_id_;
	motor_msg->header.frame_id = frame_id_;
	int status_throttle = 1000 / rc_status_rate_;
	static int seq[] = {0, 0, 0};
	try {
		for (;;) {
			boost::system_time const status_timeout =
					boost::get_system_time() + boost::posix_time::milliseconds(status_throttle);
			// acquire multiple reader shared lock
			boost::shared_lock<boost::shared_mutex> s_lock(shared_mtx_);

			if (cond_any_.timed_wait(s_lock, status_timeout) == false) {
				// check whether thread should terminate (::interrupt() appears to have no effect)
				if (must_stop_pub_)
					return;
				// TODO: implement flag to put thread into idle mode to save computational resources

				ros::Time time_stamp(ros::Time::now());
				// only publish if someone has already subscribed to topics
				if (rcdata_pub_.getNumSubscribers() > 0) {
					rcdata_msg->header.stamp = time_stamp;
					rcdata_msg->header.seq = seq[0];
					seq[0]++;
					for (int i = 0; i < NUM_RC_CHANNELS; ++i) {
						rcdata_msg->channel[i] = RO_ALL_Data_.channel[i];
					}
					rcdata_pub_.publish(rcdata_msg);
				}
				if (status_pub_.getNumSubscribers() > 0) {
					status_msg->header.stamp = time_stamp;
					status_msg->header.seq = seq[1];
					seq[1]++;

					status_msg->UAV_status = RO_ALL_Data_.UAV_status;

					if ((RO_ALL_Data_.UAV_status & 0x0F) == HLP_FLIGHTMODE_ATTITUDE)
						status_msg->flight_mode = "Attitude";
					else if ((RO_ALL_Data_.UAV_status & 0x0F) == HLP_FLIGHTMODE_HEIGHT)
						status_msg->flight_mode = "Height";
					else if ((RO_ALL_Data_.UAV_status & 0x0F) == HLP_FLIGHTMODE_GPS)
						status_msg->flight_mode = "GPS";

					status_msg->flight_time = static_cast<float>(RO_ALL_Data_.flight_time);
					status_msg->battery_voltage =
							static_cast<float>(RO_ALL_Data_.battery_voltage) * 0.001;
					status_msg->cpu_load = static_cast<float>(RO_ALL_Data_.HL_cpu_load) * 0.001;
					status_msg->up_time = static_cast<float>(RO_ALL_Data_.HL_up_time) * 0.001;
					status_msg->serial_interface_enabled =
							RO_ALL_Data_.UAV_status & SERIAL_INTERFACE_ENABLED;
					status_msg->serial_interface_active =
							RO_ALL_Data_.UAV_status & SERIAL_INTERFACE_ACTIVE;

					status_msg->motor_status = "off";
					for (int i = 0; i < NUM_MOTORS; ++i) {
						if (RO_ALL_Data_.motor_rpm[i] > 0) {
							status_msg->motor_status = "running";
							break;
						}
					}

					// bit 0: GPS lock
					if (RO_ALL_Data_.GPS_status & 0x01)
						status_msg->gps_status = "GPS fix";
					else
						status_msg->gps_status = "GPS no fix";

					status_msg->gps_num_satellites = RO_ALL_Data_.GPS_sat_num;

					// other status variables
					status_msg->ctrl_mode = RO_SDK_.ctrl_mode;
					status_msg->ctrl_enabled = RO_SDK_.ctrl_enabled;
					status_msg->disable_motor_onoff_by_stick = RO_SDK_.disable_motor_onoff_by_stick;
					status_msg->waypt_status = wayptStatus_;

					// debug variables
					//status_msg->debug1 = static_cast<unsigned short>(debug1_);
					//status_msg->debug2 = static_cast<unsigned short>(debug2_);
					//status_msg->debug3 = static_cast<unsigned short>(debug3_);

					status_pub_.publish(status_msg);
				}
				if (motor_pub_.getNumSubscribers() > 0) {
					motor_msg->header.stamp = time_stamp;
					motor_msg->header.seq = seq[2];
					seq[2]++;
					for (int i = 0; i < NUM_MOTORS; ++i) {
						motor_msg->motor_speed[i] = RO_ALL_Data_.motor_rpm[i];
					}
					motor_pub_.publish(motor_msg);
				}
			}
		}
	}
	catch (boost::thread_interrupted const&) {
		ROS_INFO("publishStatusMotorsRcData() thread interrupted");
	}
}




// by Xun
void AciRemote::publishLaserData() {
  asctec_hlp_comm::mav_laserPtr laser_msg(new asctec_hlp_comm::mav_laser);
  laser_msg->header.frame_id = frame_id_;
  int laser_throttle = 1000 / laser_rate_;
  static int seq[] = {0, 0};
  try {
    for (;;) {
      boost::system_time const laser_timeout =
          boost::get_system_time() + boost::posix_time::milliseconds(laser_throttle);
      // acquire multiple reader shared lock
      boost::shared_lock<boost::shared_mutex> s_lock(shared_mtx_);

      if (cond_any_.timed_wait(s_lock, laser_timeout) == false) {
        // check whether thread should terminate (::interrupt() appears to have no effect)
        if (must_stop_pub_)
          return;
        // TODO: implement flag to put thread into idle mode to save computational resources

        ros::Time time_stamp(ros::Time::now());
        // only publish if someone has already subscribed to topics
        if (laser_pub_.getNumSubscribers() > 0) {
          laser_msg->header.stamp = time_stamp;
          laser_msg->header.seq = seq[0];
          seq[0]++;
          laser_msg->laser_measurement = laser_distance_;
          laser_pub_.publish(laser_msg);
        }
      }
    }
  }
  catch (boost::thread_interrupted const&) {
    ROS_INFO("publishLaserData() thread interrupted");
  }
}



/*
 *
 * Flight modes are: Manual, Height and GPS (control loops running on the LLP)
 * Depending on the selected flight mode, the sticks have different effects on the UAV
 * (e.g., climb/sink rate if height control is enabled and absolute thrust if height
 * control is disabled)
 * In all flight modes, a heading hold algorithm is activated, which uses (yaw - gyro)
 *
 * Quick description:
 * Manual mode (WARNING: for experienced pilots only):
 * - only attitude control is active
 * - keeps horizontal attitude and orientation
 * - 50% thrust may cause the UAV to sink or climb, depending on payload
 * - does not compensate for the wind (will drift with wind)
 * - position of thrust stick corresponds to thrust rate: centred stick = 50% thrust,
 * 		full stick up = 100% thrust, and full stick down = 0% thrust
 * - position of yaw stick corresponds to yaw rate: centred stick = 0 degrees/s,
 * 		full stick command = approx. 200 degrees/s
 * - position of pitch stick corresponds to pitch angle: centred stick = 0 degrees,
 * 		full stick command = approx. 52 degrees
 * - position of roll stick corresponds to roll angle: centred stick = 0 degrees,
 * 		full stick command = approx. 52 degrees
 * - maximum attitude angle is 52 degrees
 * - minimum thrust of 0% means the UAV will fall like a stone
 * - maximum thrust of 100% means the UAV will climb with 4 to 5 m/s, depending on payload
 *
 * Height mode:
 * - attitude and height control are active
 * - keeps horizontal attitude, height and orientation
 * - does not compensate for the wind (will drift with wind)
 * - position of thrust stick corresponds to a climb/sink rate: centred stick = 0 m/s,
 * 		full stick command = approx. 2 m/s
 * - position of yaw stick corresponds to yaw rate: centred stick = 0 degrees/s,
 * 		full stick command = approx. 200 degrees/s
 * - position of pitch stick corresponds to pitch angle: centred stick = 0 degrees,
 * 		full stick command = approx. 52 degrees
 * - position of roll stick corresponds to roll angle: centred stick = 0 degrees,
 * 		full stick command = approx. 52 degress
 * - maximum attitude angle limited to 52 degrees
 * - 52 degrees corresponds to a relative velocity of approx. 15 m/s
 * - therefore, maximum absolute speed = relative speed + wind speed
 *
 * GPS mode:
 * - all control algorithms (attitude, height, position) are active
 * - if there is no valid GPS lock, falls back to Height mode even if GPS mode selected
 * - both control sticks centred: keeps its GPS position, height and orientation
 * - hence, compensates for winds up to 10 m/s
 * - position of thrust stick corresponds to climb/sink rate: centred stick = 0 m/s,
 * 		full stick command = approx. 2 m/s
 * - position of yaw stick corresponds to yaw rate: centred stick = 0 degrees/s,
 * 		full stick command = 200 degrees/s
 * - position of pitch stick corresponds to forward velocity: centred stick = 0 m/s,
 * 		full stick command = approx. 3 m/s ground speed
 * - position of roll stick corresponds to sideward velocity: centred stick = 0 m/s,
 * 		full stick command = approx. 3 m/s ground speed
 * - wind is also compensated whilst commanding horizontal velocity
 * - thus, regardless of wind direction, absolute speed will be the same
 * - WARNING: do not use GPS mode if wind speed exceeds 10 m/s, as UAV will not be
 * 		able to compensate for such strong wind anymore
 * - the limit of 10 m/s is a result of maximum attitude angle of approx. 23 degrees
 * 		implemented to avoid a too steep angle of the GPS module, which increases the
 * 		risk of losing sight of surrounding satellites, causing the UAV to swithc back
 * 		to Height mode
 * - WARNING: never try to fly in GPS indoors (obviously!)
 *
 * For more information regarding the flight modes, see:
 * http://wiki.asctec.de/display/AR/Get+Ready+to+Fly
 *
 *
 *
 * Control modes are (refer to AsctecSDK3.h in this package, or sdk.h of HLP firmware):
 * - 0x00: Individual motor control -- commands for each individual motor
 * 		(thus no attitude control active at all)
 * - 0x01: Direct motor control using standard output mapping: commands interpreted as
 * 		pitch, roll, yaw and thrust inputs for all motors; no attitude control active
 * - 0x02: Attitude and thrust control: commands interpreted as remote control stick inputs
 * 		and hence used as inputs for standard attitude controller running on the LLP
 * 		(i.e., desired angle pitch, desired angle roll, desired yaw rate and thrust)
 * - 0x03: GPS Waypoint navigation (position, height, heading, etc.)
 *
 * For more information regarding the control modes, see:
 * http://wiki.asctec.de/display/AR/SDK+Manual
 *
 *
 *
 * Emergency modes:
 * For more information regarding the emergency modes, see:
 * http://wiki.asctec.de/display/AR/Emergency+Mode
 *
 */

void AciRemote::ctrlTopicCallback(const geometry_msgs::TwistConstPtr& cmd) {
    // acquire shared lock in order to read from RO_ALL_Data_
    boost::shared_lock<boost::shared_mutex> s_lock(shared_mtx_);

    if (RO_ALL_Data_.UAV_status & HLP_FLIGHTMODE_GPS) {
        ROS_WARN_STREAM("UAV in GPS mode");
    }
    else if (RO_ALL_Data_.UAV_status & HLP_FLIGHTMODE_HEIGHT) {
        ROS_WARN_STREAM("UAV in Height mode");
    }
    else if (RO_ALL_Data_.UAV_status & HLP_FLIGHTMODE_ATTITUDE) {
        ROS_ERROR_STREAM("UAV in manual mode: IGNORING for safety reasons");
        return;
    }
    else {
        ROS_ERROR_STREAM("UAV in unknown control mode. How is this possible?");
        return;
    }

    /*control byte:
    bit 0: pitch control enabled
    bit 1: roll control enabled
    bit 2: yaw control enabled
    bit 3: thrust control enabled
    bit 4: height control enabled
    bit 5: GPS position control enabled
    */
    // From my understanding, whichever bit set will be controlled by the HLP,
    // which means via the corresponding geometry_msgs/Twist value in 'cmd',
    // and whichever bit not set will still be controlled by the remote control
    // (i.e., the RC sticks)
    WO_CTRL_.ctrl = 0x3F; // 0011 1111 = 3F

    // thrust range = [0, 4095]
    // max(thrust) = 2 m/s (climb/sink rate)
    // min(thrust) = 0 m/s
    WO_CTRL_.thrust = std::min<short>(4095, short(cmd->linear.z * (4095.0/2.0)));

    // yaw range = [-2047, 2047]
    // max(yaw) = 200 degrees/s = 3.49 (will limit to PI/2)
    // min(yaw) = -200 degrees/s = -3.49 (will limit to -PI/2)
    WO_CTRL_.yaw = short(cmd->angular.z * (2047.0/M_PI_2));
    if (cmd->angular.z > 0.0) {
        WO_CTRL_.yaw = std::min<short>(2047, WO_CTRL_.yaw);
    }
    else {
        WO_CTRL_.yaw = std::max<short>(-2047, WO_CTRL_.yaw);
    }

    // pitch range = [-2047, 2047]
    // max(pitch) = 3 m/s
    // min(pitch) = -3 m/s
    WO_CTRL_.pitch = short(cmd->linear.x * (2047.0/3.0));
    if (cmd->linear.x > 0.0) {
        WO_CTRL_.pitch = std::min<short>(2047, WO_CTRL_.pitch);
    }
    else {
        WO_CTRL_.pitch = std::max<short>(-2047, WO_CTRL_.pitch);
    }

    // roll range = [-2047, 2047]
    // max(roll) = 3 m/s
    // min(roll) = -3 m/s
    WO_CTRL_.roll = short(cmd->linear.y * (2047.0/3.0));
    if (cmd->linear.y > 0.0) {
        WO_CTRL_.roll = std::min<short>(2047, WO_CTRL_.roll);
    }
    else {
        WO_CTRL_.roll = std::max<short>(-2047, WO_CTRL_.roll);
    }

    // update CTRL command packet
    aciUpdateCmdPacket(1);
}

bool AciRemote::ctrlServiceCallback(asctec_hlp_comm::HlpCtrlSrv::Request& req,
		asctec_hlp_comm::HlpCtrlSrv::Response& res) {
	// TODO: analyse whether the lock should be acquired only just before the call
	// to aciUpdateCmdPacket()
	boost::mutex::scoped_lock lock(ctrl_mtx_);

	/*
	 *  truth table for flight mode-related variables
	 *
	 * 	ctrl_mode   ctrl_enabled    serial_enabled  serial_active
	 * 	0           0               0               0
	 * 	0           0               1               0
	 * 	0           1               0               0
	 * 	0           1               1               0
	 * 	1           0               0               0
	 * 	1           0               1               0
	 * 	1           1               0               0
	 * 	1           1               1               0
	 * 	2           0               0               0
	 * 	2           0               1               0
	 * 	2           1               0               0
	 * 	2           1               1               1
	 * 	3           0               0               0
	 * 	3           0               1               0
	 * 	3           1               0               0
	 * 	3           1               1               0
	 *
	 */

	WO_SDK_.ctrl_mode = req.ctrl_mode;
	WO_SDK_.ctrl_enabled = req.ctrl_enabled;
	WO_SDK_.disable_motor_onoff_by_stick = req.disable_onoff_stick;

    /*
	if (req.motor1 > 0)
		WO_DIMC_.motor[0] = 10;
	else
		WO_DIMC_.motor[0] = 0;

	if (req.motor2 > 0)
		WO_DIMC_.motor[1] = 10;
	else
		WO_DIMC_.motor[1] = 0;

	if (req.motor3 > 0)
		WO_DIMC_.motor[2] = 10;
	else
		WO_DIMC_.motor[2] = 0;

	if (req.motor4 > 0)
		WO_DIMC_.motor[3] = 10;
	else
        WO_DIMC_.motor[3] = 0;
    */

	aciUpdateCmdPacket(0);

	res.motor1 = WO_DIMC_.motor[0];
	res.motor2 = WO_DIMC_.motor[1];
	res.motor3 = WO_DIMC_.motor[2];
	res.motor4 = WO_DIMC_.motor[3];

    if (externalise_state_) {
        std_msgs::String str;
        std::ostringstream oss;
        oss << "Control mode set to ";
        if (req.ctrl_mode == 0x00) {
            oss << "zero";
        }
        else if (req.ctrl_mode == 0x01) {
            oss << "one";
        }
        else if (req.ctrl_mode == 0x02) {
            oss << "two";
        }
        else if (req.ctrl_mode == 0x03) {
            oss << "three";
        }
        else {
            oss << "invalid";
        }
        str.data = oss.str();
        extern_pub_.publish(str);

        if (req.ctrl_enabled == 0x01) {
            str.data = std::string("Control mode is enabled");
            extern_pub_.publish(str);
        }
        else {
            str.data = std::string("Control mode is disabled");
            extern_pub_.publish(str);
        }

        if (req.disable_onoff_stick == 0x01) {
            str.data = std::string("On off via stick is enabled");
            extern_pub_.publish(str);
        }
        else {
            str.data = std::string("On off via stick is disabled");
            extern_pub_.publish(str);
        }
    }

	return true;
}



} /* namespace AciRemote */
