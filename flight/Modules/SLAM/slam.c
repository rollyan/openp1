/**
 ******************************************************************************
 *
 * @file       slam.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      Example module to be used as a template for actual modules.
 *             Threaded periodic version.
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * Input object: ExampleSettings
 * Output object: ExampleObject2
 *
 * This module will periodically update the value of the ExampleObject object.
 * The module settings can configure how the ExampleObject is manipulated.
 *
 * The module executes in its own thread in this example.
 *
 * UAVObjects are automatically generated by the UAVObjectGenerator from
 * the object definition XML file.
 *
 * Modules have no API, all communication to other modules is done through UAVObjects.
 * However modules may use the API exposed by shared libraries.
 * See the OpenPilot wiki for more details.
 * http://www.openpilot.org/OpenPilot_Application_Architecture
 *
 */


#include "opencv/cv.h"		// OpenCV library
#include "opencv/highgui.h"	// HighGUI offers video IO and debug output to screen when run on a PC

#include "backgroundio.h"	// video IO runs in background even if FreeRTOS gives control to another task
#include "opencvslam.h"		// bridge to C++ part of module

#include "openpilot.h"
#include "slamsettings.h"	// object holding module settings
#include "hwsettings.h"		// object holding module system configuration
#include "attitudeactual.h"	// orientation in space
#include "velocityactual.h"	// 3d velocity
#include "positionactual.h"	// 3d position

// Private constants
#define STACK_SIZE 16386 // doesn't really mater as long as big enough
#define TASK_PRIORITY (tskIDLE_PRIORITY+1)
#define DEG2RAD (3.1415926535897932/180.0)

// Private variables
static xTaskHandle taskHandle;
static bool slamEnabled;
static SLAMSettingsData settings;

// Private functions
static void slamTask(void *parameters);
static void SettingsUpdatedCb(UAVObjEvent * ev);

/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t SLAMStart()
{
	if (slamEnabled) {
		// register callback and update settings
		SLAMSettingsConnectCallback(SettingsUpdatedCb);
		SettingsUpdatedCb(SLAMSettingsHandle());

		// Start main task
		xTaskCreate(slamTask, (signed char *)"SLAM", STACK_SIZE, NULL, TASK_PRIORITY, &taskHandle);
	}

	return 0;
}
/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t SLAMInitialize()
{

	HwSettingsInitialize();
	uint8_t optionalModules[HWSETTINGS_OPTIONALMODULES_NUMELEM];

	HwSettingsOptionalModulesGet(optionalModules);

	if (optionalModules[HWSETTINGS_OPTIONALMODULES_SLAM] == HWSETTINGS_OPTIONALMODULES_ENABLED)
		slamEnabled = true;
	else
		slamEnabled = false;

	return 0;
}
MODULE_INITCALL(SLAMInitialize, SLAMStart)


/**
 * Module thread, should not return.
 */
static void slamTask(void *parameters)
{
	AttitudeActualData attitudeActual;
	PositionActualData positionActual;
	VelocityActualData velocityActual;
	IplImage *currentFrame = NULL, *lastFrame = NULL;
	uint32_t frame = 0;

	/* Initialize OpenCV */
	//CvCapture *VideoSource = NULL; //cvCaptureFromFile("test.avi");
	CvCapture *VideoSource = cvCaptureFromFile("test.avi");
	//CvCapture *VideoSource = cvCaptureFromCAM(0);


	if (VideoSource) {
		cvGrabFrame(VideoSource);
		currentFrame = cvRetrieveFrame(VideoSource, 0);
		cvSetCaptureProperty(VideoSource, CV_CAP_PROP_FRAME_WIDTH,  settings.FrameDimensions[SLAMSETTINGS_FRAMEDIMENSIONS_X]);
		cvSetCaptureProperty(VideoSource, CV_CAP_PROP_FRAME_HEIGHT, settings.FrameDimensions[SLAMSETTINGS_FRAMEDIMENSIONS_Y]);
	}

	if (currentFrame) lastFrame = cvCloneImage(currentFrame);

	// debug output
	cvNamedWindow("debug",CV_WINDOW_AUTOSIZE);

	uint32_t timeval = PIOS_DELAY_GetRaw();
	portTickType currentTime,startTime = xTaskGetTickCount();
	portTickType increment = ((float)(1000./settings.FrameRate)) / portTICK_RATE_MS;
	fprintf(stderr,"init at %i increment is %i\n",timeval, increment);

	// synchronization delay, wait for attitude data - any attitude data
	// this is an evil hack but necessary for tests with log data to synchronize video and telemetry
	AttitudeActualGet(&attitudeActual);
	attitudeActual.Pitch=100;
	AttitudeActualSet(&attitudeActual);
	while (attitudeActual.Pitch==100) AttitudeActualGet(&attitudeActual);

	// Main task loop
	while (1) {
		frame++;
		cvWaitKey(1);
		currentTime = startTime;
		vTaskDelayUntil(&currentTime,startTime+(frame*increment));

		float dT = PIOS_DELAY_DiffuS(timeval) * 1.0e-6f;
		timeval = PIOS_DELAY_GetRaw();

		// Grab the current camera image
		if (VideoSource) {
			// frame grabbing must take place outside of FreeRTOS scheduler,
			// since OpenCV's hardware IO does not like being interrupted.
			backgroundGrabFrame(VideoSource);
		}
		
		// Get the object data
		AttitudeActualGet(&attitudeActual);
		PositionActualGet(&positionActual);
		VelocityActualGet(&velocityActual);

		if (VideoSource) currentFrame = cvRetrieveFrame(VideoSource, 0);

		struct opencvslam_input i = {.x=0};
		struct opencvslam_output *o = opencvslam_run(i);

		if (lastFrame) {
			cvReleaseImage(&lastFrame);
		}
		if (currentFrame) {
			lastFrame = cvCloneImage(currentFrame);


			// draw a line in the video coresponding to artificial horizon (roll+pitch)
			CvPoint center = cvPoint(
				  settings.FrameDimensions[SLAMSETTINGS_FRAMEDIMENSIONS_X]/2
				,
				  settings.FrameDimensions[SLAMSETTINGS_FRAMEDIMENSIONS_Y]/2
				  + attitudeActual.Pitch * settings.FrameDimensions[SLAMSETTINGS_FRAMEDIMENSIONS_Y]/60.
				);
			// i want overloaded operands damnit!
			CvPoint right = cvPoint(
				  fmin(
					settings.FrameDimensions[SLAMSETTINGS_FRAMEDIMENSIONS_X],
					settings.FrameDimensions[SLAMSETTINGS_FRAMEDIMENSIONS_Y]
				  )*cos(DEG2RAD*attitudeActual.Roll)/3
				,
				  -fmin(
					settings.FrameDimensions[SLAMSETTINGS_FRAMEDIMENSIONS_X],
					settings.FrameDimensions[SLAMSETTINGS_FRAMEDIMENSIONS_Y]
				  )*sin(DEG2RAD*attitudeActual.Roll)/3
				);
			CvPoint left = cvPoint(center.x-right.x,center.y-right.y);
			right.x += center.x;
			right.y += center.y;
			cvLine(lastFrame,left,right,CV_RGB(255,255,0),3,8,0);

			cvShowImage("debug",lastFrame);
		}


		fprintf(stderr,"frame %i at %i\n",frame,currentTime);
	}
}




static void SettingsUpdatedCb(UAVObjEvent * ev)
{
	// update global variable according to changed settings
	SLAMSettingsGet(&settings);
}
