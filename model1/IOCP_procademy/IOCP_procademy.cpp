#define _WINSOCK_DEPRECATED_NO_WARNINGS
#pragma comment(lib, "ws2_32")
#pragma comment(lib, "mswsock.lib") // AcceptEx, DisconnectEx 등

#include <winsock2.h>
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>

#define SERVERPORT 6000
#define MINUSCONCURRENTTHREADS 3

#include <locale.h>  
#include <conio.h>
#include "ringbuffer.h"
#include <map>

SRWLOCK sessionLock = SRWLOCK_INIT;
struct MYOVERLAPPED {

	OVERLAPPED recvOverlapped;
	OVERLAPPED sendOverlapped;

};

struct SOCKETINFO {
	__int64 sessionID{};
	SOCKET sock{};
	MYOVERLAPPED overlapped{};
	WSABUF RecvWSABuf{};
	WSABUF SendWSABuf{};
	RingBuffer RecvQ{};
	RingBuffer SendQ{};
	SOCKADDR_IN clientAddr{};  // 추가
	LONG ioCount = 0;
	bool disconnected = false;
};
std::map<__int64, SOCKETINFO*> sessionMap;

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

void PostSend(SOCKETINFO* session);
void PostRecv(SOCKETINFO* session);

HANDLE hSessionEmpty{};
int main()
{
	_wsetlocale(LC_ALL, L"");  // 시스템 로케일 사용

	//이벤트 생성. 세션 전부 제거 대기용
	hSessionEmpty = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (hSessionEmpty == NULL) return 1;

	int retval;

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

	SYSTEM_INFO si;
	GetSystemInfo(&si);

	HANDLE hcp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, si.dwNumberOfProcessors - MINUSCONCURRENTTHREADS);
	if (hcp == NULL) return 1;

	// 스레드 생성 시 핸들 보관
	HANDLE hThreads[64] = {};

	// 논리프로세서 * n 개 스레드 생성
	HANDLE hThread{};
	int threadCount = ((int)si.dwNumberOfProcessors - MINUSCONCURRENTTHREADS) * 2;
	for (int i = 0; i < threadCount; i++) {
		hThreads[i] = CreateThread(NULL, 0, WorkerThread, hcp, 0, NULL);
		if (hThreads[i] == NULL) {
			for (int j = 0; j < i; j++)
				CloseHandle(hThreads[j]);
			return 1;
		}
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
	if (hAcceptThread == NULL) return 1;

	// main 루프 - 키보드 서버 제어
	while (1) {
		if (_kbhit()) {
			wchar_t ch = _getwch();
			if (ch == L'q') {
				// 종료 처리

				closesocket(listen_sock);
				AcquireSRWLockExclusive(&sessionLock);
				for (auto& [id, session] : sessionMap) {
					closesocket(session->sock);
				}
				bool empty = sessionMap.empty();  // ← 락 안에서 체크 
				ReleaseSRWLockExclusive(&sessionLock);

				if (empty)
					SetEvent(hSessionEmpty);

				WaitForSingleObject(hSessionEmpty, INFINITE);  // 항상 대기

				for (int i = 0; i < threadCount; i++)
					PostQueuedCompletionStatus(hcp, 0, (ULONG_PTR)-1, NULL);

				break;
			}
		}
		Sleep(100);
	}

	WaitForMultipleObjects(threadCount, hThreads, TRUE, INFINITE);
	for (int i = 0; i < threadCount; i++)
	{
		if (hThreads[i] != NULL)
#pragma warning(suppress: 6001)
			CloseHandle(hThreads[i]);

	}

	WaitForSingleObject(hAcceptThread, INFINITE);  // ← 추가
	CloseHandle(hAcceptThread);                     // ← 추가

	CloseHandle(hcp);
	WSACleanup();
	return 0;
}

