#define _WINSOCK_DEPRECATED_NO_WARNINGS
#pragma comment(lib, "ws2_32")
#include <winsock2.h>
#include <stdlib.h>
#include <stdio.h>

#include <locale.h>  
#include <conio.h>

#define SERVERPORT 9000
#define BUFSIZE 512

struct SOCKETINFO {
	WSAOVERLAPPED overlapped;
	SOCKET sock;
	wchar_t buf[BUFSIZE + 1];
	int recvbytes;
	int sendbytes;
	WSABUF wsabuf;

};

int nTotalSockets = 0;
SOCKETINFO* SocketInfoArray[WSA_MAXIMUM_WAIT_EVENTS];
WSAEVENT EventArray[WSA_MAXIMUM_WAIT_EVENTS];
CRITICAL_SECTION cs;

DWORD WINAPI WorkerThread(LPVOID arg);
BOOL AddSocketInfo(SOCKET sock);
void RemoveSocketInfo(int nIndex);

int main()
{
	_wsetlocale(LC_ALL, L"");  // 시스템 로케일 사용

	int retval;
	InitializeCriticalSection(&cs);

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)return 1;

	SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_sock == INVALID_SOCKET) __debugbreak();

	SOCKADDR_IN serveraddr;
	ZeroMemory(&serveraddr, sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serveraddr.sin_port = htons(SERVERPORT);
	retval = bind(listen_sock, (SOCKADDR*)&serveraddr, sizeof(serveraddr));
	if (retval == SOCKET_ERROR) __debugbreak();

	retval = listen(listen_sock, SOMAXCONN);
	if (retval == SOCKET_ERROR) __debugbreak();

	// 더미 이벤트 객체 생성
	WSAEVENT hEvent = WSACreateEvent();
	if (hEvent == WSA_INVALID_EVENT) __debugbreak();
	EventArray[nTotalSockets++] = hEvent;

	// 스레드 생성
	HANDLE hThread = CreateThread(NULL, 0, WorkerThread, NULL, 0, NULL);
	if (hThread == NULL) return 1;
	CloseHandle(hThread);

	SOCKET client_sock;
	SOCKADDR_IN clientaddr;
	int addrlen;
	DWORD recvbytes, flags;

	while (1) {
		// accpet()
		addrlen = sizeof(clientaddr);
		client_sock = accept(listen_sock, (SOCKADDR*)&clientaddr, &addrlen);
		if (client_sock == INVALID_SOCKET) {
			__debugbreak();
			break;
		}
		wprintf(L"\n[TCP 서버] 클라이언트 접속: IP 주소 = %S, 포트 번호 = %d\n", inet_ntoa(clientaddr.sin_addr), ntohs(clientaddr.sin_port));

		if (AddSocketInfo(client_sock) == FALSE) {

			closesocket(client_sock);
			wprintf(L"[TCP 서버] 클라이언트 종료: IP 주소 = %S, 포트 번호 =%d\n",
				inet_ntoa(clientaddr.sin_addr), ntohs(clientaddr.sin_port));
			continue;
		}

		// 비동기 입출력 시작
		SOCKETINFO* ptr = SocketInfoArray[nTotalSockets - 1];
		flags = 0;
		retval = WSARecv(ptr->sock, &ptr->wsabuf, 1, &recvbytes, &flags, &ptr->overlapped, NULL);
		if (retval == SOCKET_ERROR) {
			if (WSAGetLastError() != WSA_IO_PENDING) {
				__debugbreak();
				RemoveSocketInfo(nTotalSockets - 1);
				continue;
			}
		}

		WSASetEvent(EventArray[0]); // 소켓 추가 삭제시 워커 스레드 깨우는 용도

	}

	WSACleanup();
	DeleteCriticalSection(&cs);
	return 0;


}

