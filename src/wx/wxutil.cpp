#include "wxutil.h"

int getKeyboardKeyCode(wxKeyEvent& event)
{
    int uc = event.GetUnicodeKey();
    if (uc != WXK_NONE) {
        return uc;
    }
    else {
        return event.GetKeyCode();
    }
}
