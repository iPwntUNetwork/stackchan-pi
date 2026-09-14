#pragma once
#include "ui.h"

// per-screen draw + touch handlers (touch coords in LCD space)
void scrFaceDraw(bool dirty);
void scrFaceTouch(int x, int y, bool pressed, bool clicked);
void scrChatDraw(bool dirty);
void scrChatTouch(int x, int y, bool pressed, bool clicked);
void scrPiDraw(bool dirty);
void scrPiTouch(int x, int y, bool pressed, bool clicked);
void scrCamDraw(bool dirty);
void scrCamTouch(int x, int y, bool pressed, bool clicked);
void scrSettingsDraw(bool dirty);
void scrSettingsTouch(int x, int y, bool pressed, bool clicked);

void kbdInit();   // wire the shared keyboard to chat/pi screens
