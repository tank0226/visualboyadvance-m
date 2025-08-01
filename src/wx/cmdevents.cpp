#include "wx/wxvbam.h"

#include <wx/aboutdlg.h>
#include <wx/ffile.h>
#include <wx/numdlg.h>
#include <wx/progdlg.h>
#include <wx/regex.h>
#include <wx/sstream.h>
#include <wx/url.h>
#include <wx/wfstream.h>
#include <wx/msgdlg.h>

#include "components/filters_interframe/interframe.h"
#include "core/base/check.h"
#include "core/base/version.h"
#include "core/gb/gb.h"
#include "core/gb/gbCheats.h"
#include "core/gb/gbGlobals.h"
#include "core/gb/gbPrinter.h"
#include "core/gb/gbSound.h"
#include "core/gba/gbaCheats.h"
#include "core/gba/gbaEeprom.h"
#include "core/gba/gbaGlobals.h"
#include "core/gba/gbaPrint.h"
#include "core/gba/gbaSound.h"
#include "wx/config/cmdtab.h"
#include "wx/config/option-proxy.h"
#include "wx/config/option.h"
#include "wx/dialogs/game-maker.h"
#include "wx/wxvbam.h"
#include "wx/widgets/group-check-box.h"
#include "wx/widgets/user-input-ctrl.h"
#include "wx/widgets/utils.h"

#define GetXRCDialog(n) \
    wxStaticCast(wxGetApp().frame->FindWindowByName(n), wxDialog)

void RefreshFrame(void)
{
    wxXmlResource* xr = wxXmlResource::Get();
    const wxRect client_rect(
        OPTION(kGeomWindowX).Get(),
        OPTION(kGeomWindowY).Get(),
        OPTION(kGeomWindowWidth).Get(),
        OPTION(kGeomWindowHeight).Get());
    const bool is_fullscreen = OPTION(kGeomFullScreen);
    const bool is_maximized = OPTION(kGeomIsMaximized);

    // note: if linking statically, next 2 pull in lot of unused code
    // maybe in future if not wxSHARED, load only builtin-needed handlers
    xr->InitAllHandlers();
    xr->AddHandler(new widgets::GroupCheckBoxXmlHandler());
    xr->AddHandler(new widgets::UserInputCtrlXmlHandler());
    wxInitAllImageHandlers();

    wxGetApp().SetExitOnFrameDelete(false);

    if (wxGetApp().frame)
        wxGetApp().frame->Destroy();

    wxGetApp().frame = wxDynamicCast(xr->LoadFrame(nullptr, "MainFrame"), MainFrame);
    if (!wxGetApp().frame) {
        wxLogError(_("Could not create main window"));
        return;
    }

    wxConfigBase* cfg = wxConfigBase::Get();
    gopts.recent = new wxFileHistory(10);
    cfg->SetPath("/Recent");
    gopts.recent->Load(*cfg);
    cfg->SetPath("/");
    cfg->Flush();

    // Create() cannot be overridden easily
    if (!wxGetApp().frame->BindControls()) {
        return;
    }

    // Ensure we are not drawing out of bounds.
    if (widgets::GetDisplayRect().Intersects(client_rect)) {
        wxGetApp().frame->SetSize(client_rect);
    }

    if (is_maximized) {
        wxGetApp().frame->Maximize();
    }

    if (is_fullscreen && wxGetApp().pending_load != wxEmptyString)
        wxGetApp().frame->ShowFullScreen(is_fullscreen);

    wxGetApp().frame->Show(true);
    
    // Windows can render the taskbar icon late if this is done in MainFrame
    // It may also not update at all until the Window has been minimized/restored
    // This seems timing related, possibly based on HWND
    // So do this here since it reliably draws the Taskbar icon on Window creation.
    wxGetApp().frame->BindAppIcon();

    wxGetApp().SetExitOnFrameDelete(true);
}

void MainFrame::GetMenuOptionBool(const wxString& menuName, bool* field)
{
    VBAM_CHECK(field);
    *field = !*field;
    int id = wxXmlResource::GetXRCID(menuName);

    for (size_t i = 0; i < checkable_mi.size(); i++) {
        if (checkable_mi[i].cmd != id)
            continue;

        *field = checkable_mi[i].mi->IsChecked();
        break;
    }
}

void MainFrame::GetMenuOptionConfig(const wxString& menu_name,
                                    const config::OptionID& option_id) {
    config::Option* option = config::Option::ByID(option_id);
    VBAM_CHECK(option);

    int id = wxXmlResource::GetXRCID(menu_name);
    for (size_t i = 0; i < checkable_mi.size(); i++) {
        if (checkable_mi[i].cmd != id)
            continue;

        const bool is_checked = checkable_mi[i].mi->IsChecked();
        switch (option->type()) {
            case config::Option::Type::kBool:
                option->SetBool(is_checked);
                break;
            case config::Option::Type::kInt:
                option->SetInt(is_checked);
                break;
            default:
                VBAM_CHECK(false);
                return;
        }
        break;
    }
}

void MainFrame::GetMenuOptionInt(const wxString& menuName, int* field, int mask)
{
    VBAM_CHECK(field);
    int value = mask;
    bool is_checked = ((*field) & (mask)) != (value);
    int id = wxXmlResource::GetXRCID(menuName);

    for (size_t i = 0; i < checkable_mi.size(); i++) {
        if (checkable_mi[i].cmd != id)
            continue;

        is_checked = checkable_mi[i].mi->IsChecked();
        break;
    }

    *field = ((*field) & ~(mask)) | (is_checked ? (value) : 0);
}

void MainFrame::SetMenuOption(const wxString& menuName, bool value)
{
    int id = wxXmlResource::GetXRCID(menuName);

    for (size_t i = 0; i < checkable_mi.size(); i++) {
        if (checkable_mi[i].cmd != id)
            continue;

        checkable_mi[i].mi->Check(value);
        break;
    }
}

static void toggleBooleanVar(bool *menuValue, bool *globalVar)
{
    if (*menuValue == *globalVar) // used accelerator
        *globalVar = !(*globalVar);
    else // used menu item
        *globalVar = *menuValue;
}

static void toggleBitVar(bool *menuValue, int *globalVar, int mask)
{
    bool isEnabled = ((*globalVar) & (mask)) != (mask);
    if (*menuValue == isEnabled)
        *globalVar = ((*globalVar) & ~(mask)) | (!isEnabled ? (mask) : 0);
    else
        *globalVar = ((*globalVar) & ~(mask)) | (*menuValue ? (mask) : 0);
    *menuValue = ((*globalVar) & (mask)) != (mask);
}

//// File menu

