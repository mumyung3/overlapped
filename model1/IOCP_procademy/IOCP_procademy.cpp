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
#include "Serialization.h"
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
	LONG disconnected = 0;
	LONG sendFlag = 0;
	SRWLOCK sessionLock = SRWLOCK_INIT; // 샌드큐 락을 세션락으로 변경
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
bool OnRecv(SOCKETINFO* session, CPacket& packet);
void TryCloseSession(SOCKETINFO* session);
bool SendPacket(__int64 sessionID, CPacket& packet);
bool SendPacket_NoLock(__int64 sessionID, CPacket& packet);
#pragma pack(push, 1)
struct PacketHeader {
	short len;
};

struct EchoPacket {
	PacketHeader header;
	__int64 data;

};
#pragma pack(pop)


CPacket& operator<<(CPacket& packet, const EchoPacket& byValue) {

#ifdef DEBUG_SERIALIZE
	swprintf_s(g_function, sizeof(g_function) / sizeof(wchar_t), L"%S", __FUNCTION__);
#endif

	if (packet.GetRemainSize() < sizeof(byValue)) { packet.bError = true; return packet; }
	memcpy(packet.buffer + packet.rear, &byValue, sizeof(byValue));
	packet.rear += sizeof(byValue);
	return packet;
}

CPacket& operator>>(CPacket& packet, EchoPacket& byValue) {

#ifdef DEBUG_SERIALIZE
	swprintf_s(g_function, sizeof(g_function) / sizeof(wchar_t), L"%S", __FUNCTION__);
#endif

	if (packet.GetUseSize() < sizeof(byValue)) { packet.bError = true; return packet; }
	memcpy(&byValue, packet.buffer + packet.front, sizeof(byValue));
	packet.front += sizeof(byValue);
	return packet;
}


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
					InterlockedExchange(&session->disconnected, 1);  // 쓰기
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
		if (retval == 0 && pov == nullptr) {
			continue;
		}


		// 비동기 입출력 결과확인
		if (retval == 0 || cbTransferred == 0) {
			InterlockedExchange(&session->disconnected, 1);  // 쓰기

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

			TryCloseSession(session);
			continue;
		}

		// RECV 완료처리 (gqcs 성공반환)
		if (pov == &session->overlapped.recvOverlapped) {
			// recv링버퍼 처리
			session->RecvQ.MoveRear(cbTransferred);

			bool sessionAlive = true;

			// 링버퍼 메시지 처리 while
			while (session->RecvQ.GetUseSize() >= sizeof(PacketHeader)) {
				PacketHeader header{};
				session->RecvQ.Peek((char*)&header, sizeof(PacketHeader));

				if (session->RecvQ.GetUseSize() < sizeof(PacketHeader) + header.len)
					break;

				CPacket packet{};
				session->RecvQ.Dequeue(packet.GetBufferPtr() + packet.rear, sizeof(EchoPacket));
				packet.MoveWritePos(sizeof(EchoPacket));
				if (!OnRecv(session, packet)) {
					sessionAlive = false;
					break;
				}

			}

			if (sessionAlive) {

				// recv 즉시 재등록
				session->RecvWSABuf.buf = session->RecvQ.GetRearBufferPtr();
				session->RecvWSABuf.len = session->RecvQ.DirectEnqueueSize();
				DWORD flags = 0;
				PostRecv(session);
			}

		}
		// SEND 완료 처리 (send 완료 처리랑 sendpacket은 다른듯함. sendpacket은 외부에서 이 데이터 보내줘 요청 send q 인큐 -> postsend() )
		else {
			// SEND QUEUE 정리 

			// SEND QUEUE 스레드가 동시 접근 방지 락
			//AcquireSRWLockExclusive(&session->sendQLock);
			AcquireSRWLockExclusive(&session->sessionLock);

			// send 쪽에서 보낸 데이터 로그 한번 찍어보기
			char* temp = new char[cbTransferred];
			memcpy(temp, session->SendQ.GetFrontBufferPtr(), cbTransferred);
			__int64* tempp = (__int64*)(temp + 2);
			wprintf(L"보낸 데이터 : %p\n", (__int64*)*tempp);
			delete[] temp;

			// send 완료 → sendQ에 남은 데이터 있으면 다시 send
			session->SendQ.MoveFront(cbTransferred);

			// sendflag 해제
			InterlockedExchange(&session->sendFlag, 0);
			//ReleaseSRWLockExclusive(&session->sendQLock);


			// 남은 데이터 SEND
			PostSend(session);
			ReleaseSRWLockExclusive(&session->sessionLock);

			// ONSEND 굳이 넣지 않음.

		}

		if (InterlockedOr(&session->disconnected, 0))
			TryCloseSession(session);
		else
			InterlockedDecrement(&session->ioCount);


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
	if (InterlockedOr(&session->disconnected, 0)) return;

	InterlockedIncrement(&session->ioCount);
	DWORD flags = 0;
	if (WSARecv(session->sock, &session->RecvWSABuf, 1, nullptr, &flags, &session->overlapped.recvOverlapped, NULL) == SOCKET_ERROR) {
		if (WSAGetLastError() != WSA_IO_PENDING) {
			// 즉 시 실패 -> iocount 되돌리고 종료
			InterlockedExchange(&session->disconnected, 1);  // 쓰기
			TryCloseSession(session);
		}
	}

}
// postsend를 호출하는 함수들이 이미 세션 락을 걸어줌을 전제
void PostSend(SOCKETINFO* session) {
	if (InterlockedOr(&session->disconnected, 0))  return;


	// sendflag 이전 값 1 이면 리턴
	if (InterlockedExchange(&session->sendFlag, 1) == 1) return;


	//AcquireSRWLockExclusive(&session->sendQLock);

	if (session->SendQ.GetUseSize() <= 0) {
		InterlockedExchange(&session->sendFlag, 0);  // 해제하고 return
		//ReleaseSRWLockExclusive(&session->sendQLock); // 락 릭 대응
		return;
	}
	session->SendWSABuf.buf = session->SendQ.GetFrontBufferPtr();
	session->SendWSABuf.len = session->SendQ.DirectDequeueSize();
	//ReleaseSRWLockExclusive(&session->sendQLock);

	InterlockedIncrement(&session->ioCount);
	if (WSASend(session->sock, &session->SendWSABuf, 1, nullptr, 0, &session->overlapped.sendOverlapped, nullptr) == SOCKET_ERROR) {
		if (WSAGetLastError() != WSA_IO_PENDING) {
			InterlockedExchange(&session->sendFlag, 0);
			InterlockedExchange(&session->disconnected, 1);  // 쓰기
			TryCloseSession(session);
		}
	}


}

