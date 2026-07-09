#pragma once
#include <iostream>
#include <string>
#include <fstream>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <skse64/NiProperties.h>
#include <skse64/NiNodes.h>

#include "skse64\GameSettings.h"
#include "Utility.hpp"

#include <skse64/GameData.h>

#include "higgsinterface001.h"
#include "vrikinterface001.h"
#include "SkyrimVRESLAPI.h"

namespace NeckSnapVR {

	const UInt32 MOD_VERSION = 0x10000;
	const std::string MOD_VERSION_STR = "1.0.0";
	extern int leftHandedMode;

	extern int logging;

	// Step 1: behind detection
	extern float fMinBehindDot;
	extern float fReachMargin;
	extern float fDefaultReach;
	extern float fMaxHeightDiff;
	extern float fLatchResetDistance;
	extern float fHeadGrabProximity;

	// Step 3: controller snap motion
	extern float fMinHandSpeed;
	extern float fMinPeakAxisStep;
	extern float fMinHandSpeedRatio;
	extern float fSoftHandSpeed;
	extern float fOppositeCosThreshold;
	extern float fMaxFrameDelta;
	extern int iOppositeMotionFrames;

	// Step 3: head twist snap
	extern float fMinHeadTwistDegrees;
	extern float fMaxHeadTwistDegrees;

	// Grab session / debug
	extern int iDualGrabReleaseGraceFrames;
	extern int iMotionDebugLogIntervalFrames;
	extern int iAllowEssentialVictims;

	void loadConfig();
	
	void Log(const int msgLogLevel, const char* fmt, ...);
	enum eLogLevels
	{
		LOGLEVEL_ERR = 0,
		LOGLEVEL_WARN,
		LOGLEVEL_INFO,
	};


#define LOG(fmt, ...) Log(LOGLEVEL_WARN, fmt, ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) Log(LOGLEVEL_ERR, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) Log(LOGLEVEL_INFO, fmt, ##__VA_ARGS__)


}