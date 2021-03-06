/*
 * HeadOSC, part of the Quick N Dirty Tracking system
 *
 * Copyright (c) 2014 Dan Wilcox <danomatika@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * See https://github.com/danomatika/QDTracker for documentation
 *
 * Largely adapted from the Kinect Titty Tracker:
 * http://danomatika.com/projects/kinect-titty-tracker
 *
 */
#include "ofApp.h"

#include "XmlSettings.h"

//--------------------------------------------------------------
void ofApp::setup(){

	ofSetVerticalSync(true);
	
	// settings
	resetSettings();
	loadSettings();

	cropW = kinect.width - cropLeft - cropRight;
	cropH = kinect.height - cropTop - cropBottom;
	
	// setup kinect
	kinect.init(false); // no IR image
	kinect.setRegistration(true);
	kinect.setDepthClipping(nearClipping, farClipping);
	kinect.open(kinectID);

	if(kinect.isConnected()) {
		ofLogNotice() << "sensor-emitter dist: " << kinect.getSensorEmitterDistance() << "cm";
		ofLogNotice() << "sensor-camera dist:  " << kinect.getSensorCameraDistance() << "cm";
		ofLogNotice() << "zero plane pixel size: " << kinect.getZeroPlanePixelSize() << "mm";
		ofLogNotice() << "zero plane dist: " << kinect.getZeroPlaneDistance() << "mm";
	}

	// setup cv
	// depthImage.allocate(kinect.width, kinect.height);
	depthImageCropped.allocate(kinect.width, kinect.height);
	depthImageCropped.setROI(cropLeft,cropTop,cropW, cropH);
	depthDiff.allocate(cropW, cropH);
	nullBg.allocate(cropW, cropH);

	// zero the tilt on startup
	angle = 0;
	kinect.setCameraTiltAngle(angle);
}

//--------------------------------------------------------------
void ofApp::update(){
	ofBackground(0, 0, 0);

	kinect.update();
	if(kinect.isFrameNew()) { // dont bother if the frames aren't new
	
		// find person-sized blobs
		// depthImage.setFromPixels(kinect.getDepthPixels(), kinect.width, kinect.height);
		depthImageCropped.setFromPixels(kinect.getDepthPixels(),kinect.width, kinect.height);


		depthDiff = depthImageCropped;

		// bg subtraction
		if (nullBgDefined && doBgSubtraction) {
			cv::Mat cvimg = depthDiff.getCvImage();
			cv::Mat bgimg = nullBg.getCvImage();
			cv::subtract(cvimg, bgimg, cvimg);
			*depthDiff.getCvImage() = cvimg;
		}

		if (captureNullBg) {
			if (nullBgFrames==7) {
				// first capture, let's take the full image
				nullBg = depthImageCropped;

			} else {
				// now lets add weighted images, to smooth it over time
				cv::Mat cvimg = nullBg.getCvImage();
				cv::Mat depthimg = depthImageCropped.getCvImage();
				cv::addWeighted(cvimg, .7, depthimg, 0.3, 0., cvimg);
				*nullBg.getCvImage() = cvimg;
			}
			nullBgFrames--;
			cout << "capture nullBg " << nullBgFrames << endl;
			if (nullBgFrames<=0) {
				// cvConvertScale( grayImg, shortImg, 65535.0f/255.0f, 0 );
				nullBgFrames = 0;
				captureNullBg = false;
				nullBgDefined = true;
			}
		}
		depthDiff.threshold(threshold);
		depthDiff.updateTexture();
		personFinder.findContours(depthDiff, personMinArea, personMaxArea, 1, false);
		
		// found person-sized blob?
		if(personFinder.blobs.size() > 0) {
			ofxCvBlob &blob = personFinder.blobs[0];
			person.position = blob.centroid;
			person.width = blob.boundingRect.width;
			person.height = blob.boundingRect.height;
			
			// find highest contour point (actually the lowest value since top is 0)
			int height = INT_MAX;
			for(int i = 0; i < blob.pts.size(); ++i) {
				ofPoint &p = blob.pts[i];
				if(p.y < height &&
				  (p.x > person.x-highestPointThreshold && p.x < person.x+highestPointThreshold)) {
					highestPoint = blob.pts[i];
					height = blob.pts[i].y;
				}
			}
			
			// compute rough head position between centroid and highest point
			// head = person.position.getInterpolated(highestPoint, headInterpolation);
			ofPoint tmpHead = person.position.getInterpolated(highestPoint, headInterpolation);

			head.x = ofLerp(tmpHead.x, head.x, smoothHead);
			head.y = ofLerp(tmpHead.y, head.y, smoothHead);

			// filter out noise, 0 values
			float headDepthValue = kinect.getDistanceAt(tmpHead);
			if (headDepthValue > 1) {
				head.z = ofLerp(headDepthValue, head.z, smoothHead);
			}

			// int dif = head.x - highestPoint.x;
			// head.z = kinect.getDistanceAt(head);
			headAdj = head;

			if (realWorldValues) {

				// Trigonometry to get real-world X and Y, based on Z
				// now X and Y will be positive/negative around center of image
				headAdj.x = -(head.x-320)/320.0 * sin((PI/180.0)*fovH/2.0)*head.z;
				headAdj.y = -(head.y-240)/240.0 * sin((PI/180.0)*fovV/2.0)*head.z;
			} else {

				// normalize values
				if(bNormalizeX) headAdj.x = ofMap(head.x, 0, kinect.width, 0, 1);
				if(bNormalizeY) headAdj.y = ofMap(head.y, 0, kinect.height, 0, 1);
				if(bNormalizeZ) headAdj.z = ofMap(head.z, kinect.getNearClipping(), kinect.getFarClipping(), 0, 1);
				
				// scale values
				if(bScaleX) headAdj.x *= scaleXAmt;
				if(bScaleY) headAdj.y *= scaleYAmt;
				if(bScaleZ) headAdj.z *= scaleZAmt;
				
			}
			
			// add constant to get proper dimension
			headAdj.y += distanceFloor;
			
			// send head position
			ofxOscMessage message;
			message.setAddress("/head");
			message.addFloatArg(headAdj.x);
			message.addFloatArg(headAdj.y);
			message.addFloatArg(headAdj.z);
			sender.sendMessage(message);
			localSender.sendMessage(message);
			sender2.sendMessage(message);

		}
	}
}

