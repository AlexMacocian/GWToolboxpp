#include "stdafx.h"

#include <GWCA/Utilities/Debug.h>
#include <GWCA/Utilities/Hooker.h>
#include <GWCA/Utilities/Scanner.h>

#include <GWCA/Managers/GameThreadMgr.h>
#include <GWCA/Managers/MemoryMgr.h>

#include <Defines.h>
#include <ImGuiAddons.h>
#include "MouseFix.h"

#include <hidusage.h>
#include <GWCA/Managers/UIMgr.h>

namespace {
    using OnProcessInput_pt = bool(__cdecl*)(uint32_t* wParam, uint32_t* lParam);
    OnProcessInput_pt ProcessInput_Func = nullptr;
    OnProcessInput_pt ProcessInput_Ret = nullptr;

    struct GwMouseMove {
        int center_x;
        int center_y;
        uint32_t unk;
        uint32_t mouse_button_state; // 0x1 - LMB, 0x2 - MMB, 0x4 - RMB
        uint32_t move_camera;        // 1 == control camera while right mouse button pressed
        int captured_x;
        int captured_y;
    };

    GwMouseMove* gw_mouse_move = nullptr;
    LONG rawInputRelativePosX = 0;
    LONG rawInputRelativePosY = 0;
    bool* HasRegisteredTrackMouseEvent = nullptr;
    using SetCursorPosCenter_pt = void(__cdecl*)(GwMouseMove* wParam);
    SetCursorPosCenter_pt SetCursorPosCenter_Func = nullptr;
    SetCursorPosCenter_pt SetCursorPosCenter_Ret = nullptr;    // Override (and rewrite) GW's handling of setting the mouse cursor to the center of the screen (bypass GameMutex, may be the cause of camera glitch)
    // This could be a patch really, but rewriting the function out is a bit more readable.
    
    bool initialized = false;
    bool enable_cursor_fix = false;
    int cursor_size = 32;
    HCURSOR current_cursor = nullptr;
    bool cursor_size_hooked = false;
    
    void OnSetCursorPosCenter(GwMouseMove* gwmm)
    {
        GW::Hook::EnterHook();
        if (!enable_cursor_fix)
            return GW::Hook::LeaveHook();
        // @Enhancement: Maybe assert that gwmm == gw_mouse_move?
        const HWND gw_window_handle = GetFocus();
        // @Enhancement: Maybe check that the focussed window handle is the GW window handle?
        RECT rect;
        if (!(gw_window_handle && GetClientRect(gw_window_handle, &rect))) {
            return GW::Hook::LeaveHook();
        }
        gwmm->center_x = (rect.left + rect.right) / 2;
        gwmm->center_y = (rect.bottom + rect.top) / 2;
        rawInputRelativePosX = rawInputRelativePosY = 0;
        SetPhysicalCursorPos(gwmm->captured_x, gwmm->captured_y);
        GW::Hook::LeaveHook();
    }

    // Override (and rewrite) GW's handling of mouse event 0x200 to stop camera glitching.
    bool OnProcessInput(uint32_t* wParam, uint32_t* lParam)
    {
        GW::Hook::EnterHook();
        if (!(enable_cursor_fix && HasRegisteredTrackMouseEvent && gw_mouse_move)) {
            goto forward_call; // Failed to find addresses for variables
        }
        if (!(wParam && wParam[1] == 0x200)) {
            goto forward_call; // Not mouse movement
        }
        if (!(*HasRegisteredTrackMouseEvent && gw_mouse_move->move_camera)) {
            goto forward_call; // Not moving the camera, or GW hasn't yet called TrackMouseEvent
        }

        lParam[0] = 0x12;
        // Set the output parameters to be the relative position of the mouse to the center of the screen
        // NB: Original function uses ClientToScreen here; we've already grabbed the correct value via CursorFixWndProc
        lParam[1] = rawInputRelativePosX;
        lParam[2] = rawInputRelativePosY;

        // Reset the cursor position to the middle of the viewport
        OnSetCursorPosCenter(gw_mouse_move);
        GW::Hook::LeaveHook();
        return true;
    forward_call:
        GW::Hook::LeaveHook();
        return ProcessInput_Ret(wParam, lParam);
    }

