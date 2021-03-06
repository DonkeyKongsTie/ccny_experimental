
#include "EKF/ekf.h"
#include <EKF/EXTERNAL/eigen3/Eigen/Eigen>
#include <EKF/EXTERNAL/eigen3/Eigen/LU>
#include <iostream>

EKF::EKF(ros::NodeHandle nh, ros::NodeHandle nh_private):
  nh_(nh), 
  nh_private_(nh_private),
  initialized_(false),
  initialized_filter_(false),
  initialized_LP_filter_(false),
  q0(1.0), q1(0.0), q2(0.0), q3(0.0)
  
{
  ROS_INFO ("Starting EKF");

  // **** get paramters 
  // nothing for now
  
  initializeParams();
  
  int queue = 10;

  imu_EKF_publisher_ = nh_.advertise<sensor_msgs::Imu>(
	    "imu_EKF/data", queue);

  roll_ekf_publisher_    = nh.advertise<std_msgs::Float32>("roll_ekf", queue);
  pitch_ekf_publisher_   = nh.advertise<std_msgs::Float32>("pitch_ekf", queue);
  bias_ax_publisher_     = nh.advertise<std_msgs::Float32>("bias_ax", queue);
  bias_ay_publisher_     = nh.advertise<std_msgs::Float32>("bias_ay", queue);
  bias_az_publisher_     = nh.advertise<std_msgs::Float32>("bias_az", queue);
  roll_acc_publisher_    = nh.advertise<std_msgs::Float32>("roll_acc", queue);
  pitch_acc_publisher_   = nh.advertise<std_msgs::Float32>("pitch_acc", queue);
  //acc_mag_publisher_ = nh.advertise<std_msgs::Float32>("acc_mag", queue);
  //g_mag_publisher_   = nh.advertise<std_msgs::Float32>("g_mag", queue);

  // **** register subscribers
  int queue_size = 100;

  imu_subscriber_ = nh_.subscribe(
    "imu/data_raw", queue_size, &EKF::imuCallback, this);


}


EKF::~EKF()
{
  ROS_INFO ("Destroying EKF");
}

void EKF::initializeParams()
{
   //last_time_ = 0.0;
 // initialized_ = false;
  if (!nh_private_.getParam ("sigma_g", sigma_g_))
    sigma_g_ = 0.01;
  
  if (!nh_private_.getParam ("sigma_a", sigma_a_))
    sigma_a_ = 0.01;
  
  if (!nh_private_.getParam ("constant_dt", constant_dt_))
      constant_dt_ = 0.0;

  if (!nh_private_.getParam ("gain", gain_))
     gain_ = 0.1;

  if (!nh_private_.getParam ("sigma_bx", sigma_bx_))
    sigma_bx_ = 0.01;
  if (!nh_private_.getParam ("sigma_by", sigma_by_))
    sigma_by_ = 0.01;
  if (!nh_private_.getParam ("sigma_bz", sigma_bz_))
    sigma_bz_ = 0.01;
  if (!nh_private_.getParam ("fixed_frame", fixed_frame_))
     fixed_frame_ = "odom";
  if (!nh_private_.getParam ("imu_frame", imu_frame_))
       imu_frame_ = "imu_niac";
  if (!nh_private_.getParam ("threshold", threshold_))
       threshold_ = 10.0;
  
  gravity_vec_ = Eigen::Vector3f(0, 0, gmag_);
  
}

