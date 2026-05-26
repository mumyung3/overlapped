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

SOCKET client_sock;
HANDLE hReadEvent, hWriteEvent;


DWORD WINAPI WorkerThread(LPVOID arg);
void CALLBACK CompletionRoutine(
	DWORD dwError, DWORD cbTransferred,
	LPWSAOVERLAPPED lpOverlapped, DWORD dwFlags);

int main()
{
	_wsetlocale(LC_ALL, L"");  // 시스템 로케일 사용

	int retval;

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

	// 이벤트 객체 생성
	hReadEvent = CreateEvent(NULL, FALSE, TRUE, NULL);
	if (hReadEvent == NULL) return 1;
	hWriteEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	if (hWriteEvent == NULL) return 1;

	// 스레드 생성
	HANDLE hThread = CreateThread(NULL, 0, WorkerThread, NULL, 0, NULL);
	if (hThread == NULL) return 1;
	CloseHandle(hThread);


	while (1) {
		WaitForSingleObject(hReadEvent, INFINITE);
		//accept
		client_sock = accept(listen_sock, NULL, NULL);
		if (client_sock == INVALID_SOCKET) {
			__debugbreak();
			break;
		}
		// 송신버퍼 0 만들기
		int sndbuf = 0;
		int ret = setsockopt(client_sock, SOL_SOCKET, SO_SNDBUF,
			(const char*)&sndbuf, sizeof(sndbuf));
		if (ret == SOCKET_ERROR)
		{
			// WSAGetLastError()로 확인
			__debugbreak();
		}

		SetEvent(hWriteEvent);
	}

	WSACleanup();
	return 0;


}

DWORD WINAPI WorkerThread(LPVOID arg) {
	int retval;

	while (1) {
		while (1) {
			// alertable wait
			DWORD result = WaitForSingleObjectEx(hWriteEvent, INFINITE, TRUE);
			if (result == WAIT_OBJECT_0) break; // 아래 코드 실행 accept 성공!
			if (result != WAIT_IO_COMPLETION) return 1; // apc 큐 호출 , 다시 while 돌며 재대기
		}

		// 접속 클라 출력
		SOCKADDR_IN clientaddr;
		int addrlen = sizeof(clientaddr);
		getpeername(client_sock, (SOCKADDR*)&clientaddr, &addrlen);
		wprintf(L"\n[TCP 서버] 클라이언트 접속: IP 주소=%S, 포트번호=%d\n",
			inet_ntoa(clientaddr.sin_addr), ntohs(clientaddr.sin_port));

		// 소켓 정보 구조체 할당과 초기화
		SOCKETINFO* ptr = new SOCKETINFO;
		if (ptr == NULL) {
			wprintf(L"[오류] 메모리가 부족합니다!\n");
			return 1;
		}
		ZeroMemory(&ptr->overlapped, sizeof(ptr->overlapped));
		ptr->sock = client_sock;
		SetEvent(hReadEvent);
		ptr->recvbytes = ptr->sendbytes = 0;
		ptr->wsabuf.buf = (char*)ptr->buf;
		ptr->wsabuf.len = BUFSIZE * sizeof(wchar_t);

		// 비동기 입출력 시작
		DWORD recvbytes;
		DWORD flags = 0;
		retval = WSARecv(ptr->sock, &ptr->wsabuf, 1, &recvbytes, &flags, &ptr->overlapped, CompletionRoutine);
		if (retval == SOCKET_ERROR) {
			int temp = WSAGetLastError();
			if (temp != WSA_IO_PENDING) {
				__debugbreak();
				return 1;
			}
		}

	}
	return 0;
}


void CALLBACK CompletionRoutine(
	DWORD dwError, DWORD cbTransferred,
	LPWSAOVERLAPPED lpOverlapped, DWORD dwFlags) {

	int retval;

	// 클라 정보 얻기
	SOCKETINFO* ptr = (SOCKETINFO*)lpOverlapped;
	SOCKADDR_IN clientaddr;
	int addrlen = sizeof(clientaddr);
	getpeername(ptr->sock, (SOCKADDR*)&clientaddr, &addrlen);

	// 비동기 입출력 결과 확인
	if (dwError != 0 || cbTransferred == 0) {
		if (dwError != 0) __debugbreak();
		closesocket(ptr->sock);
		wprintf(L"[TCP 서버] 클라이언트 종료: IP 주소= %S, 포트 번호=%d\n",
			inet_ntoa(clientaddr.sin_addr), ntohs(clientaddr.sin_port));
		delete ptr;
		return;
	}

	// 데이터 전송량 갱신
	if (ptr->recvbytes == 0) {
		ptr->recvbytes = cbTransferred;
		ptr->sendbytes = 0;
		// 받은 데이터 출력
		ptr->buf[ptr->recvbytes / sizeof(wchar_t)] = L'\0';
		wprintf(L"[TCP/%S:%d] %s\n", inet_ntoa(clientaddr.sin_addr),
			ntohs(clientaddr.sin_port), ptr->buf);
	}
	else {
		ptr->sendbytes += cbTransferred;
	}

	if (ptr->recvbytes > ptr->sendbytes) {
		// 데이터 보내기
		ZeroMemory(&ptr->overlapped, sizeof(ptr->overlapped));
		ptr->wsabuf.buf = (char*)ptr->buf + ptr->sendbytes;
		ptr->wsabuf.len = ptr->recvbytes - ptr->sendbytes;

		DWORD sendbytes;
		retval = WSASend(ptr->sock, &ptr->wsabuf, 1, &sendbytes, 0, &ptr->overlapped, CompletionRoutine); // 0반환해서 즉시 카피 확인, 
		if (retval == SOCKET_ERROR) {
			int temp = WSAGetLastError();
			if (temp != WSA_IO_PENDING) {
				__debugbreak();
				return;
			}
		}
	}
	else {
		ptr->recvbytes = 0;

		// 데이터 받기
		ZeroMemory(&ptr->overlapped, sizeof(ptr->overlapped));
		ptr->wsabuf.buf = (char*)ptr->buf;
		ptr->wsabuf.len = BUFSIZE * sizeof(wchar_t);

		DWORD recvbytes;
		DWORD flags = 0;
		retval = WSARecv(ptr->sock, &ptr->wsabuf, 1, &recvbytes, &flags, &ptr->overlapped, CompletionRoutine); // -1 반환해서 실패했고, pending 됨을 에러로 확인
		if (retval == SOCKET_ERROR) {
			int temp = WSAGetLastError();
			if (temp != WSA_IO_PENDING) {
				__debugbreak();
				return;
			}
		}
	}

}