//--------------------------------------------------------------
void ofApp::draw(){

	ofPushMatrix();
	if (ofGetHeight()>480) {
		float zoom = ofGetHeight()/480.0;
		ofTranslate( (ofGetWidth()-640*zoom)/2, 0 , 0);
		ofScale(zoom,zoom,zoom);
	}

	// draw display image
	ofSetColor(255);
	switch(displayImage) {
		case THRESHOLD:
			depthDiff.draw(cropLeft,cropTop);
			break;
		case RGB:
			kinect.draw(0, 0);
			break;
		case DEPTH:
			kinect.drawDepth(0, 0);
			break;
		case NULLBG:
			nullBg.draw(cropLeft,cropTop);
		default: // NONE
			break;
	}

	// draw crop area
	ofNoFill();
	ofSetColor(200,200,0);
	ofRect(cropLeft,cropTop,cropW,cropH);

	if(personFinder.blobs.size() > 0) {

		// draw person finder
		ofSetLineWidth(2.0);
		personFinder.draw(cropLeft,cropTop,cropW, cropH);
	
		// purple - found person centroid
		ofFill();
		ofSetColor(255, 0, 255);
		ofRect(cropLeft+person.position.x, cropTop+person.position.y, 10, 10);
		
		// gold - highest point
		ofFill();
		ofSetColor(255, 255, 0);
		ofRect(cropLeft+highestPoint.x, cropTop+highestPoint.y, 10, 10);
		
		// light blue - "head" position
		ofFill();
		ofSetColor(0, 255, 255);
		ofRect(cropLeft+head.x, cropTop+head.y, 10, 10);
		
	}
	

	ofSetColor(255);
	stringstream infoStream;
	infoStream << "KINECT" << endl;
	infoStream << "\tthreshold (-/=)\t\t" <<  ofToString(threshold) << endl;
	infoStream << "\tnearClipping \t\t" << ofToString(nearClipping) << endl;
	infoStream << "\tfarClipping \t\t" << ofToString(farClipping) << endl;
	infoStream << "\tdoBgSubtraction (b)  \t";
	if (doBgSubtraction) infoStream << "yes" << endl;
	else infoStream << "no" << endl;
	infoStream << "\tcapture nullBg  \t(n)" << endl;

	infoStream << endl << "DETECTION" << endl;
	infoStream << "\tpersonMinArea \t\t" << ofToString(personMinArea) << endl;
	infoStream << "\tpersonMaxArea \t\t" << ofToString(personMaxArea) << endl;
	infoStream << "\thighestPointThreshold \t" << ofToString(highestPointThreshold) << endl;
	infoStream << "\theadInterpolation \t" << ofToString(headInterpolation) << endl;
	infoStream << "\tsmoothHead (1/2)\t" << ofToString(smoothHead) << endl;
	infoStream << "\trealWorldValues (w)  \t";
	if (realWorldValues) infoStream << "yes" << endl;
	else infoStream << "no" << endl;
	infoStream << "\thead position \tx\t" << ofToString(headAdj.x, 2) << "\t" << ofToString(head.x,2) << endl;
	infoStream << "\t\t\ty\t" << ofToString(headAdj.y, 2) << "\t" << ofToString(head.y,2) << endl;
	infoStream << "\t\t\tz\t" << ofToString(headAdj.z, 2) << "\t" << ofToString(head.z,2) << endl;
	
	infoStream << endl << "DISPLAY" << endl;
	infoStream << "\tdisplay image (d)\t";
	switch (displayImage) {
		case 0: infoStream << "NONE" << endl;
		break;
		case 1: infoStream << "THRESHOLD" << endl;
		break;
		case 2: infoStream << "RGB" << endl;
		break;
		case 3: infoStream << "DEPTH" << endl;
		break;
		case 4: infoStream << "NULLBG" << endl;
		break;
	}
	infoStream << "\tfullscreen \t \t(f) " << endl;


	infoStream << endl << "OSC" << endl;
	infoStream << "\tsendAddress \t \t" << sendAddress << endl;
	infoStream << "\tport \t \t\t" << sendPort << endl;
	

	ofDrawBitmapString(infoStream.str(), 20, 100);


	ofPopMatrix();

	if (drawCenterLine) {
		ofNoFill();
		ofSetColor(255,0,0);
		ofLine(ofGetWidth()/2,0,ofGetWidth()/2,ofGetHeight());
	}
}