    void CursorFixWndProc(const UINT Message, const WPARAM wParam, const LPARAM lParam)
    {
        if (!(Message == WM_INPUT && GET_RAWINPUT_CODE_WPARAM(wParam) == RIM_INPUT && lParam)) {
            return; // Not raw input
        }
        if (!gw_mouse_move) {
            return; // No gw mouse move ptr; this shouldn't happen
        }

        UINT dwSize = sizeof(RAWINPUT);
        BYTE lpb[sizeof(RAWINPUT)];
        ASSERT(GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, lpb, &dwSize, sizeof(RAWINPUTHEADER)) == dwSize);

        const RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(lpb);
        if ((raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
            // If its a relative mouse move, process the action
            if (gw_mouse_move->move_camera) {
                rawInputRelativePosX += raw->data.mouse.lLastX;
                rawInputRelativePosY += raw->data.mouse.lLastY;
            }
            else {
                rawInputRelativePosX = rawInputRelativePosY = 0;
            }
        }
    }

    bool CursorFixInitialise()
    {
        if (gw_mouse_move) {
            return true;
        }
        const auto hwnd = GW::MemoryMgr::GetGWWindowHandle();
        if (!hwnd) {
            return false;
        }
        uintptr_t address = GW::Scanner::Find("\xc7\x45\xf0\x10\x00\x00\x00\xc7\x45\xf4\x02\x00\x00\x00", "xx?xxxxxx?xxxx", 0x15);
        if(address && GW::Scanner::IsValidPtr(*(uintptr_t*)address)) {
            ProcessInput_Func = (OnProcessInput_pt)GW::Scanner::ToFunctionStart(address, 0xfff);
            HasRegisteredTrackMouseEvent = *(bool**)address;
            gw_mouse_move = (GwMouseMove*)(HasRegisteredTrackMouseEvent - 0x20);
            SetCursorPosCenter_Func = (SetCursorPosCenter_pt)GW::Scanner::FunctionFromNearCall(GW::Scanner::FindInRange("\x89\x46\x08\xe8????", "xxxx????", 3, address, address + 0xff));
        }

        GWCA_INFO("[SCAN] ProcessInput_Func = %p", ProcessInput_Func);
        GWCA_INFO("[SCAN] HasRegisteredTrackMouseEvent = %p", HasRegisteredTrackMouseEvent);
        GWCA_INFO("[SCAN] gw_mouse_move = %p", gw_mouse_move);
        GWCA_INFO("[SCAN] SetCursorPosCenter_Func = %p", SetCursorPosCenter_Func);

#ifdef _DEBUG
        ASSERT(ProcessInput_Func && HasRegisteredTrackMouseEvent && gw_mouse_move && SetCursorPosCenter_Func);
#endif
        if (ProcessInput_Func && HasRegisteredTrackMouseEvent && gw_mouse_move && SetCursorPosCenter_Func) {
            GW::Hook::CreateHook((void**)&ProcessInput_Func, OnProcessInput, reinterpret_cast<void**>(&ProcessInput_Ret));
            GW::Hook::CreateHook((void**)&SetCursorPosCenter_Func, OnSetCursorPosCenter, reinterpret_cast<void**>(&SetCursorPosCenter_Ret));
        }            

        return gw_mouse_move != nullptr;
    }

    void CursorFixEnable(const bool enable)
    {
        CursorFixInitialise();
        if (!(ProcessInput_Func && HasRegisteredTrackMouseEvent && gw_mouse_move && SetCursorPosCenter_Func))
            return;
        if (!enable) {
            GW::Hook::DisableHooks(ProcessInput_Func);
            GW::Hook::DisableHooks(SetCursorPosCenter_Func);
        }
        else {
            GW::Hook::EnableHooks(ProcessInput_Func);
            GW::Hook::EnableHooks(SetCursorPosCenter_Func);
        }
    }