EVT_HANDLER(wxID_OPEN, "Open ROM...")
{
    static int open_ft = 0;
    const wxString gba_rom_dir = OPTION(kGBAROMDir);

    // FIXME: ignore if non-existent or not a dir
    wxString pats = _(
        "Game Boy Advance Files (*.agb;*.gba;*.bin;*.elf;*.mb;*.zip;*.7z;*.rar)|"
        "*.agb;*.gba;*.bin;*.elf;*.mb;"
        "*.agb.lz;*.gba.lz;*.bin.lz;*.elf.lz;*.mb.lz;"
        "*.agb.xz;*.gba.xz;*.bin.xz;*.elf.xz;*.mb.xz;"
        "*.agb.bz2;*.gba.bz2;*.bin.bz2;*.elf.bz2;*.mb.bz2;"
        "*.agb.gz;*.gba.gz;*.bin.gz;*.elf.gz;*.mb.gz;"
        "*.agb.z;*.gba.z;*.bin.z;*.elf.z;*.mb.z;"
        "*.zip;*.7z;*.rar|"
        "Game Boy Files (*.dmg;*.gb;*.gbc;*.cgb;*.sgb;*.zip;*.7z;*.rar)|"
        "*.dmg;*.gb;*.gbc;*.cgb;*.sgb;"
        "*.dmg.lz;*.gb.lz;*.gbc.lz;*.cgb.lz;*.sgb.lz;"
        "*.dmg.xz;*.gb.xz;*.gbc.xz;*.cgb.xz;*.sgb.xz;"
        "*.dmg.bz2;*.gb.bz2;*.gbc.bz2;*.cgb.bz2;*.sgb.bz2;"
        "*.dmg.gz;*.gb.gz;*.gbc.gz;*.cgb.gz;*.sgb.gz;"
        "*.dmg.z;*.gb.z;*.gbc.z;*.cgb.z;*.sgb.z;"
        "*.tar;*.zip;*.7z;*.rar|");
    pats.append(wxALL_FILES);

    wxFileDialog dlg(this, _("Open ROM file"), gba_rom_dir, "",
        pats,
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    SetGenericPath(dlg, gba_rom_dir);

    dlg.SetFilterIndex(open_ft);

    if (ShowModal(&dlg) == wxID_OK)
        wxGetApp().pending_load = dlg.GetPath();

    open_ft = dlg.GetFilterIndex();
    if (gba_rom_dir.empty()) {
        OPTION(kGBAROMDir) = dlg.GetDirectory();
    }
}

EVT_HANDLER(OpenGB, "Open GB...")
{
    static int open_ft = 0;
    const wxString gb_rom_dir = OPTION(kGBROMDir);

    // FIXME: ignore if non-existent or not a dir
    wxString pats = _(
        "Game Boy Files (*.dmg;*.gb;*.gbc;*.cgb;*.sgb;*.zip;*.7z;*.rar)|"
        "*.dmg;*.gb;*.gbc;*.cgb;*.sgb;"
        "*.dmg.lz;*.gb.lz;*.gbc.lz;*.cgb.lz;*.sgb.lz;"
        "*.dmg.xz;*.gb.xz;*.gbc.xz;*.cgb.xz;*.sgb.xz;"
        "*.dmg.bz2;*.gb.bz2;*.gbc.bz2;*.cgb.bz2;*.sgb.bz2;"
        "*.dmg.gz;*.gb.gz;*.gbc.gz;*.cgb.gz;*.sgb.gz;"
        "*.dmg.z;*.gb.z;*.gbc.z;*.cgb.z;*.sgb.z;"
        "*.tar;*.zip;*.7z;*.rar|");
    pats.append(wxALL_FILES);
    wxFileDialog dlg(this, _("Open GB ROM file"), gb_rom_dir, "",
        pats,
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    dlg.SetFilterIndex(open_ft);

    SetGenericPath(dlg, gb_rom_dir);

    if (ShowModal(&dlg) == wxID_OK)
        wxGetApp().pending_load = dlg.GetPath();

    open_ft = dlg.GetFilterIndex();
    if (gb_rom_dir.empty()) {
        OPTION(kGBROMDir) = dlg.GetDirectory();
    }
}

EVT_HANDLER(OpenGBC, "Open GBC...")
{
    static int open_ft = 0;
    const wxString gbc_rom_dir = OPTION(kGBGBCROMDir);

    // FIXME: ignore if non-existent or not a dir
    wxString pats = _(
        "Game Boy Color Files (*.dmg;*.gb;*.gbc;*.cgb;*.sgb;*.zip;*.7z;*.rar)|"
        "*.dmg;*.gb;*.gbc;*.cgb;*.sgb;"
        "*.dmg.lz;*.gb.lz;*.gbc.lz;*.cgb.lz;*.sgb.lz;"
        "*.dmg.xz;*.gb.xz;*.gbc.xz;*.cgb.xz;*.sgb.xz;"
        "*.dmg.bz2;*.gb.bz2;*.gbc.bz2;*.cgb.bz2;*.sgb.bz2;"
        "*.dmg.gz;*.gb.gz;*.gbc.gz;*.cgb.gz;*.sgb.gz;"
        "*.dmg.z;*.gb.z;*.gbc.z;*.cgb.z;*.sgb.z;"
        "*.tar;*.zip;*.7z;*.rar|");
    pats.append(wxALL_FILES);
    wxFileDialog dlg(this, _("Open GBC ROM file"), gbc_rom_dir, "",
        pats,
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    dlg.SetFilterIndex(open_ft);

    SetGenericPath(dlg, gbc_rom_dir);

    if (ShowModal(&dlg) == wxID_OK)
        wxGetApp().pending_load = dlg.GetPath();

    open_ft = dlg.GetFilterIndex();
    if (gbc_rom_dir.empty()) {
        OPTION(kGBGBCROMDir) = dlg.GetDirectory();
    }
}

EVT_HANDLER(RecentReset, "Reset recent ROM list")
{
    // only save config if there were items to remove
    if (gopts.recent->GetCount()) {
        while (gopts.recent->GetCount())
            gopts.recent->RemoveFileFromHistory(0);

        wxConfigBase* cfg = wxConfigBase::Get();
        cfg->SetPath("/Recent");
        gopts.recent->Save(*cfg);
        cfg->SetPath("/");
        cfg->Flush();
    }
}

EVT_HANDLER(RecentFreeze, "Freeze recent ROM list (toggle)")
{
    GetMenuOptionConfig("RecentFreeze", config::OptionID::kGenFreezeRecent);
}

// following 10 should really be a single ranged handler
// former names: Recent01 .. Recent10
EVT_HANDLER(wxID_FILE1, "Load recent ROM 1")
{
    panel->LoadGame(gopts.recent->GetHistoryFile(0));
}

EVT_HANDLER(wxID_FILE2, "Load recent ROM 2")
{
    panel->LoadGame(gopts.recent->GetHistoryFile(1));
}

EVT_HANDLER(wxID_FILE3, "Load recent ROM 3")
{
    panel->LoadGame(gopts.recent->GetHistoryFile(2));
}

EVT_HANDLER(wxID_FILE4, "Load recent ROM 4")
{
    panel->LoadGame(gopts.recent->GetHistoryFile(3));
}

EVT_HANDLER(wxID_FILE5, "Load recent ROM 5")
{
    panel->LoadGame(gopts.recent->GetHistoryFile(4));
}

EVT_HANDLER(wxID_FILE6, "Load recent ROM 6")
{
    panel->LoadGame(gopts.recent->GetHistoryFile(5));
}

EVT_HANDLER(wxID_FILE7, "Load recent ROM 7")
{
    panel->LoadGame(gopts.recent->GetHistoryFile(6));
}

EVT_HANDLER(wxID_FILE8, "Load recent ROM 8")
{
    panel->LoadGame(gopts.recent->GetHistoryFile(7));
}

EVT_HANDLER(wxID_FILE9, "Load recent ROM 9")
{
    panel->LoadGame(gopts.recent->GetHistoryFile(8));
}

EVT_HANDLER(wxID_FILE10, "Load recent ROM 10")
{
    panel->LoadGame(gopts.recent->GetHistoryFile(9));
}

void SetDialogLabel(wxDialog* dlg, const wxString& id, wxString ts, size_t l)
{
    (void)l; // unused params
    ts.Replace(wxT("&"), wxT("&&"), true);
    (dynamic_cast<wxControl*>((*dlg).FindWindow(wxXmlResource::GetXRCID(id))))->SetLabel(ts);
}

EVT_HANDLER_MASK(RomInformation, "ROM information...", CMDEN_GB | CMDEN_GBA)
{
    wxString s;
#define setlab(id)                            \
    do {                                      \
        /* SetLabelText is not in 2.8 */      \
        s.Replace(wxT("&"), wxT("&&"), true); \
        XRCCTRL(*dlg, id, wxControl)          \
            ->SetLabel(s);                    \
    } while (0)
#define setblab(id, b)                          \
    do {                                        \
        s.Printf(wxT("%02x"), (unsigned int)b); \
        setlab(id);                             \
    } while (0)
#define setlabs(id, ts, l)                               \
    do {                                                 \
        s = wxString((const char*)&(ts), wxConvLibc, l); \
        setlab(id);                                      \
    } while (0)

    switch (panel->game_type()) {
    case IMAGE_GB:
        ShowModal(GetXRCDialog("GBROMInfo"));
        break;
    case IMAGE_GBA: {
        IdentifyRom();
        wxDialog* dlg = GetXRCDialog("GBAROMInfo");
        wxString rom_crc32;
        rom_crc32.Printf(wxT("%08X"), panel->rom_crc32);
        SetDialogLabel(dlg, wxT("Title"), panel->rom_name, 30);
        setlabs("IntTitle", g_rom[0xa0], 12);
        SetDialogLabel(dlg, wxT("Scene"), panel->rom_scene_rls_name, 30);
        SetDialogLabel(dlg, wxT("Release"), panel->rom_scene_rls, 4);
        SetDialogLabel(dlg, wxT("CRC32"), rom_crc32, 8);
        setlabs("GameCode", g_rom[0xac], 4);
        setlabs("MakerCode", g_rom[0xb0], 2);
        s = dialogs::GetGameMakerName(s.ToStdString());
        setlab("MakerName");
        setblab("UnitCode", g_rom[0xb3]);
        s.Printf(wxT("%02x"), (unsigned int)g_rom[0xb4]);

        if (g_rom[0xb4] & 0x80)
            s.append(wxT(" (DACS)"));

        setlab("DeviceType");
        setblab("Version", g_rom[0xbc]);
        uint8_t crc = 0x19;

        for (int i = 0xa0; i < 0xbd; i++)
            crc += g_rom[i];

        crc = -crc;
        s.Printf(wxT("%02x (%02x)"), crc, g_rom[0xbd]);
        setlab("CRC");
        dlg->Fit();
        ShowModal(dlg);
    } break;

    default:
        break;
    }
}

EVT_HANDLER_MASK(ResetLoadingDotCodeFile, "Reset Loading e-Reader Dot Code", CMDEN_GBA)
{
    ResetLoadDotCodeFile();
}

EVT_HANDLER_MASK(SetLoadingDotCodeFile, "Load e-Reader Dot Code...", CMDEN_GBA)
{
    static wxString loaddotcodefile_path;
    wxFileDialog dlg(this, _("Select Dot Code file"), loaddotcodefile_path, wxEmptyString,
        _(
                         "E-Reader Dot Code (*.bin;*.raw)|"
                         "*.bin;*.raw"),
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    
    SetGenericPath(dlg, loaddotcodefile_path);
    
    int ret = ShowModal(&dlg);

    if (ret != wxID_OK)
        return;

    loaddotcodefile_path = dlg.GetPath();
    SetLoadDotCodeFile(UTF8(loaddotcodefile_path));
}

EVT_HANDLER_MASK(ResetSavingDotCodeFile, "Reset Saving e-Reader Dot Code", CMDEN_GBA)
{
    ResetLoadDotCodeFile();
}

EVT_HANDLER_MASK(SetSavingDotCodeFile, "Save e-Reader Dot Code...", CMDEN_GBA)
{
    static wxString savedotcodefile_path;
    wxFileDialog dlg(this, _("Select Dot Code file"), savedotcodefile_path, wxEmptyString,
        _(
                         "E-Reader Dot Code (*.bin;*.raw)|"
                         "*.bin;*.raw"),
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

    SetGenericPath(dlg, savedotcodefile_path);

    int ret = ShowModal(&dlg);

    if (ret != wxID_OK)
        return;

    savedotcodefile_path = dlg.GetPath();
    SetSaveDotCodeFile(UTF8(savedotcodefile_path));
}

static wxString batimp_path;

EVT_HANDLER_MASK(ImportBatteryFile, "Import battery file...", CMDEN_GB | CMDEN_GBA)
{
    if (!batimp_path.size())
        batimp_path = panel->bat_dir();

    wxFileDialog dlg(this, _("Select battery file"), batimp_path, wxEmptyString,
        _("Battery file (*.sav)|*.sav|Flash save (*.dat)|*.dat"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    SetGenericPath(dlg, batimp_path);

    int ret = ShowModal(&dlg);
    batimp_path = dlg.GetDirectory();

    if (ret != wxID_OK)
        return;

    wxString fn = dlg.GetPath();
    ret = wxMessageBox(_("Importing a battery file will erase any saved games (permanently after the next write). Do you want to continue?"),
        _("Confirm import"), wxYES_NO | wxICON_EXCLAMATION);

    if (ret == wxYES) {
        wxString msg;

        if (panel->emusys->emuReadBattery(UTF8(fn)))
            msg.Printf(_("Loaded battery %s"), fn.wc_str());
        else
            msg.Printf(_("Error loading battery %s"), fn.wc_str());

        systemScreenMessage(msg);
    }
}

EVT_HANDLER_MASK(ImportGamesharkCodeFile, "Import Game Shark code file...", CMDEN_GB | CMDEN_GBA)
{
    static wxString path;
    wxFileDialog dlg(this, _("Select code file"), path, wxEmptyString,
        panel->game_type() == IMAGE_GBA ? _("Game Shark Code File (*.spc;*.xpc)|*.spc;*.xpc") : _("Game Shark Code File (*.gcf)|*.gcf"),
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    SetGenericPath(dlg, path);

    int ret = ShowModal(&dlg);
    path = dlg.GetDirectory();

    if (ret != wxID_OK)
        return;

    wxString fn = dlg.GetPath();
    ret = wxMessageBox(_("Importing a code file will replace any loaded cheats. Do you want to continue?"),
        _("Confirm import"), wxYES_NO | wxICON_EXCLAMATION);

    if (ret == wxYES) {
        wxString msg;
        bool res;

        if (panel->game_type() == IMAGE_GB)
            // FIXME: this routine will not work on big-endian systems
            // if the underlying file format is little-endian
            // (fix in gb/gbCheats.cpp)
            res = gbCheatReadGSCodeFile(UTF8(fn));
        else {
            // need to select game first
            wxFFile f(fn, wxT("rb"));

            if (!f.IsOpened()) {
                wxLogError(_("Cannot open file %s"), fn.c_str());
                return;
            }

            // FIXME: in my code, I assume file format is little-endian
            // however, in core code, it is assumed to be native-endian
            uint32_t len;
            char buf[14];

            if (f.Read(&len, sizeof(len)) != sizeof(len) || wxUINT32_SWAP_ON_BE(len) != 14 || f.Read(buf, 14) != 14 || memcmp(buf, "SharkPortCODES", 14)) {
                wxLogError(_("Unsupported code file %s"), fn.c_str());
                return;
            }

            f.Seek(0x1e);

            if (f.Read(&len, sizeof(len)) != sizeof(len))
                len = 0;

            uint32_t game = 0;

            if (len > 1) {
                wxDialog* seldlg = GetXRCDialog("CodeSelect");
                wxControlWithItems* lst = XRCCTRL(*seldlg, "CodeList", wxControlWithItems);
                lst->Clear();

                while (len-- > 0) {
                    uint32_t slen;

                    if (f.Read(&slen, sizeof(slen)) != sizeof(slen) || slen > 1024) // arbitrary upper bound
                        break;

                    char buf2[1024];

                    if (f.Read(buf2, slen) != slen)
                        break;

                    lst->Append(wxString(buf2, wxConvLibc, slen));
                    uint32_t ncodes;

                    if (f.Read(&ncodes, sizeof(ncodes)) != sizeof(ncodes))
                        break;

                    for (; ncodes > 0; ncodes--) {
                        if (f.Read(&slen, sizeof(slen)) != sizeof(slen))
                            break;

                        f.Seek(slen, wxFromCurrent);

                        if (f.Read(&slen, sizeof(slen)) != sizeof(slen))
                            break;

                        f.Seek(slen + 4, wxFromCurrent);

                        if (f.Read(&slen, sizeof(slen)) != sizeof(slen))
                            break;

                        f.Seek(slen * 12, wxFromCurrent);
                    }
                }

                int sel = ShowModal(seldlg);

                if (sel != wxID_OK)
                    return;

                game = lst->GetSelection();

                if ((int)game == wxNOT_FOUND)
                    game = 0;
            }

            bool v3 = fn.size() >= 4 && wxString(fn.substr(fn.size() - 4)).IsSameAs(wxT(".xpc"), false);
            // FIXME: this routine will not work on big-endian systems
            // if the underlying file format is little-endian
            // (fix in gba/Cheats.cpp)
            res = cheatsImportGSACodeFile(UTF8(fn), game, v3);
        }

        if (res)
            msg.Printf(_("Loaded code file %s"), fn.wc_str());
        else
            msg.Printf(_("Error loading code file %s"), fn.wc_str());

        systemScreenMessage(msg);
    }
}

static wxString gss_path;

EVT_HANDLER_MASK(ImportGamesharkActionReplaySnapshot,
    "Import Game Shark Action Replay snapshot...", CMDEN_GB | CMDEN_GBA)
{
    wxFileDialog dlg(this, _("Select snapshot file"), gss_path, wxEmptyString,
        panel->game_type() == IMAGE_GBA ? _("Game Shark & PAC Snapshots (*.sps;*.xps)|*.sps;*.xps|Game Shark SP Snapshots (*.gsv)|*.gsv") : _("Game Boy Snapshot (*.gbs)|*.gbs"),
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    SetGenericPath(dlg, gss_path);

    int ret = ShowModal(&dlg);
    gss_path = dlg.GetDirectory();

    if (ret != wxID_OK)
        return;

    wxString fn = dlg.GetPath();
    ret = wxMessageBox(_("Importing a snapshot file will erase any saved games (permanently after the next write). Do you want to continue?"),
        _("Confirm import"), wxYES_NO | wxICON_EXCLAMATION);

    if (ret == wxYES) {
        wxString msg;
        bool res;

        if (panel->game_type() == IMAGE_GB)
            res = gbReadGSASnapshot(UTF8(fn));
        else {
            bool gsv = fn.size() >= 4 && wxString(fn.substr(fn.size() - 4)).IsSameAs(wxT(".gsv"), false);

            if (gsv)
                // FIXME: this will fail on big-endian machines if
                // file format is little-endian
                // fix in GBA.cpp
                res = CPUReadGSASPSnapshot(UTF8(fn));
            else
                // FIXME: this will fail on big-endian machines if
                // file format is little-endian
                // fix in GBA.cpp
                res = CPUReadGSASnapshot(UTF8(fn));
        }

        if (res)
            msg.Printf(_("Loaded snapshot file %s"), fn.wc_str());
        else
            msg.Printf(_("Error loading snapshot file %s"), fn.wc_str());

        systemScreenMessage(msg);
    }
}

EVT_HANDLER_MASK(ExportBatteryFile, "Export battery file...", CMDEN_GB | CMDEN_GBA)
{
    if (!batimp_path.size())
        batimp_path = panel->bat_dir();

    wxFileDialog dlg(this, _("Select battery file"), batimp_path, wxEmptyString,
        _("Battery file (*.sav)|*.sav|Flash save (*.dat)|*.dat"), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

    SetGenericPath(dlg, batimp_path);

    int ret = ShowModal(&dlg);
    batimp_path = dlg.GetDirectory();

    if (ret != wxID_OK)
        return;

    wxString fn = dlg.GetPath();
    wxString msg;

    if (panel->emusys->emuWriteBattery(UTF8(fn)))
        msg.Printf(_("Wrote battery %s"), fn.wc_str());
    else
        msg.Printf(_("Error writing battery %s"), fn.wc_str());

    systemScreenMessage(msg);
}

EVT_HANDLER_MASK(ExportGamesharkSnapshot, "Export GameShark snapshot...", CMDEN_GBA)
{
    if (eepromInUse) {
        wxLogError(_("EEPROM saves cannot be exported"));
        return;
    }

    wxString def_name = panel->game_name();
    def_name.append(wxT(".sps"));
    wxFileDialog dlg(this, _("Select snapshot file"), gss_path, def_name,
        _("Game Shark Snapshot (*.sps)|*.sps"), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

    SetGenericPath(dlg, gss_path);

    int ret = ShowModal(&dlg);
    gss_path = dlg.GetDirectory();

    if (ret != wxID_OK)
        return;

    wxString fn = dlg.GetPath();
    wxDialog* infodlg = GetXRCDialog("ExportSPS");
    wxTextCtrl *tit = XRCCTRL(*infodlg, "Title", wxTextCtrl),
               *dsc = XRCCTRL(*infodlg, "Description", wxTextCtrl),
               *n = XRCCTRL(*infodlg, "Notes", wxTextCtrl);
    tit->SetValue(wxString((const char*)&g_rom[0xa0], wxConvLibc, 12));
    dsc->SetValue(wxDateTime::Now().Format(wxT("%c")));
    n->SetValue(_("Exported from Visual Boy Advance-M"));

    if (ShowModal(infodlg) != wxID_OK)
        return;

    wxString msg;

    // FIXME: this will fail on big-endian machines if file format is
    // little-endian
    // fix in GBA.cpp
    if (CPUWriteGSASnapshot(fn.utf8_str(), tit->GetValue().utf8_str(),
            dsc->GetValue().utf8_str(), n->GetValue().utf8_str()))
        msg.Printf(_("Saved snapshot file %s"), fn.wc_str());
    else
        msg.Printf(_("Error saving snapshot file %s"), fn.wc_str());

    systemScreenMessage(msg);
}

EVT_HANDLER_MASK(ScreenCapture, "Screen capture...", CMDEN_GB | CMDEN_GBA)
{
    wxString scap_path = GetGamePath(OPTION(kGenScreenshotDir));
    wxString def_name = panel->game_name();

    const int capture_format = OPTION(kPrefCaptureFormat);
    if (capture_format == 0)
        def_name.append(".png");
    else
        def_name.append(".bmp");

    wxFileDialog dlg(this, _("Select output file"), scap_path, def_name,
        _("PNG images|*.png|BMP images|*.bmp"), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

    SetGenericPath(dlg, scap_path);

    dlg.SetFilterIndex(capture_format);
    int ret = ShowModal(&dlg);
    scap_path = dlg.GetDirectory();

    if (ret != wxID_OK)
        return;

    wxString fn = dlg.GetPath();
    int fmt = dlg.GetFilterIndex();

    if (fn.size() >= 4) {
        if (wxString(fn.substr(fn.size() - 4)).IsSameAs(wxT(".bmp"), false))
            fmt = 1;
        else if (wxString(fn.substr(fn.size() - 4)).IsSameAs(wxT(".png"), false))
            fmt = 0;
    }

    if (fmt == 0)
        panel->emusys->emuWritePNG(UTF8(fn));
    else
        panel->emusys->emuWriteBMP(UTF8(fn));

    wxString msg;
    msg.Printf(_("Wrote snapshot %s"), fn.wc_str());
    systemScreenMessage(msg);
}

EVT_HANDLER_MASK(RecordSoundStartRecording, "Start sound recording...", CMDEN_NSREC)
{
#ifndef NO_FFMPEG
    static wxString sound_exts;
    static int sound_extno;
    static wxString sound_path;

    if (!sound_exts.size()) {
        sound_extno = -1;
        int extno = 0;

        std::vector<char *> fmts = recording::getSupAudNames();
        std::vector<char *> exts = recording::getSupAudExts();

        for (size_t i = 0; i < fmts.size(); ++i)
        {
            sound_exts.append(wxString(fmts[i], wxConvLibc));
            sound_exts.append(_(" files ("));
            wxString ext(exts[i], wxConvLibc);
            ext.Replace(wxT(","), wxT(";*."));
            ext.insert(0, wxT("*."));

            if (sound_extno < 0 && ext.find(wxT("*.wav")) != wxString::npos)
                sound_extno = extno;

            sound_exts.append(ext);
            sound_exts.append(wxT(")|"));
            sound_exts.append(ext);
            sound_exts.append(wxT('|'));
            extno++;
        }

        sound_exts.append(wxALL_FILES);

        if (sound_extno < 0)
            sound_extno = extno;
    }

    sound_path = GetGamePath(OPTION(kGenRecordingDir));
    wxString def_name = panel->game_name();
    wxString extoff = sound_exts;

    for (int i = 0; i < sound_extno; i++) {
        extoff = extoff.Mid(extoff.Find(wxT('|')) + 1);
        extoff = extoff.Mid(extoff.Find(wxT('|')) + 1);
    }

    extoff = extoff.Mid(extoff.Find(wxT('|')) + 2); // skip *
    def_name += extoff.Left(wxStrcspn(extoff, wxT(";|")));
    wxFileDialog dlg(this, _("Select output file"), sound_path, def_name,
        sound_exts, wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

    SetGenericPath(dlg, sound_path);

    dlg.SetFilterIndex(sound_extno);
    int ret = ShowModal(&dlg);
    sound_extno = dlg.GetFilterIndex();
    sound_path = dlg.GetDirectory();

    if (ret != wxID_OK)
        return;

    panel->StartSoundRecording(dlg.GetPath());
#endif
}

EVT_HANDLER_MASK(RecordSoundStopRecording, "Stop sound recording", CMDEN_SREC)
{
#ifndef NO_FFMPEG
    panel->StopSoundRecording();
#endif
}

EVT_HANDLER_MASK(RecordAVIStartRecording, "Start video recording...", CMDEN_NVREC)
{
#ifndef NO_FFMPEG
    static wxString vid_exts;
    static int vid_extno;
    static wxString vid_path;

    if (!vid_exts.size()) {
        vid_extno = -1;
        int extno = 0;

        std::vector<char *> fmts = recording::getSupVidNames();
        std::vector<char *> exts = recording::getSupVidExts();

        for (size_t i = 0; i < fmts.size(); ++i)
        {
            vid_exts.append(wxString(fmts[i], wxConvLibc));
            vid_exts.append(_(" files ("));
            wxString ext(exts[i], wxConvLibc);
            ext.Replace(wxT(","), wxT(";*."));
            ext.insert(0, wxT("*."));

            if (vid_extno < 0 && ext.find(wxT("*.avi")) != wxString::npos)
                vid_extno = extno;

            vid_exts.append(ext);
            vid_exts.append(wxT(")|"));
            vid_exts.append(ext);
            vid_exts.append(wxT('|'));
            extno++;
        }

        vid_exts.append(wxALL_FILES);

        if (vid_extno < 0)
            vid_extno = extno;
    }

    vid_path = GetGamePath(OPTION(kGenRecordingDir));
    wxString def_name = panel->game_name();
    wxString extoff = vid_exts;

    for (int i = 0; i < vid_extno; i++) {
        extoff = extoff.Mid(extoff.Find(wxT('|')) + 1);
        extoff = extoff.Mid(extoff.Find(wxT('|')) + 1);
    }

    extoff = extoff.Mid(extoff.Find(wxT('|')) + 2); // skip *
    def_name += extoff.Left(wxStrcspn(extoff, wxT(";|")));
    wxFileDialog dlg(this, _("Select output file"), vid_path, def_name,
        vid_exts, wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

    SetGenericPath(dlg, vid_path);

    dlg.SetFilterIndex(vid_extno);
    int ret = ShowModal(&dlg);
    vid_extno = dlg.GetFilterIndex();
    vid_path = dlg.GetDirectory();

    if (ret != wxID_OK)
        return;

    panel->StartVidRecording(dlg.GetPath());
#endif
}

EVT_HANDLER_MASK(RecordAVIStopRecording, "Stop video recording", CMDEN_VREC)
{
#ifndef NO_FFMPEG
    panel->StopVidRecording();
#endif
}

EVT_HANDLER_MASK(RecordMovieStartRecording, "Start game recording...", CMDEN_NGREC)
{
    static wxString mov_exts;
    static int mov_extno;
    static wxString mov_path;

    if (!mov_exts.size()) {
        mov_extno = -1;
        int extno = 0;

        std::vector<char*> fmts = getSupMovNamesToRecord();
        std::vector<char*> exts = getSupMovExtsToRecord();

        for (auto&& fmt : fmts)
        {
            mov_exts.append(wxString(fmt, wxConvLibc));
            mov_exts.append(_(" files ("));
            wxString ext(exts[extno], wxConvLibc);
            ext.Replace(wxT(","), wxT(";*."));
            ext.insert(0, wxT("*."));

            if (mov_extno < 0 && ext.find(wxT("*.vmv")) != wxString::npos)
                mov_extno = extno;

            mov_exts.append(ext);
            mov_exts.append(wxT(")|"));
            mov_exts.append(ext);
            mov_exts.append(wxT('|'));
            extno++;
        }

        mov_exts.append(wxALL_FILES);

        if (mov_extno < 0)
            mov_extno = extno;
    }

    mov_path = GetGamePath(OPTION(kGenRecordingDir));
    wxString def_name = panel->game_name();
    wxString extoff = mov_exts;

    for (int i = 0; i < mov_extno; i++) {
        extoff = extoff.Mid(extoff.Find(wxT('|')) + 1);
        extoff = extoff.Mid(extoff.Find(wxT('|')) + 1);
    }

    extoff = extoff.Mid(extoff.Find(wxT('|')) + 2); // skip *
    def_name += extoff.Left(wxStrcspn(extoff, wxT(";|")));
    wxFileDialog dlg(this, _("Select output file"), mov_path, def_name,
        mov_exts, wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

    SetGenericPath(dlg, mov_path);

    dlg.SetFilterIndex(mov_extno);
    int ret = ShowModal(&dlg);
    mov_extno = dlg.GetFilterIndex();
    mov_path = dlg.GetDirectory();

    if (ret != wxID_OK)
        return;

    systemStartGameRecording(dlg.GetPath(), getSupMovFormatsToRecord()[mov_extno]);
}

EVT_HANDLER_MASK(RecordMovieStopRecording, "Stop game recording", CMDEN_GREC)
{
    systemStopGameRecording();
}

EVT_HANDLER_MASK(PlayMovieStartPlaying, "Start playing movie...", CMDEN_NGREC | CMDEN_NGPLAY)
{
    static wxString mov_exts;
    static int mov_extno;
    static wxString mov_path;

    if (!mov_exts.size()) {
        mov_extno = -1;
        int extno = 0;

        std::vector<char*> fmts = getSupMovNamesToPlayback();
        std::vector<char*> exts = getSupMovExtsToPlayback();

        for (size_t i = 0; i < fmts.size(); ++i)
        {
            mov_exts.append(wxString(fmts[i], wxConvLibc));
            mov_exts.append(_(" files ("));
            wxString ext(exts[i], wxConvLibc);
            ext.Replace(wxT(","), wxT(";*."));
            ext.insert(0, wxT("*."));

            if (mov_extno < 0 && ext.find(wxT("*.vmv")) != wxString::npos)
                mov_extno = extno;

            mov_exts.append(ext);
            mov_exts.append(wxT(")|"));
            mov_exts.append(ext);
            mov_exts.append(wxT('|'));
            extno++;
        }

        mov_exts.append(wxALL_FILES);

        if (mov_extno < 0)
            mov_extno = extno;
    }

    mov_path = GetGamePath(OPTION(kGenRecordingDir));
    systemStopGamePlayback();
    wxString def_name = panel->game_name();
    wxString extoff = mov_exts;

    for (int i = 0; i < mov_extno; i++) {
        extoff = extoff.Mid(extoff.Find(wxT('|')) + 1);
        extoff = extoff.Mid(extoff.Find(wxT('|')) + 1);
    }

    extoff = extoff.Mid(extoff.Find(wxT('|')) + 2); // skip *
    def_name += extoff.Left(wxStrcspn(extoff, wxT(";|")));
    wxFileDialog dlg(this, _("Select file"), mov_path, def_name,
        mov_exts, wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    SetGenericPath(dlg, mov_path);

    dlg.SetFilterIndex(mov_extno);
    int ret = ShowModal(&dlg);
    mov_extno = dlg.GetFilterIndex();
    mov_path = dlg.GetDirectory();

    if (ret != wxID_OK)
        return;

    systemStartGamePlayback(dlg.GetPath(), getSupMovFormatsToPlayback()[mov_extno]);
}

EVT_HANDLER_MASK(PlayMovieStopPlaying, "Stop playing movie", CMDEN_GPLAY)
{
    systemStopGamePlayback();
}

// formerly Close
EVT_HANDLER_MASK(wxID_CLOSE, "Close", CMDEN_GB | CMDEN_GBA)
{
    panel->UnloadGame();
}

// formerly Exit
EVT_HANDLER(wxID_EXIT, "Exit")
{
    Close(false);
}

// Emulation menu
EVT_HANDLER(Pause, "Pause (toggle)")
{
    bool menuPress = false;
    GetMenuOptionBool("Pause", &menuPress);
    toggleBooleanVar(&menuPress, &paused);
    SetMenuOption("Pause", paused ? 1 : 0);

    if (paused)
        panel->Pause();
    else if (!IsPaused())
        panel->Resume();

    // undo next-frame's zeroing of frameskip
    const int frame_skip = OPTION(kPrefFrameSkip);
    if (frame_skip != -1) {
        systemFrameSkip = frame_skip;
    }
}

// new
EVT_HANDLER_MASK(EmulatorSpeedupToggle, "Turbo mode (toggle)", CMDEN_GB | CMDEN_GBA)
{
    bool menuPress = false;
    GetMenuOptionBool("EmulatorSpeedupToggle", &menuPress);
    toggleBooleanVar(&menuPress, &turbo);
    SetMenuOption("EmulatorSpeedupToggle", turbo ? 1 : 0);
}

EVT_HANDLER_MASK(Reset, "Reset", CMDEN_GB | CMDEN_GBA)
{
    panel->emusys->emuReset();
    // systemScreenMessage("Reset");
}

EVT_HANDLER(ToggleFullscreen, "Full screen (toggle)")
{
    panel->ShowFullScreen(!IsFullScreen());
}

EVT_HANDLER(JoypadAutofireA, "Autofire A (toggle)")
{
    bool menuPress = false;
    GetMenuOptionBool("JoypadAutofireA", &menuPress);
    toggleBitVar(&menuPress, &autofire, KEYM_A);
    SetMenuOption("JoypadAutofireA", menuPress ? 1 : 0);
    GetMenuOptionInt("JoypadAutofireA", &autofire, KEYM_A);
}

EVT_HANDLER(JoypadAutofireB, "Autofire B (toggle)")
{
    bool menuPress = false;
    GetMenuOptionBool("JoypadAutofireB", &menuPress);
    toggleBitVar(&menuPress, &autofire, KEYM_B);
    SetMenuOption("JoypadAutofireB", menuPress ? 1 : 0);
    GetMenuOptionInt("JoypadAutofireB", &autofire, KEYM_B);
}

EVT_HANDLER(JoypadAutofireL, "Autofire L (toggle)")
{
    bool menuPress = false;
    GetMenuOptionBool("JoypadAutofireL", &menuPress);
    toggleBitVar(&menuPress, &autofire, KEYM_L);
    SetMenuOption("JoypadAutofireL", menuPress ? 1 : 0);
    GetMenuOptionInt("JoypadAutofireL", &autofire, KEYM_L);
}

EVT_HANDLER(JoypadAutofireR, "Autofire R (toggle)")
{
    bool menuPress = false;
    GetMenuOptionBool("JoypadAutofireR", &menuPress);
    toggleBitVar(&menuPress, &autofire, KEYM_R);
    SetMenuOption("JoypadAutofireR", menuPress ? 1 : 0);
    GetMenuOptionInt("JoypadAutofireR", &autofire, KEYM_R);
}

EVT_HANDLER(JoypadAutoholdUp, "Autohold Up (toggle)")
{
    bool menuPress = false;
    char keyName[] = "JoypadAutoholdUp";
    int keym = KEYM_UP;
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &autohold, keym);
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &autohold, keym);
}

EVT_HANDLER(JoypadAutoholdDown, "Autohold Down (toggle)")
{
    bool menuPress = false;
    char keyName[] = "JoypadAutoholdDown";
    int keym = KEYM_DOWN;
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &autohold, keym);
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &autohold, keym);
}

EVT_HANDLER(JoypadAutoholdLeft, "Autohold Left (toggle)")
{
    bool menuPress = false;
    char keyName[] = "JoypadAutoholdLeft";
    int keym = KEYM_LEFT;
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &autohold, keym);
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &autohold, keym);
}

EVT_HANDLER(JoypadAutoholdRight, "Autohold Right (toggle)")
{
    bool menuPress = false;
    char keyName[] = "JoypadAutoholdRight";
    int keym = KEYM_RIGHT;
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &autohold, keym);
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &autohold, keym);
}

EVT_HANDLER(JoypadAutoholdA, "Autohold A (toggle)")
{
    bool menuPress = false;
    char keyName[] = "JoypadAutoholdA";
    int keym = KEYM_A;
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &autohold, keym);
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &autohold, keym);
}

EVT_HANDLER(JoypadAutoholdB, "Autohold B (toggle)")
{
    bool menuPress = false;
    char keyName[] = "JoypadAutoholdB";
    int keym = KEYM_B;
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &autohold, keym);
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &autohold, keym);
}

EVT_HANDLER(JoypadAutoholdL, "Autohold L (toggle)")
{
    bool menuPress = false;
    char keyName[] = "JoypadAutoholdL";
    int keym = KEYM_L;
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &autohold, keym);
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &autohold, keym);
}

EVT_HANDLER(JoypadAutoholdR, "Autohold R (toggle)")
{
    bool menuPress = false;
    char keyName[] = "JoypadAutoholdR";
    int keym = KEYM_R;
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &autohold, keym);
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &autohold, keym);
}

