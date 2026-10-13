// macOS drag and drop onto the plugin window (MacDrop.mm)
#pragma once

void crtMacDropRegister(void* nsview, void (*fn)(void* ctx, const char* path), void* ctx);
void crtMacDropUnregister(void* nsview);
