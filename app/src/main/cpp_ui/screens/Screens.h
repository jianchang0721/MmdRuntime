#pragma once

namespace Bridge {
struct BridgeModel;
}

namespace Screens {

void DrawLoginScreen();
void DrawGamesScreen();
void DrawMenuScreen();

void DrawOverviewTab(Bridge::BridgeModel& Model);
void DrawRenderTab(Bridge::BridgeModel& Model);
void DrawBoneTab(Bridge::BridgeModel& Model);
void DrawDiagnosticTab(Bridge::BridgeModel& Model);
void DrawSettingsTab(Bridge::BridgeModel& Model);

}  // namespace Screens