void EKF::imuCallback(const sensor_msgs::Imu::ConstPtr& imu_msg_raw)
{

  boost::mutex::scoped_lock(mutex_);

  //sensor_msgs::Imu imu =  *imu_msg_raw;
  
  //imu.header.stamp = ros::Time::now();
    
  const geometry_msgs::Vector3& ang_vel = imu_msg_raw->angular_velocity;
  const geometry_msgs::Vector3& lin_acc = imu_msg_raw->linear_acceleration; 

  ros::Time time = imu_msg_raw->header.stamp;
  

  float a_x = lin_acc.x;
  float a_y = lin_acc.y;
  float a_z = lin_acc.z;

  //float a_magnitude = sqrt(a_x*a_x + a_y*a_y + a_z*a_z);
  
  float dt;
  if (!initialized_filter_)
    {
      
      // initialize roll/pitch orientation from acc. vector
  	  double roll  = atan2(a_y, sqrt(a_x*a_x + a_z*a_z));
  	  double pitch = atan2(-a_x, sqrt(a_y*a_y + a_z*a_z));
  	  double yaw = 0.0;

      tf::Quaternion init_q = tf::createQuaternionFromRPY(roll, pitch, yaw);

      q1 = init_q.getX();
      q2 = init_q.getY();
      q3 = init_q.getZ();
      q0 = init_q.getW();
      
      b_ax = 0.0;
      b_ay = 0.0;
      b_az = 0.0;

            
     P = Eigen::MatrixXf::Zero(7,7);
      //P = 0.00001*P;
      // initialize time
      last_time_filter_ = time;
      initialized_filter_ = true;
    }
    else
     {
      // determine dt: either constant, or from IMU timestamp

      if (constant_dt_ > 0.0)
    	  dt = constant_dt_;
      else 
      	dt = (time - last_time_filter_).toSec();
     
      last_time_filter_ = time;
      
      

      filter(ang_vel.x, ang_vel.y, ang_vel.z,
      lin_acc.x, lin_acc.y, lin_acc.z,
      dt);
      
      std_msgs::Float32 acc_mag_msg;
      //acc_mag_msg.data = a_magnitude;
      //acc_mag_publisher_.publish(acc_mag_msg);
      publishFilteredMsg(imu_msg_raw);
      //publishTransform(imu_msg_raw);
   
     }
}


