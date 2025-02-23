
#include "ros/ros.h"

#include <iostream>
#include <cstdio>
#include "sensor_msgs/Imu.h"
#include "sensor_msgs/Image.h"
#include "stereo_msgs/DisparityImage.h"
#include <camera_info_manager/camera_info_manager.h>
#include <functional>

// Inludes common necessary includes for development using depthai library
#include "depthai/depthai.hpp"
#include <depthai_bridge/BridgePublisher.hpp>
#include <depthai_bridge/ImuConverter.hpp>
#include <depthai_bridge/ImageConverter.hpp>
#include <depthai_bridge/DisparityConverter.hpp>


dai::Pipeline createPipeline(bool enableDepth, bool lrcheck, bool extended, bool subpixel, bool rectify, bool depth_aligned, int stereo_fps){
    dai::Pipeline pipeline;

    auto monoLeft             = pipeline.create<dai::node::MonoCamera>();
    auto monoRight            = pipeline.create<dai::node::MonoCamera>();
    auto stereo               = pipeline.create<dai::node::StereoDepth>();
    auto xoutDepth            = pipeline.create<dai::node::XLinkOut>();
    auto imu                  = pipeline.create<dai::node::IMU>();
    auto xoutImu              = pipeline.create<dai::node::XLinkOut>();

    if (enableDepth) {
        xoutDepth->setStreamName("depth");
    }
    else {
        xoutDepth->setStreamName("disparity");
    }

    xoutImu->setStreamName("imu");

    // MonoCamera
    monoLeft->setResolution(dai::MonoCameraProperties::SensorResolution::THE_720_P);
    monoLeft->setBoardSocket(dai::CameraBoardSocket::LEFT);
    monoLeft->setFps(stereo_fps);
    monoRight->setResolution(dai::MonoCameraProperties::SensorResolution::THE_720_P);
    monoRight->setBoardSocket(dai::CameraBoardSocket::RIGHT);
    monoRight->setFps(stereo_fps);

    // StereoDepth
    stereo->initialConfig.setConfidenceThreshold(220); //Known to be best
    stereo->setRectifyEdgeFillColor(0); // black, to better see the cutout
    stereo->initialConfig.setLeftRightCheckThreshold(10); //Known to be best
    stereo->setLeftRightCheck(lrcheck);
    stereo->setExtendedDisparity(extended);
    stereo->setSubpixel(subpixel);
    if(enableDepth && depth_aligned) stereo->setDepthAlign(dai::CameraBoardSocket::RGB);

    //Imu
    imu->enableIMUSensor({dai::IMUSensor::ROTATION_VECTOR, dai::IMUSensor::ACCELEROMETER_RAW, dai::IMUSensor::GYROSCOPE_RAW}, 400);
    imu->setMaxBatchReports(1); // Get one message only for now.

    if(enableDepth && depth_aligned){
        // RGB image
        auto camRgb               = pipeline.create<dai::node::ColorCamera>();
        auto xoutRgb              = pipeline.create<dai::node::XLinkOut>();
        xoutRgb->setStreamName("rgb");
        camRgb->setBoardSocket(dai::CameraBoardSocket::RGB);
        camRgb->setResolution(dai::ColorCameraProperties::SensorResolution::THE_1080_P);
        // the ColorCamera is downscaled from 1080p to 720p.
        // Otherwise, the aligned depth is automatically upscaled to 1080p
        camRgb->setIspScale(2, 3);
        // For now, RGB needs fixed focus to properly align with depth.
        // This value was used during calibration
        camRgb->initialControl.setManualFocus(135);
        camRgb->isp.link(xoutRgb->input);
    }else{
        // Stereo imges
        auto xoutLeft             = pipeline.create<dai::node::XLinkOut>();
        auto xoutRight            = pipeline.create<dai::node::XLinkOut>();
        // XLinkOut
        xoutLeft->setStreamName("left");
        xoutRight->setStreamName("right");
        if(rectify){
            stereo->rectifiedLeft.link(xoutLeft->input);
            stereo->rectifiedRight.link(xoutRight->input);     
        }else{
            stereo->syncedLeft.link(xoutLeft->input);
            stereo->syncedRight.link(xoutRight->input);
        }
    }

    // Link plugins CAM -> STEREO -> XLINK

    monoLeft->out.link(stereo->left);
    monoRight->out.link(stereo->right);

    if(enableDepth){
        stereo->depth.link(xoutDepth->input);
    }
    else{
        stereo->disparity.link(xoutDepth->input);
    }

    imu->out.link(xoutImu->input);

    return pipeline;
}

