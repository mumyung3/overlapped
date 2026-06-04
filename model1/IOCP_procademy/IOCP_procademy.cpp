#define _WINSOCK_DEPRECATED_NO_WARNINGS
#pragma comment(lib, "ws2_32")
#include <winsock2.h>
#include <stdlib.h>
#include <stdio.h>

#define SERVERPORT 9000
#define MINUSCONCURRENTTHREADS 3

#include <locale.h>  
#include <conio.h>
#include "ringbuffer.h"
struct MYOVERLAPPED {

	OVERLAPPED recvOverlapped;
	OVERLAPPED sendOverlapped;

};

struct SOCKETINFO {
	__int64 sessionID;
	SOCKET sock;
	MYOVERLAPPED overlapped;
	WSABUF RecvWSABuf;
	WSABUF SendWSABuf;
	RingBuffer RecvQ;
	RingBuffer SendQ;
	SOCKADDR_IN clientAddr;  // 추가
};

DWORD WINAPI WorkerThread(LPVOID arg);
__int64 GenerateSessionID() {
	static __int64 counter = 0;
	return ++counter;
}

struct ACCEPTINFO {
	SOCKET listen_sock;
	HANDLE hcp;
};

DWORD WINAPI AcceptThread(LPVOID arg);

int main()
{
	_wsetlocale(LC_ALL, L"");  // 시스템 로케일 사용

	int retval;

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

	SYSTEM_INFO si;
	GetSystemInfo(&si);

	HANDLE hcp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, si.dwNumberOfProcessors - MINUSCONCURRENTTHREADS);
	if (hcp == NULL) return 1;

	// 스레드 생성 시 핸들 보관
	HANDLE hThreads[64]{};

	// 논리프로세서 * n 개 스레드 생성
	HANDLE hThread;
	for (int i = 0; i < (int)si.dwNumberOfProcessors * 2; i++) {
		hThreads[i] = CreateThread(NULL, 0, WorkerThread, hcp, 0, NULL);
		if (hThreads[i] == NULL) return 1;
	}

	SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_sock == INVALID_SOCKET) __debugbreak();

	SOCKADDR_IN serveraddr;
	ZeroMemory(&serveraddr, sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serveraddr.sin_port = htons(SERVERPORT);

	retval = bind(listen_sock, (SOCKADDR*)&serveraddr, sizeof(serveraddr));
	if (retval == SOCKET_ERROR)  __debugbreak();

	retval = listen(listen_sock, SOMAXCONN);
	if (retval == SOCKET_ERROR) __debugbreak();

	ACCEPTINFO acceptInfo = { listen_sock, hcp };
	HANDLE hAcceptThread = CreateThread(NULL, 0, AcceptThread, &acceptInfo, 0, NULL);
	int threadCount;

	// main 루프 - 키보드 서버 제어
	while (1) {
		if (_kbhit()) {
			wchar_t ch = _getwch();
			if (ch == L'q') {
				// 종료 처리
				closesocket(listen_sock); // accept 스레드 탈출
				threadCount = (int)si.dwNumberOfProcessors * 2;
				for (int i = 0; i < threadCount; i++)
					PostQueuedCompletionStatus(hcp, 0, (ULONG_PTR)-1, NULL);

				break;
			}
		}
		Sleep(100);
	}

	WaitForMultipleObjects(threadCount, hThreads, TRUE, INFINITE);
	for (int i = 0; i < threadCount; i++)
		CloseHandle(hThreads[i]);


	CloseHandle(hcp);
	closesocket(listen_sock);
	WSACleanup();
	return 0;
}