bool OnRecv(SOCKETINFO* session, CPacket& packet) {

	EchoPacket echoPacket{};
	// 역직렬화 (구조체화)
	packet >> echoPacket;

	// 컨텐츠 로직 했다 치고.

	// 직렬화 (바이트화)
	CPacket sendPacket{};
	sendPacket << echoPacket;

	return SendPacket(session->sessionID, sendPacket);
}
// 릴리즈 세션 함수 부분
void TryCloseSession(SOCKETINFO* session) {
	LONG remaining = InterlockedDecrement(&session->ioCount);
	if (remaining == 0) {
		closesocket(session->sock);
		//  - delete 직전
		AcquireSRWLockExclusive(&sessionLock);
		sessionMap.erase(session->sessionID);
		bool empty = sessionMap.empty();  // ← 락 안에서 체크
		ReleaseSRWLockExclusive(&sessionLock);

		AcquireSRWLockExclusive(&session->sessionLock);
		// 내가 마지막인가? 확인하는 락 기능이 다름.
		delete session;
		ReleaseSRWLockExclusive(&session->sessionLock);

		if (empty)
			SetEvent(hSessionEmpty);
	}
}

bool SendPacket(__int64 sessionID, CPacket& packet) {

	// sessionId로 ptr 가져오기
	AcquireSRWLockExclusive(&sessionLock);
	auto it = sessionMap.find(sessionID);
	if (it == sessionMap.end()) {
		ReleaseSRWLockExclusive(&sessionLock);
		return false;
	}
	SOCKETINFO* session = it->second;
	ReleaseSRWLockExclusive(&sessionLock);

	// sendq 넣기
	//AcquireSRWLockExclusive(&session->sendQLock);
	AcquireSRWLockExclusive(&session->sessionLock);


	int useSize = packet.GetUseSize();
	if (session->SendQ.GetFreeSize() >= useSize) // 버퍼가 부족해서 로직 못돌아가니 사실상 여기가 연결끊기 해야함. 데이터가 안간다면 이쪽부분 따로 로직 나중에 추가하자!
	{
		session->SendQ.Enqueue((char*)packet.GetBufferPtr() + packet.front, useSize);
	}
	else // 연결끊기!
	{
		InterlockedExchange(&session->disconnected, 1);  // 쓰기
		// 락 릭 대응
		//ReleaseSRWLockExclusive(&session->sendQLock);
		ReleaseSRWLockExclusive(&session->sessionLock);
		return false;
	}
	//ReleaseSRWLockExclusive(&session->sendQLock);

	// WSASend 등록
	PostSend(session);

	ReleaseSRWLockExclusive(&session->sessionLock);

	return true;
}

// 다시 안쓰는 코드가 됨 ㅋㅋ..
bool SendPacket_NoLock(__int64 sessionID, CPacket& packet) {

	// sessionId로 ptr 가져오기
	AcquireSRWLockExclusive(&sessionLock);
	auto it = sessionMap.find(sessionID);
	if (it == sessionMap.end()) {
		ReleaseSRWLockExclusive(&sessionLock);
		return false;
	}
	SOCKETINFO* session = it->second;
	ReleaseSRWLockExclusive(&sessionLock);

	// sendq 넣기
	//AcquireSRWLockExclusive(&session->sendQLock);
	//AcquireSRWLockExclusive(&session->sessionLock);


	int useSize = packet.GetUseSize();
	if (session->SendQ.GetFreeSize() >= useSize) // 버퍼가 부족해서 로직 못돌아가니 사실상 여기가 연결끊기 해야함. 데이터가 안간다면 이쪽부분 따로 로직 나중에 추가하자!
	{
		session->SendQ.Enqueue((char*)packet.GetBufferPtr() + packet.front, useSize);
	}
	else // 연결끊기!
	{
		InterlockedExchange(&session->disconnected, 1);  // 쓰기
		// 락 릭 대응
		//ReleaseSRWLockExclusive(&session->sendQLock);
		return false;
	}
	//ReleaseSRWLockExclusive(&session->sendQLock);

	// WSASend 등록
	PostSend(session);

	//ReleaseSRWLockExclusive(&session->sessionLock);

	return true;
}