EVT_HANDLER(JoypadAutoholdSelect, "Autohold Select (toggle)")
{
    bool menuPress = false;
    char keyName[] = "JoypadAutoholdSelect";
    int keym = KEYM_SELECT;
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &autohold, keym);
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &autohold, keym);
}

EVT_HANDLER(JoypadAutoholdStart, "Autohold Start (toggle)")
{
    bool menuPress = false;
    char keyName[] = "JoypadAutoholdStart";
    int keym = KEYM_START;
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &autohold, keym);
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &autohold, keym);
}

#include "background-input.h"

EVT_HANDLER(AllowKeyboardBackgroundInput, "Allow keyboard background input (toggle)")
{
    GetMenuOptionConfig("AllowKeyboardBackgroundInput",
                        config::OptionID::kUIAllowKeyboardBackgroundInput);

    disableKeyboardBackgroundInput();
    if (OPTION(kUIAllowKeyboardBackgroundInput)) {
        if (panel && panel->panel) {
            enableKeyboardBackgroundInput(panel->panel->GetWindow()->GetEventHandler());
        }
    }
}

EVT_HANDLER(AllowJoystickBackgroundInput, "Allow joystick background input (toggle)")
{
    GetMenuOptionConfig("AllowKeyboardBackgroundInput",
                        config::OptionID::kUIAllowJoystickBackgroundInput);
}

