#include "ScreenManager.h"

#include "OverlayWindowManager.h"
#include "Screens.h"

namespace ScreenManager {
void RenderCurrentScreen() {
    if (!WindowManager::showOverlay) return;
    switch (WindowManager::currentScreen) {
        case Screen::Login: Screens::DrawLoginScreen(); break;
        case Screen::Games: Screens::DrawGamesScreen(); break;
        case Screen::Menu: Screens::DrawMenuScreen(); break;
    }
}
}
