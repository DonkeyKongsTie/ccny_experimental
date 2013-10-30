#include "EKF/ekf.h"

int main (int argc, char **argv)
{
  ros::init (argc, argv, "EKF");
  ros::NodeHandle nh;
  ros::NodeHandle nh_private("~");
  EKF ekf(nh, nh_private);
  ros::spin();
  return 0;
}
