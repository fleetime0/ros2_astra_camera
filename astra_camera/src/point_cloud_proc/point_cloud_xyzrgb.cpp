// Copyright (c) 2008, Willow Garage, Inc.
// All rights reserved.
//
// Software License Agreement (BSD License 2.0)
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above
//    copyright notice, this list of conditions and the following
//    disclaimer in the documentation and/or other materials provided
//    with the distribution.
//  * Neither the name of the Willow Garage nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
#include <rclcpp/rclcpp.hpp>
#include <image_transport/image_transport.hpp>
#include <image_transport/subscriber_filter.hpp>
#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/exact_time.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <sensor_msgs/image_encodings.hpp>
#include <sensor_msgs/point_cloud2_iterator.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <image_geometry/pinhole_camera_model.hpp>
#include <cv_bridge/cv_bridge.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include <memory>
#include <string>
#include <vector>
#include "astra_camera/point_cloud_proc/point_cloud_xyzrgb.h"
namespace astra_camera {

PointCloudXyzrgbNode::PointCloudXyzrgbNode(const rclcpp::NodeOptions& options)
    : Node("PointCloudXyzrgbNode", options) {
  // Read parameters
  int queue_size = static_cast<int>(declare_parameter<int>("queue_size", 5));
  bool use_exact_sync = declare_parameter<bool>("exact_sync", false);

  // Synchronize inputs. Topic subscriptions happen on demand in the connection callback.
  if (use_exact_sync) {
    exact_sync_ = std::make_shared<ExactSynchronizer>(ExactSyncPolicy(queue_size), sub_depth_,
                                                      sub_rgb_, sub_info_);
    exact_sync_->registerCallback(std::bind(&PointCloudXyzrgbNode::imageCb, this,
                                            std::placeholders::_1, std::placeholders::_2,
                                            std::placeholders::_3));
  } else {
    sync_ = std::make_shared<Synchronizer>(SyncPolicy(queue_size), sub_depth_, sub_rgb_, sub_info_);
    sync_->registerCallback(std::bind(&PointCloudXyzrgbNode::imageCb, this, std::placeholders::_1,
                                      std::placeholders::_2, std::placeholders::_3));
  }

  // Monitor whether anyone is subscribed to the output
  // TODO(ros2) Implement when SubscriberStatusCallback is available
  // ros::SubscriberStatusCallback connect_cb = boost::bind(&PointCloudXyzrgbNode::connectCb, this);
  connectCb();
  // TODO(ros2) Implement when SubscriberStatusCallback is available
  // Make sure we don't enter connectCb() between advertising and assigning to pub_point_cloud_
  std::lock_guard<std::mutex> lock(connect_mutex_);
  // TODO(ros2) Implement connect_cb when SubscriberStatusCallback is available
  pub_point_cloud_ = create_publisher<PointCloud2>("depth/color/points", rclcpp::SensorDataQoS());
  // TODO(ros2) Implement connect_cb when SubscriberStatusCallback is available
}

void PointCloudXyzrgbNode::convertRgb(const sensor_msgs::msg::Image::ConstSharedPtr& rgb_msg,
                                      sensor_msgs::msg::PointCloud2::SharedPtr& cloud_msg,
                                      int red_offset, int green_offset, int blue_offset,
                                      int color_step) {
  sensor_msgs::PointCloud2Iterator<uint8_t> iter_r(*cloud_msg, "r");
  sensor_msgs::PointCloud2Iterator<uint8_t> iter_g(*cloud_msg, "g");
  sensor_msgs::PointCloud2Iterator<uint8_t> iter_b(*cloud_msg, "b");
  const uint8_t* rgb = &rgb_msg->data[0];
  int rgb_skip = rgb_msg->step - rgb_msg->width * color_step;
  for (int v = 0; v < static_cast<int>(cloud_msg->height); ++v, rgb += rgb_skip) {
    for (int u = 0; u < static_cast<int>(cloud_msg->width);
         ++u, rgb += color_step, ++iter_r, ++iter_g, ++iter_b) {
      *iter_r = rgb[red_offset];
      *iter_g = rgb[green_offset];
      *iter_b = rgb[blue_offset];
    }
  }
}
// Handles (un)subscribing when clients (un)subscribe
void PointCloudXyzrgbNode::connectCb() {
  std::lock_guard<std::mutex> lock(connect_mutex_);
  // TODO(ros2) Implement getNumSubscribers when rcl/rmw support it
  // parameter for depth_image_transport hint
  std::string depth_image_transport_param = "depth_image_transport";
  image_transport::TransportHints depth_hints(this, "raw", depth_image_transport_param);

  // depth image can use different transport.(e.g. compressedDepth)
  sub_depth_.subscribe(this, "depth/image_raw", depth_hints.getTransport(),
                       rmw_qos_profile_sensor_data);

  // rgb uses normal ros transport hints.
  image_transport::TransportHints hints(this, "raw");
  sub_rgb_.subscribe(this, "color/image_raw", hints.getTransport(), rmw_qos_profile_sensor_data);
  sub_info_.subscribe(this, "color/camera_info", rmw_qos_profile_sensor_data);
}

void PointCloudXyzrgbNode::imageCb(const Image::ConstSharedPtr& depth_msg,
                                   const Image::ConstSharedPtr& rgb_msg_in,
                                   const CameraInfo::ConstSharedPtr& info_msg) {
  // Check for bad inputs
  if (depth_msg->header.frame_id != rgb_msg_in->header.frame_id) {
    static auto clock = rclcpp::Clock(RCL_ROS_TIME);
    RCLCPP_WARN_THROTTLE(logger_, clock,
                         50000,  // 5 seconds
                         "Depth image frame id [%s] doesn't match RGB image frame id [%s]",
                         depth_msg->header.frame_id.c_str(), rgb_msg_in->header.frame_id.c_str());
    return;
  }

  // Update camera model
  model_.fromCameraInfo(info_msg);

  // Check if the input image has to be resized
  Image::ConstSharedPtr rgb_msg = rgb_msg_in;
  if (depth_msg->width != rgb_msg->width || depth_msg->height != rgb_msg->height) {
    CameraInfo info_msg_tmp = *info_msg;
    info_msg_tmp.width = depth_msg->width;
    info_msg_tmp.height = depth_msg->height;
    float ratio = static_cast<float>(depth_msg->width) / static_cast<float>(rgb_msg->width);
    info_msg_tmp.k[0] *= ratio;
    info_msg_tmp.k[2] *= ratio;
    info_msg_tmp.k[4] *= ratio;
    info_msg_tmp.k[5] *= ratio;
    info_msg_tmp.p[0] *= ratio;
    info_msg_tmp.p[2] *= ratio;
    info_msg_tmp.p[5] *= ratio;
    info_msg_tmp.p[6] *= ratio;
    model_.fromCameraInfo(info_msg_tmp);

    cv_bridge::CvImageConstPtr cv_ptr;
    try {
      cv_ptr = cv_bridge::toCvShare(rgb_msg, rgb_msg->encoding);
    } catch (cv_bridge::Exception& e) {
      RCLCPP_ERROR(logger_, "cv_bridge exception: %s", e.what());
      return;
    }
    cv_bridge::CvImage cv_rsz;
    cv_rsz.header = cv_ptr->header;
    cv_rsz.encoding = cv_ptr->encoding;
    // FIXME:
    int end_raw = std::min<int>(depth_msg->height / ratio, rgb_msg->height);
    cv::resize(cv_ptr->image.rowRange(0, end_raw), cv_rsz.image,
               cv::Size(depth_msg->width, depth_msg->height));
    if ((rgb_msg->encoding == enc::RGB8) || (rgb_msg->encoding == enc::BGR8) ||
        (rgb_msg->encoding == enc::MONO8)) {
      rgb_msg = cv_rsz.toImageMsg();
    } else {
      rgb_msg = cv_bridge::toCvCopy(cv_rsz.toImageMsg(), enc::RGB8)->toImageMsg();
    }

    RCLCPP_ERROR_THROTTLE(logger_, *get_clock(), 50000,
                          "Depth resolution (%ux%u) does not match RGB resolution (%ux%u)",
                          depth_msg->width, depth_msg->height, rgb_msg->width, rgb_msg->height);
    return;
  } else {
    rgb_msg = rgb_msg_in;
  }

  // Supported color encodings: RGB8, BGR8, MONO8
  int red_offset, green_offset, blue_offset, color_step;
  if (rgb_msg->encoding == enc::RGB8) {
    red_offset = 0;
    green_offset = 1;
    blue_offset = 2;
    color_step = 3;
  } else if (rgb_msg->encoding == enc::BGR8) {
    red_offset = 2;
    green_offset = 1;
    blue_offset = 0;
    color_step = 3;
  } else if (rgb_msg->encoding == enc::MONO8) {
    red_offset = 0;
    green_offset = 0;
    blue_offset = 0;
    color_step = 1;
  } else {
    try {
      rgb_msg = cv_bridge::toCvCopy(rgb_msg, enc::RGB8)->toImageMsg();
    } catch (cv_bridge::Exception& e) {
      RCLCPP_ERROR(logger_, "Unsupported encoding [%s]: %s", rgb_msg->encoding.c_str(), e.what());
      return;
    }
    red_offset = 0;
    green_offset = 1;
    blue_offset = 2;
    color_step = 3;
  }

  auto cloud_msg = std::make_shared<PointCloud2>();
  cloud_msg->header = depth_msg->header;  // Use depth image time stamp
  cloud_msg->height = depth_msg->height;
  cloud_msg->width = depth_msg->width;
  cloud_msg->is_dense = false;
  cloud_msg->is_bigendian = false;

  sensor_msgs::PointCloud2Modifier pcd_modifier(*cloud_msg);
  pcd_modifier.setPointCloud2FieldsByString(2, "xyz", "rgb");

  // Convert Depth Image to Pointcloud
  if (depth_msg->encoding == enc::TYPE_16UC1) {
    PointCloudXyzNode::convertDepth<uint16_t>(depth_msg, cloud_msg, model_);
  } else if (depth_msg->encoding == enc::TYPE_32FC1) {
    PointCloudXyzNode::convertDepth<float>(depth_msg, cloud_msg, model_);
  } else {
    RCLCPP_ERROR(logger_, "Depth image has unsupported encoding [%s]", depth_msg->encoding.c_str());
    return;
  }

  // Convert RGB
  if (rgb_msg->encoding == enc::RGB8) {
    convertRgb(rgb_msg, cloud_msg, red_offset, green_offset, blue_offset, color_step);
  } else if (rgb_msg->encoding == enc::BGR8) {
    convertRgb(rgb_msg, cloud_msg, red_offset, green_offset, blue_offset, color_step);
  } else if (rgb_msg->encoding == enc::MONO8) {
    convertRgb(rgb_msg, cloud_msg, red_offset, green_offset, blue_offset, color_step);
  } else {
    RCLCPP_ERROR(logger_, "RGB image has unsupported encoding [%s]", rgb_msg->encoding.c_str());
    return;
  }

  pub_point_cloud_->publish(*cloud_msg);
}

}  // namespace astra_camera

#include "rclcpp_components/register_node_macro.hpp"

// Register the component with class_loader.
RCLCPP_COMPONENTS_REGISTER_NODE(astra_camera::PointCloudXyzrgbNode)
