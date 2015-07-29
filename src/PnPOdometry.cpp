#include <PnPOdometry.h>


PnPOdometry::PnPOdometry()
{
    isFrameAvailable = false;
    isCameraIntrinsicsAvailable = false;

    isNowFrameAvailable = false;
    isRefFrameAvailable = false;

    isRefFeaturesAvailable = false;
    isNowFeaturesAvailable = false;

//    detector.create( "SURF" );
//    extractor.create( "SURF" );




    sub_pyd = nh.subscribe( "/Xtion/rgbdPyramid", 1, &PnPOdometry::imageArrivedCallBack, this );
}

void PnPOdometry::eventLoop()
{
    ROS_INFO( "Start of Event loop" );

    char frameFileName[50];
    const char * folder = "TUM_RGBD/fr1_rpy";


    ros::Rate rate(30);
    int nFrame=0;

    cv::Mat iR, iT;
    iR = cv::Mat::zeros(3,3, CV_32F );
    iT = cv::Mat::zeros(3,1, CV_32F );

    while( ros::ok() )
    {
        ros::spinOnce();

        sprintf( frameFileName, "%s/framemono_%04d.xml", folder, nFrame );
        bool flag = loadFromFile(frameFileName );
        if( flag == false ) {
            ROS_ERROR( "No More files, Quitting..");
            break;
        }

        if( !isFrameAvailable )
            continue;

        ROS_INFO( "***--- FRAME #%d ---***", nFrame );

        //if( nFrame%50 == 0 )
        if( good_matches.size() < 70 )
        {
            ROS_INFO( "Set as reference frame" );
            setAsRefFrame();
            extractRefFeature();

            // reset iR,iT
            iR = cv::Mat::zeros(3,3, CV_32F );
            iT = cv::Mat::zeros(3,1, CV_32F );
        }

        setAsNowFrame();
        extractNowFeature();

        match();
        assert( isMatched );
        ROS_INFO( "Keypoints %4d ::: Total Matches %4d ::: Good Matches %4d", (int)now_pt_features.size(), (int)all_matches.size(), (int)good_matches.size() );


        // 3d-2d PnP. 3d points come from ref-image and 2d points come from now image. the pairs should be from list of `good_matches`
        std::vector<cv::Point2f> reprojectedPts, originalNowImagepts, refPts;
        pnpEstimation( iR, iT, refPts, originalNowImagepts, reprojectedPts );




        //
        // DISPLAY
        //
        {
            cv::Mat tmpnow, tmpref;
            cv::drawKeypoints( now_framemono[0], now_pt_features, tmpnow );
            cv::imshow( "now with keypoints", tmpnow );
            cv::imshow("now", now_framemono[0]);
            cv::imshow("ref", ref_framemono[0]);

            cv::Mat img_matches;
            cv::drawMatches( now_framemono[0], now_pt_features, ref_framemono[0], ref_pt_features,
                         good_matches, img_matches );
            cv::imshow( "matches", img_matches );



            performerDisplay( refPts, originalNowImagepts, reprojectedPts );



            char ch = cv::waitKey(0);
            if( ch == 27 ){ // ESC
                ROS_ERROR( "ESC pressed quitting...");
                exit(1);
            }
        }


        rate.sleep();
        nFrame++;
    }
}

void PnPOdometry::setCameraMatrix(const char *calibFile)
{
    //
    // Read calibration matrix, distortion coiffs
    //
    cv::FileStorage fs(calibFile, cv::FileStorage::READ);
    if( fs.isOpened() == false )
    {
        ROS_ERROR_STREAM( "[PnPOdometry::setCameraMatrix] Error opening camera "
                "params file : "<< calibFile );
        return;
    }

    fs["cameraMatrix"] >> cameraMatrix;
    fs["distCoeffs"] >> distCoeffs;
    //cout<< "Camera Matrix : \n"<<  cameraMatrix << endl;
    //cout<< "Distortion Coifs : \n"<< distCoeffs << endl;
    fx = (float)cameraMatrix.at<double>(0,0);
    fy = (float)cameraMatrix.at<double>(1,1);
    cx = (float)cameraMatrix.at<double>(0,2);
    cy = (float)cameraMatrix.at<double>(1,2);



    ROS_INFO( "[PnPOdometry::setCameraMatrix] Camera Matrix & Distortion Coif Loaded");
    ROS_INFO( "fx=%.4f, fy=%.4f, cx=%.4f, cy=%.4f", fx, fy, cx, cy );


    isCameraIntrinsicsAvailable = true;

}


