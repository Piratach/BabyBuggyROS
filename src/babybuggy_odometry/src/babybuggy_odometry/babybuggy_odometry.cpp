#include <babybuggy_odometry/babybuggy_odometry.h>

const string BabybuggyOdometry::BASE_LINK_FRAME_NAME = "base_link";
const string BabybuggyOdometry::ODOM_FRAME_NAME = "odom";
// const string BabybuggyOdometry::LASER_FRAME_NAME = "laser";
const string BabybuggyOdometry::IMU_FRAME_NAME = "imu";
const string BabybuggyOdometry::GPS_FRAME_NAME = "gps";

const float BabybuggyOdometry::IMU_LASER_X = 0.1016;
const float BabybuggyOdometry::IMU_LASER_Y = 0.0762;
const float BabybuggyOdometry::IMU_LASER_Z = 0.095;

const float BabybuggyOdometry::GPS_LASER_X = 0.0;
const float BabybuggyOdometry::GPS_LASER_Y = 0.0;
const float BabybuggyOdometry::GPS_LASER_Z = 0.0;

const size_t BabybuggyOdometry::NUM_ROWS_ODOM_COVARIANCE = 6;
const size_t BabybuggyOdometry::NUM_ROWS_GPS_COVARIANCE = 3;

const ros::Duration BabybuggyOdometry::DEBUG_INFO_DELAY = ros::Duration(1.0);

BabybuggyOdometry::BabybuggyOdometry(ros::NodeHandle* nodehandle):nh(*nodehandle)
{
    // setup subscribers
    gps_sub = nh.subscribe("/GpsNavSat", 5, &BabybuggyOdometry::GPSCallback, this);
    imu_sub = nh.subscribe("/BNO055", 100, &BabybuggyOdometry::IMUCallback, this);
    enc_sub = nh.subscribe("/encoder", 1000, &BabybuggyOdometry::EncoderCallback, this);

    // setup odom publisher
    odom_pub = nh.advertise<nav_msgs::Odometry>("/naive_odom", 100);
    navsat_pub = nh.advertise<sensor_msgs::NavSatFix>("/raw_gps_navsat", 5);

    // setup client for SetDatum service
    client = nh.serviceClient<robot_localization::SetDatum>("/datum");
    datum_set = false;

    // initialize odometry
    odom_x = 0.0;
    odom_y = 0.0;
    banked_dist = 0.0;

    // Initial orientation is always 0
    current_imu_orientation.setRPY(0.0, 0.0, 0.0);

    // initialize odom_msg
    odom_msg.header.frame_id = ODOM_FRAME_NAME;
    odom_msg.child_frame_id = BASE_LINK_FRAME_NAME;


    // pull parameters from launch file
    nh.param<double>("initial_compass_yaw_deg", initial_compass_yaw_deg, 0.0);

    size_t identity_col;
    XmlRpc::XmlRpcValue _odom_launch_covariances;
    if (nh.hasParam("odom_covariances")) {
        nh.getParam("odom_covariances", _odom_launch_covariances);
        ROS_ASSERT(_odom_launch_covariances.getType() == XmlRpc::XmlRpcValue::TypeArray);
        ROS_ASSERT(_odom_launch_covariances.size() == NUM_ROWS_ODOM_COVARIANCE * NUM_ROWS_ODOM_COVARIANCE);

        ROS_INFO("Using launch file's odometry covariances");
        for (size_t i = 0; i < _odom_launch_covariances.size(); i++) {
            odom_msg.pose.covariance[i] = _odom_launch_covariances[i];
        }
    }
    else {
        ROS_INFO("Using default odometry covariances");
        // Set covariance to identity multiplied by scaling factor
        identity_col = 0;
        for (size_t row = 0; row < NUM_ROWS_ODOM_COVARIANCE * NUM_ROWS_ODOM_COVARIANCE; row += NUM_ROWS_ODOM_COVARIANCE) {
            odom_msg.pose.covariance[row + identity_col] = 0.5;
            identity_col++;
        }
    }

    XmlRpc::XmlRpcValue _gps_launch_covariances;
    if (nh.hasParam("gps_covariances")) {
        nh.getParam("gps_covariances", _gps_launch_covariances);
        ROS_ASSERT(_gps_launch_covariances.getType() == XmlRpc::XmlRpcValue::TypeArray);
        ROS_ASSERT(_gps_launch_covariances.size() == NUM_ROWS_GPS_COVARIANCE * NUM_ROWS_GPS_COVARIANCE);

        ROS_INFO("Using launch file's gps covariances");
        for (size_t i = 0; i < _gps_launch_covariances.size(); i++) {
            gps_covariance_msg.position_covariance[i] = _gps_launch_covariances[i];
        }
    }
    else {
        ROS_INFO("Using default GPS covariances");
        // Set covariance of the gps (diagonal only. Assuming independence)
        identity_col = 0;
        for (size_t row = 0; row < NUM_ROWS_GPS_COVARIANCE * NUM_ROWS_GPS_COVARIANCE; row += NUM_ROWS_GPS_COVARIANCE) {
            gps_covariance_msg.position_covariance[row + identity_col] = 0.25;
            identity_col++;
        }
    }

    // tf_broadcaster = tf::TransformBroadcaster();
}

