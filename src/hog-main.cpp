#include <ros/ros.h>
#include <iostream>
#include <iomanip>
#include <vector>
#include <cmath>

#include <message_filters/sync_policies/approximate_time.h>
#include <message_filters/synchronizer.h>
#include <message_filters/subscriber.h>

#include <image_transport/image_transport.h>
#include <sensor_msgs/image_encodings.h>
#include <sensor_msgs/PointCloud2.h>
#include <sensor_msgs/Image.h>

#include <opencv2/objdetect.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <cv_bridge/cv_bridge.h>

#include <geometry_msgs/TransformStamped.h>
#include <geometry_msgs/PoseStamped.h>

#include <tf2/LinearMath/Quaternion.h>
#include <tf2_ros/transform_broadcaster.h>
#include <visualization_msgs/Marker.h>

namespace enc = sensor_msgs::image_encodings;
using namespace message_filters;
using namespace std;
using namespace cv;

/* Global variables */
#define humanFrameID "human_detected"
#define fixedFrameID "camera_link"

ros::Publisher marker_pub;
ros::Publisher pose_pub;

vector<vector<float>> VectPose;
bool ValidPose = false;

Size winStride = Size(8,8);
Size padding = Size();
double groupThreshold = 2;
double scale = 1.1;

class HumanParameters {
public:
  float Xposition;
  float Yposition;
  float Zposition;
  float Velocity;
  float Time;
  
  void addPose(const sensor_msgs::PointCloud2ConstPtr& pCloud, const vector<Rect> RectLst, const ros::Time& RosTime) {
    /* Pixel coordinates in the point cloud */   
    float X = 0.0;
    float Y = 0.0;
    float Z = 0.0;    
    int u_px = RectLst[0].x + RectLst[0].width/2;
    int v_px = RectLst[0].y + RectLst[0].height/2;
    int arrayPosition = v_px*pCloud->row_step + u_px*pCloud->point_step;
    int arrayPosX = arrayPosition + pCloud->fields[0].offset;
    int arrayPosY = arrayPosition + pCloud->fields[1].offset;
    int arrayPosZ = arrayPosition + pCloud->fields[2].offset;

    memcpy(&X, &pCloud->data[arrayPosX], sizeof(float));
    memcpy(&Y, &pCloud->data[arrayPosY], sizeof(float));
    memcpy(&Z, &pCloud->data[arrayPosZ], sizeof(float));
    
    Xposition = X;
    Yposition = Y;
    Zposition = Z;
    Time = RosTime.toSec();
  }
  
  void addVelocity(vector<vector<float>> VectPose) {
    /* Point velocity */
    float x1 = VectPose[0][0];
    float y1 = VectPose[0][1];
    float z1 = VectPose[0][2];
    float x2 = VectPose[1][0];
    float y2 = VectPose[1][1];
    float z2 = VectPose[1][2];
    Velocity = sqrt(pow(x2-x1,2) + pow(y2-y1,2) + pow(z2-z1,2))/Time;
  }
};

void publishMarker(const float Xposition, const float Yposition, const ros::Time& rosTime) {
  visualization_msgs::Marker marker;
  marker.header.frame_id = humanFrameID;
  marker.header.stamp = rosTime;
  marker.ns = "basic_shapes";
  marker.id = 0;

  marker.type = visualization_msgs::Marker::CYLINDER;
  marker.action = visualization_msgs::Marker::ADD;

  marker.pose.position.x = Xposition;
  marker.pose.position.y = Yposition;
  marker.pose.position.z = 0;
  marker.pose.orientation.x = 0.0;
  marker.pose.orientation.y = 0.0;
  marker.pose.orientation.z = 0.0;
  marker.pose.orientation.w = 1.0;

  marker.scale.x = 1.0;
  marker.scale.y = 1.0;
  marker.scale.z = 0.01;

  marker.color.r = 0.5f;
  marker.color.g = 0.0f;
  marker.color.b = 0.7f;
  marker.color.a = 1.0;

  marker.lifetime = ros::Duration();

  while (marker_pub.getNumSubscribers() < 1) {
    if (!ros::ok()) {
        return ;
      }
      sleep(1);
    }
    marker_pub.publish(marker);
}