void EKF::filter(float p, float q, float r, 
  float ax, float ay, float az, 
  float dt)
{

  /************************** PREDICTION *****************************/

  
  Eigen::Matrix<float, 7, 7> F, I7;  // State-transition matrix             
  Eigen::Matrix<float, 7, 7> Q;  // process noise covariance
  Eigen::Matrix<float, 4, 3> Xi; // quaternion covariance 
  Eigen::Matrix<float, 4, 4> W;
  Eigen::Matrix<float, 3, 7> H;  // Measurement model Jacobian
  Eigen::Matrix<float, 7, 3> K;  // Kalman gain
  Eigen::Matrix3f I3, Sigma_g, Sigma_a, Sigma_b, R, C_bn;
  Eigen::Vector3f Z_est;
  Eigen::Vector3f bias_vec;
  
  I7 = Eigen::MatrixXf::Identity(7,7);
  I3 = Eigen::Matrix3f::Identity(); 
  
  
  float pdt, qdt, rdt, inv_norm;
  
  pdt = p*dt;
  qdt = q*dt;
  rdt = r*dt;
  
   // compute "a priori" state estimate 
   
   q1 += 0.5 * dt * (p*q0 - q*q3 + r*q2);
   q2 += 0.5 * dt * (p*q3 + q*q0 - r*q1);
   q3 += 0.5 * dt * (-p*q2 + q*q1 + r*q0);
   q0 += 0.5 * dt * (-p*q1 - q*q2 - r*q3); 

   bias_vec << b_ax, b_ay, b_az;

   X << q1,
        q2,
        q3,
        q0,
        b_ax,
        b_ay,
        b_az;

   //Linearized state transition matrix   

   F <<         1,   0.5f *rdt,   -0.5f *qdt, 0.5f *pdt, 0, 0, 0, 
        -0.5f *rdt,           1,   0.5f *pdt, 0.5f *qdt, 0, 0, 0,  
         0.5f *qdt, -0.5f *pdt,            1, 0.5f *rdt, 0, 0, 0,
        -0.5f *pdt, -0.5f *qdt,   -0.5f *rdt,         1, 0, 0, 0,
                 0,          0,            0,         0, 1, 0, 0,   
                 0,          0,            0,         0, 0, 1, 0, 
                 0,          0,            0,         0, 0, 0, 1;  


     
    Sigma_g = sigma_g_ * I3; 

    Sigma_a = sigma_a_ * I3;

    Sigma_b << sigma_bx_, 0, 0,            
                    0, sigma_by_, 0,
                    0,        0, sigma_bz_;
    

    Xi << q0, -q3,  q2,
          q3,  q0, -q1,
         -q2,  q1,  q0,
         -q1, -q2, -q3;

   
    float dt2 = dt*dt*0.25;
    W.noalias() = dt2 * (Xi *Sigma_g * Xi.transpose());
    
    /*
    Eigen::Matrix<float, 4, 1> quat;
    quat << q1,q2,q3,q0;
    //std::cout << "q" <<quat << std::endl;
    Eigen::Matrix<float, 4, 4> M, I, P_q;
    I = Eigen::MatrixXf::Identity(4, 4);
    P_q = P.block<4,4>(0,0);
    M = (quat*quat.transpose()) + P_q;
    //std::cout << "M" << M << std::endl;
    W = sigma_gx_*dt2*(M.trace()*I - M);
    */
    
    Q = Eigen::MatrixXf::Zero(7,7);
    Q.block<4,4>(0,0)= W; Q.block<3,3>(4,4)= dt * Sigma_b;

    
    P = F*(P*F.transpose()) + Q;
    //std::cout << "Q" << std::endl << Q << std::endl;

    // CORRECTION /
    
    C_bn << (q1*q1-q2*q2-q3*q3+q0*q0),     2*(q1*q2+q3*q0),            2*(q1*q3-q2*q0),
                      2*(q1*q2-q3*q0),     -q1*q1+q2*q2-q3*q3+q0*q0,   2*(q2*q3+q1*q0),
                      2*(q1*q3+q2*q0),     2*(q2*q3-q1*q0),            (-q1*q1-q2*q2+q3*q3+q0*q0); 

    

    
    Z_est = (C_bn*gravity_vec_) + bias_vec;
    
    ROS_INFO("bias: %f,%f,%f", bias_vec(0),bias_vec(1),bias_vec(2));
    
    std::cout << "rotated g no b" << std::endl << C_bn*gravity_vec_ << std::endl;
    ROS_INFO("rotated g: %f,%f,%f", Z_est(0)-bias_vec(0),Z_est(1)-bias_vec(1),Z_est(2)-bias_vec(2));
    
    ROS_INFO("acc vector: %f,%f,%f", ax, ay, az);  
    Eigen::Vector3f Z_meas(ax, ay, az);
    
    // find the matrix H by Jacobian.

    H << gmag_*2*q3, -gmag_*2*q0, gmag_*2*q1, -gmag_*2*q2, 1, 0, 0,
         gmag_*2*q0,  gmag_*2*q3, gmag_*2*q2,  gmag_*2*q1, 0, 1, 0,
        -gmag_*2*q1, -gmag_*2*q2, gmag_*2*q3,  gmag_*2*q0, 0, 0, 1;
    
    R = Sigma_a;
    
    //Compute the Kalman gain K
    
    K = Eigen::MatrixXf::Zero(7,3);
    Eigen::Matrix<float, 3, 3> S;
 
    S=H*(P*H.transpose()) + R;
    K = P * H.transpose() * S.inverse();
    
    Eigen::Vector3f Error_vec;
    Error_vec = Z_meas - Z_est;
    ROS_INFO("error1: %f,%f,%f", Error_vec(0),Error_vec(1),Error_vec(2));
    //ROS_INFO("error2: %f,%f,%f", ax-Z_est(0),ay-Z_est(1),az-Z_est(2));
   std::cout << "K" << std::endl << K << std::endl;
    // Update State and Covariance  
    X.noalias() += K * Error_vec;;
    
    
    P = (I7 - K*H)*P;

    q1 = X(0); q2 = X(1); q3 = X(2); q0 = X(3);
    b_ax = X(4); b_ay = X(5); b_az = X(6);

    inv_norm = invSqrt(q1*q1+q2*q2+q3*q3+q0*q0);
    q1 *= inv_norm;
    q2 *= inv_norm;
    q3 *= inv_norm;
    q0 *= inv_norm;

}