EVT_HANDLER_MASK(LoadGameRecent, "Load most recent save", CMDEN_SAVST)
{
    panel->LoadState();
}

EVT_HANDLER(LoadGameAutoLoad, "Auto load most recent save (toggle)")
{
    GetMenuOptionConfig("LoadGameAutoLoad", config::OptionID::kGenAutoLoadLastState);
}

EVT_HANDLER_MASK(LoadGame01, "Load saved state 1", CMDEN_SAVST)
{
    panel->LoadState(1);
}

EVT_HANDLER_MASK(LoadGame02, "Load saved state 2", CMDEN_SAVST)
{
    panel->LoadState(2);
}

EVT_HANDLER_MASK(LoadGame03, "Load saved state 3", CMDEN_SAVST)
{
    panel->LoadState(3);
}

EVT_HANDLER_MASK(LoadGame04, "Load saved state 4", CMDEN_SAVST)
{
    panel->LoadState(4);
}

EVT_HANDLER_MASK(LoadGame05, "Load saved state 5", CMDEN_SAVST)
{
    panel->LoadState(5);
}

EVT_HANDLER_MASK(LoadGame06, "Load saved state 6", CMDEN_SAVST)
{
    panel->LoadState(6);
}

EVT_HANDLER_MASK(LoadGame07, "Load saved state 7", CMDEN_SAVST)
{
    panel->LoadState(7);
}

EVT_HANDLER_MASK(LoadGame08, "Load saved state 8", CMDEN_SAVST)
{
    panel->LoadState(8);
}

EVT_HANDLER_MASK(LoadGame09, "Load saved state 9", CMDEN_SAVST)
{
    panel->LoadState(9);
}

EVT_HANDLER_MASK(LoadGame10, "Load saved state 10", CMDEN_SAVST)
{
    panel->LoadState(10);
}

static wxString st_dir;

EVT_HANDLER_MASK(Load, "Load state...", CMDEN_GB | CMDEN_GBA)
{
    if (st_dir.empty())
        st_dir = panel->state_dir();

    wxFileDialog dlg(this, _("Select state file"), st_dir, wxEmptyString,
        _("Visual Boy Advance saved game files|*.sgm"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    SetGenericPath(dlg, st_dir);

    int ret = ShowModal(&dlg);
    st_dir = dlg.GetDirectory();

    if (ret != wxID_OK)
        return;

    panel->LoadState(dlg.GetPath());
}

// new
EVT_HANDLER(KeepSaves, "Do not load battery saves (toggle)")
{
    bool menuPress = false;
    GetMenuOptionBool("KeepSaves", &menuPress);
    toggleBitVar(&menuPress, &coreOptions.skipSaveGameBattery, 1);
    SetMenuOption("KeepSaves", menuPress ? 1 : 0);
    GetMenuOptionInt("KeepSaves", &coreOptions.skipSaveGameBattery, 1);
    update_opts();
}

// new
EVT_HANDLER(KeepCheats, "Do not change cheat list (toggle)")
{
    bool menuPress = false;
    GetMenuOptionBool("KeepCheats", &menuPress);
    toggleBitVar(&menuPress, &coreOptions.skipSaveGameCheats, 1);
    SetMenuOption("KeepCheats", menuPress ? 1 : 0);
    GetMenuOptionInt("KeepCheats", &coreOptions.skipSaveGameCheats, 1);
    update_opts();
}

EVT_HANDLER_MASK(SaveGameOldest, "Save state to oldest slot", CMDEN_GB | CMDEN_GBA)
{
    panel->SaveState();
}

EVT_HANDLER_MASK(SaveGame01, "Save state 1", CMDEN_GB | CMDEN_GBA)
{
    panel->SaveState(1);
}

EVT_HANDLER_MASK(SaveGame02, "Save state 2", CMDEN_GB | CMDEN_GBA)
{
    panel->SaveState(2);
}

EVT_HANDLER_MASK(SaveGame03, "Save state 3", CMDEN_GB | CMDEN_GBA)
{
    panel->SaveState(3);
}

EVT_HANDLER_MASK(SaveGame04, "Save state 4", CMDEN_GB | CMDEN_GBA)
{
    panel->SaveState(4);
}

EVT_HANDLER_MASK(SaveGame05, "Save state 5", CMDEN_GB | CMDEN_GBA)
{
    panel->SaveState(5);
}

EVT_HANDLER_MASK(SaveGame06, "Save state 6", CMDEN_GB | CMDEN_GBA)
{
    panel->SaveState(6);
}

EVT_HANDLER_MASK(SaveGame07, "Save state 7", CMDEN_GB | CMDEN_GBA)
{
    panel->SaveState(7);
}

EVT_HANDLER_MASK(SaveGame08, "Save state 8", CMDEN_GB | CMDEN_GBA)
{
    panel->SaveState(8);
}

EVT_HANDLER_MASK(SaveGame09, "Save state 9", CMDEN_GB | CMDEN_GBA)
{
    panel->SaveState(9);
}

EVT_HANDLER_MASK(SaveGame10, "Save state 10", CMDEN_GB | CMDEN_GBA)
{
    panel->SaveState(10);
}

EVT_HANDLER_MASK(Save, "Save state as...", CMDEN_GB | CMDEN_GBA)
{
    if (st_dir.empty())
        st_dir = panel->state_dir();

    wxFileDialog dlg(this, _("Select state file"), st_dir, wxEmptyString,
        _("Visual Boy Advance saved game files|*.sgm"), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

    SetGenericPath(dlg, st_dir);

    int ret = ShowModal(&dlg);
    st_dir = dlg.GetDirectory();

    if (ret != wxID_OK)
        return;

    panel->SaveState(dlg.GetPath());
}

static int state_slot = 0;

// new
EVT_HANDLER_MASK(LoadGameSlot, "Load current state slot", CMDEN_GB | CMDEN_GBA)
{
    panel->LoadState(state_slot + 1);
}

// new
EVT_HANDLER_MASK(SaveGameSlot, "Save current state slot", CMDEN_GB | CMDEN_GBA)
{
    panel->SaveState(state_slot + 1);
}

// new
EVT_HANDLER_MASK(IncrGameSlot, "Increase state slot number", CMDEN_GB | CMDEN_GBA)
{
    state_slot = (state_slot + 1) % 10;

    wxString msg;
    msg.Printf(_("Current state slot #%d"), state_slot);
    systemScreenMessage(msg);
}

// new
EVT_HANDLER_MASK(DecrGameSlot, "Decrease state slot number", CMDEN_GB | CMDEN_GBA)
{
    state_slot = (state_slot + 9) % 10;

    wxString msg;
    msg.Printf(_("Current state slot #%d"), state_slot);
    systemScreenMessage(msg);
}

// new
EVT_HANDLER_MASK(IncrGameSlotSave, "Increase state slot number and save", CMDEN_GB | CMDEN_GBA)
{
    state_slot = (state_slot + 1) % 10;
    panel->SaveState(state_slot + 1);

    wxString msg;
    msg.Printf(_("Current state slot #%d"), state_slot);
    systemScreenMessage(msg);
}

EVT_HANDLER_MASK(Rewind, "Rewind", CMDEN_REWIND)
{
    int rew_st = (panel->next_rewind_state + NUM_REWINDS - 1) % NUM_REWINDS;

    // if within 5 seconds of last one, and > 1 state, delete last state & move back
    // FIXME: 5 should actually be user-configurable
    // maybe instead of 5, 10% of rewind_interval
    if (panel->num_rewind_states > 1 && (gopts.rewind_interval <= 5 || (int)panel->rewind_time / 6 > gopts.rewind_interval - 5)) {
        --panel->num_rewind_states;
        panel->next_rewind_state = rew_st;

        if (gopts.rewind_interval > 5)
            rew_st = (rew_st + NUM_REWINDS - 1) % NUM_REWINDS;
    }

    panel->emusys->emuReadMemState(&panel->rewind_mem[rew_st * REWIND_SIZE],
        REWIND_SIZE);
    InterframeCleanup();
    // FIXME: if(paused) blank screen
    panel->do_rewind = false;
    panel->rewind_time = gopts.rewind_interval * 6;
    //    systemScreenMessage(_("Rewinded"));
}

EVT_HANDLER_MASK(CheatsList, "List cheats...", CMDEN_GB | CMDEN_GBA)
{
    wxDialog* dlg = GetXRCDialog("CheatList");
    ShowModal(dlg);
}

EVT_HANDLER_MASK(CheatsSearch, "Create cheat...", CMDEN_GB | CMDEN_GBA)
{
    wxDialog* dlg = GetXRCDialog("CheatCreate");
    ShowModal(dlg);
}

// new
EVT_HANDLER(CheatsAutoSaveLoad, "Auto save/load cheats (toggle)")
{
    GetMenuOptionConfig("CheatsAutoSaveLoad", config::OptionID::kPrefAutoSaveLoadCheatList);
}

// was CheatsDisable
// changed for convenience to match internal variable functionality
EVT_HANDLER(CheatsEnable, "Enable cheats (toggle)")
{
    bool menuPress = false;
    GetMenuOptionBool("CheatsEnable", &menuPress);
    toggleBitVar(&menuPress, &coreOptions.cheatsEnabled, 1);
    SetMenuOption("CheatsEnable", menuPress ? 1 : 0);
    GetMenuOptionInt("CheatsEnable", &coreOptions.cheatsEnabled, 1);
    update_opts();
}

EVT_HANDLER(ColorizerHack, "Enable Colorizer Hack (toggle)")
{
    GetMenuOptionConfig("ColorizerHack", config::OptionID::kGBColorizerHack);
    if (OPTION(kGBColorizerHack) && OPTION(kPrefUseBiosGB)) {
        wxLogError(
            _("Cannot use Colorizer Hack when Game Boy BIOS File is enabled."));
        SetMenuOption("ColorizerHack", 0);
        OPTION(kGBColorizerHack) = false;
    }
}

// Debug menu
EVT_HANDLER_MASK(VideoLayersBG0, "Video layer BG0 (toggle)", CMDEN_GB | CMDEN_GBA)
{
    bool menuPress = false;
    char keyName[] = "VideoLayersBG0";
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &coreOptions.layerSettings, (1 << 8));
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &coreOptions.layerSettings, (1 << 8));
    coreOptions.layerEnable = DISPCNT & coreOptions.layerSettings;
    CPUUpdateRenderBuffers(false);
}

EVT_HANDLER_MASK(VideoLayersBG1, "Video layer BG1 (toggle)", CMDEN_GB | CMDEN_GBA)
{
    bool menuPress = false;
    char keyName[] = "VideoLayersBG1";
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &coreOptions.layerSettings, (1 << 9));
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &coreOptions.layerSettings, (1 << 9));
    coreOptions.layerEnable = DISPCNT & coreOptions.layerSettings;
    CPUUpdateRenderBuffers(false);
}

