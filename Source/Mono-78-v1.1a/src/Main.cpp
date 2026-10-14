#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <thread>
#include <memory>
#include <sstream>
#include <iomanip>
#include "AudioCore.h"
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"comdlg32.lib")
constexpr UINT Done=WM_APP+1;
HWND win, fileLabel, monoBox, matchBox, repairBox, speedBox, fromEdit, toEdit, runButton, browseButton, statusLabel, progress;
HFONT font, titleFont;
std::wstring input;
bool busy=false;
std::thread worker;
std::wstring getText(HWND h) { int n=GetWindowTextLengthW(h); std::wstring s(n+1,L'\0'); GetWindowTextW(h,s.data(),n+1); s.resize(n); return s; }
std::string utf8(const std::wstring& s) { int n=WideCharToMultiByte(CP_UTF8,0,s.c_str(),-1,nullptr,0,nullptr,nullptr); std::string r(n,'\0'); WideCharToMultiByte(CP_UTF8,0,s.c_str(),-1,r.data(),n,nullptr,nullptr); r.pop_back(); return r; }
std::wstring wide(const std::string& s) { int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0); std::wstring r(n,L'\0'); MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,r.data(),n); r.pop_back(); return r; }
bool changeSpeed() { return SendMessageW(speedBox,CB_GETCURSEL,0,0)>0; }
void selectSpeed() {
    const int mode=(int)SendMessageW(speedBox,CB_GETCURSEL,0,0);
    if(mode==1)SetWindowTextW(toEdit,L"77.92");
    if(mode==2)SetWindowTextW(toEdit,L"78.26");
    if(mode==3)SetWindowTextW(toEdit,L"78.00");
}
bool checked(HWND h) { return SendMessageW(h,BM_GETCHECK,0,0)==BST_CHECKED; }
void refresh() {
    for(HWND h:{browseButton,monoBox,speedBox}) EnableWindow(h,!busy);
    EnableWindow(matchBox,!busy&&checked(monoBox));
    EnableWindow(repairBox,!busy&&checked(monoBox)&&checked(matchBox));
    EnableWindow(fromEdit,!busy&&changeSpeed()); EnableWindow(toEdit,!busy&&SendMessageW(speedBox,CB_GETCURSEL,0,0)==4);
    EnableWindow(runButton,!busy&&!input.empty()&&(checked(monoBox)||changeSpeed()));
}
HWND control(LPCWSTR cls,LPCWSTR text,DWORD style,int x,int y,int w,int h,int id=0) {
    HWND c=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,win,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);
    SendMessageW(c,WM_SETFONT,(WPARAM)font,TRUE); return c;
}
bool choose(bool save,std::wstring& path) {
    wchar_t buf[32768]{};
    if(save) { auto name=std::filesystem::path(input).stem().wstring()+L"-converted.wav"; wcsncpy_s(buf,name.c_str(),_TRUNCATE); }
    OPENFILENAMEW o{sizeof(o)}; o.hwndOwner=win; o.lpstrFile=buf; o.nMaxFile=32768;
    o.lpstrFilter=L"PCM WAV (*.wav)\0*.wav\0\0"; o.lpstrDefExt=L"wav";
    o.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    if(!(save?GetSaveFileNameW(&o):GetOpenFileNameW(&o))) return false;
    path=buf; return true;
}
double rpm(HWND h) {
    std::wstring s=getText(h); std::replace(s.begin(),s.end(),L',',L'.');
    size_t used=0; double v=std::stod(s,&used);
    if(used!=s.size()||!std::isfinite(v)||v<10||v>150) throw std::runtime_error("RPM must be between 10 and 150.");
    return v;
}
struct Result { bool ok=false; std::wstring text; };
void convert() {
    try {
        bool mono=checked(monoBox), match=checked(matchBox), repair=checked(repairBox), speed=changeSpeed();
        double from=speed?rpm(fromEdit):78, to=speed?rpm(toEdit):78, factor=to/from;
        if(factor<.25||factor>4) throw std::runtime_error("Speed ratio must be between 0.25 and 4.");
        std::wstring output; if(!choose(true,output)) return;
        if(_wcsicmp(input.c_str(),output.c_str())==0 || (std::filesystem::exists(output)&&std::filesystem::equivalent(input,output))) throw std::runtime_error("Choose a different output file. The original is protected.");
        const auto source=input;
        busy=true; refresh(); SetWindowTextW(statusLabel,L"Processing audio…"); SendMessageW(progress,PBM_SETMARQUEE,TRUE,30);
        worker=std::thread([=] {
            auto result=std::make_unique<Result>();
            auto temp=std::filesystem::path(output); temp+=L".mono78-"+std::to_wstring(GetCurrentProcessId())+L".tmp";
            bool ownTemp=false;
            try {
                // CREATE_NEW prevents collision with another conversion's temporary file.
                HANDLE h=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
                if(h==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create temporary output file.");
                CloseHandle(h); ownTemp=true;
                Wav w=readWav(utf8(source));
                if(mono&&w.channels==2) {
                    if(match) fuseStereoGrooveToMono(w,repair);
                    else { std::vector<float> m(w.frames()); for(size_t i=0;i<m.size();++i) m[i]=.5f*(w.samples[2*i]+w.samples[2*i+1]); w.samples.swap(m); w.channels=1; }
                }
                if(speed) w.samples=resamplePlaybackSpeed(w.samples,w.channels,factor);
                double peak=0; for(float v:w.samples) { if(!std::isfinite(v)) throw std::runtime_error("Invalid audio sample."); peak=std::max(peak,std::abs((double)v)); }
                double scale=peak>0.999999?0.999999/peak:1.0;
                if(scale<1) for(float& v:w.samples) v=(float)(v*scale);
                writeWavPcm(utf8(temp.wstring()),w.sampleRate,w.channels,24,w.samples);
                if(!MoveFileExW(temp.c_str(),output.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot move completed output file into place.");
                ownTemp=false;
                std::wostringstream report;
                report<<L"Complete — "<<w.channels<<L" channel(s), "<<w.sampleRate<<L" Hz / 24 bit.\r\n";
                report<<std::fixed<<std::setprecision(4)<<L"Playback speed factor: "<<factor<<L". Duration: "<<std::setprecision(2)<<(double)w.frames()/w.sampleRate<<L" s.";
                if(scale<1) report<<L"\r\nLevel reduced by "<<-20*std::log10(scale)<<L" dB to prevent clipping.";
                report<<L"\r\n"<<output; result->ok=true; result->text=report.str();
            } catch(const std::exception& e) { result->text=L"Conversion failed: "+wide(e.what()); }
            if(ownTemp) { std::error_code ec; std::filesystem::remove(temp,ec); }
            PostMessageW(win,Done,0,(LPARAM)result.release());
        });
    } catch(const std::exception& e) { busy=false; refresh(); MessageBoxW(win,wide(e.what()).c_str(),L"Check settings",MB_ICONWARNING); }
}
LRESULT CALLBACK proc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE: {
        win=h; font=CreateFontW(-17,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        titleFont=CreateFontW(-30,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        auto title=control(L"STATIC",L"MONO-78",0,26,20,640,40); SendMessageW(title,WM_SETFONT,(WPARAM)titleFont,TRUE);
        control(L"STATIC",L"Mono and playback speed • Standalone tool from Master-2000 • v1.1a",0,28,66,650,25);
        browseButton=control(L"BUTTON",L"Open WAV…",BS_PUSHBUTTON|WS_TABSTOP,28,108,150,34,1);
        fileLabel=control(L"STATIC",L"No file selected",SS_PATHELLIPSIS,190,116,475,28);
        monoBox=control(L"BUTTON",L"Convert to mono",BS_AUTOCHECKBOX|WS_TABSTOP,28,166,600,28,2); SendMessageW(monoBox,BM_SETCHECK,BST_CHECKED,0);
        matchBox=control(L"BUTTON",L"Master method: match channel level and polarity",BS_AUTOCHECKBOX|WS_TABSTOP,48,204,615,28,3); SendMessageW(matchBox,BM_SETCHECK,BST_CHECKED,0);
        repairBox=control(L"BUTTON",L"Also repair isolated clicks affecting one channel",BS_AUTOCHECKBOX|WS_TABSTOP,48,240,615,28,4);
        control(L"STATIC",L"Without the Master method, (L + R) / 2 is used. For mono records\r\ncaptured in stereo. Mono input keeps its single channel.",0,48,277,610,44);
        speedBox=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,28,340,600,200,5);
        for(const wchar_t* choice : {L"Keep playback speed unchanged",L"77.92 rpm — Europe",L"78.26 rpm — USA",L"78.00 rpm",L"Custom RPM"})
            SendMessageW(speedBox,CB_ADDSTRING,0,(LPARAM)choice);
        SendMessageW(speedBox,CB_SETCURSEL,0,0);
        control(L"STATIC",L"Recorded at (RPM)",0,48,382,220,24);
        control(L"STATIC",L"Target playback speed (RPM)",0,340,382,270,24);
        fromEdit=control(L"EDIT",L"45",WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,48,410,230,30);
        toEdit=control(L"EDIT",L"77.92",WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,340,410,230,30);
        control(L"STATIC",L"Example: 45 → 77.92. Speed and pitch change together.\r\nChoose unchanged speed if the file is already speed-corrected.",0,48,456,615,46);
        runButton=control(L"BUTTON",L"Convert and save as…",BS_DEFPUSHBUTTON|WS_TABSTOP,28,524,310,38,6);
        progress=control(PROGRESS_CLASSW,L"",PBS_MARQUEE,356,534,308,18);
        statusLabel=control(L"STATIC",L"Output: 24-bit PCM WAV, unchanged sample rate.\r\nThe original file is never overwritten.",0,28,584,640,118);
        refresh(); return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wp)==5 && HIWORD(wp)==CBN_SELCHANGE) { selectSpeed();refresh();return 0; }
        if(HIWORD(wp)==BN_CLICKED) {
            if(LOWORD(wp)==1) { std::wstring p; if(choose(false,p)) { input=p; SetWindowTextW(fileLabel,p.c_str()); } }
            if(LOWORD(wp)==6) convert();
            refresh();
        } return 0;
    case Done: {
        std::unique_ptr<Result> r((Result*)lp); if(worker.joinable()) worker.join(); busy=false;
        SendMessageW(progress,PBM_SETMARQUEE,FALSE,0); SetWindowTextW(statusLabel,r->text.c_str()); refresh();
        if(!r->ok) MessageBoxW(h,r->text.c_str(),L"Mono-78",MB_ICONERROR); return 0;
    }
    case WM_CLOSE: if(busy) { MessageBoxW(h,L"Wait until conversion has finished.",L"Mono-78",MB_OK); return 0; } DestroyWindow(h); return 0;
    case WM_DESTROY: DeleteObject(font); DeleteObject(titleFont); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h,msg,wp,lp);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    SetProcessDPIAware(); INITCOMMONCONTROLSEX ic{sizeof(ic),ICC_PROGRESS_CLASS}; InitCommonControlsEx(&ic);
    WNDCLASSW c{}; c.lpfnWndProc=proc; c.hInstance=instance; c.lpszClassName=L"Mono78Window"; c.hCursor=LoadCursorW(nullptr,IDC_ARROW); c.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); RegisterClassW(&c);
    RECT r{0,0,700,720}; AdjustWindowRect(&r,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE);
    HWND h=CreateWindowW(c.lpszClassName,L"Mono-78 v1.1a — Mono & Playback Speed",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,nullptr,nullptr,instance,nullptr);
    if(!h) return 1; ShowWindow(h,show); MSG m; while(GetMessageW(&m,nullptr,0,0)>0) { if(!IsDialogMessageW(h,&m)) { TranslateMessage(&m); DispatchMessageW(&m); } } return 0;
}
