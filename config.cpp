#include "config.h"

namespace NeckSnapVR {

	int logging = 0;
	int leftHandedMode = 0;

	float fMinBehindDot = -0.7f;
	float fReachMargin = 25.0f;
	float fDefaultReach = 64.0f;
	float fMaxHeightDiff = 60.0f;
	float fLatchResetDistance = 250.0f;
	float fHeadGrabProximity = 45.0f;

	float fMinHandSpeed = 1.5f;
	float fMinPeakAxisStep = 2.0f;
	float fMinHandSpeedRatio = 0.35f;
	float fSoftHandSpeed = 1.0f;
	float fOppositeCosThreshold = -0.35f;
	float fMaxFrameDelta = 50.0f;
	int iOppositeMotionFrames = 2;

	float fMinHeadTwistDegrees = 45.0f;
	float fMaxHeadTwistDegrees = 160.0f;

	int iDualGrabReleaseGraceFrames = 45;
	int iMotionDebugLogIntervalFrames = 20;
	int iAllowEssentialVictims = 1;

	void loadConfig()
	{
		std::string runtimeDirectory = GetRuntimeDirectory();

		if (runtimeDirectory.empty()) {
			return;
		}

		std::string filepath = runtimeDirectory + "Data\\SKSE\\Plugins\\NeckSnapVR.ini";
		std::ifstream file(filepath);

		if (!file.is_open()) {
			transform(filepath.begin(), filepath.end(), filepath.begin(), ::tolower);
			file.open(filepath);
		}

		if (!file.is_open()) {
			_MESSAGE("Config file is loaded successfully.");
			return;
		}

		std::string line;
		std::string currentSection;

		while (std::getline(file, line)) {
			trim(line);
			skipComments(line);

			if (line.empty()) {
				continue;
			}

			if (line[0] == '[') {
				size_t endBracket = line.find(']');
				if (endBracket != std::string::npos) {
					currentSection = line.substr(1, endBracket - 1);
					trim(currentSection);
				}
				continue;
			}

			std::string variableName;
			std::string variableValueStr = GetConfigSettingsStringValue(line, variableName);

			if (currentSection == "Settings") {
				if (variableName == "Logging") {
					logging = std::stoi(variableValueStr);
				}
			}
			else if (currentSection == "Detection") {
				if (variableName == "MinBehindDot") {
					fMinBehindDot = std::stof(variableValueStr);
				}
				else if (variableName == "ReachMargin") {
					fReachMargin = std::stof(variableValueStr);
				}
				else if (variableName == "DefaultReach") {
					fDefaultReach = std::stof(variableValueStr);
				}
				else if (variableName == "MaxHeightDiff") {
					fMaxHeightDiff = std::stof(variableValueStr);
				}
				else if (variableName == "LatchResetDistance") {
					fLatchResetDistance = std::stof(variableValueStr);
				}
				else if (variableName == "HeadGrabProximity") {
					fHeadGrabProximity = std::stof(variableValueStr);
				}
				else if (variableName == "MinHandSpeed") {
					fMinHandSpeed = std::stof(variableValueStr);
				}
				else if (variableName == "MinPeakAxisStep") {
					fMinPeakAxisStep = std::stof(variableValueStr);
				}
				else if (variableName == "MinHandSpeedRatio") {
					fMinHandSpeedRatio = std::stof(variableValueStr);
				}
				else if (variableName == "SoftHandSpeed") {
					fSoftHandSpeed = std::stof(variableValueStr);
				}
				else if (variableName == "OppositeCosThreshold") {
					fOppositeCosThreshold = std::stof(variableValueStr);
				}
				else if (variableName == "MaxFrameDelta") {
					fMaxFrameDelta = std::stof(variableValueStr);
				}
				else if (variableName == "OppositeMotionFrames") {
					iOppositeMotionFrames = std::stoi(variableValueStr);
				}
				else if (variableName == "MinHeadTwistDegrees") {
					fMinHeadTwistDegrees = std::stof(variableValueStr);
				}
				else if (variableName == "MaxHeadTwistDegrees") {
					fMaxHeadTwistDegrees = std::stof(variableValueStr);
				}
				else if (variableName == "DualGrabReleaseGraceFrames") {
					iDualGrabReleaseGraceFrames = std::stoi(variableValueStr);
				}
				else if (variableName == "MotionDebugLogIntervalFrames") {
					iMotionDebugLogIntervalFrames = std::stoi(variableValueStr);
				}
				else if (variableName == "AllowEssentialVictims") {
					iAllowEssentialVictims = std::stoi(variableValueStr);
				}
			}
		}

		_MESSAGE("Config file is loaded successfully.");
	}

	void Log(const int msgLogLevel, const char* fmt, ...)
	{
		if (msgLogLevel > logging) {
			return;
		}

		va_list args;
		char logBuffer[4096];

		va_start(args, fmt);
		vsprintf_s(logBuffer, sizeof(logBuffer), fmt, args);
		va_end(args);

		_MESSAGE(logBuffer);
	}

}