void drawMarker(const float X, const float Y, const float Z, const ros::Time& rosTime) {
  /* Pointcloud to plane projection */
  static tf2_ros::TransformBroadcaster br;
  geometry_msgs::TransformStamped transformStamped;
  geometry_msgs::PoseStamped humanPose;
    
  transformStamped.header.stamp = rosTime;
  transformStamped.header.frame_id = fixedFrameID;
  transformStamped.child_frame_id = humanFrameID;
  transformStamped.transform.translation.x = X;
  transformStamped.transform.translation.y = Y;
  transformStamped.transform.translation.z = Z;
  tf2::Quaternion q;
  q.setRPY(-M_PI/2.0, M_PI/2.0, M_PI);
  transformStamped.transform.rotation.x = q.x();
  transformStamped.transform.rotation.y = q.y();
  transformStamped.transform.rotation.z = q.z();
  transformStamped.transform.rotation.w = q.w();
  
  /* Human position message*/
  br.sendTransform(transformStamped);
  humanPose.header.stamp = rosTime;
  humanPose.header.frame_id = humanFrameID;
  humanPose.pose.position.x = X; //red
  humanPose.pose.position.y = Y; //green
  humanPose.pose.position.z = Z; //blue
  
  /* Publish position and marker */
  pose_pub.publish(humanPose);
  publishMarker(X, Y, rosTime);
}
    
void interface(const sensor_msgs::ImageConstPtr& msg_rgb , const sensor_msgs::PointCloud2ConstPtr& pCloud) {
  vector<Rect> RectLst;
  HumanParameters Hp;
  HOGDescriptor Hog;
  Mat im_gray;

  /* Pre-processing */
  cv_bridge::CvImageConstPtr cv_ptr;
  try {
    cv_ptr = cv_bridge::toCvShare(msg_rgb,enc::BGR8);
  } catch (cv_bridge::Exception& e) {
    ROS_ERROR("cv_bridge exception: %s", e.what());
    return;
  }
  Hog.setSVMDetector(HOGDescriptor::getDefaultPeopleDetector());
  cvtColor(cv_ptr->image, im_gray, CV_BGR2GRAY );
  equalizeHist(im_gray, im_gray);
  
  /* Human detection */
  Hog.detectMultiScale(im_gray, RectLst, 0, winStride, padding, scale, groupThreshold, false);
  ros::Time rostime = ros::Time::now();
  if (RectLst.size() > 0)
    ValidPose = true;
  
  /* Position estimation*/
  if (ValidPose == true) {
    ValidPose = false;
    Hp.addPose(pCloud, RectLst, rostime);
    VectPose.push_back({Hp.Xposition, Hp.Yposition, Hp.Zposition});
    if (VectPose.size() == 2) {
      Hp.addVelocity(VectPose);
      drawMarker(Hp.Xposition, Hp.Yposition, Hp.Zposition, rostime);
      VectPose.erase(VectPose.begin());
    }
  }
}

int main(int argc, char **argv) {
  ros::init(argc, argv, "Human_dection");
  ros::NodeHandle nh;

  message_filters::Subscriber<sensor_msgs::Image> rgb_sub(nh, "/camera/rgb/image_rect_color", 1);
  message_filters::Subscriber<sensor_msgs::PointCloud2> pcd_sub(nh, "/camera/depth_registered/points", 1);
  typedef sync_policies::ApproximateTime<sensor_msgs::Image, sensor_msgs::PointCloud2> MySyncPolicy;
  Synchronizer<MySyncPolicy> sync(MySyncPolicy(30), rgb_sub, pcd_sub);
  sync.registerCallback(boost::bind(&interface, _1, _2));

  marker_pub = nh.advertise<visualization_msgs::Marker>("human/marker", 1);
  pose_pub = nh.advertise<geometry_msgs::PoseStamped>("human/position", 1);
  
  while (ros::ok()) {
    ros::spinOnce();
  }
  return 0;
}