EVT_HANDLER_MASK(VideoLayersBG2, "Video layer BG2 (toggle)", CMDEN_GB | CMDEN_GBA)
{
    bool menuPress = false;
    char keyName[] = "VideoLayersBG2";
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &coreOptions.layerSettings, (1 << 10));
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &coreOptions.layerSettings, (1 << 10));
    coreOptions.layerEnable = DISPCNT & coreOptions.layerSettings;
    CPUUpdateRenderBuffers(false);
}

EVT_HANDLER_MASK(VideoLayersBG3, "Video layer BG3 (toggle)", CMDEN_GB | CMDEN_GBA)
{
    bool menuPress = false;
    char keyName[] = "VideoLayersBG3";
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &coreOptions.layerSettings, (1 << 11));
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &coreOptions.layerSettings, (1 << 11));
    coreOptions.layerEnable = DISPCNT & coreOptions.layerSettings;
    CPUUpdateRenderBuffers(false);
}

EVT_HANDLER_MASK(VideoLayersOBJ, "Video layer OBJ (toggle)", CMDEN_GB | CMDEN_GBA)
{
    bool menuPress = false;
    char keyName[] = "VideoLayersOBJ";
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &coreOptions.layerSettings, (1 << 12));
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &coreOptions.layerSettings, (1 << 12));
    coreOptions.layerEnable = DISPCNT & coreOptions.layerSettings;
    CPUUpdateRenderBuffers(false);
}

EVT_HANDLER_MASK(VideoLayersWIN0, "Video layer WIN0 (toggle)", CMDEN_GB | CMDEN_GBA)
{
    bool menuPress = false;
    char keyName[] = "VideoLayersWIN0";
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &coreOptions.layerSettings, (1 << 13));
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &coreOptions.layerSettings, (1 << 13));
    coreOptions.layerEnable = DISPCNT & coreOptions.layerSettings;
    CPUUpdateRenderBuffers(false);
}

EVT_HANDLER_MASK(VideoLayersWIN1, "Video layer WIN1 (toggle)", CMDEN_GB | CMDEN_GBA)
{
    bool menuPress = false;
    char keyName[] = "VideoLayersWIN1";
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &coreOptions.layerSettings, (1 << 14));
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &coreOptions.layerSettings, (1 << 14));
    coreOptions.layerEnable = DISPCNT & coreOptions.layerSettings;
    CPUUpdateRenderBuffers(false);
}

EVT_HANDLER_MASK(VideoLayersOBJWIN, "Video layer OBJWIN (toggle)", CMDEN_GB | CMDEN_GBA)
{
    bool menuPress = false;
    char keyName[] = "VideoLayersOBJWIN";
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &coreOptions.layerSettings, (1 << 15));
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &coreOptions.layerSettings, (1 << 15));
    coreOptions.layerEnable = DISPCNT & coreOptions.layerSettings;
    CPUUpdateRenderBuffers(false);
}

EVT_HANDLER_MASK(VideoLayersReset, "Show all video layers", CMDEN_GB | CMDEN_GBA)
{
#define set_vl(s)                                     \
    do {                                              \
        int id = XRCID(s);                            \
        for (size_t i = 0; i < checkable_mi.size(); i++) \
            if (checkable_mi[i].cmd == id) {          \
                checkable_mi[i].mi->Check(true);      \
                break;                                \
            }                                         \
    } while (0)
    coreOptions.layerSettings = 0x7f00;
    coreOptions.layerEnable = DISPCNT & coreOptions.layerSettings;
    set_vl("VideoLayersBG0");
    set_vl("VideoLayersBG1");
    set_vl("VideoLayersBG2");
    set_vl("VideoLayersBG3");
    set_vl("VideoLayersOBJ");
    set_vl("VideoLayersWIN0");
    set_vl("VideoLayersWIN1");
    set_vl("VideoLayersOBJWIN");
    CPUUpdateRenderBuffers(false);
}

EVT_HANDLER_MASK(SoundChannel1, "Sound Channel 1 (toggle)", CMDEN_GB | CMDEN_GBA)
{
    bool menuPress = false;
    char keyName[] = "SoundChannel1";
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &gopts.sound_en, (1 << 0));
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &gopts.sound_en, (1 << 0));
    soundSetEnable(gopts.sound_en);
    update_opts();
}

EVT_HANDLER_MASK(SoundChannel2, "Sound Channel 2 (toggle)", CMDEN_GB | CMDEN_GBA)
{
    bool menuPress = false;
    char keyName[] = "SoundChannel2";
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &gopts.sound_en, (1 << 1));
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &gopts.sound_en, (1 << 1));
    soundSetEnable(gopts.sound_en);
    update_opts();
}

EVT_HANDLER_MASK(SoundChannel3, "Sound Channel 3 (toggle)", CMDEN_GB | CMDEN_GBA)
{
    bool menuPress = false;
    char keyName[] = "SoundChannel3";
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &gopts.sound_en, (1 << 2));
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &gopts.sound_en, (1 << 2));
    soundSetEnable(gopts.sound_en);
    update_opts();
}

EVT_HANDLER_MASK(SoundChannel4, "Sound Channel 4 (toggle)", CMDEN_GB | CMDEN_GBA)
{
    bool menuPress = false;
    char keyName[] = "SoundChannel4";
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &gopts.sound_en, (1 << 3));
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &gopts.sound_en, (1 << 3));
    soundSetEnable(gopts.sound_en);
    update_opts();
}

EVT_HANDLER_MASK(DirectSoundA, "Direct Sound A (toggle)", CMDEN_GBA)
{
    bool menuPress = false;
    char keyName[] = "DirectSoundA";
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &gopts.sound_en, (1 << 8));
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &gopts.sound_en, (1 << 8));
    soundSetEnable(gopts.sound_en);
    update_opts();
}

EVT_HANDLER_MASK(DirectSoundB, "Direct Sound B (toggle)", CMDEN_GBA)
{
    bool menuPress = false;
    char keyName[] = "DirectSoundB";
    GetMenuOptionBool(keyName, &menuPress);
    toggleBitVar(&menuPress, &gopts.sound_en, (1 << 9));
    SetMenuOption(keyName, menuPress ? 1 : 0);
    GetMenuOptionInt(keyName, &gopts.sound_en, (1 << 9));
    soundSetEnable(gopts.sound_en);
    update_opts();
}

EVT_HANDLER(ToggleSound, "Enable/disable all sound channels")
{
    bool en = gopts.sound_en == 0;
    gopts.sound_en = en ? 0x30f : 0;
    SetMenuOption("SoundChannel1", en);
    SetMenuOption("SoundChannel2", en);
    SetMenuOption("SoundChannel3", en);
    SetMenuOption("SoundChannel4", en);
    SetMenuOption("DirectSoundA", en);
    SetMenuOption("DirectSoundB", en);
    soundSetEnable(gopts.sound_en);
    update_opts();
    systemScreenMessage(en ? _("Sound enabled") : _("Sound disabled"));
}

EVT_HANDLER(IncreaseVolume, "Increase volume")
{
    OPTION(kSoundVolume) += 5;

}

EVT_HANDLER(DecreaseVolume, "Decrease volume")
{
    OPTION(kSoundVolume) -= 5;
}

EVT_HANDLER_MASK(NextFrame, "Next Frame", CMDEN_GB | CMDEN_GBA)
{
    SetMenuOption("Pause", true);
    paused = true;
    pause_next = true;

    if (!IsPaused())
        panel->Resume();

    systemFrameSkip = 0;
}

EVT_HANDLER_MASK(Disassemble, "Disassemble...", CMDEN_GB | CMDEN_GBA)
{
    Disassemble();
}

EVT_HANDLER(Logging, "Logging...")
{
    wxDialog* dlg = wxGetApp().frame->logdlg.get();
    dlg->SetWindowStyle(wxCAPTION | wxRESIZE_BORDER);
    dlg->Show();
    dlg->Raise();
}

EVT_HANDLER_MASK(IOViewer, "I/O Viewer...", CMDEN_GBA)
{
    IOViewer();
}

EVT_HANDLER_MASK(MapViewer, "Map Viewer...", CMDEN_GB | CMDEN_GBA)
{
    MapViewer();
}

EVT_HANDLER_MASK(MemoryViewer, "Memory Viewer...", CMDEN_GB | CMDEN_GBA)
{
    MemViewer();
}

EVT_HANDLER_MASK(OAMViewer, "OAM Viewer...", CMDEN_GB | CMDEN_GBA)
{
    OAMViewer();
}

EVT_HANDLER_MASK(PaletteViewer, "Palette Viewer...", CMDEN_GB | CMDEN_GBA)
{
    PaletteViewer();
}

EVT_HANDLER_MASK(TileViewer, "Tile Viewer...", CMDEN_GB | CMDEN_GBA)
{
    TileViewer();
}

#if defined(VBAM_ENABLE_DEBUGGER)
extern int remotePort;

int GetGDBPort(MainFrame* mf)
{
    ModalPause mp;
    return wxGetNumberFromUser(
#ifdef __WXMSW__
        wxEmptyString,
#else
        _("Set to 0 for pseudo tty"),
#endif
        _("Port to wait for connection:"),
        _("GDB Connection"), gopts.gdb_port,
#ifdef __WXMSW__
        1025,
#else
        0,
#endif
        65535, mf);
}
#endif  // defined(VBAM_ENABLE_DEBUGGER)

EVT_HANDLER(DebugGDBPort, "Configure port...")
{
#if defined(VBAM_ENABLE_DEBUGGER)
    int port_selected = GetGDBPort(this);

    if (port_selected != -1) {
        gopts.gdb_port = port_selected;
        update_opts();
    }
#endif  // defined(VBAM_ENABLE_DEBUGGER)
}

EVT_HANDLER(DebugGDBBreakOnLoad, "Break on load")
{
#if defined(VBAM_ENABLE_DEBUGGER)
    GetMenuOptionConfig("DebugGDBBreakOnLoad", config::OptionID::kPrefGDBBreakOnLoad);
#endif  // defined(VBAM_ENABLE_DEBUGGER)
}