//--------------------------------------------------------------
void ofApp::exit(){
	kinect.close();
}

//--------------------------------------------------------------
void ofApp::keyPressed(int key){
	switch(key) {
		
		case '-':
			threshold--;
			if(threshold < 0) threshold = 0;
			// personMinArea-=100;
			// cout << "personMinArea: " << personMinArea << endl;
			cout << "threshold: " << threshold << endl;
			break;
			
		case '=':
			threshold++;
			if(threshold > 255) threshold = 255;
			// personMinArea+=100;
			// cout << "personMinArea: " << personMinArea << endl;
			cout << "threshold: " << threshold << endl;
			break;		

		case '1':
			smoothHead-=0.01;
			if(smoothHead < 0) smoothHead = 0;
			cout << "smoothHead: " << smoothHead << endl;
			break;
			
		case '2':
			smoothHead+=.01;
			if(smoothHead > 1) smoothHead = 1;
			cout << "smoothHead: " << smoothHead << endl;
			break;
			
		case 'x':
			bNormalizeX = !bNormalizeX;
			break;
			
		case 'y':
			bNormalizeY = !bNormalizeY;
			break;
			
		case 'z':
			bNormalizeZ = !bNormalizeZ;
			break;
			
		case 'd': {
			// increment enum
			int d = (int)displayImage;
			d++;
			if(d > NULLBG) {
				d = (int)NONE;
			}
			displayImage = (DisplayImage)d;
			break;
		}

		case 'b': {
			doBgSubtraction = !doBgSubtraction;
			break;
		}

		case 'n': {
			// capture empty scene for background subtraction
			captureNullBg = true;
			nullBgFrames = 7;
			break;
		}
			
		case 's':
			saveSettings();
			break;
			
		case 'l':
			loadSettings();
			break;
			
		case 'R':
			resetSettings();
			break;

		case 'c':
			drawCenterLine = !drawCenterLine;
			break;

		case OF_KEY_UP:
			angle++;
			if(angle>30) angle=30;
			kinect.setCameraTiltAngle(angle);
			break;
			
		case OF_KEY_DOWN:
			angle--;
			if(angle<-30) angle=-30;
			kinect.setCameraTiltAngle(angle);
			break;

		case 'f':
			doFullScreen = !doFullScreen;
			ofSetFullscreen(doFullScreen);
			break; 

		case 'w':
			realWorldValues = !realWorldValues;
			break;
	}
}