void BabybuggyOdometry::IMUCallback(const sensor_msgs::Imu& msg)
{
    // Convert sensor_msgs quaternion to tf quaternion
    tf::Quaternion tmp;

    tmp.setValue(
        msg.orientation.x,
        msg.orientation.y,
        msg.orientation.z,
        msg.orientation.w
    );

    // extract 3x3 rotation matrix from quaternion
    tf::Matrix3x3 m(tmp);
    m.getEulerYPR(yaw, pitch, roll);  // convert to ypr and set current_imu_orientation

    yaw = 2.0 * M_PI - yaw;
    // yaw = fmod(-yaw, 2 * M_PI);
    // if (yaw < 0.0) {
    //     yaw += 2.0 * M_PI;
    // }

    // Use yaw and the encoder's banked_dist to calculate
    odom_x += cos(yaw) * banked_dist;
    odom_y += sin(yaw) * banked_dist;
    banked_dist = 0.0;  // don't add redundant distances to the x, y position

    // update orientation with adjusted yaw values
    current_imu_orientation.setRPY(roll, pitch, yaw);

    // Only produce odometry messages if both sensors (encoders and IMU) are initialized and producing data
    if (enc_data_received)
    {
        // Form the odometry transform and broadcast it
        // odometry_transform.setOrigin(tf::Vector3(odom_x, odom_y, 0.0));
        // odometry_transform.setRotation(current_imu_orientation);

        // tf_broadcaster.sendTransform(tf::StampedTransform(odometry_transform, ros::Time::now(), ODOM_FRAME_NAME, BASE_LINK_FRAME_NAME));

        // Form the odometry message with the IMU's orientation and accumulated distance in x and y
        odom_msg.header.stamp = ros::Time::now();

        // fill out and publish odom data
        odom_msg.pose.pose.position.x = odom_x;
        odom_msg.pose.pose.position.y = odom_y;
        odom_msg.pose.pose.position.z = 0.0;

        odom_msg.pose.pose.orientation.x = current_imu_orientation.x();
        odom_msg.pose.pose.orientation.z = current_imu_orientation.z();
        odom_msg.pose.pose.orientation.y = current_imu_orientation.y();
        odom_msg.pose.pose.orientation.w = current_imu_orientation.w();

        odom_pub.publish(odom_msg);
    }

    if (ros::Time::now() - debug_info_prev_time > DEBUG_INFO_DELAY) {
        debug_info_prev_time = ros::Time::now();
        ROS_INFO("Odom pose | X: %f, Y: %f, yaw: %f", odom_x, odom_y, yaw * 180 / M_PI);
    }
}

void BabybuggyOdometry::GPSCallback(const sensor_msgs::NavSatFix& msg)
{
    // Wait for the GPS to produce data that's valid and set robot_localization's datum
    if (msg.status.status == msg.status.STATUS_FIX && msg.latitude != 0.0 && msg.longitude != 0.0) {
        if (!datum_set) {
            srv.request.geo_pose.position.latitude = msg.latitude;
            srv.request.geo_pose.position.longitude = msg.longitude;
            srv.request.geo_pose.position.altitude = msg.altitude;

            tf::Quaternion tmp;
            tmp.setRPY(0.0, 0.0, initial_compass_yaw_deg * M_PI / 180.0);

            srv.request.geo_pose.orientation.x = tmp.x();
            srv.request.geo_pose.orientation.y = tmp.y();
            srv.request.geo_pose.orientation.z = tmp.z();
            srv.request.geo_pose.orientation.w = tmp.w();

            if(client.call(srv)){
                ROS_INFO("gps datum service call - success! lat: %0.6f, long: %0.6f\n", msg.latitude, msg.longitude);
            }
            else{
                ROS_INFO("gps datum service call - failed!\n");
            }

            datum_set = true;
            ROS_INFO("gps datum service call - datum set!\n");
        }

        sensor_msgs::NavSatFix new_msg = msg;
        new_msg.position_covariance = gps_covariance_msg.position_covariance;
        new_msg.position_covariance_type = msg.COVARIANCE_TYPE_APPROXIMATED;
        navsat_pub.publish(new_msg);
    }
    else {
        datum_set = false;
    }
}

void BabybuggyOdometry::EncoderCallback(const std_msgs::Float64& msg)
{
    // append the encoder's distance to banked_dist. The arduino produces distances relative to the last measurement.
    banked_dist += msg.data / 1000.0;
    enc_data_received = true;
}