/// @brief Load RGBD frame-data (stored) from file. Stored in OpenCV XML
/// @param[in] xmlFileName : OpenCV XML file to open
/// @returns false if error loading file. True on success
bool PnPOdometry::loadFromFile(const char *xmlFileName)
{
    cv::FileStorage fs( xmlFileName, cv::FileStorage::READ );
    ROS_INFO_STREAM( "Loading : "<< xmlFileName );
    if( fs.isOpened() == false )
    {
        ROS_ERROR( "Cannot Open File %s", xmlFileName );
        return false;
    }

    isFrameAvailable = false;

    this->rcvd_framemono.clear();
    this->rcvd_dframe.clear();
    for( int i=0 ; i<4 ; i++ )
    {
        cv::Mat framemono, dframe;
        char matmonoName[100], matdepthName[100];
        sprintf( matmonoName, "mono_%d", i );
        fs[matmonoName] >> framemono;

        sprintf( matdepthName, "depth_%d", i );
        fs[matdepthName] >> dframe;


        ROS_DEBUG_STREAM( "loaded `"<< matmonoName << "` : "<< framemono.rows << ", " << framemono.cols );
        ROS_DEBUG_STREAM( "loaded `"<< matdepthName << "` : "<< dframe.rows << ", " << dframe.cols );

        rcvd_framemono.push_back(framemono);
        rcvd_dframe.push_back(dframe);
    }
    isFrameAvailable = true;

    return true;
}




/// @brief Call back when the image (RGBD) datum packed as RGBDFramePyd message arrives
void PnPOdometry::imageArrivedCallBack(rgbd_odometry::RGBDFramePydConstPtr msg)
{
    ROS_INFO_STREAM_ONCE( "1st RGBD frame received. Will continue receiving but not report anymore on this");
    isFrameAvailable=false;


    cv::Mat frame, dframe, framemono;
    rcvd_frame.clear();
    rcvd_framemono.clear();
    rcvd_dframe.clear();

    try
    {
        ROS_INFO_ONCE( "[imageArrivedCallBack] %lu images available", msg->dframe.size() );
        for( int i=0 ; i<msg->dframe.size() ; i++ ) //loop over the image pyramid (typically 4)
        {
            frame = cv_bridge::toCvCopy(msg->framergb[i], "bgr8")->image;
            framemono =  cv_bridge::toCvCopy(  msg->framemono[i], "mono8" )->image ;
            dframe =  cv_bridge::toCvCopy(  msg->dframe[i] )->image ;

            dframe.setTo(1, (dframe==0) ); //to avoid zero depth

            rcvd_frame.push_back(frame);
            rcvd_framemono.push_back(framemono);
            rcvd_dframe.push_back(dframe);

        }
    }
    catch( cv_bridge::Exception& e )
    {
        ROS_ERROR( "cv_bridge exception: %s", e.what() );
        isFrameAvailable = false;
        return;
    }

    isFrameAvailable = true;

}

void PnPOdometry::setAsNowFrame()
{
    isNowFrameAvailable = false;
    isNowFeaturesAvailable = false;
    isMatched = false;
    now_framemono = rcvd_framemono;

    //do not really need depth image of now frame. As I need only image points of now frame;
    isNowFrameAvailable = true;

}

void PnPOdometry::setAsRefFrame()
{
    isRefFrameAvailable = false;
    isRefFeaturesAvailable = false;
    isMatched = false;
    ref_framemono = rcvd_framemono;
    ref_dframe = rcvd_dframe;
    isRefFrameAvailable = true;
}

