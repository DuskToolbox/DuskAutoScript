#ifndef DAS_PLUGINS_DASWINDOWSCAPTURE_GDICAPTURE_H
#define DAS_PLUGINS_DASWINDOWSCAPTURE_GDICAPTURE_H

#include <cstdint>
#include <das/IDasBase.h>
#include <windows.h>
#include <wingdi.h>

class GDICapture
{
public:
    GDICapture() = default;
    ~GDICapture();

    DasResult Initialize(HWND hwnd);
    DasResult Capture(uint8_t** pp_data, int32_t* p_width, int32_t* p_height);
    void      Cleanup();

private:
    HDC      hdc_screen_{nullptr};
    HDC      hdc_memory_{nullptr};
    HBITMAP  h_bitmap_{nullptr};
    RECT     target_rect_{};
    HWND     hwnd_{nullptr};
    int32_t  width_{0};
    int32_t  height_{0};
    bool     initialized_{false};
    uint8_t* bitmap_data_{nullptr};
    size_t   data_size_{0};
};

#endif // DAS_PLUGINS_DASWINDOWSCAPTURE_GDICAPTURE_H
