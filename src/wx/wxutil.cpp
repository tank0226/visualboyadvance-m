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


wxAcceleratorEntryUnicode::wxAcceleratorEntryUnicode(wxAcceleratorEntry *accel)
  : wxAcceleratorEntry(accel->GetFlags(), accel->GetKeyCode(), accel->GetCommand(), accel->GetMenuItem())
{
    init(accel->GetFlags(), accel->GetKeyCode());
}


wxAcceleratorEntryUnicode::wxAcceleratorEntryUnicode(int flags, int keyCode, int cmd, wxMenuItem *item)
  : wxAcceleratorEntry(flags, keyCode, cmd, item)
{
    init(flags, keyCode);
}


void wxAcceleratorEntryUnicode::init(int flags, int keyCode)
{
    if (!(flags == 0 && keyCode == 0)) {
        ukey.Printf("%d:%d", keyCode, flags);
    }
}


KeyboardInputMap* KeyboardInputMap::instance = nullptr;


KeyboardInputMap* KeyboardInputMap::getInstance()
{
    if (!instance) {
        instance = new KeyboardInputMap();
    }
    return instance;
}


KeyboardInputMap::KeyboardInputMap(){}


void KeyboardInputMap::AddMap(wxString keyStr, int key, int mod)
{
    KeyboardInputMap* singleton = getInstance();
    singleton->keysMap[keyStr] = singleton->newPair(key, mod);
}


bool KeyboardInputMap::GetMap(wxString keyStr, int &key, int &mod)
{
    KeyboardInputMap* singleton = getInstance();
    if (singleton->keysMap.find(keyStr) != singleton->keysMap.end()) {
        key = singleton->keysMap.at(keyStr).key;
        mod = singleton->keysMap.at(keyStr).mod;
        return true;
    }
    return false;
}