int main(int argc, char** argv){
    
    ros::init(argc, argv, "stereo_inertial_node");
    ros::NodeHandle pnh("~");

    std::string deviceName, mode;
    int badParams = 0, stereo_fps;
    bool lrcheck, extended, subpixel, enableDepth, rectify, depth_aligned;

    badParams += !pnh.getParam("camera_name", deviceName);
    badParams += !pnh.getParam("mode", mode);
    badParams += !pnh.getParam("lrcheck",  lrcheck);
    badParams += !pnh.getParam("extended",  extended);
    badParams += !pnh.getParam("subpixel",  subpixel);
    badParams += !pnh.getParam("rectify",  rectify);
    badParams += !pnh.getParam("depth_aligned",  depth_aligned);
    badParams += !pnh.getParam("stereo_fps",  stereo_fps);

    if (badParams > 0)
    {   
        std::cout << " Bad parameters -> " << badParams << std::endl;
        throw std::runtime_error("Couldn't find %d of the parameters");
    }

    if(mode == "depth"){
        enableDepth = true;
    }
    else{
        enableDepth = false;
    }

    dai::Pipeline pipeline = createPipeline(enableDepth, lrcheck, extended, subpixel, rectify, depth_aligned, stereo_fps);

    dai::Device device(pipeline);

    std::shared_ptr<dai::DataOutputQueue> stereoQueue;
    if (enableDepth) {
        stereoQueue = device.getOutputQueue("depth", 30, false);
    }else{
        stereoQueue = device.getOutputQueue("disparity", 30, false);
    }
    auto imuQueue = device.getOutputQueue("imu",30,false);

    auto calibrationHandler = device.readCalibration();
    
    dai::rosBridge::ImageConverter converter(deviceName + "_left_camera_optical_frame", true);
    dai::rosBridge::ImageConverter rightconverter(deviceName + "_right_camera_optical_frame", true);
    auto leftCameraInfo = converter.calibrationToCameraInfo(calibrationHandler, dai::CameraBoardSocket::LEFT, 1280, 720); 
    auto rightCameraInfo = converter.calibrationToCameraInfo(calibrationHandler, dai::CameraBoardSocket::RIGHT, 1280, 720); 
    const std::string leftPubName = rectify?std::string("left/image_rect"):std::string("left/image_raw");
    const std::string rightPubName = rectify?std::string("right/image_rect"):std::string("right/image_raw");

    dai::rosBridge::ImuConverter imuConverter(deviceName +"_imu_frame");

    dai::rosBridge::BridgePublisher<sensor_msgs::Imu, dai::IMUData> ImuPublish(imuQueue,
                                                                                     pnh, 
                                                                                     std::string("imu"),
                                                                                     std::bind(&dai::rosBridge::ImuConverter::toRosMsg, 
                                                                                     &imuConverter, 
                                                                                     std::placeholders::_1,
                                                                                     std::placeholders::_2) , 
                                                                                     30,
                                                                                     "",
                                                                                     "imu");

    ImuPublish.addPubisherCallback();

    dai::rosBridge::ImageConverter rgbConverter(deviceName + "_rgb_camera_optical_frame", false);
    auto rgbCameraInfo = rgbConverter.calibrationToCameraInfo(calibrationHandler, dai::CameraBoardSocket::RGB, 1280, 720);
    
     if(enableDepth){
        std::cout << "In depth";
        auto depthCameraInfo = depth_aligned ? rgbConverter.calibrationToCameraInfo(calibrationHandler, dai::CameraBoardSocket::RGB, 1280, 720) : rightCameraInfo;
        auto depthconverter = depth_aligned ? rgbConverter : rgbConverter;
        dai::rosBridge::BridgePublisher<sensor_msgs::Image, dai::ImgFrame> depthPublish(stereoQueue,
                                                                                     pnh, 
                                                                                     std::string("stereo/depth"),
                                                                                     std::bind(&dai::rosBridge::ImageConverter::toRosMsg, 
                                                                                     &depthconverter, // since the converter has the same frame name
                                                                                                      // and image type is also same we can reuse it
                                                                                     std::placeholders::_1, 
                                                                                     std::placeholders::_2) , 
                                                                                     30,
                                                                                     depthCameraInfo,
                                                                                     "stereo");
        depthPublish.addPubisherCallback();
        
        if(depth_aligned){
            auto imgQueue = device.getOutputQueue("rgb", 30, false);
            dai::rosBridge::BridgePublisher<sensor_msgs::Image, dai::ImgFrame> rgbPublish(imgQueue,
                                                                                        pnh, 
                                                                                        std::string("color/image"),
                                                                                        std::bind(&dai::rosBridge::ImageConverter::toRosMsg, 
                                                                                        &rgbConverter,
                                                                                        std::placeholders::_1, 
                                                                                        std::placeholders::_2) , 
                                                                                        30,
                                                                                        rgbCameraInfo,
                                                                                        "color");
            rgbPublish.addPubisherCallback();
            ros::spin();
        }
        else {
            auto leftQueue = device.getOutputQueue("left", 30, false);
            auto rightQueue = device.getOutputQueue("right", 30, false);
            dai::rosBridge::BridgePublisher<sensor_msgs::Image, dai::ImgFrame> leftPublish(leftQueue,
                                                                                            pnh, 
                                                                                            leftPubName,
                                                                                            std::bind(&dai::rosBridge::ImageConverter::toRosMsg, 
                                                                                            &converter, 
                                                                                            std::placeholders::_1, 
                                                                                            std::placeholders::_2) , 
                                                                                            30,
                                                                                            leftCameraInfo,
                                                                                            "left");
            dai::rosBridge::BridgePublisher<sensor_msgs::Image, dai::ImgFrame> rightPublish(rightQueue,
                                                                                            pnh, 
                                                                                            rightPubName,
                                                                                            std::bind(&dai::rosBridge::ImageConverter::toRosMsg, 
                                                                                            &rightconverter, 
                                                                                            std::placeholders::_1, 
                                                                                            std::placeholders::_2) , 
                                                                                            30,
                                                                                            rightCameraInfo,
                                                                                            "right");  
            rightPublish.addPubisherCallback();
            leftPublish.addPubisherCallback();
            ros::spin();
        }
    }
    else{
        std::string tfSuffix = depth_aligned ? "_rgb_camera_optical_frame" : "_right_camera_optical_frame";
        dai::rosBridge::DisparityConverter dispConverter(deviceName + tfSuffix , 880, 7.5, 20, 2000); // TODO(sachin): undo hardcoding of baseline
        auto disparityCameraInfo = depth_aligned ? rgbConverter.calibrationToCameraInfo(calibrationHandler, dai::CameraBoardSocket::RGB, 1280, 720) : rightCameraInfo;
        auto depthconverter = depth_aligned ? rgbConverter : rgbConverter;
        dai::rosBridge::BridgePublisher<stereo_msgs::DisparityImage, dai::ImgFrame> dispPublish(stereoQueue,
                                                                                     pnh, 
                                                                                     std::string("stereo/disparity"),
                                                                                     std::bind(&dai::rosBridge::DisparityConverter::toRosMsg, 
                                                                                     &dispConverter, 
                                                                                     std::placeholders::_1, 
                                                                                     std::placeholders::_2) , 
                                                                                     30,
                                                                                     disparityCameraInfo,
                                                                                     "stereo");
        dispPublish.addPubisherCallback();
        if(depth_aligned){
            auto imgQueue = device.getOutputQueue("rgb", 30, false);
            dai::rosBridge::ImageConverter rgbConverter(deviceName + "_rgb_camera_optical_frame", false);
            dai::rosBridge::BridgePublisher<sensor_msgs::Image, dai::ImgFrame> rgbPublish(imgQueue,
                                                                                        pnh, 
                                                                                        std::string("color/image"),
                                                                                        std::bind(&dai::rosBridge::ImageConverter::toRosMsg, 
                                                                                        &rgbConverter,
                                                                                        std::placeholders::_1, 
                                                                                        std::placeholders::_2) , 
                                                                                        30,
                                                                                        rgbCameraInfo,
                                                                                        "color");
            rgbPublish.addPubisherCallback();
            ros::spin();
        }
        else {
            auto leftQueue = device.getOutputQueue("left", 30, false);
            auto rightQueue = device.getOutputQueue("right", 30, false);
            dai::rosBridge::BridgePublisher<sensor_msgs::Image, dai::ImgFrame> leftPublish(leftQueue,
                                                                                            pnh, 
                                                                                            leftPubName,
                                                                                            std::bind(&dai::rosBridge::ImageConverter::toRosMsg, 
                                                                                            &converter, 
                                                                                            std::placeholders::_1, 
                                                                                            std::placeholders::_2) , 
                                                                                            30,
                                                                                            leftCameraInfo,
                                                                                            "left");
            dai::rosBridge::BridgePublisher<sensor_msgs::Image, dai::ImgFrame> rightPublish(rightQueue,
                                                                                            pnh, 
                                                                                            rightPubName,
                                                                                            std::bind(&dai::rosBridge::ImageConverter::toRosMsg, 
                                                                                            &rightconverter, 
                                                                                            std::placeholders::_1, 
                                                                                            std::placeholders::_2) , 
                                                                                            30,
                                                                                            rightCameraInfo,
                                                                                            "right");  
            rightPublish.addPubisherCallback();
            leftPublish.addPubisherCallback();
            ros::spin();
        }
    }
    
    return 0;
}