void PnPOdometry::extractRefFeature()
{
    assert( isRefFrameAvailable );
    ref_pt_features.clear();
    ref_pt_3d.clear();
    // Detect keypoints
    detector.detect( ref_framemono[0], ref_pt_features);

    // Extract descriptors at those points
    extractor.compute( ref_framemono[0], ref_pt_features, ref_pt_descriptors );

    // Compute 3d points at keypoints
    // __ TODO __
    evalRef3dPoints();

    isRefFeaturesAvailable = true;
}

void PnPOdometry::evalRef3dPoints()
{
    assert( isCameraIntrinsicsAvailable && isRefFrameAvailable && isRefFeaturesAvailable );

    cv::Mat _depth = ref_dframe[0];
    for( int i=0 ; i<ref_pt_features.size() ; i++ )
    {
        float u = ref_pt_features[i].pt.x;
        float v = ref_pt_features[i].pt.y;

        float Z = (float) _depth.at<uint16_t>( floor(v), floor(u) );
        float X = Z * (u - cx) / fx;
        float Y = Z * (v - cy) / fy;

        ref_pt_3d.push_back( cv::Point3f(X,Y,Z) );
    }
}

void PnPOdometry::extractNowFeature()
{
    assert( isNowFrameAvailable );
    now_pt_features.clear();
    // Detect keypoints
    detector.detect( now_framemono[0], now_pt_features);

    // Extract descriptors at those points
    extractor.compute( now_framemono[0], now_pt_features, now_pt_descriptors );

    isNowFeaturesAvailable = true;
}

void PnPOdometry::match()
{
    assert( isRefFrameAvailable && isRefFeaturesAvailable && isNowFrameAvailable && isNowFeaturesAvailable );
    all_matches.clear();
    good_matches.clear();


    // This statement could be point of logic problems
    matcher.match( now_pt_descriptors, ref_pt_descriptors, all_matches );

//    std::vector< std::vector<cv::DMatch> > xmatch;
//    matcher.knnMatch( now_pt_descriptors, ref_pt_descriptors, xmatch, 1 );
//    all_matches = xmatch[0];






    float prob = 0.99;
    //while(1) {
        ransacTest( all_matches, now_pt_features, ref_pt_features, good_matches, 3, prob );
      //  if( good_matches.size() > 0 )
      //      break;
      //  prob *= .9;
    //}



        if( good_matches.size() == 0 )// this is just for safety
        {
            ROS_ERROR( "No matches found from F-constrainted filters. Attempting simple distance based thresholding" );
            //
            // Filter matches
            double max_dist = 0; double min_dist = 100;
            //-- Quick calculation of max and min distances between keypoints
            for( int i = 0; i < all_matches.size(); i++ )
            {
                double dist = all_matches[i].distance;
                if( dist < min_dist ) min_dist = dist;
                if( dist > max_dist ) max_dist = dist;
            }


            //-- Actually filter
            for( int i = 0; i < all_matches.size(); i++ )
            { if( all_matches[i].distance <= std::max(2.5*min_dist, 0.02) )  // THRESHOLD ON DISTANCE
                { good_matches.push_back( all_matches[i]); }
            }
        }



    isMatched = true;
}




void PnPOdometry::ransacTest(const std::vector<cv::DMatch> matches,const std::vector<cv::KeyPoint>&keypoints1,const std::vector<cv::KeyPoint>& keypoints2,
                std::vector<cv::DMatch>& goodMatches,double distance,double confidence )
{
    goodMatches.clear();
    // Convert keypoints into Point2f
    std::vector<cv::Point2f> points1, points2;
    for (std::vector<cv::DMatch>::const_iterator it= matches.begin();it!= matches.end(); ++it)
    {
        // Get the position of left keypoints
        float x= keypoints1[it->queryIdx].pt.x;
        float y= keypoints1[it->queryIdx].pt.y;
        points1.push_back(cv::Point2f(x,y));
        // Get the position of right keypoints
        x= keypoints2[it->trainIdx].pt.x;
        y= keypoints2[it->trainIdx].pt.y;
        points2.push_back(cv::Point2f(x,y));
    }
    // Compute F matrix using RANSAC
    std::vector<uchar> inliers(points1.size(),0);
    cv::Mat fundemental= cv::findFundamentalMat(cv::Mat(points1),cv::Mat(points2),inliers,CV_FM_RANSAC,distance,confidence); // confidence probability
    // extract the surviving (inliers) matches
    std::vector<uchar>::const_iterator
    itIn= inliers.begin();
    std::vector<cv::DMatch>::const_iterator
    itM= matches.begin();
    // for all matches
    for ( ;itIn!= inliers.end(); ++itIn, ++itM)
    {
        if (*itIn)
        { // it is a valid match
            goodMatches.push_back(*itM);
        }
    }
}