void EKF::publishTransform(const sensor_msgs::Imu::ConstPtr& imu_msg_raw)
{
  double r_raw ,p_raw ,y_raw;
  tf::Quaternion q_raw;
  tf::quaternionMsgToTF(imu_msg_raw->orientation, q_raw);
  tf::Matrix3x3 M;
  M.setRotation(q_raw);
  M.getRPY(r_raw, p_raw, y_raw);

  double r, p, y;
  tf::Quaternion q(q1, q2, q3, q0);
  M.setRotation(q);
  M.getRPY(r, p, y);

  tf::Quaternion q_final = tf::createQuaternionFromRPY(r, p, y_raw);
  //tf::Quaternion q(q1, q2, q3, q0);
  tf::Transform transform;
  transform.setOrigin( tf::Vector3( 0.0, 0.0, 0.0 ) );
  transform.setRotation( q_final );
  tf_broadcaster_.sendTransform( tf::StampedTransform( transform,
                   imu_msg_raw->header.stamp,
                   fixed_frame_,
                   imu_frame_ ) );

}

void EKF::publishFilteredMsg(const sensor_msgs::Imu::ConstPtr& imu_msg_raw)
{
  // create orientation quaternion
  // q0 is the angle, q1, q2, q3 are the axes
  tf::Quaternion q(q1, q2, q3, q0);

  // create and publish fitlered IMU message
  boost::shared_ptr<sensor_msgs::Imu> imu_msg = boost::make_shared<sensor_msgs::Imu>(*imu_msg_raw);

  imu_msg->header.frame_id = fixed_frame_;
  tf::quaternionTFToMsg(q, imu_msg->orientation);
  imu_EKF_publisher_.publish(imu_msg);

  double roll, pitch, y;
  tf::Matrix3x3 M;
  M.setRotation(q);
  M.getRPY(roll, pitch, y);
  std_msgs::Float32 roll_msg;
  std_msgs::Float32 pitch_msg;
  roll_msg.data = roll;
  pitch_msg.data = pitch;
  roll_ekf_publisher_.publish(roll_msg);
  std_msgs::Float32 bias_ax_msg;
  std_msgs::Float32 bias_ay_msg;
  std_msgs::Float32 bias_az_msg;
  bias_ax_msg.data = b_ax;
  bias_ay_msg.data = b_ay;
  bias_az_msg.data = b_az;
  
  pitch_ekf_publisher_.publish(pitch_msg);
   
  bias_ax_publisher_.publish(bias_ax_msg);
  bias_ay_publisher_.publish(bias_ay_msg);  
  bias_az_publisher_.publish(bias_az_msg);
  
  const geometry_msgs::Vector3& lin_acc = imu_msg_raw->linear_acceleration; 

  float a_x = lin_acc.x;
  float a_y = lin_acc.y;
  float a_z = lin_acc.z;
  double roll_acc, pitch_acc;
  roll_acc  = atan2(a_y, sqrt(a_x*a_x + a_z*a_z));
  pitch_acc = atan2(-a_x, sqrt(a_y*a_y + a_z*a_z));
  std_msgs::Float32 roll_acc_msg;
  std_msgs::Float32 pitch_acc_msg;
  roll_acc_msg.data = roll_acc;
  pitch_acc_msg.data = pitch_acc;
  roll_acc_publisher_.publish(roll_acc_msg);
  pitch_acc_publisher_.publish(pitch_acc_msg);
/*
   // XSens roll and pitch publishing 
   tf::Quaternion q_kf; 
   tf::quaternionMsgToTF(imu_msg_raw->orientation, q_kf);
   M.setRotation(q_kf);
   M.getRPY(roll, pitch, y);
   roll_msg.data = roll;
   pitch_msg.data = pitch;
   roll_kf_publisher_.publish(roll_msg);
   pitch_kf_publisher_.publish(pitch_msg);*/
}









  

