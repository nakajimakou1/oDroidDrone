#include <stdio.h>
#include <iostream>
#include <sys/time.h>

#include "feature.hpp"
#include "odometry.hpp"
#include "common.hpp"
#include "imu.hpp"

int main( int argc, char** argv )
{
	Calib calib;
	calib.o_x = 300.8859;
	calib.o_y = 222.5206;
	calib.f_x = 411.1170;
	calib.f_y = 409.9516;
	calib.k1 = -0.3453;
	calib.k2 = 0.1012;
	calib.t1 = -0.0003;
	calib.t2 = 0.0014;
	calib.CI_q = Eigen::Quaternion<double>(
			0.000000000000000,
			-0.000000000000000,
			0.382683432365090,
			-0.923879532511287
	);
	calib.C_p_I = Eigen::Vector3d( 0.0, 0.0, -0.056 );
	calib.g = 9.82;
	calib.delta_t = 0.0025;
	calib.imageOffset.tv_sec = 0;
	calib.imageOffset.tv_usec = 33000;
	calib.sigma_gc = 5.0e-04;
	calib.sigma_ac = 5.0e-04;
	calib.sigma_wgc = 0.05;
	calib.sigma_wac = 0.1;
	calib.sigma_Im = 40;
	calib.sigma_hc = 0.05;
	calib.minFrame = 1;
	std::cout << "calib is:\n" << calib << std::endl;


	MSCKF msckf( &calib );


	cv::VideoCapture cap(0);
	cv::Mat image;

	CameraMeasurements cameraMeasurements;
	CameraDetector cameraDetector = CameraDetector( );

	Imu imu( "/dev/spidev1.0", "/sys/class/gpio/gpio199/value" );

	// Wait for flightcontroller to start
	{
		std::cout << "Waiting for flightcontroller to boot" << std::endl;
		ImuMeas_t element;
		while( !imu.fifoPop( element ) );
		std::cout << "Flightcontroller booted" << std::endl;
	}

	struct timeval tv;
	struct timezone tz = {};
		tz.tz_minuteswest = 0;
		tz.tz_dsttime = 0;

	while( 1 ) {
		cap.grab();
		cap.grab();
		cap.grab();
		cap.grab();
		cap.grab();
		gettimeofday( &tv, &tz );
		cap.retrieve( image );

		//
		// Propagate up to new image ( can be run in parallel with feature detection)
		//
		while( 1 ) {
			ImuMeas_t element;
			// Wait for at least one imu measurement
			while( !imu.fifoPop( element ) );

			// Propagate
			msckf.propagate( element.acc, element.gyro );
			// If valid distance measurement, update with that
			if ( element.distValid ) {
				msckf.updateHeight( element.dist );
			}

			// Get time of image without delay
			struct timeval imageTime;
			timersub( &tv, &(element.timeStamp), &imageTime );

			std::cout <<
			"IMU time: " <<
			element.timeStamp.tv_sec << "." << std::setfill('0') << std::setw(6) << element.timeStamp.tv_usec << "s\n" <<
			"Image time: " <<
			imageTime.tv_sec << "." << std::setfill('0') << std::setw(6) << imageTime.tv_usec << "s\n" <<
			// If image is older that propagated point, update
			if ( timercmp( &imageTime, &(element.timeStamp), < ) )
				break;
		}

		//
		// ´Detect features ( can be run in parallel with propagation)
		//
		cameraDetector.detectFeatures( image, cameraMeasurements );

		//
		// We have propagated and got a new image, time to update with camera data
		//
		msckf.updateCamera( cameraMeasurements );

		//
		// Print state
		//
		std::cout << "msckf is:\n" << msckf << std::endl;
	}

}