DWORD WINAPI WorkerThread(LPVOID arg) {

	int retval;
	HANDLE hcp = (HANDLE)arg;

	while (1) {

		DWORD cbTransferred = 0;
		ULONG_PTR completionKey = 0;
		SOCKETINFO* session{};
		LPOVERLAPPED pov{};
		retval = GetQueuedCompletionStatus(hcp, &cbTransferred, (PULONG_PTR)&completionKey, (LPOVERLAPPED*)&pov, INFINITE);

		// 종료 신호
		if (completionKey == (ULONG_PTR)-1) {
			return 0;
		}
		session = (SOCKETINFO*)completionKey;  // 여기서 캐스팅

		// 오류 처리 timeout / iocp 자체 실패(이건 고려 x) 
		if (retval == 0 && pov == nullptr) continue;



		// 비동기 입출력 결과확인
		if (retval == 0 || cbTransferred == 0) {
			session->disconnected = true;


			// 리턴 false에 pov 널이 아닐때, (io 실패) -> transfer = 0, key = session 포인터 pov = 해당 overlap 설정됨.
			if (retval == 0) {
				// overlap null이면서 반환값 실패 (io 실패) 연결끊김
				DWORD temp1, temp2;
				WSAGetOverlappedResult(session->sock, pov,
					&temp1, FALSE, &temp2);
				//__debugbreak();
			}
			//closesocket(session->sock);
			//delete session;

			wprintf(L"[TCP 서버] 클라이언트 종료: IP 주소 = %S, 포트 번호=%d\n",
				inet_ntoa(session->clientAddr.sin_addr), ntohs(session->clientAddr.sin_port));

			LONG remaining = InterlockedDecrement(&session->ioCount);  // 종료시 io count 감소
			if (remaining == 0) {

				closesocket(session->sock); // 서버 종료시 2번 해제하는거같은데, 실제로 실패반환이면 문제 없는듯?
				// WorkerThread - delete 직전
				AcquireSRWLockExclusive(&sessionLock);
				sessionMap.erase(session->sessionID);
				bool empty = sessionMap.empty();  // ← 락 안에서 체크
				ReleaseSRWLockExclusive(&sessionLock);
				delete session;
				if (empty)
					SetEvent(hSessionEmpty);
			}

			continue;
		}

		// 리턴값 성공!
		if (pov == &session->overlapped.recvOverlapped) {
			session->RecvQ.MoveRear(cbTransferred);
			// 받은 데이터 출력
			// 스택이 너무 커짐. 더미에서 출력을 확인하던 디버깅으로 확인하자!
			/*/
			wchar_t tempBuf[BUFSIZE + 1]{};
			int useSize = session->RecvQ.GetUseSize();
			session->RecvQ.Peek((char*)tempBuf, useSize);
			tempBuf[useSize / sizeof(wchar_t)] = L'\0';
			wprintf(L"[TCP/%S:%d] %s\n",
				inet_ntoa(session->clientAddr.sin_addr),
				ntohs(session->clientAddr.sin_port),
				tempBuf);
				//*/

				// 에코: recvQ → sendQ 복사
			int useSize = session->RecvQ.GetUseSize();
			if (session->SendQ.GetFreeSize() >= useSize) // 버퍼가 부족해서 로직 못돌아가니 사실상 여기가 연결끊기 해야함. 데이터가 안간다면 이쪽부분 따로 로직 나중에 추가하자!
			{
				session->RecvQ.Dequeue(session->SendQ.GetRearBufferPtr(), useSize);
				session->SendQ.MoveRear(useSize);
			}
			else // 연결끊기!
			{
				session->disconnected = true;
				LONG remaining = InterlockedDecrement(&session->ioCount);
				if (remaining == 0) {
					closesocket(session->sock);
					AcquireSRWLockExclusive(&sessionLock);
					sessionMap.erase(session->sessionID);
					bool empty = sessionMap.empty();
					ReleaseSRWLockExclusive(&sessionLock);
					delete session;
					if (empty) SetEvent(hSessionEmpty);
				}
				continue;
			}

			// WSASend 등록
			if (session->SendQ.GetUseSize() > 0) {
				session->SendWSABuf.buf = session->SendQ.GetFrontBufferPtr();
				session->SendWSABuf.len = session->SendQ.DirectDequeueSize();
				PostSend(session);
			}

			// recv 즉시 재등록
			session->RecvWSABuf.buf = session->RecvQ.GetRearBufferPtr();
			session->RecvWSABuf.len = session->RecvQ.DirectEnqueueSize();
			DWORD flags = 0;
			PostRecv(session);


		}
		else {
			// send 완료 → sendQ에 남은 데이터 있으면 다시 send
			session->SendQ.MoveFront(cbTransferred);

			if (session->SendQ.GetUseSize() > 0) {
				session->SendWSABuf.buf = session->SendQ.GetFrontBufferPtr();
				session->SendWSABuf.len = session->SendQ.DirectDequeueSize();
				PostSend(session);
			}


		}

		LONG remaining = InterlockedDecrement(&session->ioCount);  // recv/send 완료시 io count 감소

		if (session->disconnected && remaining == 0) {
			closesocket(session->sock);
			// WorkerThread - delete 직전
			AcquireSRWLockExclusive(&sessionLock);
			sessionMap.erase(session->sessionID);
			bool empty = sessionMap.empty();  // ← 락 안에서 체크
			ReleaseSRWLockExclusive(&sessionLock);
			delete session;
			if (empty)
				SetEvent(hSessionEmpty);
		}
	}
	return 0;

}