DWORD WINAPI WorkerThread(LPVOID arg) {
	int retval;

	while (1) {
		DWORD index = WSAWaitForMultipleEvents(nTotalSockets, EventArray, FALSE, WSA_INFINITE, FALSE); // 여기 ntotalsocket을 갱신된 크기로 대기하기 위해서임! sevent[0] 하는 이유
		if (index == WSA_WAIT_FAILED) continue;
		index -= WSA_WAIT_EVENT_0;
		WSAResetEvent(EventArray[index]);
		if (index == 0) continue;

		SOCKETINFO* ptr = SocketInfoArray[index];
		SOCKADDR_IN clientaddr;
		int addrlen = sizeof(clientaddr);
		getpeername(ptr->sock, (SOCKADDR*)&clientaddr, &addrlen);

		// 비동기 입출력 결과 확인
		DWORD cbTransferred, flags;
		retval = WSAGetOverlappedResult(ptr->sock, &ptr->overlapped, &cbTransferred, FALSE, &flags);
		if (retval == FALSE || cbTransferred == 0) { // recv  : cbTransferred 0 수신 cbTransferred >0 정상 데이터 수신 
			RemoveSocketInfo(index);
			wprintf(L"[TCP 서버] 클라이언트 종료 : IP 주소= %S, 포트 번호= %d\n",
				inet_ntoa(clientaddr.sin_addr), ntohs(clientaddr.sin_port));
			continue;
		}

		// 정상 데이터 수신 후 로직

		// 데이터 전송량 갱신
		if (ptr->recvbytes == 0) {
			ptr->recvbytes = cbTransferred;
			ptr->sendbytes = 0;

			ptr->buf[ptr->recvbytes / sizeof(wchar_t)] = L'\0';
			wprintf(L"[TCP/%S:%d] %s\n", inet_ntoa(clientaddr.sin_addr), ntohs(clientaddr.sin_port), ptr->buf);
		}
		else { // 읽기 바이트가 있으면 이건 송신이 아닌 수신이 성공함.
			ptr->sendbytes += cbTransferred;
		}

		if (ptr->recvbytes > ptr->sendbytes) { // 데이터 계속 send 보내야함.
			// 마저 데이터 보내기 링버퍼로 간단하게 가능할듯함.
			ZeroMemory(&ptr->overlapped, sizeof(ptr->overlapped)); // 재사용인듯?
			ptr->overlapped.hEvent = EventArray[index];
			ptr->wsabuf.buf = (char*)ptr->buf + ptr->sendbytes;
			ptr->wsabuf.len = (ptr->recvbytes - ptr->sendbytes);


			DWORD sendbytes;
			retval = WSASend(ptr->sock, &ptr->wsabuf, 1, &sendbytes, 0, &ptr->overlapped, NULL);
			if (retval == SOCKET_ERROR) {
				if (WSAGetLastError() != WSA_IO_PENDING) {
					__debugbreak();
					// 소켓 삭제 해야하지 않나?
				}
				continue;
			}
		}
		else { // 데이터 다보냈으니 이제 다시 recv 해야함.

			ptr->recvbytes = 0;

			// 데이터 받기
			ZeroMemory(&ptr->overlapped, sizeof(ptr->overlapped));
			ptr->overlapped.hEvent = EventArray[index];
			ptr->wsabuf.buf = (char*)ptr->buf;
			ptr->wsabuf.len = BUFSIZE * sizeof(wchar_t);

			DWORD recvbytes;
			flags = 0;
			retval = WSARecv(ptr->sock, &ptr->wsabuf, 1, &recvbytes, &flags, &ptr->overlapped, NULL);
			if (retval == SOCKET_ERROR) {
				if (WSAGetLastError() != WSA_IO_PENDING) __debugbreak(); //여기도 소켓 삭제 해야하지 않나?
				continue;
			}
		}


	}
}

BOOL AddSocketInfo(SOCKET sock) {
	EnterCriticalSection(&cs);
	if (nTotalSockets >= WSA_MAXIMUM_WAIT_EVENTS) return FALSE;

	SOCKETINFO* ptr = new SOCKETINFO;

	if (ptr == NULL) return FALSE;

	WSAEVENT hEvent = WSACreateEvent();

	if (hEvent == WSA_INVALID_EVENT) return FALSE;

	ZeroMemory(&ptr->overlapped, sizeof(ptr->overlapped));
	ptr->overlapped.hEvent = hEvent;
	ptr->sock = sock;
	ptr->recvbytes = ptr->sendbytes = 0;
	ptr->wsabuf.buf = (char*)ptr->buf;
	ptr->wsabuf.len = BUFSIZE * sizeof(wchar_t);
	SocketInfoArray[nTotalSockets] = ptr;
	EventArray[nTotalSockets] = hEvent;
	++nTotalSockets;

	LeaveCriticalSection(&cs);
	return TRUE;
}

void RemoveSocketInfo(int nIndex) {
	EnterCriticalSection(&cs);

	SOCKETINFO* ptr = SocketInfoArray[nIndex];
	closesocket(ptr->sock);
	delete ptr;
	WSACloseEvent(EventArray[nIndex]);

	if (nIndex != (nTotalSockets - 1)) {
		SocketInfoArray[nIndex] = SocketInfoArray[nTotalSockets - 1];
		EventArray[nIndex] = EventArray[nTotalSockets - 1];
	}
	--nTotalSockets;

	LeaveCriticalSection(&cs);
}

// 이벤트 와 완료루틴의 차이는 이벤트는 전역 이벤트라 메인스레드에서 요청이 가능하다는점. 완료루틴같은경우 자기만의 스레드의 apc 큐를 사용하므로 다른 스레드에서 요청을 못한다는점