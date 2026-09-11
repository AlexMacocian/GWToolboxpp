#pragma once

struct IDirect3DDevice9;

namespace Skybox {
    void Initialize();
    void SignalTerminate();
    void Terminate();
    void DrawSettings();
    void SyncMasterSwitch(IDirect3DDevice9* device);
    void InvalidateDeviceResources();
    void UploadWorldLighting(IDirect3DDevice9* device, unsigned int tint_register);
}