//--------------------------------------------------------------
void ofApp::resetSettings() {

	doFullScreen = false;
	realWorldValues = true;
	drawCenterLine = true;

	distanceFloor = 1384;	// 0-point kinect distance from floor in mm
	fovH = 78;
	fovV = 42;

	doBgSubtraction = true;
	captureNullBg = false;
	nullBgFrames = 0;
	nullBgDefined = false;

	cropKinectImage = true;
	cropTop = 0;
	cropBottom = 0;
	cropLeft = 0;
	cropRight = 0;

	cropW = 640;
	cropH = 480;
	
	threshold = 160;
	nearClipping = 500;
	farClipping = 4000;
	personMinArea = 3000;
	personMaxArea = 640*480*0.5;
	highestPointThreshold = 50;
	headInterpolation = 0.6;
	
	bNormalizeX = false;
	bNormalizeY = false;
	bNormalizeZ = false;

	smoothHead = 0.7;
	
	bScaleX = false;
	bScaleY = false;
	bScaleZ = false;
	
	scaleXAmt = 1.0;
	scaleYAmt = 1.0;
	scaleZAmt = 1.0;
	
	displayImage = THRESHOLD;
	kinectID = 0;
	
	sendAddress = "127.0.0.1";
	// sendAddress = "localhost";
	sendPort = 9000;

	// setup osc
	cout << "setup OSC server " << endl;
	sender.setup(sendAddress, sendPort);
	sender2.setup("100.100.10.9", 8001);
	localSender.setup("100.100.10.11", 8001);
}

//--------------------------------------------------------------
bool ofApp::loadSettings(const string xmlFile) {
	ofxXmlSettings xml;
	if(!xml.loadFile(xmlFile)) {
		ofLogWarning() << "Couldn't load settings: "
			<< xml.doc.ErrorRow() << ", " << xml.doc.ErrorCol()
			<< " " << xml.doc.ErrorDesc();
			return false;
	}
	
	xml.pushTag("settings");
		
		displayImage = (DisplayImage)xml.getValue("displayImage", displayImage);
		kinectID = xml.getValue("kinectID", (int)kinectID);
		
		xml.pushTag("cropKinectImage");
			cropTop = xml.getValue("top", cropTop);
			cropBottom = xml.getValue("bottom", cropBottom);
			cropLeft = xml.getValue("left", cropLeft);
			cropRight = xml.getValue("right", cropRight);
		xml.popTag();
		
		xml.pushTag("trigonometry");
			fovH = xml.getValue("fovH", fovH);
			fovV = xml.getValue("fovV", fovV);
			distanceFloor = xml.getValue("distanceFloor", distanceFloor);
		xml.popTag();

		
		xml.pushTag("tracking");
			threshold = xml.getValue("threshold", threshold);
			nearClipping = xml.getValue("nearClipping", (int)nearClipping);
			farClipping = xml.getValue("farClipping", (int)farClipping);
			personMinArea = xml.getValue("personMinArea", (int)personMinArea);
			personMaxArea = xml.getValue("personMaxArea", (int)personMaxArea);
			highestPointThreshold = xml.getValue("highestPointThreshold", (int)highestPointThreshold);
			headInterpolation = xml.getValue("headInterpolation", headInterpolation);
		xml.popTag();
		
		xml.pushTag("normalize");
			bNormalizeX = xml.getValue("bNormalizeX", bNormalizeX);
			bNormalizeY = xml.getValue("bNormalizeY", bNormalizeY);
			bNormalizeZ = xml.getValue("bNormalizeZ", bNormalizeZ);
		xml.popTag();
		
		xml.pushTag("scale");
			bScaleX = xml.getValue("bScaleX", bScaleX);
			bScaleY = xml.getValue("bScaleY", bScaleY);
			bScaleZ = xml.getValue("bScaleZ", bScaleZ);
			scaleXAmt = xml.getValue("scaleXAmt", scaleXAmt);
			scaleYAmt = xml.getValue("scaleYAmt", scaleYAmt);
			scaleZAmt = xml.getValue("scaleZAmt", scaleZAmt);
		xml.popTag();
		
		xml.pushTag("osc");
			sendAddress = xml.getValue("sendAddress", sendAddress);
			sendPort = xml.getValue("sendPort", (int)sendPort);
		xml.popTag();
		
	xml.popTag();
	
	// setup kinect
	kinect.setDepthClipping(nearClipping, farClipping);
	
	// setup osc
	sender.setup(sendAddress, sendPort);
	
	return true;
}

