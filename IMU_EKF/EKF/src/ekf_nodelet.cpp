#include "EKF/ekf_nodelet.h"

PLUGINLIB_DECLARE_CLASS(EKF, EKFNodelet, EKFNodelet, nodelet::Nodelet);

void EKFNodelet::onInit()
{
  NODELET_INFO("Initializing EKF Nodelet");
  
  // TODO: Do we want the single threaded or multithreaded NH?
  ros::NodeHandle nh         = getMTNodeHandle();
  ros::NodeHandle nh_private = getMTPrivateNodeHandle();

  ekf_ = new EKF(nh, nh_private);
}