void PnPOdometry::pnpEstimation(cv::Mat &iR, cv::Mat &iT, std::vector<cv::Point2f> & ptsInRefIm, std::vector<cv::Point2f> & imagePts, std::vector<cv::Point2f> &reprojectedPts)
{
    std::vector<cv::Point3f> ref3dPts;

    imagePts.clear();
    ref3dPts.clear();
    reprojectedPts.clear();
    ptsInRefIm.clear();

    assert( isCameraIntrinsicsAvailable );
    assert( isNowFrameAvailable && isNowFeaturesAvailable );
    assert( isRefFrameAvailable && isRefFeaturesAvailable );
    assert( isMatched );



    //loop thru good_matches and get image pts (from now frame)
    for( int i=0 ; i<good_matches.size() ; i++ )
    {
        //ROS_INFO( "#%d pt of now frame <----> #%d pt of ref frame", good_matches[i].queryIdx, good_matches[i].trainIdx );
        int q = good_matches[i].queryIdx;
        int t = good_matches[i].trainIdx;

        imagePts.push_back( now_pt_features[q].pt );
        ref3dPts.push_back( ref_pt_3d[t] );
        ptsInRefIm.push_back( ref_pt_features[t].pt );
    }


    // pnp
    cv::Mat rVec;
    cv::Rodrigues( iR, rVec );
    ROS_INFO_STREAM( "in rvec : "<< rVec.t() );
    ROS_INFO_STREAM( "in tvec : "<< iT.t() );
    cv::solvePnPRansac( ref3dPts, imagePts, cameraMatrix, distCoeffs, rVec, iT, true );
    ROS_INFO_STREAM( "out rvec : "<< rVec.t() );
    ROS_INFO_STREAM( "out tvec : "<< iT.t() );
    cv::Rodrigues( rVec, iR );


    // reprojected points
    cv::projectPoints( ref3dPts, rVec, iT, cameraMatrix, distCoeffs, reprojectedPts );
    /*
    for( int i=0 ; i<ref3dPts.size() ; i++ )
    {
        cv::Mat X_r = cv::Mat::zeros(3, 1, CV_32F ); //3d pt in ref frame
        X_r.at<float>(0) = ref3dPts[i].x;
        X_r.at<float>(1) = ref3dPts[i].y;
        X_r.at<float>(2) = ref3dPts[i].z;
        cv::Mat X_n = iR * X_r + iT;


    }
    */

}

void PnPOdometry::performerDisplay(std::vector<cv::Point2f> &refPts, std::vector<cv::Point2f> &originalNowImagepts, std::vector<cv::Point2f> &reprojectedPts)
{
    cv::Mat _ref;
    cv::cvtColor( ref_framemono[0], _ref, CV_GRAY2BGR );
    for( int i=0 ; i<refPts.size() ; i++ )
    {
        cv::circle( _ref, refPts[i], 3, cv::Scalar(0,0,255), -1 );
    }
    cv::imshow( "ref with its own pts", _ref );


    cv::Mat _now;
    cv::cvtColor( now_framemono[0], _now, CV_GRAY2BGR );
    for( int i=0 ; i<originalNowImagepts.size() ; i++ )
    {
        cv::circle( _now, originalNowImagepts[i], 3, cv::Scalar(0,255,0), -1 );
        cv::circle( _now, reprojectedPts[i], 3, cv::Scalar(0,0,255), -1 );
    }
    cv::imshow( "now with its own pts (matched)", _now );

}