//--------------------------------------------------------------
bool ofApp::saveSettings(const string xmlFile) {
	
	XmlSettings xml;
	
	xml.addTag("settings");
	xml.pushTag("settings");
		
		xml.addComment(" general settings ");
		xml.addComment(" which kinect ID to open (note: doesn't change when reloading); int ");
		xml.addValue("kinectID", (int)kinectID);
		xml.addComment(" display image: 0 - none, 1 - threshold, 2 - RGB, 3 - depth");
		xml.addValue("displayImage", displayImage);
		
		xml.addComment(" crop settings ");
		xml.addTag("cropKinectImage");
		xml.pushTag("cropKinectImage");
			xml.addValue("top", cropTop);
			xml.addValue("bottom", cropBottom);
			xml.addValue("left", cropLeft);
			xml.addValue("right", cropRight);
		xml.popTag();		

		xml.addComment(" values to make sure the dimensions are correct ");
		xml.addTag("trigonometry");
		xml.pushTag("trigonometry");
			xml.addValue("fovH", fovH);
			xml.addValue("fovV", fovV);
			xml.addValue("distanceFloor", distanceFloor);
		xml.popTag();
		
				
		xml.addComment(" tracking settings ");
		xml.addTag("tracking");
		xml.pushTag("tracking");
			xml.addComment(" person finder depth clipping threshold; int 0 - 255 ");
			xml.addValue("threshold", threshold);
			xml.addComment(" kinect near clipping plane in cm; int ");
			xml.addValue("nearClipping", (int)nearClipping);
			xml.addComment(" kinect far clipping plane in cm; int ");
			xml.addValue("farClipping", (int)farClipping);
			xml.addComment(" minimum area to consider when looking for person blobs; int ");
			xml.addValue("personMinArea", (int)personMinArea);
			xml.addComment(" maximum area to consider when looking for person blobs; int ");
			xml.addValue("personMaxArea", (int)personMaxArea);
			xml.addComment(" only consider highest points +- this & the person centroid; int ");
			xml.addValue("highestPointThreshold", (int) highestPointThreshold);
			xml.addComment(" percentage to interpolate between person centroid & highest point; float 0 - 1" );
			xml.addValue("headInterpolation", headInterpolation);
		xml.popTag();

		
		xml.addComment(" normalize head position coords, enable/disable; bool 0 or 1 ");
		xml.addTag("normalize");
		xml.pushTag("normalize");
			xml.addComment(" normalized; 0 - width ");
			xml.addValue("bNormalizeX", bNormalizeX);
			xml.addComment(" normalized; 0 - height ");
			xml.addValue("bNormalizeY", bNormalizeY);
			xml.addComment(" normalized ; nearCLipping - farClipping ");
			xml.addValue("bNormalizeZ", bNormalizeZ);
		xml.popTag();
		
		xml.addComment(" scale head position coords, performed after normalization ");
		xml.addTag("scale");
		xml.pushTag("scale");
			xml.addComment(" enable/disable; bool 0 or 1 ");
			xml.addValue("bScaleX", bScaleX);
			xml.addValue("bScaleY", bScaleY);
			xml.addValue("bScaleZ", bScaleZ);
			xml.addComment(" scale amounts ");
			xml.addValue("scaleXAmt", scaleXAmt);
			xml.addValue("scaleYAmt", scaleXAmt);
			xml.addValue("scaleZAmt", scaleZAmt);
		xml.popTag();
		
		xml.addComment(" osc settings ");
		xml.addTag("osc");
		xml.pushTag("osc");
			xml.addComment(" host destination address ");
			xml.addValue("sendAddress", sendAddress);
			xml.addComment(" host destination port ");
			xml.addValue("sendPort", (int)sendPort);
		xml.popTag();
		
	xml.popTag();
	
	if(!xml.saveFile(xmlFile)) {
		ofLogWarning() << "Couldn't save settings: " << xml.doc.ErrorDesc();
			return false;
	}
	return true;
}
