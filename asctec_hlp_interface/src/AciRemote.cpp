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

#include <sensor_msgs/Imu.h>
#include <sensor_msgs/NavSatFix.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/Vector3Stamped.h>

#include "asctec_hlp_comm/mav_imu.h"
#include "asctec_hlp_comm/mav_rcdata.h"
#include "asctec_hlp_comm/mav_status.h"
#include "asctec_hlp_comm/MotorSpeed.h"
#include "asctec_hlp_comm/GpsCustom.h"

namespace AciRemote {

//extern "C" {
//	void transmit(void* bytes, unsigned short len) {
//
//	}
//}

void* aci_obj_ptr = NULL;

AciRemote::AciRemote(ros::NodeHandle& nh):
		SerialComm(), n_(nh), bytes_recv_(0),
		versions_match_(false), var_list_recv_(false),
		cmd_list_recv_(false), par_list_recv_(false) {

	// assign *this pointer of this instanced object to global pointer for use with callbacks
	aci_obj_ptr = static_cast<void*>(this);

	// fetch values from ROS parameter server
	n_.param("serial_port", port_name_, std::string("/dev/ttyUSB0"));
	n_.param("baudrate", baud_rate_, 230400);
	n_.param("frame_id", frame_id_, std::string(n_.getNamespace() + "_base_link"));
	// parameters below will (that is, should) be moving to dynamic reconfigure in a later release
	n_.param("packet_rate_imu_mag", imu_rate_, 50);
	n_.param("packet_rate_gps", gps_rate_, 5);
	n_.param("packet_rate_rcdata_status_motors", rc_status_rate_, 10);
	n_.param("aci_engine_throttle", aci_rate_, 100);
	n_.param("aci_heartbeat", aci_heartbeat_, 10);

	// fetch topic names from ROS parameter server
	n_.param("imu_topic", imu_topic_, std::string("imu"));
	n_.param("imu_custom_topic", imu_custom_topic_, std::string("imu_custom"));
	n_.param("mag_topic", mag_topic_, std::string("mag"));
	n_.param("gps_topic", gps_topic_, std::string("gps"));
	n_.param("gps_custom_topic", gps_custom_topic_, std::string("gps_custom"));
	n_.param("rcdata_topic", rcdata_topic_, std::string("rcdata"));
	n_.param("status_topic", status_topic_, std::string("status"));
	n_.param("motor_speed_topic", motor_topic_, std::string("motor_speed"));
}

AciRemote::~AciRemote() {
	// interrupt all running threads...
	aci_throttle_thread_->interrupt();
	imu_mag_thread_->interrupt();
	gps_thread_->interrupt();
	rc_status_thread_->interrupt();
	// ... and wait for them to return
	aci_throttle_thread_->join();
	imu_mag_thread_->join();
	gps_thread_->join();
	rc_status_thread_->join();

	// assign NULL to global object pointer
	aci_obj_ptr = static_cast<void*>(NULL);
}



//-------------------------------------------------------
//	Public member functions
//-------------------------------------------------------

int AciRemote::Init() {
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

int AciRemote::advertiseRosTopics() {
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
			status_pub_ = n_.advertise<asctec_hlp_comm::mav_status>(status_topic_, 1);
			motor_pub_ = n_.advertise<asctec_hlp_comm::MotorSpeed>(motor_topic_, 1);

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


	boost::mutex::scoped_lock lock(mtx_);
	var_list_recv_ = true;
}

void AciRemote::setupCmdPackets() {
	ROS_INFO("Received commands list from HLP");
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
				// throttle ACI Engine
				aciEngine();

				// lock shared mutex: get upgradable then exclusive access
				boost::upgrade_lock<boost::shared_mutex> up_lock(shared_mtx_);
				boost::upgrade_to_unique_lock<boost::shared_mutex> un_lock(up_lock);
				// synchronise variables
				aciSynchronizeVars();
			}
		}
	}
	catch (boost::thread_interrupted const&) {

	}
}

void AciRemote::publishImuMagData() {
	sensor_msgs::Imu imu_msg;
	asctec_hlp_comm::mav_imu imu_custom_msg;
	geometry_msgs::Vector3Stamped mag_msg;
	int imu_throttle = 1000 / imu_rate_;

	try {
		for (;;) {
			boost::system_time const throttle_timeout =
					boost::get_system_time() + boost::posix_time::milliseconds(imu_throttle);
			// acquire multiple reader shared lock
			boost::shared_lock<boost::shared_mutex> s_lock(shared_mtx_);
			// only publish if someone has already subscribed to topics
			if (imu_pub_.getNumSubscribers() > 0) {

			}
			if (imu_custom_pub_.getNumSubscribers() > 0) {

			}
			if (mag_pub_.getNumSubscribers() > 0) {

			}
		}
	}
	catch (boost::thread_interrupted const&) {

	}
}

void AciRemote::publishGpsData() {
	sensor_msgs::NavSatFix gps_msg;
	asctec_hlp_comm::GpsCustom gps_custom_msg;
	int gps_throttle = 1000 / gps_rate_;

	try {
		for (;;) {
			boost::system_time const throttle_timeout =
					boost::get_system_time() + boost::posix_time::milliseconds(gps_throttle);
			// acquire multiple reader shared lock
			boost::shared_lock<boost::shared_mutex> s_lock(shared_mtx_);
			// only publish if someone has already subscribed to topics
			if (gps_pub_.getNumSubscribers() > 0) {

			}
			if (gps_custom_pub_.getNumSubscribers() > 0) {

			}
		}
	}
	catch (boost::thread_interrupted const&) {

	}
}

void AciRemote::publishStatusMotorsRcData() {
	asctec_hlp_comm::mav_rcdata rcdata_msg;
	asctec_hlp_comm::mav_status status_msg;
	asctec_hlp_comm::MotorSpeed motor_msg;
	int status_throttle = 1000 / rc_status_rate_;

	try {
		for (;;) {
			boost::system_time const throttle_timeout =
					boost::get_system_time() + boost::posix_time::milliseconds(status_throttle);
			// acquire multiple reader shared lock
			boost::shared_lock<boost::shared_mutex> s_lock(shared_mtx_);
			// only publish if someone has already subscribed to topics
			if (rcdata_pub_.getNumSubscribers() > 0) {

			}
			if (status_pub_.getNumSubscribers() > 0) {

			}
			if (motor_pub_.getNumSubscribers() > 0) {

			}
		}
	}
	catch (boost::thread_interrupted const&) {

	}
}


} /* namespace AciRemote */
