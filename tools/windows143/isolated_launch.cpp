// Windows-only launcher. Creates the game suspended, confines it to a one-
// process job and denies child creation before any game instruction executes.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
static int Error(const char* operation) { fprintf(stderr,"%s failed: %lu\n",operation,GetLastError());return 125; }
int wmain(int argc,wchar_t**argv) {
    if(argc!=4){fputs("usage: isolated-launch.exe GAME_EXE OUTPUT_DIR probe|game\n",stderr);return 125;}
    const bool probe=std::wstring(argv[3])==L"probe";
    std::wstring exe=argv[1],out=argv[2],cwd=exe.substr(0,exe.find_last_of(L"\\/"));
    std::wstring command=L"\""+exe+L"\"";
    if(!probe)command+=L" -force-d3d11 -screen-width 1280 -screen-height 720 -screen-fullscreen 0 -logFile \""+out+L"\\Player.log\"";
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    HANDLE log=CreateFileW((out+L"\\launcher-game-output.txt").c_str(),GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(log==INVALID_HANDLE_VALUE)return Error("CreateFile");
    HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr);
    HANDLE job=CreateJobObjectW(nullptr,nullptr);if(!job)return Error("CreateJobObject");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_ACTIVE_PROCESS|JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    limits.BasicLimitInformation.ActiveProcessLimit=1;
    if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))return Error("SetInformationJobObject");
    SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,2,0,&size);
    std::vector<unsigned char> attributes(size);STARTUPINFOEXW start{};start.StartupInfo.cb=sizeof(start);
    start.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if(!InitializeProcThreadAttributeList(start.lpAttributeList,2,0,&size))return Error("InitializeProcThreadAttributeList");
    DWORD policy=PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
    if(!UpdateProcThreadAttribute(start.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY,&policy,sizeof(policy),nullptr,nullptr))return Error("ChildPolicy");
    HANDLE handles[]={log,input};
    if(!UpdateProcThreadAttribute(start.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),nullptr,nullptr))return Error("HandleList");
    start.StartupInfo.dwFlags=STARTF_USESTDHANDLES;start.StartupInfo.hStdInput=input;start.StartupInfo.hStdOutput=log;start.StartupInfo.hStdError=log;
    PROCESS_INFORMATION process{};
    if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,cwd.c_str(),&start.StartupInfo,&process))return Error("CreateProcess");
    if(!AssignProcessToJobObject(job,process.hProcess)){TerminateProcess(process.hProcess,125);return Error("AssignProcessToJobObject");}
    JOBOBJECT_BASIC_LIMIT_INFORMATION actual{};
    if(!QueryInformationJobObject(job,JobObjectBasicLimitInformation,&actual,sizeof(actual),nullptr)||actual.ActiveProcessLimit!=1){TerminateProcess(process.hProcess,125);return Error("VerifyJob");}
    if(ResumeThread(process.hThread)==DWORD(-1)){TerminateProcess(process.hProcess,125);return Error("ResumeThread");}
    std::wstring report=out+L"\\launch-proof.json";FILE* f=nullptr;_wfopen_s(&f,report.c_str(),L"wb");
    if(!f){TerminateProcess(process.hProcess,125);return 125;}
    fprintf(f,"{\"pid\":%lu,\"jobActiveProcessLimit\":1,\"childCreationRestricted\":true,\"assignedBeforeResume\":true}",process.dwProcessId);fclose(f);
    WaitForSingleObject(process.hProcess,INFINITE);DWORD exitCode=125;GetExitCodeProcess(process.hProcess,&exitCode);
    _wfopen_s(&f,(out+L"\\exit-code.txt").c_str(),L"wb");if(f){fprintf(f,"%lu",exitCode);fclose(f);}
    CloseHandle(process.hThread);CloseHandle(process.hProcess);DeleteProcThreadAttributeList(start.lpAttributeList);CloseHandle(job);CloseHandle(log);CloseHandle(input);
    return int(exitCode);
}