    /*
     *  Logic for scaling gw cursor up or down
     */
    HBITMAP ScaleBitmap(const HBITMAP inBitmap, const int inWidth, const int inHeight, const int outWidth, const int outHeight)
    {
        // NB: We could use GDIPlus for this logic which has better image res handling etc, but no need
        HDC srcDC = nullptr;
        BYTE* ppvBits = nullptr;
        BOOL bResult = 0;
        HBITMAP outBitmap = nullptr;

        // create a destination bitmap and DC with size w/h
        BITMAPINFO bmi;
        memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biWidth = outWidth;
        bmi.bmiHeader.biHeight = outWidth;
        bmi.bmiHeader.biPlanes = 1;

        // Do not use CreateCompatibleBitmap otherwise api will not allocate memory for bitmap
        const HDC destDC = CreateCompatibleDC(nullptr);
        if (!destDC) {
            goto cleanup;
        }
        outBitmap = CreateDIBSection(destDC, &bmi, DIB_RGB_COLORS, (void**)&ppvBits, nullptr, 0);
        if (outBitmap == nullptr) {
            goto cleanup;
        }
        if (SelectObject(destDC, outBitmap) == nullptr) {
            goto cleanup;
        }

        srcDC = CreateCompatibleDC(nullptr);
        if (!srcDC) {
            goto cleanup;
        }
        if (SelectObject(srcDC, inBitmap) == nullptr) {
            goto cleanup;
        }

        // copy and scaling to new width/height (w,h)
        if (SetStretchBltMode(destDC, WHITEONBLACK) == 0) {
            goto cleanup;
        }
        bResult = StretchBlt(destDC, 0, 0, outWidth, outHeight, srcDC, 0, 0, inWidth, inHeight, SRCCOPY);
    cleanup:
        if (!bResult) {
            if (outBitmap) {
                DeleteObject(outBitmap);
                outBitmap = nullptr;
            }
        }
        if (destDC) {
            DeleteDC(destDC);
        }
        if (srcDC) {
            DeleteDC(srcDC);
        }

        return outBitmap;
    }


    HCURSOR ScaleCursor(const HCURSOR cursor, const int targetSize)
    {
        ICONINFO icon_info = { 0 };
        HCURSOR new_cursor = nullptr;
        BITMAP tmpBitmap = { 0 };
        HBITMAP scaledMask = nullptr, scaledColor = nullptr;
        if (!GetIconInfo(cursor, &icon_info)) {
            goto cleanup;
        }
        if (GetObject(icon_info.hbmMask, sizeof(BITMAP), &tmpBitmap) == 0) {
            goto cleanup;
        }
        if (!(tmpBitmap.bmHeight && tmpBitmap.bmWidth))
            goto cleanup;
        if (tmpBitmap.bmWidth == targetSize) {
            goto cleanup;
        }
        scaledMask = ScaleBitmap(icon_info.hbmMask, tmpBitmap.bmWidth, tmpBitmap.bmHeight, targetSize, targetSize);
        if (!scaledMask) {
            goto cleanup;
        }
        if (GetObject(icon_info.hbmColor, sizeof(BITMAP), &tmpBitmap) == 0) {
            goto cleanup;
        }
        scaledColor = ScaleBitmap(icon_info.hbmColor, tmpBitmap.bmWidth, tmpBitmap.bmHeight, targetSize, targetSize);
        if (!scaledColor) {
            goto cleanup;
        }
        icon_info.hbmColor = scaledColor;
        icon_info.hbmMask = scaledMask;
        new_cursor = CreateIconIndirect(&icon_info);
    cleanup:
        if (icon_info.hbmColor)
            DeleteObject(icon_info.hbmColor);
        if (icon_info.hbmMask)
            DeleteObject(icon_info.hbmMask);
        if (scaledColor)
            DeleteObject(scaledColor);
        if (scaledMask)
            DeleteObject(scaledMask);
        return new_cursor;
    }


    using ChangeCursorIcon_pt = void(__cdecl*)(void*);
    ChangeCursorIcon_pt ChangeCursorIcon_Func = nullptr;
    ChangeCursorIcon_pt ChangeCursorIcon_Ret = nullptr;

    void OnChangeCursorIcon(char* user_data)
    {
        GW::Hook::EnterHook();
        ChangeCursorIcon_Ret(user_data);
        // Cursor has been changed by the game; pull it back out, scale it to target size..
        if (cursor_size < 0 || cursor_size > 64 || cursor_size == 32) {
            return GW::Hook::LeaveHook();
        }

        HCURSOR* cursor = (HCURSOR*)&user_data[0xd6c];
        HWND* window_handle = (HWND*)&user_data[0xe20];

        if (!(user_data && *cursor && *cursor != current_cursor)) {
            return GW::Hook::LeaveHook();
        }
        const HCURSOR new_cursor = ScaleCursor(*cursor, cursor_size);
        if (!new_cursor) {
            return GW::Hook::LeaveHook();
        }
        if (*cursor == new_cursor) {
            return GW::Hook::LeaveHook();
        }
        if (*cursor) {
            // Don't forget to free the original cursor before overwriting the handle
            DestroyCursor(*cursor);
            SetClassLongA(*window_handle, GCL_HCURSOR, 0);
            SetCursor(nullptr);
            *cursor = nullptr;
        }
        *cursor = new_cursor;
        SetCursor(new_cursor);
        // Also override the window class for the cursor
        SetClassLongA(*window_handle, GCL_HCURSOR, reinterpret_cast<LONG>(new_cursor));
        current_cursor = new_cursor;
        GW::Hook::LeaveHook();
    }