#if defined(VBAM_ENABLE_DEBUGGER)
void MainFrame::GDBBreak()
{
    ModalPause mp;

    if (gopts.gdb_port == 0) {
        int port_selected = GetGDBPort(this);

        if (port_selected != -1) {
            gopts.gdb_port = port_selected;
            update_opts();
        }
    }

    if (gopts.gdb_port > 0) {
        if (!remotePort) {
            wxString msg;
#ifndef __WXMSW__

            if (!gopts.gdb_port) {
                if (!debugOpenPty())
                    return;

                msg.Printf(_("Waiting for connection at %s"), debugGetSlavePty().wc_str());
            } else
#endif
            {
                if (!debugStartListen(gopts.gdb_port))
                    return;

                msg.Printf(_("Waiting for connection on port %d"), gopts.gdb_port);
            }

            wxProgressDialog dlg(_("Waiting for GDB..."), msg, 100, this,
                wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);
            bool connected = false;

            while (dlg.Pulse()) {
#ifndef __WXMSW__

                if (!gopts.gdb_port)
                    connected = debugWaitPty();
                else
#endif
                    connected = debugWaitSocket();

                if (connected)
                    break;

                // sleep a bit more in case of infinite loop
                wxMilliSleep(10);
            }

            if (connected) {
                remotePort = gopts.gdb_port;
                emulating = 1;
                dbgMain = remoteStubMain;
                dbgSignal = remoteStubSignal;
                dbgOutput = remoteOutput;
                cmd_enable &= ~(CMDEN_NGDB_ANY | CMDEN_NGDB_GBA);
                cmd_enable |= CMDEN_GDB;
                enable_menus();
                debugger = true;
            } else {
                remoteCleanUp();
            }
        } else {
            if (armState) {
                armNextPC -= 4;
                reg[15].I -= 4;
            } else {
                armNextPC -= 2;
                reg[15].I -= 2;
            }

            debugger = true;
        }
    }
}
#endif  // defined(VBAM_ENABLE_DEBUGGER)

EVT_HANDLER_MASK(DebugGDBBreak, "Break into GDB", CMDEN_NGDB_GBA | CMDEN_GDB)
{
#if defined(VBAM_ENABLE_DEBUGGER)
    GDBBreak();
#endif  // defined(VBAM_ENABLE_DEBUGGER)
}

EVT_HANDLER_MASK(DebugGDBDisconnect, "Disconnect GDB", CMDEN_GDB)
{
#if defined(VBAM_ENABLE_DEBUGGER)
    debugger = false;
    dbgMain = NULL;
    dbgSignal = NULL;
    dbgOutput = NULL;
    remotePort = 0;
    remoteCleanUp();
    cmd_enable &= ~CMDEN_GDB;
    cmd_enable |= CMDEN_NGDB_GBA | CMDEN_NGDB_ANY;
    enable_menus();
#endif  // defined(VBAM_ENABLE_DEBUGGER)
}

// Options menu
EVT_HANDLER(GeneralConfigure, "General options...")
{
    int rew = gopts.rewind_interval;
    wxDialog* dlg = GetXRCDialog("GeneralConfig");

    if (ShowModal(dlg) == wxID_OK)
        update_opts();

    if (panel->game_type() != IMAGE_UNKNOWN)
        soundSetThrottle(coreOptions.throttle);

    if (rew != gopts.rewind_interval) {
        if (!gopts.rewind_interval) {
            if (panel->num_rewind_states) {
                cmd_enable &= ~CMDEN_REWIND;
                enable_menus();
            }

            panel->num_rewind_states = 0;
            panel->do_rewind = false;
        } else {
            if (!panel->num_rewind_states)
                panel->do_rewind = true;

            panel->rewind_time = gopts.rewind_interval * 6;
        }
    }
}

EVT_HANDLER(SpeedupConfigure, "Speedup / Turbo options...")
{
    wxDialog* dlg = GetXRCDialog("SpeedupConfig");

    unsigned save_speedup_throttle            = coreOptions.speedup_throttle;
    unsigned save_speedup_frame_skip          = coreOptions.speedup_frame_skip;
    bool     save_speedup_throttle_frame_skip = coreOptions.speedup_throttle_frame_skip;

    if (ShowModal(dlg) == wxID_OK)
        update_opts();
    else {
        // Restore values if cancel pressed.
        coreOptions.speedup_throttle            = save_speedup_throttle;
        coreOptions.speedup_frame_skip          = save_speedup_frame_skip;
        coreOptions.speedup_throttle_frame_skip = save_speedup_throttle_frame_skip;
    }
}

EVT_HANDLER(GameBoyConfigure, "Game Boy options...")
{
    ShowModal(GetXRCDialog("GameBoyConfig"));
}

EVT_HANDLER(SetSize1x, "1x")
{
    OPTION(kDispScale) = 1;
}

EVT_HANDLER(SetSize2x, "2x")
{
    OPTION(kDispScale) = 2;
}

EVT_HANDLER(SetSize3x, "3x")
{
    OPTION(kDispScale) = 3;
}

EVT_HANDLER(SetSize4x, "4x")
{
    OPTION(kDispScale) = 4;
}

EVT_HANDLER(SetSize5x, "5x")
{
    OPTION(kDispScale) = 5;
}

EVT_HANDLER(SetSize6x, "6x")
{
    OPTION(kDispScale) = 6;
}

EVT_HANDLER(GameBoyAdvanceConfigure, "Game Boy Advance options...")
{
    wxDialog* dlg = GetXRCDialog("GameBoyAdvanceConfig");
    wxTextCtrl* ovcmt = XRCCTRL(*dlg, "Comment", wxTextCtrl);
    wxString cmt;
    wxChoice *ovrtc = XRCCTRL(*dlg, "OvRTC", wxChoice),
             *ovst = XRCCTRL(*dlg, "OvSaveType", wxChoice),
             *ovfs = XRCCTRL(*dlg, "OvFlashSize", wxChoice),
             *ovmir = XRCCTRL(*dlg, "OvMirroring", wxChoice);

    if (panel->game_type() == IMAGE_GBA) {
        wxString s = wxString((const char*)&g_rom[0xac], wxConvLibc, 4);
        XRCCTRL(*dlg, "GameCode", wxControl)
            ->SetLabel(s);
        cmt = wxString((const char*)&g_rom[0xa0], wxConvLibc, 12);
        wxFileConfig* cfg = wxGetApp().overrides_.get();

        if (cfg->HasGroup(s)) {
            cfg->SetPath(s);
            cmt = cfg->Read(wxT("comment"), cmt);
            ovcmt->SetValue(cmt);
            ovrtc->SetSelection(cfg->Read(wxT("rtcEnabled"), -1) + 1);
            ovst->SetSelection(cfg->Read(wxT("saveType"), -1) + 1);
            ovfs->SetSelection((cfg->Read(wxT("flashSize"), -1) >> 17) + 1);
            ovmir->SetSelection(cfg->Read(wxT("mirroringEnabled"), -1) + 1);
            cfg->SetPath(wxT("/"));
        } else {
            ovcmt->SetValue(cmt);
            ovrtc->SetSelection(0);
            ovst->SetSelection(0);
            ovfs->SetSelection(0);
            ovmir->SetSelection(0);
        }
    } else {
        XRCCTRL(*dlg, "GameCode", wxControl)
            ->SetLabel(wxEmptyString);
        ovcmt->SetValue(wxEmptyString);
        ovrtc->SetSelection(0);
        ovst->SetSelection(0);
        ovfs->SetSelection(0);
        ovmir->SetSelection(0);
    }

    if (ShowModal(dlg) != wxID_OK)
        return;

    if (panel->game_type() == IMAGE_GBA) {
        agbPrintEnable(OPTION(kPrefAgbPrint));
        wxString s = wxString((const char*)&g_rom[0xac], wxConvLibc, 4);
        wxFileConfig* cfg = wxGetApp().overrides_.get();
        bool chg;

        if (cfg->HasGroup(s)) {
            cfg->SetPath(s);
            chg = ovcmt->GetValue() != cmt || ovrtc->GetSelection() != cfg->Read(wxT("rtcEnabled"), -1) + 1 || ovst->GetSelection() != cfg->Read(wxT("saveType"), -1) + 1 || ovfs->GetSelection() != (cfg->Read(wxT("flashSize"), -1) >> 17) + 1 || ovmir->GetSelection() != cfg->Read(wxT("mirroringEnabled"), -1) + 1;
            cfg->SetPath(wxT("/"));
        } else
            chg = ovrtc->GetSelection() != 0 || ovst->GetSelection() != 0 || ovfs->GetSelection() != 0 || ovmir->GetSelection() != 0;

        if (chg) {
            wxString vba_over;
            wxFileName fn(wxGetApp().GetConfigurationPath(), wxT("vba-over.ini"));

            if (fn.FileExists()) {
                wxFileInputStream fis(fn.GetFullPath());
                wxStringOutputStream sos(&vba_over);
                fis.Read(sos);
            }

            if (cfg->HasGroup(s)) {
                cfg->SetPath(s);

                if (cfg->Read(wxT("path"), wxEmptyString) == fn.GetPath()) {
                    // EOL can be either \n (unix), \r\n (dos), or \r (old mac)
                    wxString res(wxT("(^|[\n\r])" // a new line
                                     L"(" // capture group as \2
                                     L"(#[^\n\r]*(\r?\n|\r))?" // an optional comment line
                                     L"\\[")); // the group header
                    res += s;
                    res += wxT("\\]"
                               L"([^[#]" // non-comment non-group-start chars
                               L"|[^\r\n \t][ \t]*[[#]" // or comment/grp start chars in middle of line
                               L"|#[^\n\r]*(\r?\n|\r)[^[]" // or comments not followed by grp start
                               L")*"
                               L")" // end of group
                        // no need to try to describe what's next
                        // as the regex should maximize match size
                        );
                    wxRegEx re(res);

                    // there may be more than one group if it was hand-edited
                    // so remove them all
                    // could use re.Replace(), but this is more reliable
                    while (re.Matches(vba_over)) {
                        size_t beg, end;
                        re.GetMatch(&beg, &end, 2);
                        vba_over.erase(beg, end - beg);
                    }
                }

                cfg->SetPath(wxT("/"));
                cfg->DeleteGroup(s);
            }

            cfg->SetPath(s);
            cfg->Write(wxT("path"), fn.GetPath());
            cfg->Write(wxT("comment"), ovcmt->GetValue());
            vba_over.append(wxT("# "));
            vba_over.append(ovcmt->GetValue());
            vba_over.append(wxTextFile::GetEOL());
            vba_over.append(wxT('['));
            vba_over.append(s);
            vba_over.append(wxT(']'));
            vba_over.append(wxTextFile::GetEOL());
            int sel;
#define appendval(n)                                   \
    do {                                               \
        vba_over.append(wxT(n));                       \
        vba_over.append(wxT('='));                     \
        vba_over.append((wxChar)(wxT('0') + sel - 1)); \
        vba_over.append(wxTextFile::GetEOL());         \
        cfg->Write(wxT(n), sel - 1);                   \
    } while (0)

            if ((sel = ovrtc->GetSelection()) > 0)
                appendval("rtcEnabled");

            if ((sel = ovst->GetSelection()) > 0)
                appendval("saveType");

            if ((sel = ovfs->GetSelection()) > 0) {
                vba_over.append(wxT("flashSize="));
                vba_over.append(sel == 1 ? wxT("65536") : wxT("131072"));
                vba_over.append(wxTextFile::GetEOL());
                cfg->Write(wxT("flashSize"), 0x10000 << (sel - 1));
            }

            if ((sel = ovmir->GetSelection()) > 0)
                appendval("mirroringEnabled");

            cfg->SetPath(wxT("/"));
            vba_over.append(wxTextFile::GetEOL());
            fn.Mkdir(0777, wxPATH_MKDIR_FULL);
            wxTempFileOutputStream fos(fn.GetFullPath());
            fos.Write(vba_over.c_str(), vba_over.size());
            fos.Commit();
        }
    }

    update_opts();
}

EVT_HANDLER_MASK(DisplayConfigure, "Display options...", CMDEN_NREC_ANY)
{
    wxDialog* dlg = GetXRCDialog("DisplayConfig");
    if (ShowModal(dlg) != wxID_OK) {
        return;
    }

    const uint32_t bitdepth = OPTION(kBitDepth);
    systemColorDepth = (int)((bitdepth + 1) << 3);

    const int frame_skip = OPTION(kPrefFrameSkip);
    if (frame_skip != -1) {
        systemFrameSkip = frame_skip;
    }

    update_opts();
}

EVT_HANDLER_MASK(ChangeFilter, "Change Pixel Filter", CMDEN_NREC_ANY)
{
    OPTION(kDispFilter).Next();
}

EVT_HANDLER_MASK(ChangeIFB, "Change Interframe Blending", CMDEN_NREC_ANY)
{
    OPTION(kDispIFB).Next();
}

EVT_HANDLER_MASK(SoundConfigure, "Sound options...", CMDEN_NREC_ANY)
{
    if (ShowModal(GetXRCDialog("SoundConfig")) != wxID_OK)
        return;

    // No point in observing these since they can only be set in this dialog.
    gb_effects_config.echo = (float)OPTION(kSoundGBEcho) / 100.0;
    gb_effects_config.stereo = (float)OPTION(kSoundGBStereo) / 100.0;
    soundFiltering = (float)OPTION(kSoundGBAFiltering) / 100.0f;
}

EVT_HANDLER(EmulatorDirectories, "Directories...")
{
    ShowModal(GetXRCDialog("DirectoriesConfig"));
}

EVT_HANDLER(JoypadConfigure, "Joypad options...")
{
    if (ShowModal(GetXRCDialog("JoypadConfig")) == wxID_OK) {
        update_shortcut_opts();
    }
}

EVT_HANDLER(Customize, "Customize UI...")
{
    if (ShowModal(GetXRCDialog("AccelConfig")) == wxID_OK) {
        update_shortcut_opts();
        ResetMenuAccelerators();
    }
}

#ifndef NO_ONLINEUPDATES
#include "autoupdater/autoupdater.h"
#endif // NO_ONLINEUPDATES

