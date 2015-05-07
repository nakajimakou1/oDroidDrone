#include <stdio.h>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sys/time.h>
#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <cmath>
#include <ctype.h>
#include <random>
#include "lkTracker.hpp"
#include "odometry.hpp"
#include "imu.hpp"
#include "videoIO.hpp"
#include "telemetry.hpp"


using namespace cv;
using namespace std;
using namespace Eigen;

int main( int argc, char** argv )
{
	std::ofstream logFile;
	logFile.open ("log.csv");
	Telemetry telemetry( 55000 );


	Calib calib;
	calib.o_x = 300.8859;
	calib.o_y = 222.5206;
	calib.f_x = 411.1170;
	calib.f_y = 409.9516;
	calib.k1 = -0.3453;
	calib.k2 = 0.1012;
	calib.t1 = -0.0003;
	calib.t2 = 0.0014;
	calib.CI_q = Eigen::QuaternionAlias<double>(
			 0.000000000000000,
			 0.707106781186548,
			 0.707106781186548,
			 0.000000000000000
	);
	calib.C_p_I = Eigen::Vector3d( 0.0, -0.044, -0.037 );
	calib.g = 9.82;
	calib.delta_t = 0.0025;
	calib.imageOffset.tv_sec = 0;
	calib.imageOffset.tv_usec = 33000 + 7000; // delay of 1 frame period + some
	calib.sigma_gc = 0.001;//5.0e-04;
	calib.sigma_ac = 0.008;//5.0e-04;
	calib.sigma_wgc = 0.0001;
	calib.sigma_wac = 0.0001;
	calib.sigma_Im = 40;
	calib.sigma_hc = 0.05;
	calib.minFrame = 1;
	std::cout << "calib is:\n" << calib << std::endl;


	Odometry odometry( &calib );
	// Start upside down
	odometry.x.block<4,1>(0,0) << 0, 0, 0, 1; // upright
	// Start 10cm off the ground
	odometry.x.block<3,1>(4,0) << 0, 0, 0.15; // 50cm from ground
	//acc offset
	odometry.x.block<3,1>(4+3+3+3,0) << 0, 0, 0;

	// Set initial uncertancy
	odometry.sigma.diagonal().block<3,1>(0,0) << 0.05, 0.05, 0.05;
	odometry.sigma.diagonal().block<3,1>(3,0) << 0, 0, 0.2;
	odometry.sigma.diagonal().block<3,1>(6,0) << 0, 0, 0;
	odometry.sigma.diagonal().block<3,1>(9,0) << 0.1, 0.1, 0.1;
	odometry.sigma.diagonal().block<3,1>(12,0) << 0.1, 0.1, 0.1;



	VideoIn videoIn( 0 );
	cv::Mat frame;

	namedWindow( "Features", 1 );

	Imu imu( "/dev/spidev1.0", "/sys/class/gpio/gpio199/value" );
	struct timeval tv;
	struct timezone tz = {};
		tz.tz_minuteswest = 0;
		tz.tz_dsttime = 0;
	//
	// Clear Imu buffer
	//
	gettimeofday( &tv, &tz );
	{
		ImuMeas_t element;
		while( imu.fifoPop( element ) )
		{
			// Get time of image without delay
			struct timeval imageTime;
			timersub( &tv, &(calib.imageOffset), &imageTime );

			// If image is older that now
			if ( timercmp( &imageTime, &(element.timeStamp), < ) )
			{
				timersub( &(element.timeStamp), &imageTime, &imageTime );
				break;
			}
		}
	}

	Mat gray, prevGray;
	LKTracker tracker;
	double pX=0, pY=0;

	int ignoredHeights = 0;
	int telemetryCounter = 0;
	for(;;)
	{
		videoIn.requestImage( frame, tv );

		cvtColor(frame, gray, COLOR_BGR2GRAY);
		if(prevGray.empty())
		{
			gray.copyTo(prevGray);
			// skip first image
			odometry.augmentState( );
			continue;
		}

		//
		// Propagate up to new image ( can be run in parallel with feature detection)
		//
		while( 1 ) {
			ImuMeas_t element;
			// Wait for at least one imu measurement
			while( !imu.fifoPop( element ) );

			// Propagate
			odometry.propagate( element.acc, element.gyro );

			// log to file
			logFile << odometry.x.block<16,1>(0,0).transpose() << "\t";
			logFile << odometry.sigma.diagonal().block<15,1>(0,0).transpose() << "\t";
			logFile << odometry.sigma.determinant() << "\t";
			logFile << odometry.sigma.diagonal().mean() << "\t";
			logFile << ( odometry.sigma - odometry.sigma.transpose() ).sum() << "\n";

			// log over telemetry
			if ( telemetryCounter++ > 40 ) {
				telemetryCounter = 0;
				telemetry.send( odometry.x.data(), sizeof(double)*10 ); // send quaternion, position and velocity
			}

			// If valid distance measurement, update with that
			if ( element.distValid )
			{
				if ( ignoredHeights >= 0 )
				{
					odometry.updateHeight( element.dist );
					ignoredHeights = 0;
				}
				else
				{
					ignoredHeights++;
				}
			}

			// Get time of image without delay
			struct timeval imageTime;
			timersub( &tv, &(calib.imageOffset), &imageTime );

			// If image is older that propagated point, update
			if ( timercmp( &imageTime, &(element.timeStamp), < ) ) {
				timersub( &(element.timeStamp), &imageTime, &imageTime );
				std::cout << "Image/IMU time difference: " <<
				imageTime.tv_sec << "." << std::setfill('0') << std::setw(6) << imageTime.tv_usec << "s" << std::setfill(' ') << std::endl;
				break;
			}
		}

		//
		// Update from camera
		//

		tracker.detectFeatures( gray, prevGray );

		//
		// Debug draw detected features: (TODO: draw where features would have moved)
		//
		for( int i = 0; i < tracker.points.size(); i++ )
		{
			line( frame, tracker.points[i], tracker.prevPoints[i], Scalar(0,255,0) );
			circle( frame, tracker.points[i], 2, Scalar(0,255,0) );
		}

		//
		// undistort points
		//

		// Initialize
		Matrix2Xd points(2, tracker.points.size());
		Matrix2Xd prevPoints(2, tracker.points.size());

		const Calib* calib = odometry.calib;
		VectorXd &x = odometry.x;
		MatrixXd &sigma = odometry.sigma;

		// Undistort
		for ( int i = 0; i < points.cols(); i++ )
		{
			points.col(i) = featureUndistort( Vector2d( tracker.points[i].x, tracker.points[i].y ), calib);
			prevPoints.col(i) = featureUndistort( Vector2d( tracker.prevPoints[i].x, tracker.prevPoints[i].y ), calib);
		}


		//
		// Project on ground
		//
		for ( int i = 0; i < points.cols(); i++ )
		{
			// Calculate camera state
			QuaternionAlias<double> IG_q( x.block<4,1>( 0, 0 ) );
			QuaternionAlias<double> CG_q = calib->CI_q * IG_q;
			Vector3d G_p_I = x.block<3,1>( 4, 0 );
			// force x and y to 0 to get points relative to position
			G_p_I(0) = 0;
			G_p_I(1) = 0;
			Vector3d G_p_C = G_p_I - CG_q.conjugate()._transformVector( calib->C_p_I );

			// Calculate feature position estimate
			Vector3d C_theta_i( points(0,i), points(1,i), 1 );
			Vector3d G_theta_i = CG_q.conjugate()._transformVector( C_theta_i );
			double t_i = - G_p_C( 2 ) / G_theta_i( 2 );
			points.col( i ) = ( t_i * G_theta_i + G_p_C ).block<2,1>(0,0);

			// Calculate previous camera state
			QuaternionAlias<double> IpG_q( x.block<4,1>( ODO_STATE_SIZE + 0, 0 ) );
			QuaternionAlias<double> CpG_q = calib->CI_q * IpG_q;
			Vector3d G_p_Ip = x.block<3,1>( ODO_STATE_SIZE + 4, 0 );
			// force x and y to 0 to get points relative to position
			G_p_Ip(0) = 0;
			G_p_Ip(1) = 0;
			Vector3d G_p_Cp = G_p_Ip - CpG_q.conjugate()._transformVector( calib->C_p_I );

			// Calculate feature position estimate
			Vector3d Cp_theta_i( prevPoints(0,i), prevPoints(1,i), 1 );
			Vector3d Gp_theta_i = CpG_q.conjugate()._transformVector( Cp_theta_i );
			double t_pi = - G_p_Cp( 2 ) / Gp_theta_i( 2 );
			prevPoints.col( i ) = ( t_pi * Gp_theta_i + G_p_Cp ).block<2,1>(0,0);
		}

		//
		// Debug draw of estimated new position
		//
		// Estimate projection of old features in new image
		for ( int i = 0; i < prevPoints.cols(); i++ ) {
			Vector3d G_p_f;
			G_p_f << prevPoints.col(i) + x.block<2,1>( 4+ODO_STATE_SIZE, 0 ), 0;
			const QuaternionAlias<double> &IG_q = x.block<4,1>(0,0);
			const QuaternionAlias<double> &CI_q = calib->CI_q;
			const Vector3d &G_p_I = x.block<3,1>(4,0);
			const Vector3d &C_p_I = calib->C_p_I;
			QuaternionAlias<double> CG_q = (CI_q * IG_q);
			Vector3d C_p_f = CG_q._transformVector( G_p_f - G_p_I + CG_q.conjugate()._transformVector( C_p_I ) );
			Vector2d z = cameraProject( C_p_f(0), C_p_f(1), C_p_f(2), calib );
			circle( frame, Point2f( z(0), z(1) ), 2, Scalar(0,0,255) );
		}

		//
		// Find geometric transform
		//

		// construct constraints
		Matrix<double, Dynamic, 5> C( points.cols()*2, 5 );
		for ( int i = 0; i < points.cols(); i++ )
		{
			const double &x = points(0,i);
			const double &y = points(1,i);
			const double &x_ = prevPoints(0,i);
			const double &y_ = prevPoints(1,i);
			C.row( i*2 )    << -y,  x,  0, -1,  y_;
			C.row( i*2 + 1) <<  x,  y,  1,  0, -x_;
		}


		if ( points.cols() >= 3 ) // only if 3 or more points (2 is needed, 1 extra for redundancy)
		{
			JacobiSVD<MatrixXd> svd( C, ComputeThinV );
			VectorXd V = svd.matrixV().rightCols<1>();
			// find transformation
			VectorXd h = V.head<4>() / V(4);
			// remove scaling
			h.block<2,1>(0,0).normalize();

			// calculate residual

			// Measured rotation
			double dTheta_m = atan2( h(1), h(0) );
			// Calculate estimated rotation
			Vector3d dir(1,0,0); // vector orthogonal to Z axis
			// Calculate quaternion of rotation
			QuaternionAlias<double> IpG_q( x.block<4,1>(0+ODO_STATE_SIZE,0) );
			QuaternionAlias<double> IG_q( x.block<4,1>(0,0) );
			QuaternionAlias<double> IpI_q = IpG_q * IG_q.conjugate();
			// rotate dir by IpI_q
			dir = IpI_q._transformVector( dir );
			double dTheta_e = atan2( dir(1), dir(0) );
			Matrix<double,3,1> r;
			r <<
			dTheta_m - dTheta_e,
			/* this is: Previous (x,y) + measured movement - propageted (x,y) */
			x.block<2,1>(4+ODO_STATE_SIZE,0) + Vector2d( h(2), h(3) ) - x.block<2,1>(4,0);

			// Noise
			Matrix<double,3,3> R;
			R << MatrixXd::Identity(3, 3) * 0.05*0.05; // TODO: make dependant on number of features

			// calculate Measurement jacobian
			Matrix<double,3,Dynamic> H( 3, sigma.cols() );
			H <<
				/*  xy rotation      z rot     p(xyz) v(xyz) bg(xyz) ba(xyz) */
				MatrixXd::Zero( 1, 2 ), 1, MatrixXd::Zero( 1, 12 ),
				/*  xy rotation             z rot                all the rest*/
					MatrixXd::Zero( 1, 2 ), -1, MatrixXd::Zero( 1, sigma.cols() - 18 ),
				/*  xyz rotation        px py  pz v(xyz) bg(xyz) ba(xyz) */
				MatrixXd::Zero( 1, 3 ), 1, 0, MatrixXd::Zero( 1, 1+9 ),
				/*  xyz rotation            px py                   all the rest*/
					MatrixXd::Zero( 1, 3 ), -1, 0, MatrixXd::Zero( 1, sigma.cols() - 20 ),
				MatrixXd::Zero( 1, 3 ), 0, 1, MatrixXd::Zero( 1, 1+9 ),
					MatrixXd::Zero( 1, 3 ), 0, -1, MatrixXd::Zero( 1, sigma.cols() - 20 );

			// TODO: inlier?

			// Calculate kalman gain
			MatrixXd K = sigma * H.transpose() * ( H * sigma * H.transpose() + R ).inverse();

			// apply kalman gain
			// state
			VectorXd delta_x = K * r;
			// covariance
			MatrixXd A = MatrixXd::Identity( K.rows(), H.cols() ) - K * H;
			sigma = A * sigma * A.transpose() + K * R * K.transpose();

			// apply d_x
			odometry.performUpdate( delta_x );

			cout << endl;
			cout << endl;
			cout << "n Points: " << points.cols() << endl;
			cout << "Moved: " << h(2) << ", " << h(3) << endl;
			cout << "Total: " << pX << ", " << pY << endl;
			cout << "State: " << odometry << endl;
		}
		// update state fifo
		odometry.removeOldStates( 1 );
		// Make sure sigma is symetric
		sigma = ( sigma + sigma.transpose() )/2;
		// update state fifo
		odometry.augmentState( );


		//
		// Window managing
		//
		imshow("Features", frame);

		char c = (char)waitKey(1);
		if( c == 27 )
			break;
		switch( c )
		{
		case 'c':
			tracker.prevPoints.clear();
			tracker.points.clear();
			pX = 0;
			pY = 0;
			// Start upside down
			odometry.x.block<4,1>(0,0) << 0, 0, 0, 1; // upright
			// Start 10cm off the ground
			odometry.x.block<3,1>(4,0) << 0, 0, 0.5; // 50cm from ground
			break;
		}

		//
		// Move current image to old one
		//
		cv::swap(prevGray, gray);
	}
	logFile.close();
}