DWORD WINAPI AcceptThread(LPVOID arg) {
	ACCEPTINFO* info = (ACCEPTINFO*)arg;

	SOCKADDR_IN clientaddr{};
	int addrlen;
	DWORD recvbytes{}, flags{};

	while (1) {
		addrlen = sizeof(clientaddr);
		SOCKET client_sock = accept(info->listen_sock, (SOCKADDR*)&clientaddr, &addrlen);
		if (client_sock == INVALID_SOCKET) break;

		wprintf(L"[TCP 서버] 클라이언트 접속: IP 주소 =%S, 포트 번호 =%d\n",
			inet_ntoa(clientaddr.sin_addr), ntohs(clientaddr.sin_port));

		SOCKETINFO* ptr = new SOCKETINFO;
		ptr->sessionID = GenerateSessionID();
		// AcceptThread - 추가
		AcquireSRWLockExclusive(&sessionLock);
		sessionMap[ptr->sessionID] = ptr;
		ReleaseSRWLockExclusive(&sessionLock);

		ZeroMemory(&ptr->overlapped, sizeof(ptr->overlapped));
		ptr->sock = client_sock;
		ptr->clientAddr = clientaddr;
		ptr->RecvWSABuf.buf = ptr->RecvQ.GetRearBufferPtr();
		ptr->RecvWSABuf.len = ptr->RecvQ.DirectEnqueueSize();

		CreateIoCompletionPort((HANDLE)client_sock, info->hcp, (ULONG_PTR)ptr, 0);

		flags = 0;
		PostRecv(ptr);
	}
	return 0;
}

void PostRecv(SOCKETINFO* session) {
	if (session->disconnected) return;

	InterlockedIncrement(&session->ioCount);
	DWORD flags = 0;
	if (WSARecv(session->sock, &session->RecvWSABuf, 1, nullptr, &flags, &session->overlapped.recvOverlapped, NULL) == SOCKET_ERROR) {
		if (WSAGetLastError() != WSA_IO_PENDING) {
			// 즉 시 실패 -> iocount 되돌리고 종료
			LONG remaining = InterlockedDecrement(&session->ioCount);
			session->disconnected = true;
			if (remaining == 0) {
				closesocket(session->sock);
				//  - delete 직전
				AcquireSRWLockExclusive(&sessionLock);
				sessionMap.erase(session->sessionID);
				bool empty = sessionMap.empty();  // ← 락 안에서 체크
				ReleaseSRWLockExclusive(&sessionLock);
				delete session;
				if (empty)
					SetEvent(hSessionEmpty);
			}
		}
	}

}

void PostSend(SOCKETINFO* session) {
	if (session->disconnected) return;

	InterlockedIncrement(&session->ioCount);
	if (WSASend(session->sock, &session->SendWSABuf, 1, nullptr, 0, &session->overlapped.sendOverlapped, nullptr) == SOCKET_ERROR) {
		if (WSAGetLastError() != WSA_IO_PENDING) {
			LONG remaining = InterlockedDecrement(&session->ioCount);
			session->disconnected = true;
			if (remaining == 0) {
				closesocket(session->sock);
				//  - delete 직전
				AcquireSRWLockExclusive(&sessionLock);
				sessionMap.erase(session->sessionID);
				bool empty = sessionMap.empty();  // ← 락 안에서 체크
				ReleaseSRWLockExclusive(&sessionLock);
				delete session;
				if (empty)
					SetEvent(hSessionEmpty);
			}
		}
	}
}