DWORD WINAPI WorkerThread(LPVOID arg) {

	int retval;
	HANDLE hcp = (HANDLE)arg;

	while (1) {

		DWORD cbTransferred = 0;
		ULONG_PTR completionKey = 0;
		SOCKETINFO* session;
		LPOVERLAPPED pov;
		retval = GetQueuedCompletionStatus(hcp, &cbTransferred, (PULONG_PTR)&completionKey, (LPOVERLAPPED*)&pov, INFINITE);
		session = (SOCKETINFO*)completionKey;  // 여기서 캐스팅

		// 종료 신호
		if (completionKey == (ULONG_PTR)-1) {
			return 0;
		}

		// 오류 처리
		if (pov == nullptr) continue;

		// 비동기 입출력 결과확인
		if (retval == 0 || cbTransferred == 0) {
			if (retval == 0) {
				// overlap null이면서 반환값 실패 (io 실패) 연결끊김
				DWORD temp1, temp2;
				WSAGetOverlappedResult(session->sock, pov,
					&temp1, FALSE, &temp2);
				__debugbreak();
			}
			closesocket(session->sock);
			wprintf(L"[TCP 서버] 클라이언트 종료: IP 주소 = %S, 포트 번호=%d\n",
				inet_ntoa(session->clientAddr.sin_addr), ntohs(session->clientAddr.sin_port));
			delete session;
			continue;
		}

		if (pov == &session->overlapped.recvOverlapped) {
			session->RecvQ.MoveRear(cbTransferred);
			// 받은 데이터 출력
			wchar_t tempBuf[BUFSIZE + 1];
			int useSize = session->RecvQ.GetUseSize();
			session->RecvQ.Peek((char*)tempBuf, useSize);
			tempBuf[useSize / sizeof(wchar_t)] = L'\0';
			wprintf(L"[TCP/%S:%d] %s\n",
				inet_ntoa(session->clientAddr.sin_addr),
				ntohs(session->clientAddr.sin_port),
				tempBuf);

			// 에코: recvQ → sendQ 복사
			session->RecvQ.Dequeue((char*)tempBuf, useSize);
			session->SendQ.Enqueue((char*)tempBuf, useSize);

			// WSASend 등록
			session->SendWSABuf.buf = session->SendQ.GetFrontBufferPtr();
			session->SendWSABuf.len = session->SendQ.DirectDequeueSize();
			WSASend(session->sock, &session->SendWSABuf, 1, NULL, 0,
				&session->overlapped.sendOverlapped, NULL);
		}
		else {
			// send 완료 → sendQ에 남은 데이터 있으면 다시 send
			session->SendQ.MoveFront(cbTransferred);

			if (session->SendQ.GetUseSize() > 0) {
				session->SendWSABuf.buf = session->SendQ.GetFrontBufferPtr();
				session->SendWSABuf.len = session->SendQ.DirectDequeueSize();
				WSASend(session->sock, &session->SendWSABuf, 1, NULL, 0,
					&session->overlapped.sendOverlapped, NULL);
			}
			else {
				// 다 보냈으면 recv 등록
				session->RecvWSABuf.buf = session->RecvQ.GetRearBufferPtr();
				session->RecvWSABuf.len = session->RecvQ.DirectEnqueueSize();
				DWORD flags = 0;
				WSARecv(session->sock, &session->RecvWSABuf, 1, NULL, &flags,
					&session->overlapped.recvOverlapped, NULL);
			}

		}

	}
	return 0;

}

DWORD WINAPI AcceptThread(LPVOID arg) {
	ACCEPTINFO* info = (ACCEPTINFO*)arg;

	SOCKADDR_IN clientaddr;
	int addrlen;
	DWORD recvbytes, flags;

	while (1) {
		addrlen = sizeof(clientaddr);
		SOCKET client_sock = accept(info->listen_sock, (SOCKADDR*)&clientaddr, &addrlen);
		if (client_sock == INVALID_SOCKET) break;

		wprintf(L"[TCP 서버] 클라이언트 접속: IP 주소 =%S, 포트 번호 =%d\n",
			inet_ntoa(clientaddr.sin_addr), ntohs(clientaddr.sin_port));

		SOCKETINFO* ptr = new SOCKETINFO;
		ptr->sessionID = GenerateSessionID();
		ZeroMemory(&ptr->overlapped, sizeof(ptr->overlapped));
		ptr->sock = client_sock;
		ptr->clientAddr = clientaddr;
		ptr->RecvWSABuf.buf = ptr->RecvQ.GetRearBufferPtr();
		ptr->RecvWSABuf.len = ptr->RecvQ.DirectEnqueueSize();

		CreateIoCompletionPort((HANDLE)client_sock, info->hcp, (ULONG_PTR)ptr, 0);

		flags = 0;
		WSARecv(client_sock, &ptr->RecvWSABuf, 1, &recvbytes, &flags,
			&ptr->overlapped.recvOverlapped, NULL);
	}
	return 0;
}