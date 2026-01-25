#include "GDICapture.h"
#include <das/DasApi.h>
#include <das/IDasBase.h>
#include <windows.h>
#include <wingdi.h>

#include <cstring>
#include <memory>
#include <stdexcept>

GDICapture::~GDICapture() { Cleanup(); }

DasResult GDICapture::Initialize(HWND hwnd)
{
    if (hwnd == nullptr)
    {
        DAS_LOG_ERROR("Invalid HWND for GDI capture");
        return DAS_E_INVALID_ARGUMENT;
    }

    if (IsIconic(hwnd))
    {
        DAS_LOG_ERROR("GDI capture does not support minimized windows");
        return DAS_E_INVALID_ARGUMENT;
    }

    hwnd_ = hwnd;

    GetWindowRect(hwnd, &target_rect_);
    width_ = target_rect_.right - target_rect_.left;
    height_ = target_rect_.bottom - target_rect_.top;

    hdc_screen_ = GetDC(nullptr);
    if (hdc_screen_ == nullptr)
    {
        DAS_LOG_ERROR("Failed to get screen DC");
        return DAS_E_CAPTURE_FAILED;
    }

    hdc_memory_ = CreateCompatibleDC(hdc_screen_);
    if (hdc_memory_ == nullptr)
    {
        DAS_LOG_ERROR("Failed to create compatible DC");
        ReleaseDC(nullptr, hdc_screen_);
        hdc_screen_ = nullptr;
        return DAS_E_CAPTURE_FAILED;
    }

    h_bitmap_ = CreateCompatibleBitmap(hdc_screen_, width_, height_);
    if (h_bitmap_ == nullptr)
    {
        DAS_LOG_ERROR("Failed to create compatible bitmap");
        DeleteDC(hdc_memory_);
        hdc_memory_ = nullptr;
        ReleaseDC(nullptr, hdc_screen_);
        hdc_screen_ = nullptr;
        return DAS_E_CAPTURE_FAILED;
    }

    SelectObject(hdc_memory_, h_bitmap_);

    initialized_ = true;
    return DAS_S_OK;
}

DasResult GDICapture::Capture(
    uint8_t** pp_data,
    int32_t*  p_width,
    int32_t*  p_height)
{
    if (!initialized_)
    {
        DAS_LOG_ERROR("GDICapture not initialized");
        return DAS_E_CAPTURE_FAILED;
    }

    *p_width = width_;
    *p_height = height_;

    if (!BitBlt(
            hdc_memory_,
            0,
            0,
            width_,
            height_,
            hdc_screen_,
            target_rect_.left,
            target_rect_.top,
            SRCCOPY | CAPTUREBLT))
    {
        DAS_LOG_ERROR("BitBlt failed, trying PrintWindow");
        if (!PrintWindow(hwnd_, hdc_memory_, PW_RENDERFULLCONTENT))
        {
            DAS_LOG_ERROR("PrintWindow failed");
            return DAS_E_CAPTURE_FAILED;
        }
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width_;
    bmi.bmiHeader.biHeight = -height_;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;

    int32_t pitch = ((width_ * 3 + 3) & ~3);
    data_size_ = pitch * height_;

    bitmap_data_ = new (std::nothrow) uint8_t[data_size_];
    if (bitmap_data_ == nullptr)
    {
        DAS_LOG_ERROR("Failed to allocate bitmap data");
        return DAS_E_CAPTURE_FAILED;
    }

    if (!GetDIBits(
            hdc_memory_,
            h_bitmap_,
            0,
            height_,
            bitmap_data_,
            &bmi,
            DIB_RGB_COLORS))
    {
        DAS_LOG_ERROR("GetDIBits failed");
        delete[] bitmap_data_;
        bitmap_data_ = nullptr;
        return DAS_E_CAPTURE_FAILED;
    }

    *pp_data = bitmap_data_;
    return DAS_S_OK;
}

void GDICapture::Cleanup()
{
    if (!initialized_)
    {
        return;
    }

    if (h_bitmap_ != nullptr)
    {
        DeleteObject(h_bitmap_);
        h_bitmap_ = nullptr;
    }

    if (hdc_memory_ != nullptr)
    {
        DeleteDC(hdc_memory_);
        hdc_memory_ = nullptr;
    }

    if (hdc_screen_ != nullptr)
    {
        ReleaseDC(nullptr, hdc_screen_);
        hdc_screen_ = nullptr;
    }

    if (bitmap_data_ != nullptr)
    {
        delete[] bitmap_data_;
        bitmap_data_ = nullptr;
    }

    data_size_ = 0;
    initialized_ = false;
}