EVT_HANDLER(UpdateEmu, "Check for updates...")
{
#ifndef NO_ONLINEUPDATES
    checkUpdatesUi();
#endif // NO_ONLINEUPDATES
}

EVT_HANDLER(FactoryReset, "Factory Reset...")
{
    wxMessageDialog dlg(
        nullptr, _("YOUR CONFIGURATION WILL BE DELETED!\n\nAre you sure?"),
        _("FACTORY RESET"), wxYES_NO | wxNO_DEFAULT | wxCENTRE);

    if (dlg.ShowModal() == wxID_YES) {
        wxConfigBase::Get()->DeleteAll();
        wxExecute(wxStandardPaths::Get().GetExecutablePath(), wxEXEC_ASYNC);
        Close(true);
    }
}

EVT_HANDLER(BugReport, "Report bugs...")
{
    wxLaunchDefaultBrowser(wxT("https://github.com/visualboyadvance-m/visualboyadvance-m/issues"));
}

EVT_HANDLER(FAQ, "VBA-M support forum")
{
    wxLaunchDefaultBrowser(wxT("https://github.com/visualboyadvance-m/visualboyadvance-m/"));
}

EVT_HANDLER(Translate, "Translations")
{
    wxLaunchDefaultBrowser(wxT("https://explore.transifex.com/bgk/vba-m/"));
}

// was About
EVT_HANDLER(wxID_ABOUT, "About...")
{
    wxAboutDialogInfo ai;
    ai.SetName(wxT("VisualBoyAdvance-M"));
    wxString version(kVbamVersion);
    ai.SetVersion(version);
    // setting website, icon, license uses custom aboutbox on win32 & macosx
    // but at least win32 standard about is nothing special
    ai.SetWebSite(wxT("http://visualboyadvance-m.org/"));
    ai.SetIcon(GetIcons().GetIcon(wxSize(32, 32), wxIconBundle::FALLBACK_NEAREST_LARGER));
    ai.SetDescription(_("Nintendo Game Boy / Color / Advance emulator."));
    ai.SetCopyright(_("Copyright (C) 1999-2003 Forgotten\nCopyright (C) 2004-2006 VBA development team\nCopyright (C) 2007-2020 VBA-M development team"));
    ai.SetLicense(_(
"This program is free software: you can redistribute it and / or modify\n"
"it under the terms of the GNU General Public License as published by\n"
"the Free Software Foundation, either version 2 of the License, or\n"
"(at your option) any later version.\n"
"\n"
"This program is distributed in the hope that it will be useful,\n"
"but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
"MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the\n"
"GNU General Public License for more details.\n"
"\n"
"You should have received a copy of the GNU General Public License\n"
"along with this program. If not, see http://www.gnu.org/licenses ."
    ));
    // from gtk
    ai.AddDeveloper(wxT("Forgotten"));
    ai.AddDeveloper(wxT("kxu"));
    ai.AddDeveloper(wxT("Pokemonhacker"));
    ai.AddDeveloper(wxT("Spacy51"));
    ai.AddDeveloper(wxT("mudlord"));
    ai.AddDeveloper(wxT("Nach"));
    ai.AddDeveloper(wxT("jbo_85"));
    ai.AddDeveloper(wxT("bgK"));
    ai.AddArtist(wxT("Matteo Drera"));
    ai.AddArtist(wxT("Jakub Steiner"));
    ai.AddArtist(wxT("Jones Lee"));
    // from win32
    ai.AddDeveloper(wxT("Jonas Quinn"));
    ai.AddDeveloper(wxT("DJRobX"));
    ai.AddDeveloper(wxT("Spacy"));
    ai.AddDeveloper(wxT("Squall Leonhart"));
    // wx
    ai.AddDeveloper(wxT("Thomas J. Moore"));
    // from win32 "thanks"
    ai.AddDeveloper(wxT("blargg"));
    ai.AddDeveloper(wxT("Costis"));
    ai.AddDeveloper(wxT("chrono"));
    ai.AddDeveloper(wxT("xKiv"));
    ai.AddDeveloper(wxT("skidau"));
    ai.AddDeveloper(wxT("TheCanadianBacon"));
    ai.AddDeveloper(wxT("rkitover"));
    ai.AddDeveloper(wxT("Mystro256"));
    ai.AddDeveloper(wxT("retro-wertz"));
    ai.AddDeveloper(wxT("denisfa"));
    ai.AddDeveloper(wxT("orbea"));
    ai.AddDeveloper(wxT("Orig. VBA team"));
    ai.AddDeveloper(wxT("... many contributors who send us patches/PRs"));
    wxAboutBox(ai);
}

EVT_HANDLER(Bilinear, "Use bilinear filter with 3d renderer")
{
    GetMenuOptionConfig("Bilinear", config::OptionID::kDispBilinear);
}

EVT_HANDLER(RetainAspect, "Retain aspect ratio when resizing")
{
    GetMenuOptionConfig("RetainAspect", config::OptionID::kDispStretch);
}

EVT_HANDLER(Printer, "Enable printer emulation")
{
    GetMenuOptionInt("Printer", &coreOptions.gbPrinterEnabled, 1);
#if (defined __WIN32__ || defined _WIN32)
#ifndef NO_LINK
    gbSerialFunction = gbStartLink;
#else
    gbSerialFunction = NULL;
#endif
#endif
    if (coreOptions.gbPrinterEnabled)
        gbSerialFunction = gbPrinterSend;

    update_opts();
}

EVT_HANDLER(PrintGather, "Automatically gather a full page before printing")
{
    GetMenuOptionConfig("PrintGather", config::OptionID::kGBPrintAutoPage);
}

EVT_HANDLER(PrintSnap, "Automatically save printouts as screen captures with -print suffix")
{
    GetMenuOptionConfig("PrintSnap", config::OptionID::kGBPrintScreenCap);
}

EVT_HANDLER(GBASoundInterpolation, "GBA sound interpolation")
{
    GetMenuOptionConfig("GBASoundInterpolation", config::OptionID::kSoundGBAInterpolation);
}

EVT_HANDLER(GBDeclicking, "GB sound declicking")
{
    GetMenuOptionConfig("GBDeclicking", config::OptionID::kSoundGBDeclicking);
}

EVT_HANDLER(GBEnhanceSound, "Enable GB sound effects")
{
    GetMenuOptionConfig("GBEnhanceSound", config::OptionID::kSoundGBEnableEffects);
}

EVT_HANDLER(GBSurround, "GB surround sound effect (%)")
{
    GetMenuOptionConfig("GBSurround",config::OptionID::kSoundGBSurround);
}

EVT_HANDLER(AGBPrinter, "Enable AGB printer")
{
    GetMenuOptionConfig("AGBPrinter", config::OptionID::kPrefAgbPrint);
}

EVT_HANDLER_MASK(GBALcdFilter, "Enable LCD filter", CMDEN_GBA)
{
    GetMenuOptionConfig("GBALcdFilter", config::OptionID::kGBALCDFilter);
}

EVT_HANDLER_MASK(GBLcdFilter, "Enable LCD filter", CMDEN_GB)
{
    GetMenuOptionConfig("GBLcdFilter", config::OptionID::kGBLCDFilter);
}

EVT_HANDLER(GBColorOption, "Enable GB color option")
{
    GetMenuOptionConfig("GBColorOption", config::OptionID::kGBColorOption);
}

EVT_HANDLER(ApplyPatches, "Apply IPS/UPS/IPF patches if found")
{
    GetMenuOptionConfig("ApplyPatches", config::OptionID::kPrefAutoPatch);
}

EVT_HANDLER(KeepOnTop, "Keep window on top")
{
    GetMenuOptionConfig("KeepOnTop", config::OptionID::kDispKeepOnTop);
}

EVT_HANDLER(StatusBar, "Enable status bar")
{
    GetMenuOptionConfig("StatusBar", config::OptionID::kGenStatusBar);
}

EVT_HANDLER(NoStatusMsg, "Disable on-screen status messages")
{
    GetMenuOptionConfig("NoStatusMsg", config::OptionID::kPrefDisableStatus);
}

EVT_HANDLER(BitDepth, "Bit depth")
{
    GetMenuOptionConfig("BitDepth", config::OptionID::kBitDepth);
}

EVT_HANDLER(FrameSkipAuto, "Auto Skip frames.")
{
    GetMenuOptionConfig("FrameSkipAuto", config::OptionID::kPrefAutoFrameSkip);
}

EVT_HANDLER(Fullscreen, "Enter fullscreen mode at startup")
{
    GetMenuOptionConfig("Fullscreen", config::OptionID::kGeomFullScreen);
}

EVT_HANDLER(PauseWhenInactive, "Pause game when main window loses focus")
{
    GetMenuOptionConfig("PauseWhenInactive", config::OptionID::kPrefPauseWhenInactive);
}

EVT_HANDLER(RTC, "Enable RTC (vba-over.ini override is rtcEnabled")
{
    GetMenuOptionInt("RTC", &coreOptions.rtcEnabled, 1);
    update_opts();
}

EVT_HANDLER(Transparent, "Draw on-screen messages transparently")
{
    GetMenuOptionConfig("Transparent", config::OptionID::kPrefShowSpeedTransparent);
}

EVT_HANDLER(SkipIntro, "Skip BIOS initialization")
{
    GetMenuOptionConfig("SkipIntro", config::OptionID::kPrefSkipBios);
}

EVT_HANDLER(BootRomEn, "Use the specified BIOS file for GBA")
{
    GetMenuOptionConfig("BootRomEn", config::OptionID::kPrefUseBiosGBA);
}

EVT_HANDLER(BootRomGB, "Use the specified BIOS file for GB")
{
    GetMenuOptionConfig("BootRomGB", config::OptionID::kPrefUseBiosGB);
    if (OPTION(kPrefUseBiosGB) && OPTION(kGBColorizerHack)) {
        wxLogError(_("Cannot use Game Boy BIOS when Colorizer Hack is enabled."));
        SetMenuOption("BootRomGB", 0);
        OPTION(kPrefUseBiosGB) = false;
    }
}

EVT_HANDLER(BootRomGBC, "Use the specified BIOS file for GBC")
{
    GetMenuOptionConfig("BootRomGBC", config::OptionID::kPrefUseBiosGBC);
}

EVT_HANDLER(VSync, "Wait for vertical sync")
{
    GetMenuOptionConfig("VSync", config::OptionID::kPrefVsync);
}

EVT_HANDLER(HideMenuBar, "Hide menu bar when mouse is inactive")
{
    GetMenuOptionConfig("HideMenuBar", config::OptionID::kUIHideMenuBar);
}

EVT_HANDLER(SuspendScreenSaver, "Suspend screensaver when game is running")
{
    GetMenuOptionConfig("SuspendScreenSaver", config::OptionID::kUISuspendScreenSaver);
}

#ifndef NO_LINK

void MainFrame::EnableNetworkMenu()
{
    cmd_enable &= ~CMDEN_LINK_ANY;

    if (gopts.gba_link_type != 0)
        cmd_enable |= CMDEN_LINK_ANY;

    if (OPTION(kGBALinkProto))
        cmd_enable &= ~CMDEN_LINK_ANY;

    enable_menus();
}

void SetLinkTypeMenu(const char* type, int value)
{
    MainFrame* mf = wxGetApp().frame;
    mf->SetMenuOption("LinkType0Nothing", 0);
    mf->SetMenuOption("LinkType1Cable", 0);
    mf->SetMenuOption("LinkType2Wireless", 0);
    mf->SetMenuOption("LinkType3GameCube", 0);
    mf->SetMenuOption("LinkType4Gameboy", 0);
    mf->SetMenuOption(type, 1);
    gopts.gba_link_type = value;
    update_opts();
    CloseLink();
    mf->EnableNetworkMenu();
}

#endif  // NO_LINK

EVT_HANDLER_MASK(LanLink, "Start Network link", CMDEN_LINK_ANY)
{
#ifndef NO_LINK
    LinkMode mode = GetLinkMode();

    if (mode != LINK_DISCONNECTED) {
        // while we could deactivate the command when connected, it is more
        // user-friendly to display a message indidcating why
        wxLogError(_("LAN link is already active. Disable link mode to disconnect."));
        return;
    }

    if (OPTION(kGBALinkProto)) {
        // see above comment
        wxLogError(_("Network is not supported in local mode."));
        return;
    }

    wxDialog* dlg = GetXRCDialog("NetLink");
    ShowModal(dlg);
    panel->SetFrameTitle();
#endif
}

EVT_HANDLER(LinkType0Nothing, "Link nothing")
{
#ifndef NO_LINK
    SetLinkTypeMenu("LinkType0Nothing", 0);
#endif
}

EVT_HANDLER(LinkType1Cable, "Link cable")
{
#ifndef NO_LINK
    SetLinkTypeMenu("LinkType1Cable", 1);
#endif
}

EVT_HANDLER(LinkType2Wireless, "Link wireless")
{
#ifndef NO_LINK
    SetLinkTypeMenu("LinkType2Wireless", 2);
#endif
}

EVT_HANDLER(LinkType3GameCube, "Link GameCube")
{
#ifndef NO_LINK
    SetLinkTypeMenu("LinkType3GameCube", 3);
#endif
}

EVT_HANDLER(LinkType4Gameboy, "Link Gameboy")
{
#ifndef NO_LINK
    SetLinkTypeMenu("LinkType4Gameboy", 4);
#endif
}

EVT_HANDLER(LinkAuto, "Enable link at boot")
{
#ifndef NO_LINK
    GetMenuOptionConfig("LinkAuto", config::OptionID::kGBALinkAuto);
#endif
}

