#include <cassert>
#include <iostream>
#include "../../Source/RifeSettingsDialog.cpp"

int wmain(int argc, wchar_t** argv)
{
    assert(argc == 2);
    // Load compiled resources only. No renderer creation, registration or GPU.
    const HMODULE module = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    assert(module);
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDD_FRAMEINTERPOLATION), RT_DIALOG);
    assert(resource);
    const auto* resourceTemplate = static_cast<const DLGTEMPLATE*>(LockResource(LoadResource(module, resource)));
    assert(resourceTemplate);
    Settings_t settings;
    assert(settings.iRifeSceneDetection == RIFE_SCENE_SVPflow1);
    assert(settings.iRifeProcessingResolution == RIFE_RESOLUTION_Source);
    assert(settings.iRifeProcessingLimit == 720);
    settings.iRifeMode = RIFE_MODE_Movie4x;
    assert(!settings.bRifePreservePrecision && !settings.bRifeFeatureReuse);
    settings.bRifePreservePrecision = settings.bRifeFeatureReuse = true;
    const HWND dialog = CreateDialogIndirectParamW(module, resourceTemplate, nullptr, DialogProc,
        reinterpret_cast<LPARAM>(&settings));
    assert(dialog && !IsWindowVisible(dialog));
    for (const int model : {RIFE_MODEL_44, RIFE_MODEL_46, RIFE_MODEL_415_LITE, RIFE_MODEL_425, RIFE_MODEL_425_LITE}) {
        SelectComboValue(dialog, IDC_RIFE_MODEL, model);
        EnableControls(dialog);
        assert(!!IsWindowEnabled(GetDlgItem(dialog, IDC_RIFE_FEATURE_REUSE))
            == (model == RIFE_MODEL_415_LITE || model == RIFE_MODEL_425));
        Settings_t readback;
        assert(ReadControls(dialog, readback));
        assert(readback.bRifePreservePrecision && readback.bRifeFeatureReuse);
        Settings_t copied;
        CopyRifeSettings(copied, readback);
        assert(RifeSettingsEqual(copied, readback));
        SetControls(dialog, copied);
    }
    const auto sceneFirst = SendDlgItemMessageW(dialog, IDC_RIFE_SCENE_DETECTION, CB_GETITEMDATA, 0, 0);
    assert(sceneFirst == RIFE_SCENE_SVPflow1);
    for (const int mode : {RIFE_SCENE_NVOF, RIFE_SCENE_Image, RIFE_SCENE_Disabled, RIFE_SCENE_SVPflow1}) {
        SelectComboValue(dialog, IDC_RIFE_SCENE_DETECTION, mode);
        assert(ComboValue(dialog, IDC_RIFE_SCENE_DETECTION) == mode);
    }
    SelectComboValue(dialog, IDC_RIFE_SCENE_DETECTION, RIFE_SCENE_SVPflow1);
    for (const int mode : {RIFE_RESOLUTION_Source, RIFE_RESOLUTION_Display, RIFE_RESOLUTION_Limit}) {
        SelectComboValue(dialog, IDC_RIFE_PROCESSING_RESOLUTION, mode);
        EnableControls(dialog);
        assert(ComboValue(dialog, IDC_RIFE_PROCESSING_RESOLUTION) == mode);
        assert(!!IsWindowEnabled(GetDlgItem(dialog, IDC_RIFE_PROCESSING_LIMIT)) == (mode == RIFE_RESOLUTION_Limit));
        wchar_t selected[80] = {};
        assert(GetWindowTextW(GetDlgItem(dialog, IDC_RIFE_PROCESSING_RESOLUTION), selected, 80) > 0);
        for (const int limit : {64, 540, 720, 1080, 4320}) {
            SetDlgItemInt(dialog, IDC_RIFE_PROCESSING_LIMIT, limit, FALSE);
            Settings_t readback;
            assert(ReadControls(dialog, readback));
            assert(readback.iRifeProcessingResolution == mode && readback.iRifeProcessingLimit == limit);
            Settings_t copied;
            CopyRifeSettings(copied, readback);
            assert(RifeSettingsEqual(copied, readback));
            SetControls(dialog, copied);
            assert(ComboValue(dialog, IDC_RIFE_PROCESSING_RESOLUTION) == mode);
        }
    }
    DialogProc(dialog, WM_COMMAND, MAKEWPARAM(IDC_BUTTON_RIFE_DEFAULTS, BN_CLICKED), 0);
    assert(settings.iRifeSceneDetection == RIFE_SCENE_SVPflow1);
    assert(settings.iRifeProcessingResolution == RIFE_RESOLUTION_Source);
    assert(!settings.bRifePreservePrecision && !settings.bRifeFeatureReuse);
    assert(!IsWindowEnabled(GetDlgItem(dialog, IDC_RIFE_FEATURE_REUSE)));
    assert(!IsWindowEnabled(GetDlgItem(dialog, IDC_RIFE_PRESERVE_PRECISION)));
    assert(!IsWindowEnabled(GetDlgItem(dialog, IDC_RIFE_PROCESSING_RESOLUTION)));
    RECT client = {};
    GetClientRect(dialog, &client);
    for (const int id : {IDC_RIFE_PROCESSING_RESOLUTION, IDC_RIFE_PROCESSING_LIMIT, IDC_RIFE_SCENE_DETECTION,
            IDC_RIFE_RULES_LIST, IDC_RIFE_FEATURE_REUSE, IDC_RIFE_PRESERVE_PRECISION, IDOK, IDCANCEL, IDC_BUTTON_RIFE_DEFAULTS}) {
        RECT rect = {};
        GetWindowRect(GetDlgItem(dialog, id), &rect);
        MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&rect), 2);
        assert(rect.left >= 0 && rect.top >= 0 && rect.right <= client.right && rect.bottom <= client.bottom);
    }
    DestroyWindow(dialog);
    FreeLibrary(module);
    std::cout << "Compiled dialog: selections, defaults, saved-value mapping, limit enablement and bounds passed\n";
}
