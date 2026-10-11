#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <xinput.h>
#include <cmath>
#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>
#include <array>
#include <cstdio>

static HMODULE ownModule=nullptr;
static constexpr UINT TICK=1, TRAY=WM_APP+1;
static constexpr int MLEFT=256,MRIGHT=257,MMIDDLE=258;
static const wchar_t* labels[]={L"A",L"B",L"X",L"Y",L"LB",L"RB",L"Back",L"Start",L"L3",L"DPadUp",L"DPadDown",L"DPadLeft",L"DPadRight",L"LT",L"RT"};
static const WORD masks[]={XINPUT_GAMEPAD_A,XINPUT_GAMEPAD_B,XINPUT_GAMEPAD_X,XINPUT_GAMEPAD_Y,XINPUT_GAMEPAD_LEFT_SHOULDER,XINPUT_GAMEPAD_RIGHT_SHOULDER,XINPUT_GAMEPAD_BACK,XINPUT_GAMEPAD_START,XINPUT_GAMEPAD_LEFT_THUMB,XINPUT_GAMEPAD_DPAD_UP,XINPUT_GAMEPAD_DPAD_DOWN,XINPUT_GAMEPAD_DPAD_LEFT,XINPUT_GAMEPAD_DPAD_RIGHT,0,0};
static const wchar_t* defaults[]={L"SPACE",L"MIDDLE_MOUSE",L"E",L"R",L"SHIFT",L"Q",L"TAB",L"ESC",L"CTRL",L"F",L"C",L"B",L"M",L"RIGHT_MOUSE",L"LEFT_MOUSE"};
struct Stick{double x,y;};
static Stick stick(SHORT x,SHORT y,double dead){
    double nx=x/32768.0,ny=y/32768.0,m=std::sqrt(nx*nx+ny*ny);
    if(m<=dead)return {0,0};
    double scaled=(std::min(m,1.0)-dead)/(1.0-dead);
    return {nx/m*scaled,ny/m*scaled};
}
static int key(const std::wstring& text){
    if(text==L"LEFT_MOUSE")return MLEFT;
    if(text==L"RIGHT_MOUSE")return MRIGHT;
    if(text==L"MIDDLE_MOUSE")return MMIDDLE;
    if(text==L"SPACE")return VK_SPACE;
    if(text==L"SHIFT")return VK_SHIFT;
    if(text==L"CTRL")return VK_CONTROL;
    if(text==L"ALT")return VK_MENU;
    if(text==L"TAB")return VK_TAB;
    if(text==L"ESC")return VK_ESCAPE;
    if(text==L"ENTER")return VK_RETURN;
    if(text==L"NONE")return -1;
    if(text.size()==1&&((text[0]>=L'A'&&text[0]<=L'Z')||(text[0]>=L'0'&&text[0]<=L'9')))return text[0];
    return -2;
}
struct Mapper{
    HWND window{};NOTIFYICONDATAW tray{};HMODULE xinput{};
    DWORD(WINAPI* getState)(DWORD,XINPUT_STATE*){};
    std::wstring directory,ini,gameDirectory;
    std::array<int,15> bindings{};
    std::array<bool,259> held{};
    double leftDead=.24,rightDead=.18,cameraSpeed=1000,menuSpeed=650,fx=0,fy=0;
    WORD previous=0;bool menu=false,connected=false,focused=false,hadGame=false,autoStop=false;
    ULONGLONG last=0,missingSince=0;DWORD gamePid=0;
    bool inject(int code,bool down){
        INPUT input{};
        if(code>=MLEFT){
            input.type=INPUT_MOUSE;
            static const DWORD on[]={MOUSEEVENTF_LEFTDOWN,MOUSEEVENTF_RIGHTDOWN,MOUSEEVENTF_MIDDLEDOWN};
            static const DWORD off[]={MOUSEEVENTF_LEFTUP,MOUSEEVENTF_RIGHTUP,MOUSEEVENTF_MIDDLEUP};
            input.mi.dwFlags=down?on[code-MLEFT]:off[code-MLEFT];
        }else{
            input.type=INPUT_KEYBOARD;
            input.ki.wScan=(WORD)MapVirtualKeyW(code,MAPVK_VK_TO_VSC);
            input.ki.dwFlags=KEYEVENTF_SCANCODE|(down?0:KEYEVENTF_KEYUP);
        }
        return SendInput(1,&input,sizeof(input))==1;
    }
    void release(){for(int i=0;i<(int)held.size();++i)if(held[i]){inject(i,false);held[i]=false;}fx=fy=0;}
    std::wstring pathFor(DWORD pid){
        HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
        if(!process)return L"";
        wchar_t path[32768];DWORD length=32768;
        bool ok=QueryFullProcessImageNameW(process,0,path,&length)!=0;CloseHandle(process);
        return ok?std::wstring(path,length):L"";
    }
    bool matches(const std::wstring& path){
        return !_wcsicmp(path.c_str(),(gameDirectory+L"\\Cube.exe").c_str())||!_wcsicmp(path.c_str(),(gameDirectory+L"\\Cube-SteamWater-Preview.exe").c_str());
    }
    bool foreground(){
        DWORD pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&pid);
        bool result=pid&&matches(pathFor(pid));
        if(result){gamePid=pid;hadGame=true;missingSince=0;}return result;
    }
    void status(){
        std::wstring text=L"Alpha controller: ";
        text+=!connected?L"Xbox disconnected":!focused?L"waiting for alpha":menu?L"menu mouse mode":L"gameplay mode";
        wcsncpy_s(tray.szTip,text.c_str(),_TRUNCATE);Shell_NotifyIconW(NIM_MODIFY,&tray);
    }
    bool load(){
        wchar_t file[32768];GetModuleFileNameW(ownModule,file,32768);
        directory=file;directory=directory.substr(0,directory.find_last_of(L"\\/"));
        gameDirectory=directory.substr(0,directory.find_last_of(L"\\/"));
        #ifdef ALPHA_CONTROLLER_DLL
        ini=gameDirectory+L"\\controller\\AlphaController.ini";
#else
        ini=directory+L"\\AlphaController.ini";
#endif
        wchar_t value[256];
        for(int i=0;i<15;++i){GetPrivateProfileStringW(L"Bindings",labels[i],defaults[i],value,256,ini.c_str());CharUpperBuffW(value,(DWORD)wcslen(value));bindings[i]=key(value);if(bindings[i]==-2)return false;}
        auto setting=[&](const wchar_t* name,const wchar_t* fallback){GetPrivateProfileStringW(L"Settings",name,fallback,value,256,ini.c_str());return wcstod(value,nullptr);};
        leftDead=setting(L"LeftDeadzone",L"0.24");rightDead=setting(L"RightDeadzone",L"0.18");
        cameraSpeed=setting(L"CameraPixelsPerSecond",L"1000");menuSpeed=setting(L"MenuPixelsPerSecond",L"650");
        if(!(leftDead>=0&&leftDead<.95&&rightDead>=0&&rightDead<.95&&cameraSpeed>0&&cameraSpeed<=10000&&menuSpeed>0&&menuSpeed<=10000))return false;
        for(const wchar_t* name:{L"xinput1_4.dll",L"xinput1_3.dll",L"xinput9_1_0.dll"}){
            xinput=LoadLibraryW(name);if(!xinput)continue;
            getState=(decltype(getState))GetProcAddress(xinput,"XInputGetState");
            if(getState)return true;FreeLibrary(xinput);xinput=nullptr;
        }return false;
    }
    void tick(){
        ULONGLONG now=GetTickCount64();double dt=last?std::min((now-last)/1000.0,.05):.008;last=now;
        XINPUT_STATE state{};bool found=false;
        for(DWORD i=0;i<4;++i)if(getState(i,&state)==ERROR_SUCCESS){found=true;break;}
        bool active=foreground();
        if(connected!=found||focused!=active){connected=found;focused=active;status();}
        if(autoStop&&hadGame&&!active&&gamePid){
            HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,gamePid);
            bool ended=!process||WaitForSingleObject(process,0)==WAIT_OBJECT_0;
            if(process)CloseHandle(process);
            if(ended){if(!missingSince)missingSince=now;if(now-missingSince>2000){DestroyWindow(window);return;}}
        }
        if(!found||!active){release();previous=found?state.Gamepad.wButtons:0;return;}
        auto& gp=state.Gamepad;WORD buttons=gp.wButtons;
        bool toggle=(buttons&XINPUT_GAMEPAD_RIGHT_THUMB)&&!(previous&XINPUT_GAMEPAD_RIGHT_THUMB);
        if(toggle){release();menu=!menu;status();}
        previous=buttons;
        std::array<bool,259> desired{};
        auto want=[&](int code,bool down){if(code>=0&&down)desired[code]=true;};
        Stick left=stick(gp.sThumbLX,gp.sThumbLY,leftDead);
        if(!menu){want('A',left.x<-.25);want('D',left.x>.25);want('W',left.y>.25);want('S',left.y<-.25);}
        for(int i=0;i<15;++i){
            bool down=i==13?gp.bLeftTrigger>40:i==14?gp.bRightTrigger>40:(buttons&masks[i])!=0;
            int binding=menu&&i==0?MLEFT:menu&&i==1?MRIGHT:bindings[i];want(binding,down);
        }
        for(int i=0;i<(int)held.size();++i)if(held[i]!=desired[i]){
            if(inject(i,desired[i]))held[i]=desired[i];
        }
        Stick right=stick(gp.sThumbRX,gp.sThumbRY,rightDead);
        double magnitude=std::sqrt(right.x*right.x+right.y*right.y);
        double speed=menu?menuSpeed:cameraSpeed;
        fx+=right.x*magnitude*speed*dt;fy-=right.y*magnitude*speed*dt;
        LONG dx=(LONG)fx,dy=(LONG)fy;fx-=dx;fy-=dy;
        if(dx||dy){INPUT input{};input.type=INPUT_MOUSE;input.mi.dx=dx;input.mi.dy=dy;input.mi.dwFlags=MOUSEEVENTF_MOVE;SendInput(1,&input,sizeof(input));}
    }
};
static Mapper* mapper=nullptr;
static LRESULT CALLBACK windowProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l){
    if(msg==WM_TIMER&&mapper){mapper->tick();return 0;}
    if(msg==TRAY&&mapper&&(l==WM_RBUTTONUP||l==WM_LBUTTONUP)){
        HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,1,L"Open bindings");AppendMenuW(menu,MF_STRING,2,L"Toggle menu mouse mode (R3)");AppendMenuW(menu,MF_STRING,3,L"Exit controller mapper");
        POINT p;GetCursorPos(&p);SetForegroundWindow(hwnd);int selected=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,p.x,p.y,0,hwnd,nullptr);DestroyMenu(menu);
        if(selected==1)ShellExecuteW(nullptr,L"open",mapper->ini.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
        if(selected==2){mapper->release();mapper->menu=!mapper->menu;mapper->status();}
        if(selected==3)DestroyWindow(hwnd);return 0;
    }
    if(msg==WM_DESTROY){if(mapper){mapper->release();Shell_NotifyIconW(NIM_DELETE,&mapper->tray);}PostQuitMessage(0);return 0;}
    return DefWindowProcW(hwnd,msg,w,l);
}
static int selfTest(){
    int failures=0;auto check=[&](bool test){if(!test)++failures;};
    check(stick(0,0,.24).x==0);check(stick(1000,-1000,.24).y==0);
    auto diagonal=stick(32767,32767,.24);check(std::abs(std::sqrt(diagonal.x*diagonal.x+diagonal.y*diagonal.y)-1)<.0001);
    check(stick(-32768,0,.24).x==-1);check(key(L"MIDDLE_MOUSE")==MMIDDLE);check(key(L"Q")=='Q');check(key(L"NONE")==-1);check(key(L"INVALID")==-2);
    return failures?1:0;
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR args,int){
    if(wcsstr(args,L"--self-test"))return selfTest();
    HANDLE mutex=CreateMutexW(nullptr,FALSE,L"Local\\CubeAlphaXboxMapper");if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(mutex);return 0;}
    Mapper app;mapper=&app;app.autoStop=wcsstr(args,L"--auto-stop")!=nullptr;
    if(!app.load()){MessageBoxW(nullptr,L"Check AlphaController.ini settings and Windows XInput availability.",L"Alpha controller could not start",MB_ICONERROR);CloseHandle(mutex);return 1;}
    WNDCLASSW cls{};cls.lpfnWndProc=windowProc;cls.hInstance=instance;cls.lpszClassName=L"CubeAlphaControllerMapper";RegisterClassW(&cls);
    app.window=CreateWindowW(cls.lpszClassName,L"Alpha Xbox Controller",0,0,0,0,0,nullptr,nullptr,instance,nullptr);
    app.tray.cbSize=sizeof(app.tray);app.tray.hWnd=app.window;app.tray.uID=1;app.tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;app.tray.uCallbackMessage=TRAY;app.tray.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    wcscpy_s(app.tray.szTip,L"Alpha controller: waiting for game");Shell_NotifyIconW(NIM_ADD,&app.tray);
    SetTimer(app.window,TICK,8,nullptr);
    MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    FreeLibrary(app.xinput);CloseHandle(mutex);return 0;
}


#ifdef ALPHA_CONTROLLER_DLL
extern "C" __declspec(dllexport) int __cdecl ControllerSelfTest(){return selfTest();}
static DWORD WINAPI controllerThread(LPVOID parameter){
    HMODULE module=(HMODULE)parameter;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,(LPCWSTR)&controllerThread,&module);
    wchar_t empty[]=L"";
    DWORD result=wWinMain(module,nullptr,empty,SW_HIDE);
    FreeLibraryAndExitThread(module,result);
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){
        wchar_t exe[32768];GetModuleFileNameW(nullptr,exe,32768);
        const wchar_t* name=wcsrchr(exe,L'\\');name=name?name+1:exe;
        if(_wcsicmp(name,L"Cube.exe")&&_wcsicmp(name,L"Cube-SteamWater-Preview.exe"))return TRUE;
        ownModule=module;DisableThreadLibraryCalls(module);
        HANDLE thread=CreateThread(nullptr,0,controllerThread,module,0,nullptr);
        if(thread)CloseHandle(thread);
    }
    return TRUE;
}
#endif