EVT_HANDLER(SpeedOn, "Enable faster network protocol by default")
{
#ifndef NO_LINK
    GetMenuOptionConfig("SpeedOn", config::OptionID::kGBALinkFast);
#endif
}

EVT_HANDLER(LinkProto, "Local host IPC")
{
#ifndef NO_LINK
    GetMenuOptionConfig("LinkProto", config::OptionID::kGBALinkProto);
    EnableNetworkMenu();
#endif
}

EVT_HANDLER(LinkConfigure, "Link options...")
{
#ifndef NO_LINK
    wxDialog* dlg = GetXRCDialog("LinkConfig");

    if (ShowModal(dlg) != wxID_OK)
        return;

    SetLinkTimeout(gopts.link_timeout);
    update_opts();
#endif
}

EVT_HANDLER(ExternalTranslations, "Use external translations")
{
    GetMenuOptionConfig("ExternalTranslations", config::OptionID::kExternalTranslations);
}

EVT_HANDLER(Language0, "Default Language")
{
    OPTION(kLocale) = wxLANGUAGE_DEFAULT;
    
    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);
    
    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_DEFAULT);
    
    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language1, "Bulgarian")
{
    OPTION(kLocale) = wxLANGUAGE_BULGARIAN;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_BULGARIAN);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language2, "Breton")
{
    OPTION(kLocale) = wxLANGUAGE_BRETON;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_BRETON);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language3, "Czech")
{
    OPTION(kLocale) = wxLANGUAGE_CZECH;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_CZECH);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language4, "German")
{
    OPTION(kLocale) = wxLANGUAGE_GERMAN;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_GERMAN);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language5, "Greek")
{
    OPTION(kLocale) = wxLANGUAGE_GREEK;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_GREEK);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language6, "English (US)")
{
    OPTION(kLocale) = wxLANGUAGE_ENGLISH_US;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_ENGLISH_US);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language7, "Spanish (Latin American)")
{
    OPTION(kLocale) = wxLANGUAGE_SPANISH_LATIN_AMERICA;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_SPANISH_LATIN_AMERICA);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language8, "Spanish (Colombia)")
{
    OPTION(kLocale) = wxLANGUAGE_SPANISH_COLOMBIA;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_SPANISH_COLOMBIA);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language9, "Spanish (Peru)")
{
    OPTION(kLocale) = wxLANGUAGE_SPANISH_PERU;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_SPANISH_PERU);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language10, "Spanish (US)")
{
    OPTION(kLocale) = wxLANGUAGE_SPANISH_US;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_SPANISH_US);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language11, "Spanish")
{
    OPTION(kLocale) = wxLANGUAGE_SPANISH;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_SPANISH);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language12, "French (France)")
{
    OPTION(kLocale) = wxLANGUAGE_FRENCH_FRANCE;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_FRENCH_FRANCE);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language13, "French")
{
    OPTION(kLocale) = wxLANGUAGE_FRENCH;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_FRENCH);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language14, "Galician")
{
    OPTION(kLocale) = wxLANGUAGE_GALICIAN;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_GALICIAN);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language15, "Hebrew (Israel)")
{
    OPTION(kLocale) = wxLANGUAGE_HEBREW_ISRAEL;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_HEBREW_ISRAEL);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language16, "Hungarian (Hungary)")
{
    OPTION(kLocale) = wxLANGUAGE_HUNGARIAN_HUNGARY;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_HUNGARIAN_HUNGARY);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language17, "Hungarian")
{
    OPTION(kLocale) = wxLANGUAGE_HUNGARIAN;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_HUNGARIAN);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language18, "Indonesian")
{
    OPTION(kLocale) = wxLANGUAGE_INDONESIAN;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_INDONESIAN);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language19, "Italian")
{
    OPTION(kLocale) = wxLANGUAGE_ITALIAN_ITALY;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_ITALIAN_ITALY);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language20, "Japanese")
{
    OPTION(kLocale) = wxLANGUAGE_JAPANESE;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_JAPANESE);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language21, "Korean (Korea)")
{
    OPTION(kLocale) = wxLANGUAGE_KOREAN_KOREA;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_KOREAN_KOREA);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language22, "Korean")
{
    OPTION(kLocale) = wxLANGUAGE_KOREAN;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_KOREAN);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language23, "Malay (Malaysia)")
{
    OPTION(kLocale) = wxLANGUAGE_MALAY_MALAYSIA;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_MALAY_MALAYSIA);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language24, "Norwegian")
{
    OPTION(kLocale) = wxLANGUAGE_NORWEGIAN;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_NORWEGIAN);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language25, "Dutch")
{
    OPTION(kLocale) = wxLANGUAGE_DUTCH;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_DUTCH);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language26, "Polish (Poland)")
{
    OPTION(kLocale) = wxLANGUAGE_POLISH_POLAND;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_POLISH_POLAND);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language27, "Polish")
{
    OPTION(kLocale) = wxLANGUAGE_POLISH;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_POLISH);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language28, "Portuguese (Brazil)")
{
    OPTION(kLocale) = wxLANGUAGE_PORTUGUESE_BRAZILIAN;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_PORTUGUESE_BRAZILIAN);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language29, "Portuguese (Portugal)")
{
    OPTION(kLocale) = wxLANGUAGE_PORTUGUESE_PORTUGAL;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_PORTUGUESE_PORTUGAL);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language30, "Russian (Russia)")
{
    OPTION(kLocale) = wxLANGUAGE_RUSSIAN_RUSSIA;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_RUSSIAN_RUSSIA);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language31, "Swedish")
{
    OPTION(kLocale) = wxLANGUAGE_SWEDISH;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_SWEDISH);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language32, "Turkish")
{
    OPTION(kLocale) = wxLANGUAGE_TURKISH;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_TURKISH);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language33, "Ukrainian")
{
    OPTION(kLocale) = wxLANGUAGE_UKRAINIAN;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_UKRAINIAN);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language34, "Urdu (Pakistan)")
{
    OPTION(kLocale) = wxLANGUAGE_URDU_PAKISTAN;

    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);

    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_URDU_PAKISTAN);

    update_opts();
    RefreshFrame();
}

EVT_HANDLER(Language35, "Chinese (China)")
{
    OPTION(kLocale) = wxLANGUAGE_CHINESE_CHINA;
    
    if (wxvbam_locale != NULL)
        wxDELETE(wxvbam_locale);
    
    wxvbam_locale = new wxLocale;
    wxvbam_locale->Init(OPTION(kLocale), wxLOCALE_LOAD_DEFAULT);

#ifdef _WIN32
    if (OPTION(kExternalTranslations) == false)
        wxTranslations::Get()->SetLoader(new wxResourceTranslationsLoader);
#endif

    wxvbam_locale->AddCatalog("wxvbam", wxLANGUAGE_CHINESE_CHINA);
    
    update_opts();
    RefreshFrame();
}

// Dummy for disabling system key bindings
EVT_HANDLER_MASK(NOOP, "Do nothing", CMDEN_NEVER)
{
}

// The following have been moved to dialogs
// I will not implement as command unless there is great demand cvbn,;
// CheatsList
//EVT_HANDLER(CheatsLoad, "Load Cheats...")
//EVT_HANDLER(CheatsSave, "Save Cheats...")
//GeneralConfigure
//EVT_HANDLER(EmulatorRewindInterval, "EmulatorRewindInterval")
//EVT_HANDLER(EmulatorAutoApplyPatchFiles, "EmulatorAutoApplyPatchFiles")
//EVT_HANDLER(ThrottleNone, "ThrottleNone")
//EVT_HANDLER(Throttle025%, "Throttle025%")
//EVT_HANDLER(Throttle050%, "Throttle050%")
//EVT_HANDLER(Throttle100%, "Throttle100%")
//EVT_HANDLER(Throttle150%, "Throttle150%")
//EVT_HANDLER(Throttle200%, "Throttle200%")
//EVT_HANDLER(ThrottleOther, "ThrottleOther")
//GameBoyConfigure/GameBoyAdvanceConfigure
//EVT_HANDLER(FrameSkip0, "FrameSkip0")
//EVT_HANDLER(FrameSkip1, "FrameSkip1")
//EVT_HANDLER(FrameSkip2, "FrameSkip2")
//EVT_HANDLER(FrameSkip3, "FrameSkip3")
//EVT_HANDLER(FrameSkip4, "FrameSkip4")
//EVT_HANDLER(FrameSkip5, "FrameSkip5")
//EVT_HANDLER(FrameSkip6, "FrameSkip6")
//EVT_HANDLER(FrameSkip7, "FrameSkip7")
//EVT_HANDLER(FrameSkip8, "FrameSkip8")
//EVT_HANDLER(FrameSkip9, "FrameSkip9")
// GameBoyConfigure
//EVT_HANDLER(GameboyBorder, "GameboyBorder")
//EVT_HANDLER(GameboyBorderAutomatic, "GameboyBorderAutomatic")
//EVT_HANDLER(GameboyColors, "GameboyColors")
//GameBoyAdvanceConfigure
//EVT_HANDLER(EmulatorAGBPrint, "EmulatorAGBPrint")
//EVT_HANDLER(EmulatorSaveAuto, "EmulatorSaveAuto")
//EVT_HANDLER(EmulatorSaveEEPROM, "EmulatorSaveEEPROM")
//EVT_HANDLER(EmulatorSaveSRAM, "EmulatorSaveSRAM")
//EVT_HANDLER(EmulatorSaveFLASH, "EmulatorSaveFLASH")
//EVT_HANDLER(EmulatorSaveEEPROMSensor, "EmulatorSaveEEPROMSensor")
//EVT_HANDLER(EmulatorSaveFlash64K, "EmulatorSaveFlash64K")
//EVT_HANDLER(EmulatorSaveFlash128K, "EmulatorSaveFlash128K")
//EVT_HANDLER(EmulatorSaveDetectNow, "EmulatorSaveDetectNow")
//EVT_HANDLER(EmulatorRTC, "EmulatorRTC")
//DisplayConfigure
//EVT_HANDLER(EmulatorShowSpeedNone, "EmulatorShowSpeedNone")
//EVT_HANDLER(EmulatorShowSpeedPercentage, "EmulatorShowSpeedPercentage")
//EVT_HANDLER(EmulatorShowSpeedDetailed, "EmulatorShowSpeedDetailed")
//EVT_HANDLER(EmulatorShowSpeedTransparent, "EmulatorShowSpeedTransparent")
//EVT_HANDLER(VideoX1, "VideoX1")
//EVT_HANDLER(VideoX2, "VideoX2")
//EVT_HANDLER(VideoX3, "VideoX3")
//EVT_HANDLER(VideoX4, "VideoX4")
//EVT_HANDLER(VideoX5, "VideoX5")
//EVT_HANDLER(VideoX6, "VideoX6")
//EVT_HANDLER(Video320x240, "Video320x240")
//EVT_HANDLER(Video640x480, "Video640x480")
//EVT_HANDLER(Video800x600, "Video800x600")
//EVT_HANDLER(VideoFullscreen, "VideoFullscreen")
//EVT_HANDLER(VideoFullscreenMaxScale, "VideoFullscreenMaxScale")
//EVT_HANDLER(VideoRenderDDRAW, "VideoRenderDDRAW")
//EVT_HANDLER(VideoRenderD3D, "VideoRenderD3D")
//EVT_HANDLER(VideoRenderOGL, "VideoRenderOGL")
//EVT_HANDLER(VideoVsync, "VideoVsync")
//EVT_HANDLER(FilterNormal, "FilterNormal")
//EVT_HANDLER(FilterTVMode, "FilterTVMode")
//EVT_HANDLER(Filter2xSaI, "Filter2xSaI")
//EVT_HANDLER(FilterSuper2xSaI, "FilterSuper2xSaI")
//EVT_HANDLER(FilterSuperEagle, "FilterSuperEagle")
//EVT_HANDLER(FilterPixelate, "FilterPixelate")
//EVT_HANDLER(FilterMotionBlur, "FilterMotionBlur")
//EVT_HANDLER(FilterAdMameScale2x, "FilterAdMameScale2x")
//EVT_HANDLER(FilterSimple2x, "FilterSimple2x")
//EVT_HANDLER(FilterBilinear, "FilterBilinear")
//EVT_HANDLER(FilterBilinearPlus, "FilterBilinearPlus")
//EVT_HANDLER(FilterScanlines, "FilterScanlines")
//EVT_HANDLER(FilterHq2x, "FilterHq2x")
//EVT_HANDLER(FilterLq2x, "FilterLq2x")
//EVT_HANDLER(FilterIFBNone, "FilterIFBNone")
//EVT_HANDLER(FilterIFBMotionBlur, "FilterIFBMotionBlur")
//EVT_HANDLER(FilterIFBSmart, "FilterIFBSmart")
//EVT_HANDLER(FilterDisableMMX, "FilterDisableMMX")
//JoypadConfigure
//EVT_HANDLER(JoypadConfigure1, "JoypadConfigure1")
//EVT_HANDLER(JoypadConfigure2, "JoypadConfigure2")
//EVT_HANDLER(JoypadConfigure3, "JoypadConfigure3")
//EVT_HANDLER(JoypadConfigure4, "JoypadConfigure4")
//EVT_HANDLER(JoypadMotionConfigure, "JoypadMotionConfigure")

// The following functionality has been removed
// It should be done in OS, rather than in vbam
//EVT_HANDLER(EmulatorAssociate, "EmulatorAssociate")

// The following functionality has been removed
// It should be done at OS level (e.g. window manager)
//EVT_HANDLER(SystemMinimize, "SystemMinimize")
