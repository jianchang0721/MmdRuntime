#pragma once

#include <string>

namespace Recovery {
extern bool enabled;
extern int recoveryCount;
extern int lastRecoveryFrame;
extern std::string lastRecoveryAction;

void ValidateAndRepairWindowState();
void ValidateAndRepairThemeState();
void ValidateAndRepairScreenState();
void ValidateAndRepairModuleState();
void ValidateAndRepairConfigState();
void RunStartupRecovery();
void RunFrameRecoveryIfNeeded();
}