    void RedrawCursorIcon()
    {
        GW::GameThread::Enqueue([] {
            // Force redraw
            const auto user_data = (void*)GetWindowLongA(GW::MemoryMgr::GetGWWindowHandle(), -0xc);
            current_cursor = nullptr;
            if (user_data && ChangeCursorIcon_Func) {
                ChangeCursorIcon_Func(user_data);
            }
        });
    }

    void SetCursorSize(const int new_size)
    {
        cursor_size = new_size;
        RedrawCursorIcon();
    }

    GW::HookEntry UIMessage_HookEntry;

    void OnUIMessage(GW::HookStatus*, GW::UI::UIMessage message_id, void*, void*) {
        switch (message_id) {
        case GW::UI::UIMessage::kLogout:
            CursorFixEnable(false);
            break;
        case GW::UI::UIMessage::kMapLoaded:
            CursorFixEnable(enable_cursor_fix);
            break;
        }
    }

} // namespace

void MouseFix::Initialize()
{
    ToolboxModule::Initialize();

    auto address = GW::Scanner::Find("\xf7\xc3\x80\x20\x00\x00\x74\x09\x56", "xxxxxxxxx", 0x9);
    address = GW::Scanner::FunctionFromNearCall(address);
    if (GW::Scanner::IsValidPtr(address, GW::ScannerSection::Section_TEXT)) {
        ChangeCursorIcon_Func = (ChangeCursorIcon_pt)address;
        GW::Hook::CreateHook((void**)&ChangeCursorIcon_Func, OnChangeCursorIcon, (void**)&ChangeCursorIcon_Ret);
        GW::Hook::EnableHooks(ChangeCursorIcon_Func);
    }

#if _DEBUG
    ASSERT(ChangeCursorIcon_Func);
#endif

    const GW::UI::UIMessage ui_messages[] = {
        GW::UI::UIMessage::kLogout,
        GW::UI::UIMessage::kMapLoaded
    };

    for (const auto ui_message : ui_messages) {
        GW::UI::RegisterUIMessageCallback(&UIMessage_HookEntry, ui_message, OnUIMessage);
    }
}

void MouseFix::LoadSettings(ToolboxIni* ini)
{
    LOAD_BOOL(enable_cursor_fix);
    SetCursorSize(ini->GetLongValue(Name(), VAR_NAME(cursor_size), cursor_size));
    RedrawCursorIcon();
}

void MouseFix::SaveSettings(ToolboxIni* ini)
{
    SAVE_BOOL(enable_cursor_fix);
    SAVE_UINT(cursor_size);
}

void MouseFix::Terminate()
{
    ToolboxModule::Terminate();
    CursorFixEnable(false);
    GW::UI::RemoveUIMessageCallback(&UIMessage_HookEntry);
    GW::Hook::RemoveHook(ChangeCursorIcon_Func);

    gw_mouse_move = nullptr;

}

void MouseFix::DrawSettingsInternal()
{
    if (ImGui::Checkbox("Enable cursor fix", &enable_cursor_fix)) {
        CursorFixEnable(enable_cursor_fix);
    }
    ImGui::SliderInt("Guild Wars cursor size", &cursor_size, 16, 64);
    ImGui::ShowHelp("Sizes other than 32 might lead the the cursor disappearing at random.\n"
        "Right click to make the cursor dis- and reappear for this to take effect.");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        SetCursorSize(cursor_size);
        RedrawCursorIcon();
    }
    if (ImGui::Button("Reset")) {
        SetCursorSize(32);
        RedrawCursorIcon();
    }
}

bool MouseFix::WndProc(const UINT Message, const WPARAM wParam, const LPARAM lParam)
{
    if (!enable_cursor_fix) {
        return false;
    }
    if (!initialized) {
        CursorFixEnable(enable_cursor_fix);
        initialized = true;
    }
    CursorFixWndProc(Message, wParam, lParam);
    return false;
}
