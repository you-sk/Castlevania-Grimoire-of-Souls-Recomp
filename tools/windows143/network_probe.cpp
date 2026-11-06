// No Apple/account calls. TEST-NET-1 is used only after active firewall rules
// have been verified by the launcher. WSAEACCES is required before game launch.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <iterator>
int wmain(){
    WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))return 10;
    SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(s==INVALID_SOCKET)return 11;
    u_long nonblocking=1;ioctlsocket(s,FIONBIO,&nonblocking);
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(9);InetPtonW(AF_INET,L"192.0.2.1",&address.sin_addr);
    int result=connect(s,reinterpret_cast<sockaddr*>(&address),sizeof(address));int error=result==SOCKET_ERROR?WSAGetLastError():0;
    if(error==WSAEWOULDBLOCK){fd_set write,errorSet;FD_ZERO(&write);FD_ZERO(&errorSet);FD_SET(s,&write);FD_SET(s,&errorSet);timeval timeout{3,0};select(0,nullptr,&write,&errorSet,&timeout);int len=sizeof(error);getsockopt(s,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&len);}
    closesocket(s);WSACleanup();
    wchar_t own[MAX_PATH*4];GetModuleFileNameW(nullptr,own,DWORD(std::size(own)));
    STARTUPINFOW start{sizeof(start)};PROCESS_INFORMATION child{};
    BOOL spawned=CreateProcessW(own,nullptr,nullptr,nullptr,FALSE,CREATE_SUSPENDED,nullptr,nullptr,&start,&child);
    DWORD childError=spawned?0:GetLastError();if(spawned){TerminateProcess(child.hProcess,99);CloseHandle(child.hThread);CloseHandle(child.hProcess);}
    printf("{\"tcpConnectError\":%d,\"expectedAccessDenied\":10013,\"childDenied\":%s,\"childError\":%lu}\n",error,spawned?"false":"true",childError);
    return error==WSAEACCES&&!spawned?0:12;
}
