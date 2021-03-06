/*
 * WaypointGPSActionServer.h
 *
 *  Created on: 19 Feb 2014
 *      Author: Murilo F. M.
 *      Email:  muhrix@gmail.com
 *
 */

#ifndef WAYPOINTGPSACTIONSERVER_H_
#define WAYPOINTGPSACTIONSERVER_H_

#include <vector>
#include <utility>
#include <algorithm>

#include <boost/function.hpp>
#include <boost/thread/mutex.hpp>

// notice that the header-only implementation of Boost Geometry
// is part of the asctec_hlp_interface ROS package (current version: v1.55)
#include "boost/geometry/geometry.hpp"
#include "boost/geometry/geometries/point_xy.hpp"
#include "boost/geometry/geometries/polygon.hpp"

#include <ros/ros.h>
#include <geometry_msgs/Vector3.h>
#include <geometry_msgs/Quaternion.h>
#include <tf/transform_datatypes.h>
#include <geographic_msgs/GeoPoint.h>
#include <geographic_msgs/GeoPose.h>
#include <actionlib/server/simple_action_server.h>
#include "asctec_hlp_comm/WaypointGPSAction.h"
#include "asctec_hlp_comm/GeofenceSrv.h"

#include "asctec_hlp_interface/AsctecSDK3.h"
#include "asctec_hlp_interface/AciRemote.h"

typedef boost::geometry::model::point
	<
		double, 2, boost::geometry::cs::spherical_equatorial<boost::geometry::degree>
	> spherical_point_type;
typedef boost::geometry::model::polygon<spherical_point_type> polygon_type;

namespace Waypoint {
	namespace Action {
		typedef enum {SUCCEEDED = 0, RUNNING, PREEMPTED, ABORTED,
			OUT_OF_GEOFENCE, VALID, WRONG_FLIGHT_MODE, WRONG_CTRL_MODE, NOT_READY} action_t;
	}
	// the values attributed to the enum statuses are in accordance with HLP's sdk.h
	namespace Status {
		typedef enum {REACHED_POS = 0x01, REACHED_POS_TIME = 0x02,
			WITHIN_20M = 0x04, PILOT_ABORT = 0x08} status_t;
	}
	// the states below refer to the state machine implemented on the HLP
	// and must obviously be in sync (see sdk.c)
	namespace State {
		typedef enum {RESET = 0, LOCK1 = 1, LOCK2 = 2, LOCK3 = 3,
			READY = 4, LLP_CHECKING = 5} state_t;
	}
}

class WaypointGPSActionServer {
public:
	WaypointGPSActionServer(const std::string&, boost::shared_ptr<AciRemote::AciRemote>&);
	~WaypointGPSActionServer();

	// getters
	unsigned int getWaypointIterationRate() const;
	double getWaypointMaxSpeed() const;
	double getWaypointPositionAccuracy() const;
	double getWaypointTimeout() const;

	// setters
	void setWaypointIterationRate(const unsigned int);
	void setWaypointMaxSpeed(const double);
	void setWaypointPositionAccuracy(const double);
	void setWaypointTimeout(const double);

//protected:
private:
	ros::NodeHandle n_;
	std::string action_name_;
	std::string geofence_srv_name_;

	ros::ServiceServer geofence_srv_;

	actionlib::SimpleActionServer<asctec_hlp_comm::WaypointGPSAction> as_;
	asctec_hlp_comm::WaypointGPSFeedback feedback_;
	asctec_hlp_comm::WaypointGPSResult result_;

	int iter_rate_;
	double waypt_max_speed_;
	double waypt_pos_acc_;
	double waypt_timeout_;
	bool valid_geofence_;
	boost::mutex geo_mtx_;
	polygon_type geofence_;
	std::vector<std::pair<double, double> > geo_points_;

//private:
	void GpsWaypointAction(const asctec_hlp_comm::WaypointGPSGoalConstPtr&);

	bool geofenceServiceCallback(asctec_hlp_comm::GeofenceSrv::Request&,
			asctec_hlp_comm::GeofenceSrv::Response&);

	void verifyGeofence();
	Waypoint::Action::action_t verifyGpsWaypoint(const asctec_hlp_comm::WaypointGPSGoalConstPtr&);

	boost::function<int (const asctec_hlp_comm::WaypointGPSGoalConstPtr&)>
		sendGpsWaypointToHlp;
	boost::function<void (unsigned short&)> fetchWayptState;
	boost::function<void (unsigned short&, double&)> fetchWayptNavStatus;
	boost::function<void (asctec_hlp_comm::WaypointGPSResult&)> fetchWayptResultPose;
};

#endif /* WAYPOINTGPSACTIONSERVER_H_ */
